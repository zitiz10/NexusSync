/* server.c — accept thread, worker pool, connection loop. See server.h. */
#include "net/server.h"

#include "net/http.h"
#include "net/router.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define IDLE_TIMEOUT_MS 5000u /* keep-alive idle limit, and per-read limit */
#define SEND_TIMEOUT_MS 30000u
#define MAX_REQUESTS 100u

typedef struct {
    plat_sock sock;
    char ip[64];
} pending;

struct nx_server {
    nx_app *app;
    plat_sock listener;
    uint16_t port;
    pthread_t acceptor;
    pthread_t *workers;
    uint32_t nworkers;

    pthread_mutex_t mu;
    pthread_cond_t cv;
    pending *q; /* ring of accepted connections */
    uint32_t qcap, qhead, qlen;
    uint32_t active; /* connections a worker is serving */
    plat_sock *serving; /* per worker, so stop can wake it */
    bool stop;
};

/* ---- one connection ---------------------------------------------------- */

static bool has_body_method(const char *m)
{
    return strcmp(m, "POST") == 0 || strcmp(m, "PUT") == 0 || strcmp(m, "PATCH") == 0 ||
           strcmp(m, "DELETE") == 0;
}

static const char *code_for(int status)
{
    switch (status) {
    case 411: return "length_required";
    case 413: return "too_large";
    case 414: return "uri_too_long";
    case 417: return "expectation_failed";
    case 431: return "headers_too_large";
    case 501: return "not_implemented";
    case 505: return "http_version";
    default: return "bad_request";
    }
}

/* Serves requests until the client closes, goes idle, or asks to close.
 * Returns true if the socket now belongs to the WebSocket hub. */
static bool serve_connection(nx_server *srv, plat_sock s, const char *ip)
{
    char *buf = malloc(NX_HTTP_MAX_HEAD + 1);
    if (!buf)
        return false;
    size_t have = 0;
    bool upgraded = false;
    for (uint32_t served = 0; served < MAX_REQUESTS;) {
        /* 1. Read until a complete head is buffered. */
        nx_http_req req;
        int rc;
        for (;;) {
            rc = nx_http_parse_head(buf, have, &req);
            if (rc != NX_HTTP_NEED_MORE)
                break;
            int64_t n = plat_recv(s, buf + have, NX_HTTP_MAX_HEAD - have);
            if (n <= 0)
                goto done; /* closed, error, or idle: just hang up */
            have += (size_t)n;
        }
        if (rc != NX_HTTP_OK) {
            nx_http_send_error(s, rc, code_for(rc), nx_http_reason(rc), false);
            goto done;
        }
        served++;
        bool stopping = atomic_load(&srv->app->state) == NX_APP_STOPPING;
        nx_ctx c;
        memset(&c, 0, sizeof c);
        c.app = srv->app;
        c.sock = s;
        c.req = &req;
        c.peer_ip = ip;
        c.head_only = strcmp(req.method, "HEAD") == 0;
        c.keep_alive = req.keep_alive && served < MAX_REQUESTS && !stopping;
        if (nx_url_decode(req.path, strlen(req.path), c.path, sizeof c.path) != 0) {
            nx_http_send_error(s, 400, "bad_path", "Malformed percent-encoding.", false);
            goto done;
        }

        /* 2. Body: only by Content-Length, within the route's limit. */
        uint64_t clen = req.has_body_length ? req.content_length : 0;
        if (clen && !has_body_method(req.method)) {
            nx_http_send_error(s, 400, "unexpected_body", "This method takes no body.", false);
            goto done;
        }
        size_t limit = nx_route_body_limit(req.method, c.path);
        if (clen > limit) {
            /* Refuse before reading it; the unread body makes the
             * connection unusable, so close it. */
            nx_http_send_error(s, 413, "too_large", "The request body is too large.", false);
            goto done;
        }
        if (req.expect_continue && clen &&
            plat_send_all(s, "HTTP/1.1 100 Continue\r\n\r\n", 25) != 0)
            goto done;
        uint8_t *body = NULL;
        size_t extra = have - req.head_len; /* bytes after the head already read */
        if (clen) {
            body = malloc((size_t)clen);
            if (!body)
                goto done;
            size_t got = extra < clen ? extra : (size_t)clen;
            memcpy(body, buf + req.head_len, got);
            while (got < clen) {
                int64_t n = plat_recv(s, body + got, (size_t)clen - got);
                if (n <= 0) {
                    free(body);
                    goto done;
                }
                got += (size_t)n;
            }
        }
        c.body = body;
        c.body_len = (size_t)clen;

        /* 3. Handle it. */
        nx_route(&c);
        free(body);
        if (c.upgraded) {
            upgraded = true;
            goto done;
        }
        if (!c.keep_alive)
            goto done;
        /* Keep pipelined bytes that followed this request's body. */
        size_t used = req.head_len + (extra < clen ? extra : (size_t)clen);
        memmove(buf, buf + used, have - used);
        have -= used;
    }
done:
    free(buf);
    return upgraded;
}

/* ---- threads -------------------------------------------------------------- */

static void *worker_main(void *arg)
{
    void **a = arg;
    nx_server *srv = a[0];
    uint32_t idx = (uint32_t)(uintptr_t)a[1];
    free(a);
    for (;;) {
        pthread_mutex_lock(&srv->mu);
        while (!srv->stop && srv->qlen == 0)
            pthread_cond_wait(&srv->cv, &srv->mu);
        if (srv->stop) { /* queued connections are closed by nx_server_stop */
            pthread_mutex_unlock(&srv->mu);
            return NULL;
        }
        pending p = srv->q[srv->qhead];
        srv->qhead = (srv->qhead + 1) % srv->qcap;
        srv->qlen--;
        srv->active++;
        srv->serving[idx] = p.sock;
        pthread_mutex_unlock(&srv->mu);

        plat_sock_set_timeouts(p.sock, IDLE_TIMEOUT_MS, SEND_TIMEOUT_MS);
        bool handed_off = serve_connection(srv, p.sock, p.ip);

        pthread_mutex_lock(&srv->mu);
        srv->serving[idx] = PLAT_SOCK_INVALID;
        srv->active--;
        pthread_mutex_unlock(&srv->mu);
        if (!handed_off)
            plat_sock_close(p.sock);
    }
}

static void *accept_main(void *arg)
{
    nx_server *srv = arg;
    for (;;) {
        plat_sock s;
        char ip[64] = "";
        int rc = plat_tcp_accept(srv->listener, &s, ip, sizeof ip);
        pthread_mutex_lock(&srv->mu);
        bool stop = srv->stop;
        pthread_mutex_unlock(&srv->mu);
        if (stop) {
            if (rc == 0)
                plat_sock_close(s);
            return NULL;
        }
        if (rc != 0) {
            plat_sleep_ms(10); /* e.g. out of file descriptors: back off */
            continue;
        }
        pthread_mutex_lock(&srv->mu);
        bool full = srv->qlen + srv->active >= srv->app->cfg.max_connections ||
                    srv->qlen == srv->qcap;
        if (!full) {
            pending *p = &srv->q[(srv->qhead + srv->qlen) % srv->qcap];
            p->sock = s;
            memcpy(p->ip, ip, sizeof p->ip);
            srv->qlen++;
            pthread_cond_signal(&srv->cv);
        }
        pthread_mutex_unlock(&srv->mu);
        if (full) {
            /* Answer from the accept thread, briefly, then hang up. */
            plat_sock_set_timeouts(s, 1000, 1000);
            nx_http_send_error(s, 503, "busy", "The server is busy. Try again shortly.", false);
            plat_sock_close(s);
        }
    }
}

int nx_server_start(nx_app *app, const char *host, uint16_t port, nx_server **out)
{
    nx_server *srv = calloc(1, sizeof *srv);
    if (!srv)
        return -1;
    srv->app = app;
    srv->nworkers = app->cfg.workers;
    srv->qcap = app->cfg.max_connections;
    srv->q = calloc(srv->qcap, sizeof *srv->q);
    srv->workers = calloc(srv->nworkers, sizeof *srv->workers);
    srv->serving = calloc(srv->nworkers, sizeof *srv->serving);
    if (!srv->q || !srv->workers || !srv->serving)
        goto fail;
    for (uint32_t i = 0; i < srv->nworkers; i++)
        srv->serving[i] = PLAT_SOCK_INVALID;
    pthread_mutex_init(&srv->mu, NULL);
    pthread_cond_init(&srv->cv, NULL);
    if (plat_tcp_listen(host, port, 128, &srv->listener) != 0) {
        LOG_ERROR("server", "cannot listen host=%s port=%u err=%s", host, (unsigned)port,
                  plat_last_error());
        goto fail;
    }
    if (plat_sock_local_port(srv->listener, &srv->port) != 0)
        goto fail_sock;
    uint32_t started = 0;
    for (; started < srv->nworkers; started++) {
        void **a = malloc(2 * sizeof *a);
        if (!a)
            break;
        a[0] = srv;
        a[1] = (void *)(uintptr_t)started;
        if (pthread_create(&srv->workers[started], NULL, worker_main, a) != 0) {
            free(a);
            break;
        }
    }
    if (started < srv->nworkers || pthread_create(&srv->acceptor, NULL, accept_main, srv) != 0) {
        pthread_mutex_lock(&srv->mu);
        srv->stop = true;
        pthread_cond_broadcast(&srv->cv);
        pthread_mutex_unlock(&srv->mu);
        for (uint32_t i = 0; i < started; i++)
            pthread_join(srv->workers[i], NULL);
        goto fail_sock;
    }
    LOG_INFO("server", "listening host=%s port=%u workers=%u", host, (unsigned)srv->port,
             (unsigned)srv->nworkers);
    *out = srv;
    return 0;
fail_sock:
    plat_sock_close(srv->listener);
fail:
    free(srv->q);
    free(srv->workers);
    free(srv->serving);
    free(srv);
    return -1;
}

uint16_t nx_server_port(nx_server *s) { return s->port; }

void nx_server_stop(nx_server *srv)
{
    if (!srv)
        return;
    pthread_mutex_lock(&srv->mu);
    srv->stop = true;
    /* Wake workers waiting on an idle keep-alive connection. */
    for (uint32_t i = 0; i < srv->nworkers; i++)
        if (srv->serving[i] != PLAT_SOCK_INVALID)
            plat_sock_shutdown(srv->serving[i]);
    pthread_cond_broadcast(&srv->cv);
    pthread_mutex_unlock(&srv->mu);
    /* Wake accept(): shutdown() does this on Linux; a connection to
     * ourselves does it everywhere. */
    plat_sock_shutdown(srv->listener);
    plat_sock wake;
    if (plat_tcp_connect("127.0.0.1", srv->port, &wake) == 0)
        plat_sock_close(wake);
    pthread_join(srv->acceptor, NULL);
    for (uint32_t i = 0; i < srv->nworkers; i++)
        pthread_join(srv->workers[i], NULL);
    /* Connections still queued were never served. */
    while (srv->qlen) {
        plat_sock_close(srv->q[srv->qhead].sock);
        srv->qhead = (srv->qhead + 1) % srv->qcap;
        srv->qlen--;
    }
    plat_sock_close(srv->listener);
    pthread_cond_destroy(&srv->cv);
    pthread_mutex_destroy(&srv->mu);
    free(srv->q);
    free(srv->workers);
    free(srv->serving);
    free(srv);
    LOG_INFO("server", "stopped");
}

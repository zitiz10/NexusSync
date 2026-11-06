/* ws.c — WebSocket codec and event hub. See ws.h. */
#include "net/ws.h"

#include "crypto/sha1.h"
#include "util/json_out.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== codec ================================================================ */

int nx_ws_accept_key(const char *client_key, char out[29])
{
    /* The key is base64 of 16 random bytes: exactly 24 characters. */
    if (!client_key || strlen(client_key) != 24 || client_key[22] != '=' || client_key[23] != '=')
        return -1;
    for (int i = 0; i < 22; i++) {
        char c = client_key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '/'))
            return -1;
    }
    nx_sha1_ctx c;
    nx_sha1_init(&c);
    nx_sha1_update(&c, client_key, 24);
    nx_sha1_update(&c, NX_WS_GUID, strlen(NX_WS_GUID));
    uint8_t digest[NX_SHA1_LEN];
    nx_sha1_final(&c, digest);
    nx_base64_encode(digest, sizeof digest, out);
    return 0;
}

int nx_ws_parse_frame(const uint8_t *buf, size_t len, size_t max_payload, nx_ws_frame *f)
{
    if (len < 2)
        return NX_WS_NEED_MORE;
    memset(f, 0, sizeof *f);
    f->fin = (buf[0] & 0x80) != 0;
    if (buf[0] & 0x70)
        return NX_WS_PROTOCOL_ERROR; /* no extensions were negotiated */
    f->opcode = buf[0] & 0x0F;
    bool masked = (buf[1] & 0x80) != 0;
    if (!masked)
        return NX_WS_PROTOCOL_ERROR; /* RFC 6455 §5.1: clients must mask */
    uint64_t plen = buf[1] & 0x7F;
    size_t pos = 2;
    if (plen == 126) {
        if (len < 4)
            return NX_WS_NEED_MORE;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        pos = 4;
    } else if (plen == 127) {
        if (len < 10)
            return NX_WS_NEED_MORE;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | buf[2 + i];
        pos = 10;
    }
    bool control = f->opcode >= 0x8;
    if (f->opcode != NX_WS_CONT && f->opcode != NX_WS_TEXT && f->opcode != NX_WS_BINARY &&
        f->opcode != NX_WS_CLOSE && f->opcode != NX_WS_PING && f->opcode != NX_WS_PONG)
        return NX_WS_PROTOCOL_ERROR;
    if (control && (!f->fin || plen > 125))
        return NX_WS_PROTOCOL_ERROR;
    if (plen > max_payload)
        return NX_WS_TOO_BIG;
    if (len < pos + 4)
        return NX_WS_NEED_MORE;
    memcpy(f->mask, buf + pos, 4);
    pos += 4;
    if (len - pos < plen)
        return NX_WS_NEED_MORE;
    f->payload_len = plen;
    f->header_len = pos;
    return 0;
}

void nx_ws_unmask(uint8_t *payload, size_t len, const uint8_t mask[4])
{
    for (size_t i = 0; i < len; i++)
        payload[i] ^= mask[i & 3];
}

size_t nx_ws_frame_header(uint8_t out[10], uint8_t opcode, uint64_t len)
{
    out[0] = (uint8_t)(0x80 | opcode);
    if (len < 126) {
        out[1] = (uint8_t)len;
        return 2;
    }
    if (len <= 0xFFFF) {
        out[1] = 126;
        out[2] = (uint8_t)(len >> 8);
        out[3] = (uint8_t)len;
        return 4;
    }
    out[1] = 127;
    for (int i = 0; i < 8; i++)
        out[2 + i] = (uint8_t)(len >> (56 - 8 * i));
    return 10;
}

/* ===== hub ===================================================================== */

typedef struct {
    uint8_t *frame; /* complete frame bytes */
    size_t len;
    bool is_stats; /* stats.updated: dropped first when the queue is full */
} msg;

struct nx_hub;

typedef struct client {
    struct client *next;
    struct nx_hub *hub;
    plat_sock sock;
    uint32_t user_id;
    bool admin;
    pthread_t reader;
    msg q[NX_WS_QUEUE]; /* ring, under hub mu */
    size_t qhead, qlen;
    bool closing;       /* send what is queued (a close frame last), then shut down */
    _Atomic bool dead;  /* reader finished: ready to be reaped */
    int busy;           /* broadcaster is sending to it (under hub mu) */
} client;

struct nx_hub {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    client *clients;
    size_t count;
    bool stop;
    bool work; /* something to send or reap */
    pthread_t broadcaster, ticker;
    uint64_t last_ping_ms;
};

static void q_free_all(client *c)
{
    for (size_t i = 0; i < c->qlen; i++)
        free(c->q[(c->qhead + i) % NX_WS_QUEUE].frame);
    c->qlen = 0;
}

/* Caller holds mu. Takes ownership of frame. Control frames (pong, close)
 * go to the front. */
static void enqueue(nx_hub *h, client *c, uint8_t *frame, size_t len, bool is_stats, bool urgent)
{
    if (c->closing && !urgent) {
        free(frame);
        return;
    }
    if (c->qlen == NX_WS_QUEUE) {
        /* Full: drop the oldest stats.updated, else the oldest message. */
        size_t drop = 0;
        for (size_t i = 0; i < c->qlen; i++)
            if (c->q[(c->qhead + i) % NX_WS_QUEUE].is_stats) {
                drop = i;
                break;
            }
        size_t at = (c->qhead + drop) % NX_WS_QUEUE;
        free(c->q[at].frame);
        for (size_t i = drop; i > 0; i--) /* close the gap towards the head */
            c->q[(c->qhead + i) % NX_WS_QUEUE] = c->q[(c->qhead + i - 1) % NX_WS_QUEUE];
        c->qhead = (c->qhead + 1) % NX_WS_QUEUE;
        c->qlen--;
    }
    msg m = {frame, len, is_stats};
    if (urgent) {
        c->qhead = (c->qhead + NX_WS_QUEUE - 1) % NX_WS_QUEUE;
        c->q[c->qhead] = m;
    } else {
        c->q[(c->qhead + c->qlen) % NX_WS_QUEUE] = m;
    }
    c->qlen++;
    h->work = true;
}

static uint8_t *make_frame(uint8_t opcode, const void *payload, size_t len, size_t *out_len)
{
    uint8_t hdr[10];
    size_t hl = nx_ws_frame_header(hdr, opcode, len);
    uint8_t *f = malloc(hl + len + 1);
    if (!f)
        return NULL;
    memcpy(f, hdr, hl);
    if (len)
        memcpy(f + hl, payload, len);
    *out_len = hl + len;
    return f;
}

static void *reader_main(void *arg);

int nx_hub_add(nx_hub *h, plat_sock s, uint32_t user_id, bool admin)
{
    client *c = calloc(1, sizeof *c);
    if (!c) {
        plat_sock_close(s);
        return -1;
    }
    c->hub = h;
    c->sock = s;
    c->user_id = user_id;
    c->admin = admin;
    atomic_init(&c->dead, false);
    /* Reads wait up to 60 s (we ping every 25 s); a send that blocks 5 s
     * means the client stopped reading: drop it. */
    plat_sock_set_timeouts(s, 60000, 5000);
    pthread_mutex_lock(&h->mu);
    if (h->stop || h->count >= NX_WS_MAX_CLIENTS) {
        pthread_mutex_unlock(&h->mu);
        plat_sock_close(s);
        free(c);
        return -1;
    }
    if (pthread_create(&c->reader, NULL, reader_main, c) != 0) {
        pthread_mutex_unlock(&h->mu);
        plat_sock_close(s);
        free(c);
        return -1;
    }
    c->next = h->clients;
    h->clients = c;
    h->count++;
    pthread_mutex_unlock(&h->mu);
    return 0;
}

static void reader_close(client *c, uint16_t code)
{
    nx_hub *hub = c->hub;
    uint8_t p[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    size_t len;
    uint8_t *f = make_frame(NX_WS_CLOSE, p, 2, &len);
    pthread_mutex_lock(&hub->mu);
    if (f && !c->closing)
        enqueue(hub, c, f, len, false, true);
    else
        free(f);
    c->closing = true;
    hub->work = true;
    pthread_cond_signal(&hub->cv);
    pthread_mutex_unlock(&hub->mu);
}

static void *reader_main(void *arg)
{
    client *c = arg;
    nx_hub *hub = c->hub;
    uint8_t buf[NX_WS_MAX_CLIENT_PAYLOAD + 14];
    size_t have = 0;
    for (;;) {
        int64_t n = plat_recv(c->sock, buf + have, sizeof buf - have);
        if (n <= 0)
            break; /* closed, error, or 60 s of silence */
        have += (size_t)n;
        for (;;) {
            nx_ws_frame f;
            int rc = nx_ws_parse_frame(buf, have, NX_WS_MAX_CLIENT_PAYLOAD, &f);
            if (rc == NX_WS_NEED_MORE)
                break;
            if (rc != 0) {
                reader_close(c, (uint16_t)rc);
                goto drain;
            }
            uint8_t *payload = buf + f.header_len;
            nx_ws_unmask(payload, (size_t)f.payload_len, f.mask);
            if (f.opcode == NX_WS_PING) {
                size_t len;
                uint8_t *pong = make_frame(NX_WS_PONG, payload, (size_t)f.payload_len, &len);
                pthread_mutex_lock(&hub->mu);
                if (pong)
                    enqueue(hub, c, pong, len, false, true);
                pthread_cond_signal(&hub->cv);
                pthread_mutex_unlock(&hub->mu);
            } else if (f.opcode == NX_WS_CLOSE) {
                reader_close(c, 1000);
                goto drain;
            }
            /* Text, binary, continuation, pong: the client has nothing to
             * tell us over this socket; ignore. */
            size_t used = f.header_len + (size_t)f.payload_len;
            memmove(buf, buf + used, have - used);
            have -= used;
        }
    }
    goto done;
drain:
    /* Our close frame is queued: the broadcaster sends it and then shuts
     * the socket down. Shutting down here would race it and lose the
     * frame, so read (and ignore) until that shutdown ends the stream. */
    while (plat_recv(c->sock, buf, sizeof buf) > 0) {
    }
done:
    /* Wake anything still blocked on this socket; the broadcaster reaps. */
    plat_sock_shutdown(c->sock);
    atomic_store(&c->dead, true);
    pthread_mutex_lock(&hub->mu);
    hub->work = true;
    pthread_cond_signal(&hub->cv);
    pthread_mutex_unlock(&hub->mu);
    return NULL;
}

static void reap(nx_hub *h)
{
    /* Caller holds mu. Joining a finished reader does not block. */
    client **pp = &h->clients;
    while (*pp) {
        client *c = *pp;
        if (atomic_load(&c->dead) && c->busy == 0) {
            *pp = c->next;
            h->count--;
            pthread_mutex_unlock(&h->mu);
            pthread_join(c->reader, NULL);
            plat_sock_close(c->sock);
            pthread_mutex_lock(&h->mu);
            q_free_all(c);
            free(c);
        } else {
            pp = &c->next;
        }
    }
}

static void *broadcaster_main(void *arg)
{
    nx_hub *h = arg;
    pthread_mutex_lock(&h->mu);
    while (!h->stop) {
        while (!h->work && !h->stop)
            pthread_cond_wait(&h->cv, &h->mu);
        if (h->stop)
            break;
        h->work = false;
        uint64_t now = plat_now_mono_ms();
        if (now - h->last_ping_ms >= 25000) {
            h->last_ping_ms = now;
            for (client *c = h->clients; c; c = c->next) {
                size_t len;
                uint8_t *f = make_frame(NX_WS_PING, NULL, 0, &len);
                if (f)
                    enqueue(h, c, f, len, false, false);
            }
        }
        for (client *c = h->clients; c; c = c->next) {
            if (atomic_load(&c->dead))
                continue;
            while (c->qlen > 0) {
                msg m = c->q[c->qhead];
                c->qhead = (c->qhead + 1) % NX_WS_QUEUE;
                c->qlen--;
                c->busy++;
                pthread_mutex_unlock(&h->mu);
                int rc = plat_send_all(c->sock, m.frame, m.len);
                free(m.frame);
                pthread_mutex_lock(&h->mu);
                c->busy--;
                if (rc != 0) {
                    /* Slow or gone: the reader will see the shutdown. */
                    plat_sock_shutdown(c->sock);
                    break;
                }
            }
            if (c->closing && c->qlen == 0)
                plat_sock_shutdown(c->sock);
        }
        reap(h);
    }
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static void *ticker_main(void *arg)
{
    nx_hub *h = arg;
    for (;;) {
        plat_sleep_ms(250);
        pthread_mutex_lock(&h->mu);
        bool stop = h->stop;
        if (plat_now_mono_ms() - h->last_ping_ms >= 25000)
            h->work = true;
        pthread_cond_signal(&h->cv);
        pthread_mutex_unlock(&h->mu);
        if (stop)
            return NULL;
    }
}

int nx_hub_create(nx_hub **out)
{
    nx_hub *h = calloc(1, sizeof *h);
    if (!h)
        return -1;
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->last_ping_ms = plat_now_mono_ms();
    if (pthread_create(&h->broadcaster, NULL, broadcaster_main, h) != 0) {
        free(h);
        return -1;
    }
    if (pthread_create(&h->ticker, NULL, ticker_main, h) != 0) {
        pthread_mutex_lock(&h->mu);
        h->stop = true;
        pthread_cond_signal(&h->cv);
        pthread_mutex_unlock(&h->mu);
        pthread_join(h->broadcaster, NULL);
        free(h);
        return -1;
    }
    *out = h;
    return 0;
}

void nx_hub_destroy(nx_hub *h)
{
    if (!h)
        return;
    /* Let the broadcaster deliver what is queued (e.g. server.shutdown). */
    for (int i = 0; i < 40; i++) {
        pthread_mutex_lock(&h->mu);
        bool pending = false;
        for (client *c = h->clients; c; c = c->next)
            if (c->qlen && !atomic_load(&c->dead))
                pending = true;
        pthread_mutex_unlock(&h->mu);
        if (!pending)
            break;
        plat_sleep_ms(25);
    }
    pthread_mutex_lock(&h->mu);
    h->stop = true;
    pthread_cond_signal(&h->cv);
    for (client *c = h->clients; c; c = c->next)
        plat_sock_shutdown(c->sock); /* wakes the readers */
    pthread_mutex_unlock(&h->mu);
    pthread_join(h->broadcaster, NULL);
    pthread_join(h->ticker, NULL);
    client *c = h->clients;
    while (c) {
        client *next = c->next;
        pthread_join(c->reader, NULL);
        plat_sock_close(c->sock);
        q_free_all(c);
        free(c);
        c = next;
    }
    pthread_cond_destroy(&h->cv);
    pthread_mutex_destroy(&h->mu);
    free(h);
}

void nx_hub_publish(nx_hub *h, const char *type, const char *data_json, nx_audience aud)
{
    char at[40];
    plat_format_utc_iso(plat_now_unix_ms(), at, sizeof at);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "type", type);
    nx_json_key_str(&j, "at", at);
    nx_json_key(&j, "data");
    if (data_json)
        nx_json_raw(&j, data_json, strlen(data_json));
    else
        nx_json_null(&j);
    nx_json_obj_close(&j);
    if (j.oom) {
        nx_json_free(&j);
        return;
    }
    bool is_stats = strcmp(type, "stats.updated") == 0;
    pthread_mutex_lock(&h->mu);
    for (client *c = h->clients; c; c = c->next) {
        bool to = aud.kind == NX_AUD_OTHERS
                      ? !c->admin && c->user_id != aud.user_id
                      : aud.kind == NX_AUD_ALL || c->admin ||
                            (aud.kind == NX_AUD_USER && c->user_id == aud.user_id);
        if (!to || atomic_load(&c->dead))
            continue;
        size_t len;
        uint8_t *f = make_frame(NX_WS_TEXT, j.buf, j.len, &len);
        if (f)
            enqueue(h, c, f, len, is_stats, false);
    }
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
    nx_json_free(&j);
}

void nx_hub_kick_user(nx_hub *h, uint32_t user_id)
{
    pthread_mutex_lock(&h->mu);
    for (client *c = h->clients; c; c = c->next)
        if (c->user_id == user_id && !c->closing) {
            uint8_t p[2] = {0x03, 0xE8}; /* 1000 normal closure */
            size_t len;
            uint8_t *f = make_frame(NX_WS_CLOSE, p, 2, &len);
            if (f)
                enqueue(h, c, f, len, false, true);
            c->closing = true;
        }
    h->work = true;
    pthread_cond_signal(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

size_t nx_hub_client_count(nx_hub *h)
{
    pthread_mutex_lock(&h->mu);
    size_t n = h->count;
    pthread_mutex_unlock(&h->mu);
    return n;
}

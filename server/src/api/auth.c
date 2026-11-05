/* auth.c — accounts, sessions, rate limits, auth handlers. See auth.h. */
#include "api/auth.h"

#include "cJSON.h"
#include "crypto/kdf.h"
#include "crypto/sha256.h"
#include "util/hmap.h"
#include "util/log.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== sessions ============================================================= */

#define MAX_SESSIONS 10000u
#define RL_SLOTS 1024u
#define RL_KEEP 20u /* timestamps kept per key: the larger of the two limits */
#define RL_WINDOW_MS 60000u

typedef struct {
    uint8_t token_hash[32];
    uint32_t user_id;
    uint64_t expires_ms;
    bool used;
} session;

typedef struct {
    char key[72]; /* "u:<username>" or "i:<ip>"; "" = free */
    uint64_t fails[RL_KEEP]; /* ring of failure times */
    uint32_t n, head;
    uint64_t last_ms;
} rl_slot;

struct nx_auth {
    pthread_mutex_t mu;
    uint32_t ttl_s;
    session *s;
    size_t cap, count;
    nx_hmap32 by_hash; /* token hash -> index in s */
    rl_slot *rl;
};

int nx_auth_create(uint32_t session_ttl_s, nx_auth **out)
{
    nx_auth *a = calloc(1, sizeof *a);
    if (!a)
        return -1;
    a->ttl_s = session_ttl_s;
    a->cap = 64;
    a->s = calloc(a->cap, sizeof *a->s);
    a->rl = calloc(RL_SLOTS, sizeof *a->rl);
    if (!a->s || !a->rl || nx_hmap32_init(&a->by_hash, 64) != 0) {
        free(a->s);
        free(a->rl);
        free(a);
        return -1;
    }
    pthread_mutex_init(&a->mu, NULL);
    *out = a;
    return 0;
}

void nx_auth_destroy(nx_auth *a)
{
    if (!a)
        return;
    nx_hmap32_free(&a->by_hash);
    free(a->s);
    free(a->rl);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

static bool hex_token(const char *hex, uint8_t hash[32])
{
    if (!hex || strlen(hex) != 64)
        return false;
    uint8_t raw[32];
    if (nx_unhex(hex, raw, 32) != 0)
        return false;
    nx_sha256(raw, 32, hash);
    return true;
}

/* Caller holds mu. */
static void remove_session(nx_auth *a, size_t i)
{
    nx_hmap32_remove(&a->by_hash, a->s[i].token_hash);
    a->s[i].used = false;
    a->count--;
}

int nx_session_create(nx_auth *a, uint32_t user_id, char token_hex[65])
{
    uint8_t raw[32];
    if (plat_random_bytes(raw, sizeof raw) != 0) {
        /* SPEC §13: never fall back to a weak source. */
        LOG_ERROR("auth", "secure random bytes unavailable");
        return -1;
    }
    session ns = {.user_id = user_id, .used = true};
    nx_sha256(raw, 32, ns.token_hash);
    uint64_t now = plat_now_unix_ms();
    ns.expires_ms = now + (uint64_t)a->ttl_s * 1000u;
    pthread_mutex_lock(&a->mu);
    size_t slot = SIZE_MAX, oldest = 0;
    for (size_t i = 0; i < a->cap; i++) {
        if (a->s[i].used && a->s[i].expires_ms <= now)
            remove_session(a, i); /* sweep expired ones while we are here */
        if (!a->s[i].used && slot == SIZE_MAX)
            slot = i;
        if (a->s[i].used && a->s[i].expires_ms < a->s[oldest].expires_ms)
            oldest = i;
    }
    if (slot == SIZE_MAX && a->cap < MAX_SESSIONS) {
        size_t cap = a->cap * 2 > MAX_SESSIONS ? MAX_SESSIONS : a->cap * 2;
        session *ns2 = realloc(a->s, cap * sizeof *ns2);
        if (ns2) {
            memset(ns2 + a->cap, 0, (cap - a->cap) * sizeof *ns2);
            slot = a->cap;
            a->s = ns2;
            a->cap = cap;
        }
    }
    if (slot == SIZE_MAX) { /* full: the session closest to expiry goes */
        remove_session(a, oldest);
        slot = oldest;
    }
    uint64_t *v = nx_hmap32_upsert(&a->by_hash, ns.token_hash, NULL);
    int rc = -1;
    if (v) {
        *v = slot;
        a->s[slot] = ns;
        a->count++;
        rc = 0;
    }
    pthread_mutex_unlock(&a->mu);
    if (rc == 0)
        nx_hex(raw, 32, token_hex);
    return rc;
}

bool nx_session_lookup(nx_auth *a, const char *token_hex, uint32_t *user_id)
{
    uint8_t h[32];
    if (!hex_token(token_hex, h))
        return false;
    bool ok = false;
    pthread_mutex_lock(&a->mu);
    uint64_t *v = nx_hmap32_get(&a->by_hash, h);
    if (v) {
        session *s = &a->s[*v];
        if (s->expires_ms > plat_now_unix_ms()) {
            *user_id = s->user_id;
            ok = true;
        } else {
            remove_session(a, (size_t)*v);
        }
    }
    pthread_mutex_unlock(&a->mu);
    return ok;
}

void nx_session_delete(nx_auth *a, const char *token_hex)
{
    uint8_t h[32];
    if (!hex_token(token_hex, h))
        return;
    pthread_mutex_lock(&a->mu);
    uint64_t *v = nx_hmap32_get(&a->by_hash, h);
    if (v)
        remove_session(a, (size_t)*v);
    pthread_mutex_unlock(&a->mu);
}

void nx_session_delete_user(nx_auth *a, uint32_t user_id)
{
    pthread_mutex_lock(&a->mu);
    for (size_t i = 0; i < a->cap; i++)
        if (a->s[i].used && a->s[i].user_id == user_id)
            remove_session(a, i);
    pthread_mutex_unlock(&a->mu);
}

size_t nx_session_count(nx_auth *a)
{
    pthread_mutex_lock(&a->mu);
    size_t n = a->count;
    pthread_mutex_unlock(&a->mu);
    return n;
}

/* ===== rate limits ================================================================ */

/* Caller holds mu. Finds the slot for key, or (create) takes a free or the
 * least recently used one. */
static rl_slot *rl_find(nx_auth *a, const char *key, bool create)
{
    rl_slot *free_s = NULL, *lru = &a->rl[0];
    for (size_t i = 0; i < RL_SLOTS; i++) {
        rl_slot *s = &a->rl[i];
        if (s->key[0] && strcmp(s->key, key) == 0)
            return s;
        if (!s->key[0] && !free_s)
            free_s = s;
        if (s->last_ms < lru->last_ms)
            lru = s;
    }
    if (!create)
        return NULL;
    rl_slot *s = free_s ? free_s : lru;
    memset(s, 0, sizeof *s);
    snprintf(s->key, sizeof s->key, "%s", key);
    return s;
}

static uint32_t rl_recent(const rl_slot *s, uint64_t now)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->n; i++)
        if (now - s->fails[(s->head + i) % RL_KEEP] < RL_WINDOW_MS)
            n++;
    return n;
}

static void rl_keys(const char *username, const char *ip, char uk[72], char ik[72])
{
    snprintf(uk, 72, "u:%s", username ? username : "");
    snprintf(ik, 72, "i:%s", ip ? ip : "");
}

bool nx_rl_blocked(nx_auth *a, const char *username, const char *ip, uint64_t now_ms)
{
    char uk[72], ik[72];
    rl_keys(username, ip, uk, ik);
    pthread_mutex_lock(&a->mu);
    rl_slot *us = username ? rl_find(a, uk, false) : NULL, *is = rl_find(a, ik, false);
    bool blocked = (us && rl_recent(us, now_ms) >= NX_LOGIN_FAILS_PER_USER) ||
                   (is && rl_recent(is, now_ms) >= NX_LOGIN_FAILS_PER_IP);
    pthread_mutex_unlock(&a->mu);
    return blocked;
}

static void rl_add(rl_slot *s, uint64_t now)
{
    if (s->n < RL_KEEP) {
        s->fails[(s->head + s->n) % RL_KEEP] = now;
        s->n++;
    } else {
        s->fails[s->head] = now; /* overwrite the oldest */
        s->head = (s->head + 1) % RL_KEEP;
    }
    s->last_ms = now;
}

void nx_rl_fail(nx_auth *a, const char *username, const char *ip, uint64_t now_ms)
{
    char uk[72], ik[72];
    rl_keys(username, ip, uk, ik);
    pthread_mutex_lock(&a->mu);
    if (username) {
        rl_slot *us = rl_find(a, uk, true);
        rl_add(us, now_ms);
    }
    rl_slot *is = rl_find(a, ik, true);
    rl_add(is, now_ms);
    pthread_mutex_unlock(&a->mu);
}

/* ===== passwords and validation ======================================================= */

int nx_password_set(nx_user *u, const char *password, uint32_t iterations)
{
    if (plat_random_bytes(u->salt, sizeof u->salt) != 0)
        return -1;
    u->pbkdf2_iters = iterations;
    nx_pbkdf2_sha256(password, strlen(password), u->salt, sizeof u->salt, iterations, u->pw_hash,
                     sizeof u->pw_hash);
    return 0;
}

bool nx_password_check(const nx_user *u, const char *password)
{
    if (u->pbkdf2_iters == 0)
        return false;
    uint8_t h[32];
    nx_pbkdf2_sha256(password, strlen(password), u->salt, sizeof u->salt, u->pbkdf2_iters, h,
                     sizeof h);
    return nx_ct_equal(h, u->pw_hash, sizeof h);
}

bool nx_username_normalize(const char *in, char out[NX_USERNAME_MAX + 1])
{
    size_t n = strlen(in);
    if (n < 3 || n > NX_USERNAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char ch = in[i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch + 32);
        bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (!alnum && !(i > 0 && (ch == '.' || ch == '_' || ch == '-')))
            return false;
        out[i] = ch;
    }
    out[n] = '\0';
    return true;
}

bool nx_utf8_valid(const char *str, size_t n)
{
    const unsigned char *s = (const unsigned char *)str;
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t len;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > n)
            return false;
        for (size_t k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        /* Overlong forms, surrogates, and values past U+10FFFF are invalid. */
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

bool nx_display_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > NX_DISPLAY_MAX || !nx_utf8_valid(s, n))
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
            return false;
    return s[0] != ' ' && s[n - 1] != ' ';
}

/* ===== handlers ========================================================================= */

/* Parses the JSON object body. On failure replies and returns NULL. */
static cJSON *json_body(nx_ctx *c)
{
    const char *ct = nx_http_get_header(c->req, "content-type");
    if (!ct || strncmp(ct, "application/json", 16) != 0) {
        nx_reply_error(c, 415, "unsupported_media_type", "Send JSON (Content-Type: application/json).");
        return NULL;
    }
    cJSON *j = c->body_len ? cJSON_ParseWithLength((const char *)c->body, c->body_len) : NULL;
    if (!j || !cJSON_IsObject(j)) {
        cJSON_Delete(j);
        nx_reply_error(c, 400, "invalid_json", "The request body is not a JSON object.");
        return NULL;
    }
    return j;
}

static const char *jstr(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

static int set_cookie_and_reply(nx_ctx *c, int status, const nx_user *u)
{
    char token[65];
    if (nx_session_create(c->app->auth, u->id, token) != 0)
        return nx_reply_error(c, 500, "internal", "Could not create a session.");
    char extra[256];
    snprintf(extra, sizeof extra,
             "Set-Cookie: nx_session=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%" PRIu32 "\r\n"
             "Cache-Control: no-store\r\n",
             token, c->app->cfg.session_ttl_s);
    nx_json j;
    nx_json_init(&j);
    nx_json_user(&j, u);
    int rc = nx_reply_json(c, status, &j, extra);
    nx_json_free(&j);
    return rc;
}

void nx_h_join(nx_ctx *c)
{
    nx_app *app = c->app;
    uint64_t now = plat_now_mono_ms();
    if (nx_rl_blocked(app->auth, NULL, c->peer_ip, now)) {
        nx_reply_error(c, 429, "too_many_attempts", "Too many attempts. Try again in a minute.");
        return;
    }
    cJSON *b = json_body(c);
    if (!b)
        return;
    const char *username = jstr(b, "username"), *display = jstr(b, "displayName"),
               *password = jstr(b, "password"), *code = jstr(b, "joinCode");
    nx_user u;
    memset(&u, 0, sizeof u);
    if (!username || !display || !password || !code) {
        nx_reply_error(c, 400, "missing_fields", "username, displayName, password, and joinCode are required.");
    } else if (!nx_username_normalize(username, u.username)) {
        nx_reply_error(c, 400, "invalid_username",
                       "Usernames are 3 to 32 letters, digits, dots, dashes, or underscores.");
    } else if (!nx_display_valid(display)) {
        nx_reply_error(c, 400, "invalid_display_name", "Display names are 1 to 64 characters.");
    } else if (strlen(password) < NX_PASSWORD_MIN || strlen(password) > NX_PASSWORD_MAX) {
        nx_reply_error(c, 400, "weak_password", "Passwords need at least 10 characters.");
    } else {
        char want[NX_JOIN_CODE_LEN + 1], got[NX_JOIN_CODE_LEN + 1];
        nx_app_join_code(app, want);
        size_t cl = strlen(code);
        bool code_ok = cl == NX_JOIN_CODE_LEN;
        for (size_t i = 0; code_ok && i < cl; i++)
            got[i] = code[i] >= 'a' && code[i] <= 'z' ? (char)(code[i] - 32) : code[i];
        got[NX_JOIN_CODE_LEN] = '\0';
        code_ok = code_ok && nx_ct_equal(got, want, NX_JOIN_CODE_LEN);
        if (!code_ok) {
            nx_rl_fail(app->auth, NULL, c->peer_ip, now);
            nx_reply_error(c, 403, "bad_join_code", "That join code is not right. Ask the lab admin.");
        } else {
            snprintf(u.display, sizeof u.display, "%s", display);
            u.role = NX_ROLE_STUDENT;
            u.status = NX_STATUS_ACTIVE;
            u.created_at = plat_now_unix_ms();
            u.last_seen = u.created_at;
            int rc = nx_password_set(&u, password, app->pbkdf2_iters);
            if (rc == 0)
                rc = nx_engine_add_user(app->engine, &u);
            if (rc == NX_ENGINE_EXISTS)
                nx_reply_error(c, 409, "username_taken", "That username is taken.");
            else if (rc != 0)
                nx_reply_error(c, 500, "internal", "Could not create the account.");
            else {
                LOG_INFO("auth", "joined user_id=%" PRIu32 " username=%s", u.id, u.username);
                set_cookie_and_reply(c, 201, &u);
            }
        }
    }
    cJSON_Delete(b);
}

void nx_h_login(nx_ctx *c)
{
    nx_app *app = c->app;
    cJSON *b = json_body(c);
    if (!b)
        return;
    const char *username = jstr(b, "username"), *password = jstr(b, "password");
    char uname[NX_USERNAME_MAX + 1];
    uint64_t now = plat_now_mono_ms();
    if (!username || !password || strlen(password) > NX_PASSWORD_MAX) {
        nx_reply_error(c, 400, "missing_fields", "username and password are required.");
        cJSON_Delete(b);
        return;
    }
    bool name_ok = nx_username_normalize(username, uname);
    if (!name_ok)
        snprintf(uname, sizeof uname, "-");
    if (nx_rl_blocked(app->auth, uname, c->peer_ip, now)) {
        nx_reply_error(c, 429, "too_many_attempts", "Too many attempts. Try again in a minute.");
        cJSON_Delete(b);
        return;
    }
    nx_user u;
    bool found = name_ok && nx_catalog_user_by_name(nx_engine_catalog(app->engine), uname, &u);
    bool ok;
    if (found) {
        ok = nx_password_check(&u, password);
    } else {
        /* Same work as a real check, so response time does not reveal
         * which usernames exist. */
        nx_user dummy;
        memset(&dummy, 0, sizeof dummy);
        dummy.pbkdf2_iters = app->pbkdf2_iters;
        nx_password_check(&dummy, password);
        ok = false;
    }
    if (!ok) {
        nx_rl_fail(app->auth, uname, c->peer_ip, now);
        LOG_INFO("auth", "login failed ip=%s", c->peer_ip);
        nx_reply_error(c, 401, "bad_credentials", "Wrong username or password.");
    } else if (u.status != NX_STATUS_ACTIVE) {
        nx_reply_error(c, 403, "account_disabled", "This account is disabled. Ask the lab admin.");
    } else {
        u.last_seen = plat_now_unix_ms();
        if (nx_engine_update_user(app->engine, &u) != 0)
            LOG_WARN("auth", "could not record last_seen user_id=%" PRIu32, u.id);
        LOG_INFO("auth", "login user_id=%" PRIu32, u.id);
        set_cookie_and_reply(c, 200, &u);
    }
    cJSON_Delete(b);
}

void nx_h_logout(nx_ctx *c)
{
    if (c->authed) {
        nx_session_delete(c->app->auth, c->session_token);
        nx_hub_kick_user(c->app->hub, c->user.id);
    }
    nx_reply_empty(c, 204,
                   "Set-Cookie: nx_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0\r\n");
}

void nx_h_me(nx_ctx *c)
{
    nx_json j;
    nx_json_init(&j);
    nx_json_user(&j, &c->user);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

void nx_h_admin_join_code(nx_ctx *c)
{
    char code[NX_JOIN_CODE_LEN + 1];
    nx_app_join_code(c->app, code);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "joinCode", code);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

void nx_h_admin_join_code_rotate(nx_ctx *c)
{
    char code[NX_JOIN_CODE_LEN + 1];
    if (nx_app_rotate_join_code(c->app, code) != 0) {
        nx_reply_error(c, 500, "internal", "Could not save the new join code.");
        return;
    }
    LOG_INFO("auth", "join code rotated by user_id=%" PRIu32, c->user.id);
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_str(&j, "joinCode", code);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

bool nx_auth_from_cookie(nx_ctx *c)
{
    const char *ch = nx_http_get_header(c->req, "cookie");
    uint32_t uid;
    if (!ch || !nx_cookie_get(ch, "nx_session", c->session_token, sizeof c->session_token) ||
        !nx_session_lookup(c->app->auth, c->session_token, &uid))
        return false;
    if (!nx_catalog_user_by_id(nx_engine_catalog(c->app->engine), uid, &c->user) ||
        c->user.status != NX_STATUS_ACTIVE) {
        nx_session_delete(c->app->auth, c->session_token);
        return false;
    }
    c->authed = true;
    return true;
}

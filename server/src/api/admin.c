/* admin.c — the admin API. See admin.h. Every route here is admin-only
 * (the router's role gate), so handlers do not check the role again. */
#include "api/admin.h"

#include "api/auth.h"
#include "cJSON.h"
#include "util/bytes.h"
#include "util/crc32.h"
#include "util/hmap.h"
#include "util/json_in.h"
#include "util/log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== recovery history ======================================================== */

static void recovery_path(const char *dir, char *out, size_t n)
{
    snprintf(out, n, "%s/recoveries", dir);
}

int nx_recovery_log_append(const char *data_dir, uint64_t at_ms, const nx_engine_recovery *r)
{
    char path[1100];
    recovery_path(data_dir, path, sizeof path);
    plat_file *f;
    if (plat_file_open(path, PLAT_O_WRITE | PLAT_O_CREATE, &f) != 0)
        return -1;
    uint64_t size;
    int rc = plat_file_size(f, &size);
    if (rc == 0 && size < 8) { /* new (or a torn header): write the header */
        uint8_t h[8] = {'N', 'X', 'R', 'V', 1, 0, 0, 0};
        rc = plat_pwrite_all(f, h, sizeof h, 0);
        size = 8;
    }
    if (rc == 0) {
        /* Append after the last whole record; a torn tail is overwritten. */
        uint64_t at = 8 + (size - 8) / NX_RECOVERY_REC * NX_RECOVERY_REC;
        uint8_t rec[NX_RECOVERY_REC] = {0};
        nx_put_u64(rec + 0, at_ms);
        nx_put_u64(rec + 8, r->records_replayed);
        nx_put_u64(rec + 16, r->txns_redone);
        nx_put_u64(rec + 24, r->txns_discarded);
        nx_put_u64(rec + 32, r->pages_repaired);
        nx_put_u64(rec + 40, r->wal_bytes_cut);
        nx_put_u64(rec + 48, r->ms);
        nx_put_u32(rec + 60, nx_crc32(0, rec, 60));
        rc = plat_pwrite_all(f, rec, sizeof rec, at);
    }
    if (rc == 0)
        rc = plat_fsync_file(f);
    if (plat_file_close(f) != 0)
        rc = -1;
    return rc;
}

int nx_recovery_log_read(const char *data_dir, nx_recovery_entry *out, size_t max)
{
    char path[1100];
    recovery_path(data_dir, path, sizeof path);
    if (!plat_path_exists(path))
        return 0;
    plat_file *f;
    if (plat_file_open(path, 0, &f) != 0)
        return -1;
    uint64_t size;
    uint8_t h[8];
    int n = 0;
    if (plat_file_size(f, &size) != 0 || size < 8 || plat_pread(f, h, 8, 0) != 8 ||
        memcmp(h, "NXRV", 4) != 0 || nx_get_u16(h + 4) != 1) {
        plat_file_close(f);
        return size < 8 ? 0 : -1;
    }
    uint64_t count = (size - 8) / NX_RECOVERY_REC;
    for (uint64_t i = count; i-- > 0 && (size_t)n < max;) {
        uint8_t rec[NX_RECOVERY_REC];
        if (plat_pread(f, rec, sizeof rec, 8 + i * NX_RECOVERY_REC) != (int64_t)sizeof rec)
            break;
        if (nx_get_u32(rec + 60) != nx_crc32(0, rec, 60))
            continue; /* torn or damaged: skip it */
        nx_recovery_entry *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->at_ms = nx_get_u64(rec + 0);
        e->r.records_replayed = nx_get_u64(rec + 8);
        e->r.txns_redone = nx_get_u64(rec + 16);
        e->r.txns_discarded = nx_get_u64(rec + 24);
        e->r.pages_repaired = nx_get_u64(rec + 32);
        e->r.wal_bytes_cut = nx_get_u64(rec + 40);
        e->r.ms = nx_get_u64(rec + 48);
    }
    plat_file_close(f);
    return n;
}

void nx_h_admin_recoveries(nx_ctx *c)
{
    nx_recovery_entry *v = calloc(NX_RECOVERY_SHOWN, sizeof *v);
    int n = v ? nx_recovery_log_read(c->app->cfg.data_dir, v, NX_RECOVERY_SHOWN) : -1;
    if (n < 0) {
        free(v);
        nx_reply_error(c, 500, "internal", "Could not read the recovery history.");
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "items");
    nx_json_arr_open(&j);
    for (int i = 0; i < n; i++) {
        const nx_recovery_entry *e = &v[i];
        char at[40];
        plat_format_utc_iso(e->at_ms, at, sizeof at);
        bool clean = e->r.records_replayed == 0 && e->r.txns_discarded == 0 && e->r.pages_repaired == 0 &&
                     e->r.wal_bytes_cut == 0;
        nx_json_obj_open(&j);
        nx_json_key_str(&j, "at", at);
        nx_json_key_u64(&j, "recordsReplayed", e->r.records_replayed);
        nx_json_key_u64(&j, "transactionsRedone", e->r.txns_redone);
        nx_json_key_u64(&j, "transactionsDiscarded", e->r.txns_discarded);
        nx_json_key_u64(&j, "pagesRepaired", e->r.pages_repaired);
        nx_json_key_u64(&j, "walBytesCut", e->r.wal_bytes_cut);
        nx_json_key_u64(&j, "durationMs", e->r.ms);
        /* "clean": nothing to redo (a normal start); "recovered": the log
         * brought back committed work after an unexpected stop. */
        nx_json_key_str(&j, "result", clean ? "clean" : "recovered");
        nx_json_obj_close(&j);
    }
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    free(v);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

/* ===== users ===================================================================== */

typedef struct {
    nx_hmap64 files, bytes; /* owner id -> count / sum of sizes */
    bool oom;
} per_user;

static bool tally_owner(const nx_file *f, void *ctx)
{
    per_user *p = ctx;
    if (f->flags & NX_FILE_DELETED)
        return false;
    uint64_t n = 0, b = 0;
    nx_hmap64_get(&p->files, f->owner_id, &n);
    nx_hmap64_get(&p->bytes, f->owner_id, &b);
    if (nx_hmap64_put(&p->files, f->owner_id, n + 1) != 0 ||
        nx_hmap64_put(&p->bytes, f->owner_id, b + f->size) != 0)
        p->oom = true;
    return false;
}

static void json_admin_user(nx_json *j, const nx_user *u, const per_user *p)
{
    char created[40], seen[40];
    uint64_t files = 0, bytes = 0;
    if (p) {
        nx_hmap64_get(&p->files, u->id, &files);
        nx_hmap64_get(&p->bytes, u->id, &bytes);
    }
    plat_format_utc_iso(u->created_at, created, sizeof created);
    plat_format_utc_iso(u->last_seen, seen, sizeof seen);
    nx_json_obj_open(j);
    nx_json_key_u64(j, "id", u->id);
    nx_json_key_str(j, "username", u->username);
    nx_json_key_str(j, "displayName", u->display);
    nx_json_key_str(j, "role", u->role == NX_ROLE_ADMIN ? "admin" : "student");
    nx_json_key_str(j, "status", u->status == NX_STATUS_DISABLED ? "disabled" : "active");
    nx_json_key_u64(j, "files", files);
    nx_json_key_u64(j, "bytes", bytes);
    nx_json_key_str(j, "createdAt", created);
    nx_json_key(j, "lastSeenAt");
    if (u->last_seen)
        nx_json_str(j, seen);
    else
        nx_json_null(j);
    nx_json_obj_close(j);
}

static int load_per_user(nx_engine *e, per_user *p)
{
    memset(p, 0, sizeof *p);
    if (nx_hmap64_init(&p->files, 64) != 0)
        return -1;
    if (nx_hmap64_init(&p->bytes, 64) != 0) {
        nx_hmap64_free(&p->files);
        return -1;
    }
    nx_file *none;
    size_t n;
    if (nx_catalog_files(nx_engine_catalog(e), tally_owner, p, &none, &n) != 0 || p->oom) {
        nx_hmap64_free(&p->files);
        nx_hmap64_free(&p->bytes);
        return -1;
    }
    free(none);
    return 0;
}

void nx_h_admin_users(nx_ctx *c)
{
    nx_engine *e = c->app->engine;
    nx_user *users;
    size_t n;
    per_user p;
    if (nx_catalog_users(nx_engine_catalog(e), &users, &n) != 0) {
        nx_reply_error(c, 500, "internal", "Could not read the users.");
        return;
    }
    if (load_per_user(e, &p) != 0) {
        free(users);
        nx_reply_error(c, 500, "internal", "Could not read the files.");
        return;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "items");
    nx_json_arr_open(&j);
    for (size_t i = 0; i < n; i++)
        json_admin_user(&j, &users[i], &p);
    nx_json_arr_close(&j);
    nx_json_obj_close(&j);
    nx_hmap64_free(&p.files);
    nx_hmap64_free(&p.bytes);
    free(users);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

/* POST /api/admin/users/{id} {role?: "admin"|"student", status?: "active"|"disabled"} */
void nx_h_admin_user_update(nx_ctx *c)
{
    nx_app *app = c->app;
    const char *ct = nx_http_get_header(c->req, "content-type");
    if (!ct || strncmp(ct, "application/json", 16) != 0) {
        nx_reply_error(c, 415, "unsupported_media_type", "Send JSON (Content-Type: application/json).");
        return;
    }
    cJSON *b = nx_json_parse(c->body, c->body_len);
    if (!b || !cJSON_IsObject(b)) {
        cJSON_Delete(b);
        nx_reply_error(c, 400, "invalid_json", "The request body is not a JSON object.");
        return;
    }
    const cJSON *jr = cJSON_GetObjectItemCaseSensitive(b, "role"),
                *js = cJSON_GetObjectItemCaseSensitive(b, "status");
    int role = -1, status = -1;
    if (jr && cJSON_IsString(jr))
        role = strcmp(jr->valuestring, "admin") == 0 ? NX_ROLE_ADMIN
               : strcmp(jr->valuestring, "student") == 0 ? NX_ROLE_STUDENT : -2;
    else if (jr)
        role = -2;
    if (js && cJSON_IsString(js))
        status = strcmp(js->valuestring, "active") == 0 ? NX_STATUS_ACTIVE
                 : strcmp(js->valuestring, "disabled") == 0 ? NX_STATUS_DISABLED : -2;
    else if (js)
        status = -2;
    cJSON_Delete(b);
    if (role == -2 || status == -2 || (role < 0 && status < 0)) {
        nx_reply_error(c, 400, "invalid_change",
                       "Send role (admin or student) and/or status (active or disabled).");
        return;
    }
    if (c->id > UINT32_MAX) {
        nx_reply_error(c, 404, "not_found", "No such user.");
        return;
    }
    nx_user u;
    if (!nx_catalog_user_by_id(nx_engine_catalog(app->engine), (uint32_t)c->id, &u)) {
        nx_reply_error(c, 404, "not_found", "No such user.");
        return;
    }
    /* An admin cannot demote or disable themself: the lab would be left
     * without anyone able to undo it (the server terminal can still
     * create an admin, but that is not a web action). */
    if (u.id == c->user.id) {
        nx_reply_error(c, 409, "cannot_change_self",
                       "You cannot change your own role or status. Ask another admin.");
        return;
    }
    if (role >= 0)
        u.role = (uint8_t)role;
    if (status >= 0)
        u.status = (uint8_t)status;
    if (nx_engine_update_user(app->engine, &u) != 0) {
        nx_reply_error(c, 500, "internal", "Could not save the change.");
        return;
    }
    if (u.status == NX_STATUS_DISABLED) {
        /* Signed out everywhere, at once (SPEC §13). */
        nx_session_delete_user(app->auth, u.id);
        nx_hub_kick_user(app->hub, u.id);
    }
    LOG_INFO("admin", "user changed user_id=%" PRIu32 " role=%s status=%s by=%" PRIu32, u.id,
             u.role == NX_ROLE_ADMIN ? "admin" : "student",
             u.status == NX_STATUS_DISABLED ? "disabled" : "active", c->user.id);
    per_user p;
    bool counted = load_per_user(app->engine, &p) == 0;
    nx_json j;
    nx_json_init(&j);
    json_admin_user(&j, &u, counted ? &p : NULL);
    if (counted) {
        nx_hmap64_free(&p.files);
        nx_hmap64_free(&p.bytes);
    }
    nx_reply_json(c, 200, &j, NULL);
    nx_json_free(&j);
}

/* ===== events, addresses ===================================================== */

void nx_h_admin_events(nx_ctx *c)
{
    char v[16];
    size_t limit = 200;
    if (nx_query_get(c->req->query, "limit", v, sizeof v)) {
        char *end;
        unsigned long n = strtoul(v, &end, 10);
        if (v[0] < '0' || v[0] > '9' || *end || n == 0 || n > NX_EVENT_LOG) {
            nx_reply_error(c, 400, "bad_limit", "limit must be 1 to 256.");
            return;
        }
        limit = n;
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key(&j, "items");
    nx_hub_recent(c->app->hub, limit, &j);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

void nx_h_admin_addresses(nx_ctx *c)
{
    char ips[16][16];
    int n = plat_lan_ipv4(ips, 16);
    /* Only addresses other laptops can really reach: all of them when the
     * server listens on every interface, else just the one it listens on
     * (none for loopback; the screen then says how to open it up). */
    const char *lh = c->app->cfg.listen_host;
    bool any = lh[0] == '\0' || strcmp(lh, "0.0.0.0") == 0;
    if (!any) {
        bool listed = false;
        for (int i = 0; i < n; i++)
            listed = listed || strcmp(ips[i], lh) == 0;
        n = 0;
        if (listed) {
            snprintf(ips[0], sizeof ips[0], "%s", lh);
            n = 1;
        }
    }
    nx_json j;
    nx_json_init(&j);
    nx_json_obj_open(&j);
    nx_json_key_u64(&j, "port", c->app->cfg.listen_port);
    nx_json_key(&j, "items");
    nx_json_arr_open(&j);
    for (int i = 0; i < n; i++) {
        char url[64];
        snprintf(url, sizeof url, "http://%s:%u", ips[i], (unsigned)c->app->cfg.listen_port);
        nx_json_obj_open(&j);
        nx_json_key_str(&j, "address", ips[i]);
        nx_json_key_str(&j, "url", url);
        nx_json_obj_close(&j);
    }
    nx_json_arr_close(&j);
    nx_json_key_str(&j, "listen", c->app->cfg.listen_host);
    nx_json_obj_close(&j);
    nx_reply_json(c, 200, &j, "Cache-Control: no-store\r\n");
    nx_json_free(&j);
}

/*
 * test_uploads.c — uploads and files over real sockets (SPEC §12, M7):
 *   - the full protocol: create, manifest, chunks, commit; the download
 *     is byte-identical and matches the SHA-256
 *   - every validation: bad names/sizes/counts/hashes, wrong manifest page
 *     or length, a changed resent page, per-chunk rejections
 *     (unknown_index, length_mismatch, hash_mismatch), commit before the
 *     manifest is complete, missing chunks (409 with indices), a wrong
 *     whole-file hash (422), the per-user session limit (429)
 *   - deduplication: an identical second upload needs nothing; versions
 *     chain; versionOf null starts a new file
 *   - resume: a new session after an interrupted one needs only the rest
 *   - cancel; access rules (owner, shared, admin); list filters and
 *     paging; detail with versions and block counts; delete; fsck clean
 */
#include "api/stats.h"
#include "crypto/sha256.h"
#include "engine/chunk.h"
#include "nx_server_util.h"
#include "util/bytes.h"

/* ---- client helpers ------------------------------------------------------- */

typedef struct {
    int status;
    char head[8192];
    uint8_t *body;
    size_t len;
} big;

/* Any request with a binary body; the response body is malloc'd. */
static int call(uint16_t port, const char *method, const char *path, const char *hdrs,
                const char *ctype, const void *body, size_t len, big *r)
{
    memset(r, 0, sizeof *r);
    plat_sock s = dial(port);
    if (s == PLAT_SOCK_INVALID)
        return -1;
    char head[4096];
    int n = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: h\r\nConnection: close\r\nX-NexusSync: 1\r\n%s"
                     "%s%s%sContent-Length: %zu\r\n\r\n",
                     method, path, hdrs ? hdrs : "", ctype ? "Content-Type: " : "",
                     ctype ? ctype : "", ctype ? "\r\n" : "", len);
    int rc = plat_send_all(s, head, (size_t)n);
    if (rc == 0 && len)
        rc = plat_send_all(s, body, len);
    size_t have = 0;
    char *end = NULL;
    while (rc == 0 && !end) {
        int64_t k = plat_recv(s, r->head + have, 1);
        if (k <= 0 || have + 2 >= sizeof r->head)
            rc = -1;
        else {
            have += (size_t)k;
            r->head[have] = '\0';
            end = strstr(r->head, "\r\n\r\n");
        }
    }
    if (rc == 0 && sscanf(r->head, "HTTP/1.1 %d", &r->status) != 1)
        rc = -1;
    const char *cl = rc == 0 ? strstr(r->head, "Content-Length: ") : NULL;
    size_t want = cl && strcmp(method, "HEAD") != 0 ? (size_t)strtoull(cl + 16, NULL, 10) : 0;
    r->body = malloc(want + 1);
    while (rc == 0 && r->body && r->len < want) {
        int64_t k = plat_recv(s, r->body + r->len, want - r->len);
        if (k <= 0)
            rc = -1;
        else
            r->len += (size_t)k;
    }
    if (r->body)
        r->body[r->len] = '\0';
    plat_sock_close(s);
    return rc;
}

static void big_free(big *r)
{
    free(r->body);
    r->body = NULL;
}

static const char *bstr(const big *r) { return r->body ? (const char *)r->body : ""; }

typedef struct {
    nx_chunk_ref refs[2048];
    size_t offs[2048];
    uint32_t n;
    char sha[65];
} plan;

static void chunk_plan(const uint8_t *d, size_t len, bool fixed, plan *p)
{
    p->n = 0;
    for (size_t pos = 0; pos < len;) {
        size_t k = fixed ? nx_fixed_cut(d + pos, len - pos) : nx_cdc_cut(d + pos, len - pos);
        nx_sha256(d + pos, k, p->refs[p->n].hash);
        p->refs[p->n].len = (uint32_t)k;
        p->offs[p->n++] = pos;
        pos += k;
    }
    uint8_t h[32];
    nx_sha256(d, len, h);
    nx_hex(h, 32, p->sha);
}

static uint8_t *manifest_body(const plan *p, uint32_t from, uint32_t n)
{
    uint8_t *b = malloc((size_t)n * 36 + 1);
    for (uint32_t i = 0; i < n; i++) {
        memcpy(b + (size_t)i * 36, p->refs[from + i].hash, 32);
        nx_put_u32(b + (size_t)i * 36 + 32, p->refs[from + i].len);
    }
    return b;
}

/* (u32 index, u32 len, data) for the given indices. */
static uint8_t *chunks_body(const plan *p, const uint8_t *d, const uint32_t *idx, size_t n, size_t *len)
{
    size_t total = 0;
    for (size_t i = 0; i < n; i++)
        total += 8 + p->refs[idx[i]].len;
    uint8_t *b = malloc(total + 1), *q = b;
    for (size_t i = 0; i < n; i++) {
        nx_put_u32(q, idx[i]);
        nx_put_u32(q + 4, p->refs[idx[i]].len);
        memcpy(q + 8, d + p->offs[idx[i]], p->refs[idx[i]].len);
        q += 8 + p->refs[idx[i]].len;
    }
    *len = total;
    return b;
}

#define NX_TEST_TILES_CAP 4096

/* Parses "[1,2,3]" after key in a JSON text into out. Returns the count. */
static size_t int_list(const char *json, const char *key, uint32_t *out, size_t cap)
{
    const char *p = strstr(json, key);
    if (!p || !(p = strchr(p, '[')))
        return 0;
    size_t n = 0;
    for (p++; *p && *p != ']' && n < cap;) {
        char *end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p)
            break;
        out[n++] = (uint32_t)v;
        p = end;
        while (*p == ',' || *p == ' ')
            p++;
    }
    return n;
}

static uint64_t json_u64(const char *json, const char *key)
{
    char k[64];
    snprintf(k, sizeof k, "\"%s\":", key);
    const char *p = strstr(json, k);
    return p ? strtoull(p + strlen(k), NULL, 10) : 0;
}

/* Starts an upload; writes the upload id. Returns the HTTP status. */
static int start(fx *f, const char *cookie, const char *name, size_t size, const plan *p,
                 const char *chunker, const char *extra_json, char uid[40])
{
    char body[1024];
    snprintf(body, sizeof body,
             "{\"name\":\"%s\",\"size\":%zu,\"sha256\":\"%s\",\"chunker\":\"%s\",\"chunkCount\":%u%s}",
             name, size, p->sha, chunker, p->n, extra_json ? extra_json : "");
    big r;
    int rc = call(f->port, "POST", "/api/uploads", cookie, "application/json", body, strlen(body), &r);
    int status = rc == 0 ? r.status : -1;
    const char *u = strstr(bstr(&r), "\"uploadId\":\"");
    if (uid && u)
        snprintf(uid, 40, "%.32s", u + 12);
    big_free(&r);
    return status;
}

/* The whole client: start, manifest, send needs, commit. Returns the
 * commit status; file id in *id, bytes sent in *sent. */
static int upload(fx *f, const char *cookie, const char *name, const uint8_t *d, size_t len,
                  const char *extra_json, uint64_t *id, uint64_t *sent)
{
    static plan p;
    chunk_plan(d, len, false, &p);
    char uid[40] = "";
    if (start(f, cookie, name, len, &p, "cdc", extra_json, uid) != 201)
        return -1;
    char path[160];
    static uint32_t need[2048];
    size_t nneed = 0;
    if (p.n) {
        uint8_t *mb = manifest_body(&p, 0, p.n);
        snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
        big r;
        if (call(f->port, "POST", path, cookie, "application/octet-stream", mb, (size_t)p.n * 36, &r) != 0 ||
            r.status != 200) {
            free(mb);
            big_free(&r);
            return -1;
        }
        nneed = int_list(bstr(&r), "\"need\"", need, 2048);
        free(mb);
        big_free(&r);
    }
    *sent = 0;
    if (nneed) {
        size_t bl;
        uint8_t *cb = chunks_body(&p, d, need, nneed, &bl);
        for (size_t i = 0; i < nneed; i++)
            *sent += p.refs[need[i]].len;
        snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
        big r;
        int rc = call(f->port, "POST", path, cookie, "application/octet-stream", cb, bl, &r);
        free(cb);
        bool ok = rc == 0 && r.status == 200 && strstr(bstr(&r), "\"rejected\":[]");
        big_free(&r);
        if (!ok)
            return -1;
    }
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    big r;
    if (call(f->port, "POST", path, cookie, NULL, NULL, 0, &r) != 0)
        return -1;
    *id = json_u64(bstr(&r), "id");
    int st = r.status;
    big_free(&r);
    return st;
}

static uint8_t *content(nx_splitmix *rng, size_t len)
{
    uint8_t *b = malloc(len + 1);
    size_t pos = 0;
    while (pos < len) {
        char row[96];
        int n = snprintf(row, sizeof row, "%" PRIu64 ",%" PRIu64 ",lab-%" PRIu64 "\n",
                         nx_splitmix_next(rng) % 100000, nx_splitmix_next(rng) % 977,
                         nx_splitmix_next(rng) % 50);
        size_t take = (size_t)n < len - pos ? (size_t)n : len - pos;
        memcpy(b + pos, row, take);
        pos += take;
    }
    return b;
}

static bool download_matches(fx *f, const char *cookie, uint64_t id, const uint8_t *d, size_t len)
{
    char path[64];
    snprintf(path, sizeof path, "/api/files/%" PRIu64 "/content", id);
    big r;
    bool ok = call(f->port, "GET", path, cookie, NULL, NULL, 0, &r) == 0 && r.status == 200 &&
              r.len == len && (len == 0 || memcmp(r.body, d, len) == 0) &&
              strstr(r.head, "Content-Disposition: attachment; filename*=UTF-8''");
    big_free(&r);
    return ok;
}

/* Joins a student and returns their cookie header. */
static void join(fx *f, const char *name, char hdr[300])
{
    char body[256];
    snprintf(body, sizeof body,
             "{\"username\":\"%s\",\"displayName\":\"%s\",\"password\":\"long-enough-1\",\"joinCode\":\"ABCD2345\"}",
             name, name);
    resp r;
    hdr[0] = '\0';
    if (req(f->port, "POST", "/api/auth/join", "X-NexusSync: 1\r\n", body, &r) == 0 && r.status == 201) {
        char ck[200];
        if (cookie_of(&r, ck))
            snprintf(hdr, 300, "Cookie: %s\r\n", ck);
    }
}

/* ---- tests -------------------------------------------------------------------------- */

static void test_protocol_and_validation(void)
{
    fx f;
    REQUIRE(fx_start(&f, "up_protocol", true, 4, 16) == 0);
    char riya[300];
    join(&f, "riya", riya);
    REQUIRE(riya[0]);
    nx_splitmix rng = {1};
    size_t len = 200000;
    uint8_t *d = content(&rng, len);
    static plan p;
    chunk_plan(d, len, false, &p);
    REQUIRE(p.n > 20);

    /* Create: every field validated. */
    char uid[40];
    CHECK(start(&f, riya, "a/b.csv", len, &p, "cdc", NULL, uid) == 400);
    CHECK(start(&f, riya, "", len, &p, "cdc", NULL, uid) == 400);
    CHECK(start(&f, riya, "x.csv", len, &p, "zip", NULL, uid) == 400);
    plan wrong = p;
    wrong.n = 3; /* far too few blocks for 200 KB */
    CHECK(start(&f, riya, "x.csv", len, &wrong, "cdc", NULL, uid) == 400);
    wrong.n = 5000; /* too many: every CDC block but the last is >= 2 KB */
    CHECK(start(&f, riya, "x.csv", len, &wrong, "cdc", NULL, uid) == 400);
    CHECK(start(&f, riya, "x.csv", (size_t)9 << 30, &p, "cdc", NULL, uid) == 400);
    memcpy(wrong.sha, "zz", 2);
    wrong.n = p.n;
    CHECK(start(&f, riya, "x.csv", len, &wrong, "cdc", NULL, uid) == 400);
    CHECK(start(&f, riya, "x.csv", len, &p, "cdc", ",\"versionOf\":999", uid) == 400);
    big r;
    REQUIRE(call(f.port, "POST", "/api/uploads", NULL, "application/json", "{}", 2, &r) == 0);
    CHECK(r.status == 401);
    big_free(&r);

    REQUIRE(start(&f, riya, "results.csv", len, &p, "cdc", NULL, uid) == 201);
    char path[160];
    uint8_t *mb = manifest_body(&p, 0, p.n);
    /* Commit before the manifest: refused. */
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 409 && strstr(bstr(&r), "manifest_incomplete"));
    big_free(&r);
    /* Manifest: wrong page, wrong length, impossible chunk length. */
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=1", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36 - 1, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    nx_put_u32(mb + 32, 100); /* a 100-byte block that is not the last */
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 400 && strstr(bstr(&r), "invalid_chunk_length"));
    big_free(&r);
    nx_put_u32(mb + 32, p.refs[0].len);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 200);
    uint32_t need[2048];
    size_t nneed = int_list(bstr(&r), "\"need\"", need, 2048);
    CHECK(nneed == p.n); /* nothing stored yet */
    big_free(&r);
    /* Resending the same page is fine; a different one is not. */
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 200);
    big_free(&r);
    mb[5] ^= 1;
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 409 && strstr(bstr(&r), "page_changed"));
    big_free(&r);
    free(mb);

    /* Chunks: send the first half; three of them broken on purpose. */
    size_t half = p.n / 2, bl;
    uint8_t *cb = chunks_body(&p, d, need, half, &bl);
    /* index 0: wrong data byte; index 1: wrong declared length (the record
     * still parses); index 2 renamed to an index that does not exist. */
    cb[8 + 10] ^= 0xFF;
    size_t off1 = 8 + p.refs[need[0]].len;
    nx_put_u32(cb + off1 + 4, p.refs[need[1]].len); /* unchanged length... */
    nx_put_u32(cb + off1, need[2]);                 /* ...but under index 2's slot */
    size_t off2 = off1 + 8 + p.refs[need[1]].len;
    nx_put_u32(cb + off2, 999999);
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", cb, bl, &r) == 0);
    CHECK(r.status == 200);
    CHECK(strstr(bstr(&r), "\"reason\":\"hash_mismatch\""));
    CHECK(strstr(bstr(&r), "\"reason\":\"length_mismatch\"") || strstr(bstr(&r), "\"index\":2,\"reason\":\"hash_mismatch\""));
    CHECK(strstr(bstr(&r), "\"index\":999999,\"reason\":\"unknown_index\""));
    uint32_t acc[2048];
    CHECK(int_list(bstr(&r), "\"accepted\"", acc, 2048) == half - 3);
    big_free(&r);
    free(cb);
    /* Garbage framing: refused as a whole. */
    uint8_t junk[6] = {1, 2, 3, 4, 5, 6};
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", junk, sizeof junk, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);

    /* Commit now: 409 lists exactly the blocks still missing. */
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 409 && strstr(bstr(&r), "missing_chunks"));
    uint32_t miss[2048];
    size_t nmiss = int_list(bstr(&r), "\"indices\"", miss, 2048);
    CHECK(nmiss == p.n - (half - 3));
    big_free(&r);
    cb = chunks_body(&p, d, miss, nmiss, &bl);
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", cb, bl, &r) == 0);
    CHECK(r.status == 200 && strstr(bstr(&r), "\"rejected\":[]"));
    big_free(&r);
    free(cb);
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 201 && strstr(bstr(&r), "\"name\":\"results.csv\"") &&
          strstr(bstr(&r), "\"version\":1") && strstr(bstr(&r), p.sha) &&
          strstr(bstr(&r), "\"newChunks\":"));
    uint64_t id = json_u64(bstr(&r), "id");
    big_free(&r);
    CHECK(download_matches(&f, riya, id, d, len));
    /* The session is gone after commit. */
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);

    /* A wrong whole-file hash is caught at commit (verify_on_commit). */
    plan liar = p;
    memset(liar.sha, 'a', 64);
    REQUIRE(start(&f, riya, "liar.csv", len, &liar, "cdc", NULL, uid) == 201);
    mb = manifest_body(&p, 0, p.n);
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    CHECK(r.status == 200 && int_list(bstr(&r), "\"need\"", need, 2048) == 0);
    big_free(&r);
    free(mb);
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 422 && strstr(bstr(&r), "file_hash_mismatch"));
    big_free(&r);

    /* Eight open sessions per user; the ninth is refused. */
    for (int i = 0; i < 8; i++)
        CHECK(start(&f, riya, "many.csv", len, &p, "cdc", NULL, uid) == 201);
    CHECK(start(&f, riya, "many.csv", len, &p, "cdc", NULL, uid) == 429);
    /* Cancel frees a slot; the cancelled id is gone. */
    snprintf(path, sizeof path, "/api/uploads/%s", uid);
    REQUIRE(call(f.port, "DELETE", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 204);
    big_free(&r);
    REQUIRE(call(f.port, "DELETE", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    CHECK(start(&f, riya, "many.csv", len, &p, "cdc", NULL, uid) == 201);
    free(d);
    fx_stop(&f);
}

static void test_dedup_versions_resume(void)
{
    fx f;
    REQUIRE(fx_start(&f, "up_dedup", true, 4, 16) == 0);
    char riya[300];
    join(&f, "riya", riya);
    nx_splitmix rng = {2};
    size_t len = 300000;
    uint8_t *d = content(&rng, len);
    uint64_t id1, id2, id3, id4, sent;
    REQUIRE(upload(&f, riya, "data.csv", d, len, NULL, &id1, &sent) == 201);
    CHECK(sent == len);
    REQUIRE(upload(&f, riya, "data.csv", d, len, NULL, &id2, &sent) == 201);
    CHECK(sent == 0); /* identical: nothing crosses the network */
    big r;
    char path[96];
    snprintf(path, sizeof path, "/api/files/%" PRIu64, id2);
    REQUIRE(call(f.port, "GET", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 200 && strstr(bstr(&r), "\"version\":2") &&
          json_u64(bstr(&r), "prevVersionId") == id1 && strstr(bstr(&r), "\"reusedPercent\":100"));
    CHECK(strstr(bstr(&r), "\"versions\":[{\"id\":"));
    CHECK(strstr(bstr(&r), "\"uniqueToFile\":0")); /* every block also in version 1 */
    big_free(&r);
    /* An edited copy sends only the blocks near the edit. */
    uint8_t *e = malloc(len + 20);
    memcpy(e, d, 150000);
    memcpy(e + 150000, "edited,row,here\n", 16);
    memcpy(e + 150016, d + 150000, len - 150000);
    REQUIRE(upload(&f, riya, "data.csv", e, len + 16, NULL, &id3, &sent) == 201);
    CHECK(sent > 0 && sent < 20000);
    fprintf(stderr, "    edited copy: sent %" PRIu64 " of %zu bytes\n", sent, len + 16);
    /* versionOf: null starts a new file with the same name. */
    REQUIRE(upload(&f, riya, "data.csv", d, len, ",\"versionOf\":null", &id4, &sent) == 201);
    snprintf(path, sizeof path, "/api/files/%" PRIu64, id4);
    REQUIRE(call(f.port, "GET", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "\"version\":1") && strstr(bstr(&r), "\"prevVersionId\":null"));
    big_free(&r);

    /* Resume: an interrupted upload (half the blocks sent, client gone);
     * a new session for the same file needs only the other half. */
    uint8_t *g = content(&rng, 250000);
    static plan p;
    chunk_plan(g, 250000, false, &p);
    char uid[40];
    REQUIRE(start(&f, riya, "big.csv", 250000, &p, "cdc", NULL, uid) == 201);
    uint8_t *mb = manifest_body(&p, 0, p.n);
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    uint32_t need[2048];
    size_t nneed = int_list(bstr(&r), "\"need\"", need, 2048);
    big_free(&r);
    size_t bl, half = nneed / 2;
    uint8_t *cb = chunks_body(&p, g, need, half, &bl);
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", cb, bl, &r) == 0);
    big_free(&r);
    free(cb);
    REQUIRE(start(&f, riya, "big.csv", 250000, &p, "cdc", NULL, uid) == 201);
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    size_t nneed2 = int_list(bstr(&r), "\"need\"", need, 2048);
    big_free(&r);
    CHECK(nneed2 == nneed - half);
    fprintf(stderr, "    resume: first session needed %zu blocks, the retry %zu\n", nneed, nneed2);
    /* Finish the retry: bytesSent counts what both sessions sent, since the
     * first session's bytes really crossed the network (D-041). */
    cb = chunks_body(&p, g, need, nneed2, &bl);
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", cb, bl, &r) == 0);
    big_free(&r);
    free(cb);
    snprintf(path, sizeof path, "/api/uploads/%s/commit", uid);
    REQUIRE(call(f.port, "POST", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 201 && json_u64(bstr(&r), "bytesSent") == 250000 &&
          strstr(bstr(&r), "\"reusedPercent\":0"));
    big_free(&r);
    free(mb);

    /* The same after a cancel: start, send a third, cancel, start again. */
    uint8_t *k = content(&rng, 120000);
    chunk_plan(k, 120000, false, &p);
    REQUIRE(start(&f, riya, "cancelled.csv", 120000, &p, "cdc", NULL, uid) == 201);
    mb = manifest_body(&p, 0, p.n);
    snprintf(path, sizeof path, "/api/uploads/%s/manifest?page=0", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", mb, (size_t)p.n * 36, &r) == 0);
    nneed = int_list(bstr(&r), "\"need\"", need, 2048);
    big_free(&r);
    free(mb);
    cb = chunks_body(&p, k, need, nneed / 3, &bl);
    snprintf(path, sizeof path, "/api/uploads/%s/chunks", uid);
    REQUIRE(call(f.port, "POST", path, riya, "application/octet-stream", cb, bl, &r) == 0);
    big_free(&r);
    free(cb);
    snprintf(path, sizeof path, "/api/uploads/%s", uid);
    REQUIRE(call(f.port, "DELETE", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 204);
    big_free(&r);
    uint64_t kid;
    REQUIRE(upload(&f, riya, "cancelled.csv", k, 120000, NULL, &kid, &sent) == 201);
    CHECK(sent < 120000); /* this session sent only the rest... */
    snprintf(path, sizeof path, "/api/files/%" PRIu64, kid);
    REQUIRE(call(f.port, "GET", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(json_u64(bstr(&r), "bytesSent") == 120000); /* ...but the file counts both */
    big_free(&r);
    /* Another user uploading the same bytes takes over nothing. */
    char kabir[300];
    join(&f, "kabir", kabir);
    REQUIRE(upload(&f, kabir, "cancelled.csv", k, 120000, NULL, &kid, &sent) == 201);
    snprintf(path, sizeof path, "/api/files/%" PRIu64, kid);
    REQUIRE(call(f.port, "GET", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(sent == 0 && json_u64(bstr(&r), "bytesSent") == 0);
    big_free(&r);
    free(k);
    nx_fsck_report fr;
    CHECK(nx_engine_fsck(f.app->engine, &fr) == 0);
    free(d);
    free(e);
    free(g);
    fx_stop(&f);
}

static void test_files_access_and_listing(void)
{
    fx f;
    REQUIRE(fx_start(&f, "up_files", true, 4, 16) == 0);
    char riya[300], kabir[300];
    join(&f, "riya", riya);
    join(&f, "kabir", kabir);
    nx_splitmix rng = {3};
    uint64_t ids[12], sent;
    uint8_t *d[12];
    for (int i = 0; i < 12; i++) {
        char name[32];
        snprintf(name, sizeof name, "%s-%02d.csv", i % 2 ? "Notes" : "lab", i);
        size_t len = 3000 + (size_t)i * 1000;
        d[i] = content(&rng, len);
        REQUIRE(upload(&f, i < 10 ? riya : kabir, name, d[i], len, NULL, &ids[i], &sent) == 201);
    }
    big r;
    /* Kabir sees only his own two files... */
    REQUIRE(call(f.port, "GET", "/api/files", kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 200 && strstr(bstr(&r), "lab-10.csv") && !strstr(bstr(&r), "lab-00.csv"));
    big_free(&r);
    char path[128];
    snprintf(path, sizeof path, "/api/files/%" PRIu64, ids[0]);
    REQUIRE(call(f.port, "GET", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404); /* not even told it exists */
    big_free(&r);
    /* ...until Riya shares one: then he can read it, not change it. */
    REQUIRE(call(f.port, "PATCH", path, riya, "application/json", "{\"shared\":true}", 15, &r) == 0);
    CHECK(r.status == 200 && strstr(bstr(&r), "\"shared\":true"));
    big_free(&r);
    REQUIRE(call(f.port, "GET", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 200);
    big_free(&r);
    CHECK(download_matches(&f, kabir, ids[0], d[0], 3000));
    REQUIRE(call(f.port, "DELETE", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 403);
    big_free(&r);
    REQUIRE(call(f.port, "PATCH", path, kabir, "application/json", "{\"shared\":false}", 16, &r) == 0);
    CHECK(r.status == 403);
    big_free(&r);
    REQUIRE(call(f.port, "PATCH", path, riya, "application/json", "{\"shared\":\"yes\"}", 16, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files?scope=shared", kabir, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "lab-00.csv") && !strstr(bstr(&r), "lab-10.csv"));
    big_free(&r);

    /* Filters, sorting, paging (Riya: 10 files). */
    REQUIRE(call(f.port, "GET", "/api/files?q=NOTES&sort=name", riya, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "Notes-01.csv") && !strstr(bstr(&r), "lab-00.csv"));
    CHECK(strstr(bstr(&r), "Notes-01.csv") < strstr(bstr(&r), "Notes-09.csv"));
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files?sort=size&limit=4", riya, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "lab-08.csv") && strstr(bstr(&r), "\"nextCursor\":\"4\""));
    CHECK(strstr(bstr(&r), "Notes-09.csv") < strstr(bstr(&r), "lab-08.csv")); /* largest first */
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files?sort=size&limit=4&cursor=8", riya, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "lab-00.csv") && strstr(bstr(&r), "\"nextCursor\":null"));
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files?sort=random", riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files?limit=0", riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    /* The admin sees everyone's files and may delete any. */
    REQUIRE(call(f.port, "GET", "/api/files?limit=200", f.admin_cookie, NULL, NULL, 0, &r) == 0);
    CHECK(strstr(bstr(&r), "lab-00.csv") && strstr(bstr(&r), "lab-10.csv"));
    big_free(&r);
    snprintf(path, sizeof path, "/api/files/%" PRIu64, ids[10]);
    REQUIRE(call(f.port, "DELETE", path, f.admin_cookie, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 204);
    big_free(&r);
    REQUIRE(call(f.port, "GET", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    snprintf(path, sizeof path, "/api/files/%" PRIu64 "/content", ids[10]);
    REQUIRE(call(f.port, "GET", path, kabir, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    /* Bad ids never reach a handler. */
    REQUIRE(call(f.port, "GET", "/api/files/0", riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files/01", riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    REQUIRE(call(f.port, "GET", "/api/files/99999999999999999999", riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    /* HEAD of the content: the length, no body. */
    snprintf(path, sizeof path, "/api/files/%" PRIu64 "/content", ids[1]);
    REQUIRE(call(f.port, "HEAD", path, riya, NULL, NULL, 0, &r) == 0);
    CHECK(r.status == 200 && strstr(r.head, "Content-Length: 4000\r\n") && r.len == 0);
    big_free(&r);
    nx_fsck_report fr;
    CHECK(nx_engine_fsck(f.app->engine, &fr) == 0);
    for (int i = 0; i < 12; i++)
        free(d[i]);
    fx_stop(&f);
}

/* GET /api/stats and /api/blockmap (M9), plus the events that drive the
 * Overview: upload.progress with real newTiles (and the anonymous copy for
 * other students) and stats.updated. */
static int get(fx *f, const char *cookie, const char *path, big *r)
{
    return call(f->port, "GET", path, cookie, NULL, NULL, 0, r);
}

static bool all_tiles_level(const char *json, unsigned level, size_t want)
{
    static uint32_t t[NX_TEST_TILES_CAP];
    size_t n = int_list(json, "\"tiles\"", t, NX_TEST_TILES_CAP);
    if (n != want)
        return false;
    for (size_t i = 0; i < n; i++)
        if (t[i] != level)
            return false;
    return true;
}

static void test_stats_and_blockmap(void)
{
    fx f;
    /* Share levels (D-040): mean share count -> 0 empty, 1 once, 2 two,
     * 3 three or four, 4 five or more. */
    CHECK(nx_share_level(0.0) == 0 && nx_share_level(1.0) == 1 && nx_share_level(1.49) == 1 &&
          nx_share_level(1.5) == 2 && nx_share_level(2.49) == 2 && nx_share_level(2.5) == 3 &&
          nx_share_level(4.49) == 3 && nx_share_level(4.5) == 4 && nx_share_level(40.0) == 4);
    REQUIRE(fx_start(&f, "up_stats", true, 4, 16) == 0);
    const char *admin = f.admin_cookie;
    big r;

    /* A fresh store: zeros, an empty map, engine internals for the admin. */
    REQUIRE(get(&f, admin, "/api/stats", &r) == 0);
    CHECK(r.status == 200 && json_u64(bstr(&r), "logicalBytes") == 0 &&
          json_u64(bstr(&r), "physicalBytes") == 0 && strstr(bstr(&r), "\"ratio\":1"));
    CHECK(strstr(bstr(&r), "\"wal\":{\"lsn\":") && strstr(bstr(&r), "\"liveHistogram\":[") &&
          strstr(bstr(&r), "\"lastScanAt\":null") && strstr(bstr(&r), "\"nextRunAt\":null") &&
          strstr(bstr(&r), "\"mine\":{\"files\":0"));
    big_free(&r);
    REQUIRE(get(&f, admin, "/api/blockmap", &r) == 0);
    CHECK(r.status == 200 && strstr(bstr(&r), "\"tileBytes\":0") && all_tiles_level(bstr(&r), 0, 384));
    big_free(&r);
    REQUIRE(get(&f, admin, "/api/blockmap?tiles=0", &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    REQUIRE(get(&f, admin, "/api/blockmap?tiles=4097", &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    REQUIRE(get(&f, admin, "/api/blockmap?tiles=1x", &r) == 0);
    CHECK(r.status == 400);
    big_free(&r);
    REQUIRE(get(&f, NULL, "/api/stats", &r) == 0);
    CHECK(r.status == 401);
    big_free(&r);

    char riya[300], kabir[300];
    join(&f, "riya", riya);
    join(&f, "kabir", kabir);
    resp wr;
    plat_sock ws_riya = ws_open(&f, riya, NULL, &wr);
    REQUIRE(ws_riya != PLAT_SOCK_INVALID && wr.status == 101);
    plat_sock ws_kabir = ws_open(&f, kabir, NULL, &wr);
    REQUIRE(ws_kabir != PLAT_SOCK_INVALID && wr.status == 101);

    /* Riya uploads. She sees her own progress with the tiles her blocks
     * landed in; Kabir gets only an anonymous copy (no id, no name). */
    nx_splitmix rng = {9};
    size_t len = 300000;
    uint8_t *d = content(&rng, len);
    uint64_t id1, id2, id3, sent;
    REQUIRE(upload(&f, riya, "notes.csv", d, len, NULL, &id1, &sent) == 201);
    static char ev[16384];
    CHECK(ws_wait_for_copy(ws_riya, "\"phase\":\"sending\"", ev, sizeof ev));
    CHECK(strstr(ev, "\"fileName\":\"notes.csv\"") && strstr(ev, "\"newTiles\":[0,") &&
          strstr(ev, ",383]"));
    CHECK(ws_wait_for_copy(ws_kabir, "\"type\":\"upload.progress\"", ev, sizeof ev));
    CHECK(strstr(ev, "\"uploadId\":null") && !strstr(ev, "notes.csv") && strstr(ev, "\"newTiles\":[0,"));
    CHECK(ws_wait_for_copy(ws_kabir, "\"type\":\"stats.updated\"", ev, sizeof ev));
    CHECK(json_u64(ev, "logicalBytes") == len && !strstr(ev, "\"wal\""));

    REQUIRE(get(&f, riya, "/api/stats", &r) == 0);
    uint64_t physical = json_u64(bstr(&r), "physicalBytes");
    CHECK(r.status == 200 && json_u64(bstr(&r), "logicalBytes") == len && physical == len);
    CHECK(strstr(bstr(&r), "\"mine\":{\"files\":1,\"bytes\":300000,\"reusedPercent\":0"));
    CHECK(!strstr(bstr(&r), "\"wal\"") && !strstr(bstr(&r), "\"containers\"")); /* admin only */
    CHECK(json_u64(bstr(&r), "uploadsThisWeek") == 1);
    big_free(&r);
    REQUIRE(get(&f, kabir, "/api/stats", &r) == 0);
    CHECK(strstr(bstr(&r), "\"mine\":{\"files\":0,\"bytes\":0,\"reusedPercent\":100"));
    big_free(&r);

    /* The map is rebuilt at most every 2 s after a change. */
    plat_sleep_ms(2100);
    REQUIRE(get(&f, riya, "/api/blockmap", &r) == 0);
    CHECK(json_u64(bstr(&r), "physicalBytes") == len && all_tiles_level(bstr(&r), 1, 384));
    big_free(&r);
    REQUIRE(get(&f, riya, "/api/blockmap?tiles=10", &r) == 0);
    CHECK(all_tiles_level(bstr(&r), 1, 10));
    big_free(&r);

    /* An identical copy: nothing new on disk, every block now shared by 2. */
    REQUIRE(upload(&f, riya, "notes.csv", d, len, ",\"versionOf\":null", &id2, &sent) == 201);
    CHECK(sent == 0);
    plat_sleep_ms(2100);
    REQUIRE(get(&f, riya, "/api/blockmap", &r) == 0);
    CHECK(json_u64(bstr(&r), "physicalBytes") == len && all_tiles_level(bstr(&r), 2, 384));
    big_free(&r);
    REQUIRE(get(&f, admin, "/api/stats", &r) == 0);
    CHECK(json_u64(bstr(&r), "logicalBytes") == 2 * len && json_u64(bstr(&r), "physicalBytes") == len &&
          json_u64(bstr(&r), "writesSkippedBytes") == len && strstr(bstr(&r), "\"ratio\":2"));
    big_free(&r);

    /* Kabir uploads something else: it lands at the end of the store, so
     * his file's tiles are the tail, and the event named the same tiles. */
    size_t len2 = 100000;
    uint8_t *e = content(&rng, len2);
    REQUIRE(upload(&f, kabir, "other.csv", e, len2, NULL, &id3, &sent) == 201);
    CHECK(ws_wait_for_copy(ws_kabir, "\"fileName\":\"other.csv\",\"phase\":\"sending\"", ev, sizeof ev));
    static uint32_t ev_tiles[NX_TEST_TILES_CAP], file_tiles[NX_TEST_TILES_CAP];
    size_t nev = int_list(ev, "\"newTiles\"", ev_tiles, NX_TEST_TILES_CAP);
    char path[96];
    snprintf(path, sizeof path, "/api/blockmap?file=%" PRIu64, id3);
    REQUIRE(get(&f, kabir, path, &r) == 0); /* right after the upload: forced rebuild */
    size_t nft = int_list(bstr(&r), "\"tiles\"", file_tiles, NX_TEST_TILES_CAP);
    big_free(&r);
    fprintf(stderr, "    other.csv: event tiles %zu, file tiles %zu (of 384)\n", nev, nft);
    CHECK(nft >= 90 && nft <= 100 && file_tiles[nft - 1] == 383 && nev == nft &&
          memcmp(ev_tiles, file_tiles, nft * sizeof *ev_tiles) == 0);
    snprintf(path, sizeof path, "/api/blockmap?file=%" PRIu64, id1);
    REQUIRE(get(&f, riya, path, &r) == 0);
    size_t n1 = int_list(bstr(&r), "\"tiles\"", file_tiles, NX_TEST_TILES_CAP);
    CHECK(r.status == 200 && n1 >= 288 && n1 <= 289 && file_tiles[0] == 0);
    big_free(&r);
    /* Riya's file is private: Kabir gets 404, as for a missing id. */
    REQUIRE(get(&f, kabir, path, &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    REQUIRE(get(&f, kabir, "/api/blockmap?file=999", &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);
    REQUIRE(get(&f, kabir, "/api/blockmap?file=abc", &r) == 0);
    CHECK(r.status == 404);
    big_free(&r);

    /* Containers: every stored byte is live, nothing to reclaim yet. */
    REQUIRE(get(&f, admin, "/api/stats", &r) == 0);
    CHECK(strstr(bstr(&r), "\"reclaimableBytes\":0") && strstr(bstr(&r), "\"pendingBytes\":0") &&
          strstr(bstr(&r), "\"liveHistogram\":[0,0,0,0,0,0,0,0,0,1]"));
    big_free(&r);

    plat_sock_close(ws_riya);
    plat_sock_close(ws_kabir);
    nx_fsck_report fr;
    CHECK(nx_engine_fsck(f.app->engine, &fr) == 0);
    free(d);
    free(e);
    fx_stop(&f);
}

static const nx_test_case TESTS[] = {
    NX_CASE(test_protocol_and_validation),
    NX_CASE(test_dedup_versions_resume),
    NX_CASE(test_files_access_and_listing),
    NX_CASE(test_stats_and_blockmap),
};

int main(int argc, char **argv)
{
    if (plat_net_init() != 0)
        return 2;
    int rc = nx_test_run(TESTS, sizeof TESTS / sizeof TESTS[0], argc, argv);
    plat_net_cleanup();
    return rc;
}

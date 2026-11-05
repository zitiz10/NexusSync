/*
 * json_out.h — builds JSON text into a growing buffer. Request JSON is
 * parsed with the vendored cJSON; responses are written with this, so
 * every byte we send is produced by code in this repo.
 *
 *   nx_json j; nx_json_init(&j);
 *   nx_json_obj_open(&j);
 *   nx_json_key_str(&j, "username", u->username);
 *   nx_json_key_u64(&j, "id", u->id);
 *   nx_json_obj_close(&j);
 *   ... j.buf, j.len ...; nx_json_free(&j);
 *
 * Commas are inserted automatically. On out-of-memory the builder stops
 * and j.oom is set; callers check it once at the end.
 */
#ifndef NX_JSON_OUT_H
#define NX_JSON_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *buf;
    size_t len, cap;
    bool oom;
    bool need_comma;
} nx_json;

void nx_json_init(nx_json *j);
void nx_json_free(nx_json *j);

void nx_json_obj_open(nx_json *j);
void nx_json_obj_close(nx_json *j);
void nx_json_arr_open(nx_json *j);
void nx_json_arr_close(nx_json *j);
/* Starts a member: writes "key": (the value follows). */
void nx_json_key(nx_json *j, const char *key);

void nx_json_str(nx_json *j, const char *s);
void nx_json_strn(nx_json *j, const char *s, size_t n);
void nx_json_u64(nx_json *j, uint64_t v);
void nx_json_i64(nx_json *j, int64_t v);
void nx_json_f64(nx_json *j, double v); /* finite only; NaN/inf become null */
void nx_json_bool(nx_json *j, bool v);
void nx_json_null(nx_json *j);
/* Inserts already-valid JSON text as a value. */
void nx_json_raw(nx_json *j, const char *json, size_t n);

/* Shorthands: key + value. */
void nx_json_key_str(nx_json *j, const char *key, const char *s);
void nx_json_key_u64(nx_json *j, const char *key, uint64_t v);
void nx_json_key_bool(nx_json *j, const char *key, bool v);

/* Base64 (RFC 4648, with padding). out needs 4 * ceil(n / 3) + 1 bytes. */
void nx_base64_encode(const uint8_t *in, size_t n, char *out);

#endif

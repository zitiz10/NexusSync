/*
 * config.h — nexussync.conf (SPEC §3): INI style, "key = value", '#'
 * comments, one setting per line. Unknown keys are errors, so a typo does
 * not silently fall back to a default.
 */
#ifndef NX_CONFIG_H
#define NX_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char listen_host[64];
    uint16_t listen_port;
    char data_dir[512];
    char web_root[512];
    uint32_t workers;
    uint32_t max_connections;
    uint8_t chunker; /* enum nx_chunker_kind */
    uint64_t container_size;
    uint32_t buffer_pool_pages;
    uint32_t checkpoint_interval_s;
    uint32_t gc_grace_s;
    bool verify_on_read;
    bool verify_on_commit;
    uint32_t upload_session_ttl_s;
    uint32_t session_ttl_s;
    char join_code[16]; /* only used by --init as the first code ("" = random); 8 chars */
} nx_config;

void nx_config_defaults(nx_config *c);
/* Parses config text over the current values. On error returns -1 and
 * writes "line N: reason" into err. */
int nx_config_parse(const char *text, nx_config *c, char *err, size_t err_len);
/* Reads and parses a file. */
int nx_config_load(const char *path, nx_config *c, char *err, size_t err_len);

#endif

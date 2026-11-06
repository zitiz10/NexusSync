/*
 * ws.h — WebSocket (RFC 6455) for live events at /ws (SPEC §11, §12).
 *
 * Codec (pure functions, unit-tested):
 *   - Sec-WebSocket-Accept = base64(SHA-1(key + GUID))
 *   - frame parsing: client frames must be masked; payloads are capped
 *   - frame headers for server frames (never masked)
 *
 * Hub: owns every upgraded connection, so WebSocket clients never occupy
 * an HTTP worker (8 open tabs would otherwise starve the server).
 *   - one reader thread per client: answers ping, handles close, notices
 *     a dead peer (no frame for 60 s; browsers answer our pings)
 *   - one broadcaster thread: the only thread that writes to clients, so
 *     frames from different threads can never interleave on a socket
 *   - each client has a bounded queue; when it is full the oldest
 *     stats.updated is dropped first, otherwise the oldest message
 *   - a ticker thread wakes the broadcaster every 250 ms (pings every 25 s)
 */
#ifndef NX_WS_H
#define NX_WS_H

#include "platform/platform.h"
#include "util/json_out.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NX_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define NX_WS_MAX_CLIENT_PAYLOAD 4096u /* the client only sends control frames */
#define NX_WS_QUEUE 64u
#define NX_WS_MAX_CLIENTS 64u

enum {
    NX_WS_CONT = 0x0,
    NX_WS_TEXT = 0x1,
    NX_WS_BINARY = 0x2,
    NX_WS_CLOSE = 0x8,
    NX_WS_PING = 0x9,
    NX_WS_PONG = 0xA
};

/* out: 28 base64 characters + NUL. key must be the client's 24-character
 * base64 nonce (16 bytes); -1 otherwise. */
int nx_ws_accept_key(const char *client_key, char out[29]);

typedef struct {
    bool fin;
    uint8_t opcode;
    uint64_t payload_len;
    size_t header_len; /* payload starts here */
    uint8_t mask[4];
} nx_ws_frame;

#define NX_WS_NEED_MORE 1
#define NX_WS_PROTOCOL_ERROR 1002 /* close code */
#define NX_WS_TOO_BIG 1009

/* Parses one client frame at buf[0..len). 0 when complete (header and
 * payload present), NX_WS_NEED_MORE, or a close code for a bad frame. */
int nx_ws_parse_frame(const uint8_t *buf, size_t len, size_t max_payload, nx_ws_frame *f);
/* Unmasks the payload in place. */
void nx_ws_unmask(uint8_t *payload, size_t len, const uint8_t mask[4]);
/* Header for an unmasked server frame with FIN set. Returns 2, 4, or 10. */
size_t nx_ws_frame_header(uint8_t out[10], uint8_t opcode, uint64_t len);

/* ---- hub ---- */

typedef struct nx_hub nx_hub;

/* NX_AUD_USER: this user and the admins. NX_AUD_OTHERS: everyone else
 * (students other than user_id), for the anonymous copy of an event. */
enum nx_audience_kind { NX_AUD_ALL, NX_AUD_ADMINS, NX_AUD_USER, NX_AUD_OTHERS };
typedef struct {
    enum nx_audience_kind kind;
    uint32_t user_id;
} nx_audience;

int nx_hub_create(nx_hub **out);
/* Closes every client and stops the threads. */
void nx_hub_destroy(nx_hub *h);
/* Takes ownership of an upgraded socket (the 101 response already sent).
 * Returns -1 (and closes the socket) when the hub is full or stopping. */
int nx_hub_add(nx_hub *h, plat_sock s, uint32_t user_id, bool admin);
/* Sends {"type":..,"at":..,"data":<data_json>} to the audience. */
void nx_hub_publish(nx_hub *h, const char *type, const char *data_json, nx_audience aud);
/* Same, but not kept in the event log: progress ticks (engine.gc
 * "compacting", engine.scan "scanning") would push the engine events out
 * of the 256-entry ring (M12). */
void nx_hub_publish_transient(nx_hub *h, const char *type, const char *data_json, nx_audience aud);
/* Drops every connection of a user (logout, disabled account). */
void nx_hub_kick_user(nx_hub *h, uint32_t user_id);
size_t nx_hub_client_count(nx_hub *h);

/* The last NX_EVENT_LOG events published (except the chatty stats.updated
 * and upload.progress), oldest first, for the admin event log (DESIGN
 * §8.6). Appends a JSON array of {type, at, data} to out. */
#define NX_EVENT_LOG 256u
void nx_hub_recent(nx_hub *h, size_t limit, nx_json *out);

#endif

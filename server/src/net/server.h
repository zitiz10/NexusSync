/*
 * server.h — the HTTP server (SPEC §2, §11).
 *
 * One accept thread pushes connections onto a bounded queue; a fixed pool
 * of workers each serves one connection at a time (keep-alive: 5 s idle,
 * at most 100 requests). When max_connections are already queued or being
 * served, a new connection gets 503 at once. A WebSocket upgrade hands the
 * socket to the hub, which frees the worker.
 */
#ifndef NX_SERVER_H
#define NX_SERVER_H

#include "api/app.h"

#include <stdint.h>

typedef struct nx_server nx_server;

/* Listens on host:port (port 0 picks a free one) and starts the threads. */
int nx_server_start(nx_app *app, const char *host, uint16_t port, nx_server **out);
uint16_t nx_server_port(nx_server *s);
/* Stops accepting, wakes idle connections, waits for workers. */
void nx_server_stop(nx_server *s);

#endif

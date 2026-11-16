/* The network side of the Saturn (NetLink) console-to-console service: the
 * TCP greeting and UDP tunnel for direct play, and STUN + the match server
 * for matchmaking (saturn.h, stun.h). It runs in the daemon's loop, once
 * per block, on non-blocking sockets and child processes; srv/main.c owns
 * the per-port server objects and the log (daemon.h).
 *
 * The daemon registers satlink_from_modem() as each saturn port's raw sink
 * (srv_set_raw), passes any test overrides, and calls satlink_poll() every
 * block. Daemon only. Portable C99, no allocation after startup. */
#ifndef V90D_SATLINK_H
#define V90D_SATLINK_H
#include <stddef.h>
#include <stdint.h>

/* octets from a console's modem, for the far player (an srv_raw_fn) */
void satlink_from_modem(void *ctx, const uint8_t *oct, size_t n);
/* once per line block */
void satlink_poll(uint32_t now_ms);

/* test overrides (the shipped daemon uses none of them): the STUN server
 * address and port, a command to run instead of wget for a match query
 * (args: action game client_ip port), and accepting any opponent address */
void satlink_set_stun(uint32_t ip, uint16_t port);
void satlink_set_match(const char *cmd);
void satlink_set_any(int on);

#endif

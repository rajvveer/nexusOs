/* ============================================================================
 * NexusOS — Cloud File Sync + Settings Sync (Header) — Phase 45
 * ============================================================================
 * Era 6 (Polish). Two of the four Phase 45 "Cloud & Sync" pillars:
 *
 *   1. Cloud file sync — a tiny, line-oriented sync protocol over TCP. A
 *      NexusOS instance can act as a sync SERVER (serve its RAM-FS files to a
 *      peer) or a sync CLIENT (push/pull files to/from a peer). Designed for
 *      the constrained TCP stack: 1 KB RX window, 4 KB per-file cap, integer
 *      math only. Demos against a loopback / hostfwd peer under QEMU user-net.
 *
 *   2. Settings sync — serializes the live desktop configuration (theme, font
 *      scale, accessibility flags, wallpaper) into a "settings.cfg" file in the
 *      RAM-FS. That file then rides the ordinary file-sync channel, so settings
 *      propagate "across installations" exactly like any other synced file.
 *      Also persists/restores within a session and can be reloaded on demand.
 *
 * Wire protocol (text, CRLF-terminated lines; mirrors how rshell speaks):
 *   Client -> Server:   "LIST\r\n"                  list files
 *                       "GET <name>\r\n"            fetch one file
 *                       "PUT <name> <size>\r\n" + <size> raw bytes
 *                       "BYE\r\n"
 *   Server -> Client:   "FILE <name> <size>\r\n" + <size> raw bytes   (per file)
 *                       "END\r\n"                                       (list end)
 *                       "OK\r\n" / "ERR <reason>\r\n"
 * ============================================================================ */

#ifndef SYNC_H
#define SYNC_H

#include "types.h"

#define SYNC_PORT 7070   /* TCP port for the NexusOS sync service */

/* --- Lifecycle (mirrors httpd/rshell) ------------------------------------- */
void sync_init(void);          /* one-time init at boot                       */
void sync_server_start(void);  /* begin listening on SYNC_PORT                */
void sync_server_stop(void);
void sync_poll(void);          /* service the server; call from a main loop   */
bool sync_server_running(void);

/* --- Client operations (blocking, bounded; safe to call from a shell cmd) -
 * Each opens a TCP connection to host_ip:SYNC_PORT, performs the exchange,
 * and closes. Return 0 on success, negative on error. They drive net_poll()
 * internally and time out after a few seconds. host_ip is host byte order. */
int sync_client_list(uint32_t host_ip);                 /* print remote files  */
int sync_client_pull(uint32_t host_ip, const char* name); /* fetch one file    */
int sync_client_push(uint32_t host_ip, const char* name); /* upload one file   */
int sync_client_pull_all(uint32_t host_ip);             /* fetch every file    */

/* --- Settings sync -------------------------------------------------------- */
/* Serialize live settings (theme/font-scale/reader/contrast/wallpaper) into a
 * RAM-FS file (default "settings.cfg"). Returns bytes written, or -1. */
int  settings_save(const char* path);
/* Parse a settings file and apply it to the live system. Returns 0 on
 * success, -1 if the file is missing/invalid. */
int  settings_load(const char* path);
/* Render the current settings as a text blob into buf (returns length).
 * Exposed for the `synccfg show` command. */
int  settings_serialize(char* buf, int max);

/* --- Stats (for the `sync` status command) -------------------------------- */
uint32_t sync_files_sent(void);
uint32_t sync_files_recv(void);

#endif /* SYNC_H */

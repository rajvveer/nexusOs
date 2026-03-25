/* ============================================================================
 * NexusOS — Pipe IPC (Header) — Phase 22
 * ============================================================================
 * Ring buffer pipes for inter-process communication.
 * ============================================================================ */

#ifndef PIPE_H
#define PIPE_H

#include "types.h"

/* Pipe configuration */
#define PIPE_MAX       8
#define PIPE_BUF_SIZE  4096    /* 4KB ring buffer per pipe */

/* Pipe structure */
typedef struct {
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;     /* Read cursor */
    uint32_t write_pos;    /* Write cursor */
    uint32_t count;        /* Bytes in buffer */
    uint32_t reader_pid;   /* Reader process PID (0 = any) */
    uint32_t writer_pid;   /* Writer process PID (0 = any) */
    bool     active;       /* Pipe is open */
    bool     read_closed;  /* Read end closed */
    bool     write_closed; /* Write end closed */
} pipe_t;

/* Initialize pipe subsystem */
void pipe_init(void);

/* Create a new pipe, returns pipe ID or -1 */
int pipe_create(void);

/* Read from pipe. Returns bytes read, 0 if empty and write-closed, -1 on error */
int32_t pipe_read(int pipe_id, uint8_t* buf, uint32_t count);

/* Write to pipe. Returns bytes written, -1 on error */
int32_t pipe_write(int pipe_id, const uint8_t* buf, uint32_t count);

/* Close read or write end of pipe */
void pipe_close_read(int pipe_id);
void pipe_close_write(int pipe_id);

/* Get pipe by ID */
pipe_t* pipe_get(int pipe_id);

#endif /* PIPE_H */

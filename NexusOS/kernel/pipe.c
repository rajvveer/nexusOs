/* ============================================================================
 * NexusOS — Pipe IPC (Implementation) — Phase 22
 * ============================================================================
 * Ring buffer pipes for inter-process communication.
 * ============================================================================ */

#include "pipe.h"
#include "string.h"
#include "vga.h"

/* Pipe table */
static pipe_t pipes[PIPE_MAX];

/* --------------------------------------------------------------------------
 * pipe_init: Initialize pipe subsystem
 * -------------------------------------------------------------------------- */
void pipe_init(void) {
    memset(pipes, 0, sizeof(pipes));

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Pipe IPC initialized (8 pipes)\n");
}

/* --------------------------------------------------------------------------
 * pipe_create: Create a new pipe
 * -------------------------------------------------------------------------- */
int pipe_create(void) {
    for (int i = 0; i < PIPE_MAX; i++) {
        if (!pipes[i].active) {
            memset(&pipes[i], 0, sizeof(pipe_t));
            pipes[i].active = true;
            pipes[i].read_closed = false;
            pipes[i].write_closed = false;
            return i;
        }
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * pipe_read: Read from pipe ring buffer
 * -------------------------------------------------------------------------- */
int32_t pipe_read(int pipe_id, uint8_t* buf, uint32_t count) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipes[pipe_id].active)
        return -1;

    pipe_t* p = &pipes[pipe_id];

    /* Nothing to read */
    if (p->count == 0) {
        /* If write end closed, EOF */
        if (p->write_closed) return 0;
        /* Otherwise just return 0 bytes (non-blocking) */
        return 0;
    }

    /* Read up to count bytes */
    uint32_t to_read = count;
    if (to_read > p->count) to_read = p->count;

    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }
    p->count -= to_read;

    return (int32_t)to_read;
}

/* --------------------------------------------------------------------------
 * pipe_write: Write to pipe ring buffer
 * -------------------------------------------------------------------------- */
int32_t pipe_write(int pipe_id, const uint8_t* buf, uint32_t count) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipes[pipe_id].active)
        return -1;

    pipe_t* p = &pipes[pipe_id];

    /* If read end closed, signal broken pipe */
    if (p->read_closed) return -1;

    /* Write up to available space */
    uint32_t space = PIPE_BUF_SIZE - p->count;
    uint32_t to_write = count;
    if (to_write > space) to_write = space;

    for (uint32_t i = 0; i < to_write; i++) {
        p->buffer[p->write_pos] = buf[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
    }
    p->count += to_write;

    return (int32_t)to_write;
}

/* --------------------------------------------------------------------------
 * pipe_close_read / pipe_close_write
 * -------------------------------------------------------------------------- */
void pipe_close_read(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    pipes[pipe_id].read_closed = true;
    if (pipes[pipe_id].write_closed) {
        pipes[pipe_id].active = false; /* Both ends closed */
    }
}

void pipe_close_write(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX) return;
    pipes[pipe_id].write_closed = true;
    if (pipes[pipe_id].read_closed) {
        pipes[pipe_id].active = false; /* Both ends closed */
    }
}

/* --------------------------------------------------------------------------
 * pipe_get: Get pipe by ID
 * -------------------------------------------------------------------------- */
pipe_t* pipe_get(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= PIPE_MAX || !pipes[pipe_id].active)
        return NULL;
    return &pipes[pipe_id];
}

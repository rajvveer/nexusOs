/* ============================================================================
 * NexusOS — VT100/ANSI terminal engine (Header) — Phase 57 "apps"
 * ============================================================================
 * A character-cell screen model with a VT100 escape-sequence parser. One
 * instance backs the text console while a user program owns it (cells are
 * painted through a callback onto the 80x24 VGA grid), another backs the GUI
 * Terminal window (the window's draw callback reads the cells). The engine
 * itself never touches hardware.
 *
 * Supported: CR LF BS TAB; CSI H/f (CUP) A B C D (CUU/CUD/CUF/CUB) G (CHA)
 * d (VPA) J (ED 0/1/2) K (EL 0/1/2) L/M (IL/DL) @/P (ICH/DCH) X (ECH) r (scroll
 * region) s/u (save/restore) m (SGR: 0 1 7 22 27 30-37 39 40-47 49 90-97
 * 100-107) ?25h/l (cursor) ?1049h/l (alt screen = clear) n (DSR 6 -> reply)
 * ESC 7/8 c D M E; deferred wrap at the last column like a real terminal.
 * ============================================================================ */
#ifndef VT_H
#define VT_H

#include "types.h"

#define VT_MAX_COLS 128
#define VT_MAX_ROWS 48
#define VT_CELLS    (VT_MAX_COLS * VT_MAX_ROWS)

typedef struct vt vt_t;

/* Paint one cell (console mode). NULL when the owner repaints from the grid. */
typedef void (*vt_paint_fn)(vt_t* v, int row, int col, uint8_t ch, uint8_t attr);
/* Deliver a reply string (DSR cursor report) back into the terminal's input. */
typedef void (*vt_reply_fn)(vt_t* v, const char* s);

struct vt {
    int      cols, rows;
    uint8_t  ch[VT_CELLS];          /* row-major characters                   */
    uint8_t  at[VT_CELLS];          /* VGA attribute bytes (bg<<4 | fg)       */
    int      cx, cy;                /* cursor                                 */
    int      scx, scy;              /* saved cursor (ESC 7 / CSI s)           */
    uint8_t  attr;                  /* current attribute                      */
    uint8_t  def_attr;
    bool     bold, inverse;
    uint8_t  fg, bg;                /* current colours before inverse/bold    */
    bool     wrap_pending;
    bool     cursor_visible;
    int      top, bot;              /* scroll region (inclusive rows)         */
    int      state;                 /* parser state                           */
    int      params[8];
    int      nparams;
    bool     priv;                  /* CSI '?' prefix                         */
    bool     dirty;                 /* something changed since last clear     */
    vt_paint_fn paint;
    vt_reply_fn reply;
    void*    owner;
};

void vt_init(vt_t* v, int cols, int rows, vt_paint_fn paint, vt_reply_fn reply, void* owner);
void vt_putc(vt_t* v, uint8_t c);
void vt_write(vt_t* v, const char* s, int n);
void vt_puts(vt_t* v, const char* s);
/* Seed a cell without painting (import of what is already on screen). */
void vt_import(vt_t* v, int row, int col, uint8_t ch, uint8_t attr);
void vt_set_cursor(vt_t* v, int row, int col);
/* Repaint every cell through the paint callback (after a scroll/clear). */
void vt_repaint(vt_t* v);
/* Copy row r as a NUL-terminated string (trailing blanks trimmed). */
int  vt_row_text(const vt_t* v, int r, char* out, int max);

#endif /* VT_H */

/* Aurora accessibility (aurora_a11y.c): larger text, increased contrast, inverted colours, a magnifier lens that
 * follows the pointer, reduced motion and sticky keys. Settings are per machine in AURORA_A11Y_CONF, changed from
 * System Settings > Accessibility through the control socket ("a11y", "set-a11y key=value..."); Aurora applications
 * read the text size when they start. Contrast and inversion are applied by the compositor to everything on screen
 * (Chrome, GTK, Wine...), after composition; the magnifier enlarges the composed screen around the pointer (Super+=
 * / Super+- change its zoom); sticky keys latch Shift, Ctrl, Alt or Super pressed alone for the next key. */
#ifndef AURORA_A11Y_H
#define AURORA_A11Y_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AURORA_A11Y_CONF "/home/nexus/.aurora/a11y.conf"
struct aurora_a11y { bool larger_text, contrast, invert, magnifier, reduce_motion, sticky_keys; int zoom; };   /* zoom: 150..400 % */
const struct aurora_a11y* aurora_a11y(void);
bool aurora_a11y_load(struct aurora_a11y* out);    /* AURORA_A11Y_CONF (defaults when missing); also for applications */

/* ---- services of desktop.c ---- */
void aurora_host_a11y_changed(void);                /* repaint everything with the new settings */

/* ---- aurora_a11y.c (compositor) ---- */
void aurora_a11y_init(void);                         /* load, apply, control commands */
void aurora_a11y_zoom(int step);                     /* +1 / -1: magnifier zoom (switches it on/off at the ends) */
/* Colour filter on physical pixels x,y,w,h of canvas (stride in pixels); no-op when neither contrast nor invert. */
void aurora_a11y_filter(uint32_t* canvas, size_t stride, int x, int y, int w, int h);
bool aurora_a11y_filtering(void);
/* The lens rectangle (desktop coordinates) for the pointer at px,py; false when the magnifier is off. */
bool aurora_a11y_lens(int px, int py, int* x, int* y, int* w, int* h);
/* Draw the lens (physical rectangle lx,ly,lw,lh of the canvas) magnifying the canvas around physical cx,cy. */
void aurora_a11y_magnify(uint32_t* canvas, int cw, int ch, size_t stride, int lx, int ly, int lw, int lh, int cx, int cy);
#endif

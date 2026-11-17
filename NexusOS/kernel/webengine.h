/* ============================================================================
 * NexusOS — Web engine (Phase 57): HTML tokenizer, element tree, layout, paint
 * ============================================================================
 * A small HTML renderer for the graphical browser. The engine normalises the
 * page source into its own text buffer (entity decoding, whitespace
 * collapsing, UTF-8 -> ASCII), builds an element tree, lays it out into a display list of positioned runs/images/rules for
 * a given content width, and paints a scrolled viewport of that list into the
 * back buffer. All state is static (no heap). One page at a time.
 * ============================================================================ */
#ifndef WEBENGINE_H
#define WEBENGINE_H

#include "types.h"

#define WE_PAGE_MAX      (256 * 1024)   /* page source cap (bytes)                 */
#define WE_MAX_NODES     6000
#define WE_MAX_ITEMS     14000
#define WE_MAX_LINKS     700
#define WE_MAX_IMAGES    48
#define WE_STRPOOL       (48 * 1024)    /* href / src / alt strings                */
#define WE_TITLE_MAX     120
#define WE_URL_MAX       512
#define WE_IMG_FILE_MAX  (256 * 1024)   /* per-image download cap                  */
#define WE_IMG_POOL_PX   (640 * 1024)   /* decoded (display-size) pixels, all imgs */
#define WE_IMG_MAX_DIM   1600           /* decode cap per side                     */
#define WE_IMG_MAX_PX    (300 * 1024)   /* decode cap (w*h)                        */

/* Image slot state */
enum { WE_IMG_NEW = 0, WE_IMG_LOADING, WE_IMG_READY, WE_IMG_FAILED };

typedef struct {
    int   src;            /* strpool offset of the (raw) src attribute          */
    int   alt;            /* strpool offset of alt text, or -1                  */
    int   attr_w, attr_h; /* width/height attributes (0 = none)                 */
    int   state;          /* WE_IMG_*                                            */
    int   nat_w, nat_h;   /* natural size once decoded                           */
    int   disp_w, disp_h; /* size of the stored (display) pixels                 */
    uint32_t* px;         /* into the engine's pixel pool (NULL until ready)     */
    char  url[WE_URL_MAX];/* resolved absolute URL                               */
} we_image_t;

typedef struct {
    int x, y, w, h;       /* document coordinates (y grows downwards)            */
    int href;             /* strpool offset of the raw href                      */
    int first_item;       /* first display-list item of this link                */
} we_link_t;

/* Reset the engine and parse `len` bytes of HTML from `src`. Text is
 * normalised into the engine's own buffer, so `src` may be a const string and
 * may be re-parsed as it grows (progressive rendering). base_url is used for
 * relative href/src resolution. */
void we_parse(const char* src, int len, const char* base_url);

/* Treat `src` as plain text (monospace, preserved newlines). */
void we_parse_plain(const char* src, int len, const char* base_url);

/* Lay the parsed tree out for a content width. Call after we_parse and again
 * whenever the width changes or an image finishes loading. */
void we_layout(int content_width);

/* Paint the document region [scroll_y, scroll_y+vh) into the back buffer at
 * (vx, vy) with viewport size vw x vh. Links are drawn blue+underlined; the
 * focused link index (or -1) gets a focus ring. */
void we_paint(int vx, int vy, int vw, int vh, int scroll_y, int focused_link);

/* Document height after layout, page title ("" if none). */
int         we_doc_height(void);
const char* we_title(void);

/* Links */
int               we_link_count(void);
const we_link_t*  we_link(int i);
/* Hit-test a document point; returns link index or -1. */
int               we_link_at(int doc_x, int doc_y);
/* Resolve a strpool href against the page base URL into out[WE_URL_MAX]. */
void              we_resolve_href(int href_off, char* out);
const char*       we_str(int off);                     /* strpool string      */

/* Images */
int         we_image_count(void);
we_image_t* we_image(int i);
/* Decode an image blob into slot i (uses the engine's decode arena + pixel
 * pool, scaled to fit max_w). Sets state READY or FAILED. */
void        we_image_decoded(int i, const uint8_t* data, uint32_t size, int max_w);

/* True if `ct` (a Content-Type header value) names an image type. */
bool we_is_image_type(const char* ct);

/* Draw helpers shared with the browser chrome. */
void we_fill(int x, int y, int w, int h, uint32_t color);

#endif /* WEBENGINE_H */

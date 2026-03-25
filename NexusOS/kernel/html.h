/* ============================================================================
 * NexusOS — HTML Parser Header — Phase 29
 * ============================================================================
 * Parses a subset of HTML into a flat array of renderable elements.
 * ============================================================================ */

#ifndef HTML_H
#define HTML_H

#include "types.h"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */
#define HTML_MAX_ELEMENTS  128
#define HTML_MAX_TEXT      80      /* Max text per element */
#define HTML_MAX_HREF      80      /* Max href per link */
#define HTML_MAX_TITLE     40

/* --------------------------------------------------------------------------
 * Element Types
 * -------------------------------------------------------------------------- */
typedef enum {
    HTML_TEXT,
    HTML_HEADING,       /* h1-h6 */
    HTML_PARAGRAPH,
    HTML_LINK,
    HTML_LIST_ITEM,
    HTML_PREFORMATTED,
    HTML_LINEBREAK,
    HTML_BOLD,
    HTML_ITALIC
} html_type_t;

/* --------------------------------------------------------------------------
 * Parsed Element
 * -------------------------------------------------------------------------- */
typedef struct {
    html_type_t type;
    char        text[HTML_MAX_TEXT];
    char        href[HTML_MAX_HREF];    /* Only for LINK type */
    uint8_t     level;                  /* Heading level (1-6) */
    bool        active;
} html_element_t;

/* --------------------------------------------------------------------------
 * Parsed Page
 * -------------------------------------------------------------------------- */
typedef struct {
    html_element_t elements[HTML_MAX_ELEMENTS];
    int            count;
    char           title[HTML_MAX_TITLE];
} html_page_t;

/* --------------------------------------------------------------------------
 * API
 * -------------------------------------------------------------------------- */

/* Parse HTML string into element array */
void html_parse(const char* html, int len, html_page_t* page);

#endif /* HTML_H */

/* Keyboard layouts for the kernel's early prompts (keymaps.h): the encrypted-root and crypttab passphrases
 * (cryptroot.c, crypttab.c) and the installer's (osinstall.c), typed before Aurora runs. The tables are the layouts
 * Aurora offers (aurora_keyboard.c CATALOG), generated from the same xkeyboard-config symbols Aurora's xkbcommon uses
 * (keymaps_data.h, gen_keymaps.py): four levels (Shift, AltGr = right Alt), Caps Lock on letter keys, dead keys
 * composed with the next character (NFC; a dead key that does not compose gives its spacing accent first). The
 * passphrase is the UTF-8 text Aurora/a terminal with the same layout would produce. Which layout: the system's
 * configuration (keymap_configure); an encrypted root's prompt runs before its files are readable, so the boot
 * command line carries it (vconsole.keymap=, written by the installer and kept in sync at shutdown, osinstall.c). */
#include "native.h"
#include "boot_info.h"
#include "vfs.h"
#include "keymaps.h"
#include "storage_util.h"
#include "../../kernel/vga.h"
#include "keymaps_data.h"

static const struct keymap_table* active = &KEYMAP_TABLES[0];
static void cat(char* out, const char* src, unsigned capacity) { unsigned n = strlen(out); while (*src && n + 1 < capacity) out[n++] = *src++; out[n] = 0; }
static char active_name[24] = "us";

static const struct { const char* alias; const char* name; } ALIASES[] = {   /* kbd (console) keymap names */
    {"uk", "gb"}, {"gb", "gb"}, {"de-latin1", "de"}, {"de-latin1-nodeadkeys", "de"}, {"de-nodeadkeys", "de"}, {"fr-latin1", "fr"},
    {"fr-latin9", "fr"}, {"fr-pc", "fr"}, {"es-cp850", "es"}, {"it2", "it"}, {"pt-latin1", "pt"}, {"pt-latin9", "pt"},
    {"ru4", "ru"}, {"ruwin_alt-utf-8", "ru"}, {"gr-pc", "gr"}, {"jp106", "jp"}, {"trq", "tr"}, {"tr_q-latin5", "tr"},
    {"pl2", "pl"}, {"se-lat6", "se"}, {"sv-latin1", "se"}, {"se-latin1", "se"}, {"us-acentos", "us(intl)"}, {"us-intl", "us(intl)"},
    {"us-international", "us(intl)"}, {"hebrew", "il"}, {"arabic", "ara"},
};
static const struct keymap_table* find(const char* layout, const char* variant) {
    for (unsigned i = 0; i < KEYMAP_COUNT; i++) if (!strcmp(KEYMAP_TABLES[i].layout, layout) && !strcmp(KEYMAP_TABLES[i].variant, variant)) return &KEYMAP_TABLES[i];
    return NULL;
}
/* name -> table; *fallback when only the layout (not its variant) is known */
static const struct keymap_table* resolve(const char* name, bool* fallback) {
    char text[48], layout[24], variant[24] = ""; unsigned n = 0; *fallback = false;
    while (*name == ' ' || *name == '"' || *name == '\'') name++;
    for (; name[n] && name[n] != ' ' && name[n] != '"' && name[n] != '\'' && name[n] != '\n' && name[n] != '\r' && n + 1 < sizeof(text); n++)
        text[n] = name[n] >= 'A' && name[n] <= 'Z' ? (char)(name[n] + 32) : name[n];
    text[n] = 0;
    if (!n) return NULL;
    for (unsigned i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) if (!strcmp(text, ALIASES[i].alias)) { strcpy(text, ALIASES[i].name); break; }
    char* split = NULL;
    for (char* p = text; *p; p++) if (*p == '(' || *p == ':' || *p == '-' || *p == '+') { split = p; break; }
    if (split) {
        char sep = *split; *split = 0; cat(variant, split + 1, sizeof(variant));
        if (sep == '(') { char* close = s_strchr(variant, ')'); if (close) *close = 0; }
    }
    if (strlen(text) >= sizeof(layout)) return NULL;
    strcpy(layout, text);
    const struct keymap_table* t = find(layout, variant);
    if (!t && variant[0]) { t = find(layout, ""); *fallback = t != NULL; }
    return t;
}
static void table_name(const struct keymap_table* t, char* out, unsigned capacity) {
    out[0] = 0; cat(out, t->layout, capacity);
    if (t->variant[0]) { cat(out, "(", capacity); cat(out, t->variant, capacity); cat(out, ")", capacity); }
}
bool keymap_canonical(const char* name, char* out, unsigned capacity) {
    bool fallback; const struct keymap_table* t = resolve(name, &fallback);
    if (!t) return false;
    table_name(t, out, capacity); return true;
}
bool keymap_select(const char* name, const char* source) {
    bool fallback; const struct keymap_table* t = resolve(name, &fallback);
    if (!t) { vga_print("[64] KEYMAP unknown layout '"); vga_print(name); vga_print("' ("); vga_print(source); vga_print("): keeping "); vga_print(active_name); vga_print("\n"); return false; }
    active = t; table_name(t, active_name, sizeof(active_name));
    vga_print("[64] KEYMAP "); vga_print(active_name); vga_print(" ("); vga_print(t->title); vga_print(", from "); vga_print(source);
    vga_print(fallback ? "; variant not available, base layout)\n" : ")\n");
    return true;
}
const char* keymap_name(void) { return active_name; }
const char* keymap_title(void) { return active->title; }

static uint32_t compose(uint32_t mark, uint32_t base) {
    for (unsigned i = 0; i < KEYMAP_COMPOSE; i++) if (KEYMAP_COMPOSITIONS[i].mark == mark && KEYMAP_COMPOSITIONS[i].base == base) return KEYMAP_COMPOSITIONS[i].result;
    return 0;
}
static uint32_t spacing(uint32_t mark) { uint32_t c = compose(mark, ' '); return c ? c : mark; }
unsigned keymap_translate(struct keymap_state* s, unsigned code, bool shift, bool altgr, bool caps, uint32_t out[2]) {
    uint32_t cp = 0;
    if (code == 57) cp = ' ';
    else {
        unsigned k = 0; while (k < KEYMAP_KEYS && KEYMAP_CODES[k] != code) k++;
        if (k == KEYMAP_KEYS) return 0;
        bool level2 = shift != (caps && !altgr && (active->alphabetic >> k & 1));
        cp = active->level[k][(altgr ? 2 : 0) + (level2 ? 1 : 0)];
        if (!cp && altgr && level2) cp = active->level[k][2];
        if (!cp) return 0;
    }
    if (cp >= 0x300 && cp <= 0x36F) {   /* a dead key */
        uint32_t pending = s->dead;
        if (pending == cp) { s->dead = 0; out[0] = spacing(cp); return 1; }   /* twice: the accent itself */
        s->dead = cp;
        if (pending) { out[0] = spacing(pending); return 1; }
        return 0;
    }
    if (s->dead) {
        uint32_t mark = s->dead, c = compose(mark, cp); s->dead = 0;
        if (c) { out[0] = c; return 1; }
        out[0] = spacing(mark); out[1] = cp; return 2;
    }
    out[0] = cp; return 1;
}
unsigned keymap_utf8(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | cp >> 12); out[1] = (char)(0x80 | (cp >> 6 & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    out[0] = (char)(0xF0 | cp >> 18); out[1] = (char)(0x80 | (cp >> 12 & 0x3F)); out[2] = (char)(0x80 | (cp >> 6 & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

/* ---- configuration ---- */
static int read_small(const char* path, char* text, unsigned capacity) {   /* the file's first bytes, NUL-terminated; -1 if absent */
    fat32_file_t f;
    if (!disk_lookup(path, &f) || f.is_dir) return -1;
    uint64_t n = f.size < capacity - 1 ? f.size : capacity - 1;
    int32_t got = n ? disk_read_file(&f, 0, n, text) : 0;
    if (got < 0) return -1;
    text[got] = 0; return got;
}
/* KEY=value in a shell-style file (first item of a comma list, quotes removed) */
static bool value_of(const char* text, const char* key, char* out, unsigned capacity) {
    unsigned k = strlen(key);
    for (const char* line = text; *line; ) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, key, k) && line[k] == '=') {
            const char* v = line + k + 1; unsigned n = 0;
            if (*v == '"' || *v == '\'') v++;
            while (v[n] && v[n] != '"' && v[n] != '\'' && v[n] != ',' && v[n] != '\n' && v[n] != '\r' && v[n] != ' ' && n + 1 < capacity) { out[n] = v[n]; n++; }
            out[n] = 0; return true;
        }
        while (*line && *line != '\n') line++;
        if (*line) line++;
    }
    return false;
}
static bool layout_variant(const char* layout, const char* variant, char* out, unsigned capacity) {
    char name[64]; name[0] = 0; cat(name, layout, sizeof(name));
    if (variant[0]) { cat(name, "(", sizeof(name)); cat(name, variant, sizeof(name)); cat(name, ")", sizeof(name)); }
    return layout[0] && keymap_canonical(name, out, capacity);
}
bool keymap_from_files(char* out, unsigned capacity) {
    char text[1024], layout[32], variant[32];
    if (read_small("/etc/vconsole.conf", text, sizeof(text)) > 0 && value_of(text, "KEYMAP", layout, sizeof(layout)) && keymap_canonical(layout, out, capacity)) return true;
    if (read_small("/home/nexus/.aurora/keyboard.conf", text, sizeof(text)) > 0 && value_of(text, "layouts", layout, sizeof(layout))) {
        if (!value_of(text, "variants", variant, sizeof(variant))) variant[0] = 0;
        if (layout_variant(layout, variant, out, capacity)) return true;
    }
    if (read_small("/etc/default/keyboard", text, sizeof(text)) > 0 && value_of(text, "XKBLAYOUT", layout, sizeof(layout))) {
        if (!value_of(text, "XKBVARIANT", variant, sizeof(variant))) variant[0] = 0;
        if (layout_variant(layout, variant, out, capacity)) return true;
    }
    return false;
}
void keymap_configure(bool root_mounted) {
    char value[64];
    if (boot_value("rd.vconsole.keymap", value, sizeof(value)) && value[0] && keymap_select(value, "rd.vconsole.keymap")) return;
    if (boot_value("vconsole.keymap", value, sizeof(value)) && value[0] && keymap_select(value, "vconsole.keymap")) return;
    if (root_mounted && keymap_from_files(value, sizeof(value)) && keymap_select(value, "the system configuration")) return;
    if (strcmp(active_name, "us")) keymap_select("us", "default");
}

/* Aurora keyboard layouts (aurora_keyboard.h). */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "aurora_keyboard.h"
#include "aurora_control.h"

static const struct aurora_layout CATALOG[] = {
    {"us", "", "US", "English (US)"}, {"gb", "", "GB", "English (UK)"}, {"us", "intl", "US-I", "English (US, international)"},
    {"de", "", "DE", "German"}, {"fr", "", "FR", "French"}, {"es", "", "ES", "Spanish"}, {"it", "", "IT", "Italian"},
    {"pt", "", "PT", "Portuguese"}, {"in", "", "HI", "Hindi (InScript)"}, {"ru", "", "RU", "Russian"}, {"gr", "", "GR", "Greek"},
    {"il", "", "HE", "Hebrew"}, {"ara", "", "AR", "Arabic"}, {"th", "", "TH", "Thai"}, {"jp", "", "JP", "Japanese (kana keys)"},
    {"kr", "", "KR", "Korean"}, {"tr", "", "TR", "Turkish"}, {"pl", "", "PL", "Polish"}, {"se", "", "SE", "Swedish"},
};
#define CATALOG_COUNT ((int)(sizeof(CATALOG) / sizeof(CATALOG[0])))
const struct aurora_layout* aurora_keyboard_catalog(int* count) { *count = CATALOG_COUNT; return CATALOG; }

static int active[AURORA_LAYOUTS_MAX], count = 0; static unsigned current;
static int find(const char* code, const char* variant) {
    for (int i = 0; i < CATALOG_COUNT; i++) if (!strcmp(CATALOG[i].code, code) && !strcmp(CATALOG[i].variant, variant ? variant : "")) return i;
    return -1;
}
/* "us,de" + ",nodeadkeys" -> catalog indices; false on anything unknown or too many */
static bool parse(const char* layouts, const char* variants, int* out, int* n) {
    char l[128], v[128]; snprintf(l, sizeof(l), "%s", layouts); snprintf(v, sizeof(v), "%s", variants ? variants : "");
    char* ls = NULL, *vs = v; *n = 0;
    for (char* code = strtok_r(l, ",", &ls); code; code = strtok_r(NULL, ",", &ls)) {
        char* comma = vs ? strchr(vs, ',') : NULL; char variant[32] = "";
        if (vs) { snprintf(variant, sizeof(variant), "%.*s", comma ? (int)(comma - vs) : (int)strlen(vs), vs); vs = comma ? comma + 1 : NULL; }
        int k = find(code, variant); if (k < 0 || *n == AURORA_LAYOUTS_MAX) return false;
        for (int i = 0; i < *n; i++) if (out[i] == k) return false;
        out[(*n)++] = k;
    }
    return *n > 0;
}
static void names(char* layouts, char* variants, size_t size) {
    layouts[0] = variants[0] = 0;
    for (int i = 0; i < count; i++) {
        snprintf(layouts + strlen(layouts), size - strlen(layouts), "%s%s", i ? "," : "", CATALOG[active[i]].code);
        snprintf(variants + strlen(variants), size - strlen(variants), "%s%s", i ? "," : "", CATALOG[active[i]].variant);
    }
}
static bool apply(const int* list, int n, bool save) {
    int old[AURORA_LAYOUTS_MAX], old_count = count; memcpy(old, active, sizeof(old));
    memcpy(active, list, sizeof(int) * (size_t)n); count = n;
    char layouts[128], variants[128]; names(layouts, variants, sizeof(layouts));
    if (!aurora_host_keymap(layouts, variants)) { memcpy(active, old, sizeof(old)); count = old_count; return false; }
    current = 0;
    printf("[64] AURORA KEYBOARD layouts=%s variants=%s active=%s\n", layouts, variants, aurora_keyboard_label()); fflush(stdout);
    if (save) {
        mkdir("/home/nexus/.aurora", 0755);
        FILE* f = fopen(AURORA_KEYBOARD_CONF ".tmp", "we");
        if (f) { fprintf(f, "# Aurora input sources (System Settings > Keyboard)\nlayouts=%s\nvariants=%s\n", layouts, variants);
            bool ok = !fflush(f) && !fsync(fileno(f)); fclose(f); if (ok) rename(AURORA_KEYBOARD_CONF ".tmp", AURORA_KEYBOARD_CONF); }
    }
    return true;
}
void aurora_keyboard_next(void) {
    if (count < 2) return;
    current = (current + 1) % (unsigned)count; aurora_host_keyboard_group(current);
    printf("[64] AURORA KEYBOARD active=%s\n", aurora_keyboard_label()); fflush(stdout);
}
const char* aurora_keyboard_label(void) { return count ? CATALOG[active[current < (unsigned)count ? current : 0]].label : "US"; }
int aurora_keyboard_count(void) { return count; }
static bool control_get(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; char layouts[128], variants[128]; names(layouts, variants, sizeof(layouts));
    aurora_control_print(reply, "layouts=%s variants=%s active=%u\n", layouts, variants, current);
    for (int i = 0; i < CATALOG_COUNT; i++) aurora_control_print(reply, "layout %s %s %s %s\n", CATALOG[i].code, CATALOG[i].variant[0] ? CATALOG[i].variant : "-", CATALOG[i].label, CATALOG[i].name);
    return true;
}
static bool control_set(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)body; (void)reply; char layouts[128] = "", variants[128] = "";
    for (const char* p = args; p && *p;) {
        while (*p == ' ') p++;
        if (!strncmp(p, "layouts=", 8)) sscanf(p + 8, "%127s", layouts);
        else if (!strncmp(p, "variants=", 9)) sscanf(p + 9, "%127s", variants);
        p = strchr(p, ' ');
    }
    int list[AURORA_LAYOUTS_MAX], n;
    if (!parse(layouts, variants, list, &n)) { snprintf(error, size, "unknown or too many layouts (1 to %d of the catalog)", AURORA_LAYOUTS_MAX); return false; }
    if (!apply(list, n, true)) { snprintf(error, size, "the keymap could not be compiled"); return false; }
    return true;
}
static bool control_next(const char* args, const char* body, struct aurora_control_reply* reply, char* error, size_t size) {
    (void)args; (void)body; (void)error; (void)size; aurora_keyboard_next(); aurora_control_print(reply, "active=%s\n", aurora_keyboard_label()); return true;
}
void aurora_keyboard_init(void) {
    int list[AURORA_LAYOUTS_MAX] = {0}, n = 1;   /* default: US */
    FILE* f = fopen(AURORA_KEYBOARD_CONF, "re");
    if (f) {
        char line[200], layouts[128] = "", variants[128] = "";
        while (fgets(line, sizeof(line), f)) { if (sscanf(line, "layouts=%127s", layouts) == 1) {} else if (sscanf(line, "variants=%127s", variants) == 1) {} }
        fclose(f);
        int saved[AURORA_LAYOUTS_MAX], m; if (parse(layouts, variants, saved, &m)) { memcpy(list, saved, sizeof(saved)); n = m; }
    }
    if (n != 1 || list[0] != 0) apply(list, n, false); else { active[0] = 0; count = 1; }
    aurora_control_register("keyboard", false, control_get);
    aurora_control_register("set-keyboard", false, control_set);
    aurora_control_register("next-layout", false, control_next);
}

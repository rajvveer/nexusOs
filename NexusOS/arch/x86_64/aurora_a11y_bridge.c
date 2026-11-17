/* aurora-a11y-bridge (/usr/libexec/aurora-a11y-bridge): the Aurora shell as an AT-SPI2 application, for Orca.
 *
 * Runs as the session's user (Aurora starts it with --orca when the screen reader is switched on: System Settings >
 * Accessibility or Super+Alt+S; aurora_reader.h). It joins the user's accessibility bus (org.a11y.Bus.GetAddress on
 * the session bus), embeds itself in the AT-SPI registry as the application "Aurora", and asks Aurora over the
 * session's control socket ("a11y-watch") for the shell's state: the menu bar and the open menu, the Dock's keyboard
 * focus, Spotlight, Launchpad, the greeter / lock screen, and banners. Each change becomes the accessible tree that
 * Orca reads (org.a11y.atspi.Accessible / Application, properties) and the events it follows: window:activate and
 * window:deactivate for the shell window that takes or leaves the keyboard, object:state-changed:focused for the
 * focused element, object:children-changed on the application, and a notification object
 * (object:state-changed:showing) for every banner. With --orca it starts Orca (orca-wayland without an X display)
 * and exits with it; it also exits when Aurora closes the watch or sends "quit". GTK, Firefox and Chrome talk to
 * Orca over AT-SPI2 themselves; Aurora's own aurora-app windows (System Settings, Files, the dialogs, the
 * authentication dialog) do not have a toolkit that does: they send their frame's tree (aurora_a11y_client.h) to
 * $XDG_RUNTIME_DIR/aurora-a11y-apps, which the bridge listens on (the session user's processes only), and each window
 * becomes a frame of the application "Aurora" with its labels, buttons, entries, check boxes and list items.
 * Entries (Spotlight's search field, aurora-app's fields) have the Text interface and text-changed / caret-moved
 * events; password fields (the lock screen, the greeter, the authentication dialog) are password_text objects whose
 * text is always empty - the bridge never receives what is typed into them.
 * Keyboard: it owns org.freedesktop.a11y.Manager on the session bus and implements its KeyboardMonitor interface
 * (WatchKeyboard, UnwatchKeyboard, GrabKeyboard, UngrabKeyboard, SetKeyGrabs; signal KeyEvent(b released, u state,
 * u keysym, u unichar, q keycode)), which libatspi's keyboard device uses when there is no X display: Aurora reports
 * the session's keys to it and keeps the grabbed ones from applications (aurora_reader.h). Only the Orca it started
 * may watch the keyboard; KeyEvent goes to that connection alone (not broadcast).
 * Usage: aurora-a11y-bridge [--orca] [--trace] [-- ORCA-ARGUMENTS]   (--trace: also every D-Bus call on stderr) */
#define _GNU_SOURCE
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ROOT_PATH "/org/a11y/atspi/accessible/root"
#define NODE_PREFIX "/org/a11y/atspi/accessible/"
#define ACCESSIBLE "org.a11y.atspi.Accessible"
#define APPLICATION "org.a11y.atspi.Application"
#define TEXT "org.a11y.atspi.Text"
#define PROPERTIES "org.freedesktop.DBus.Properties"
#define MAX_NODES 256
#define MAX_IDS 4096
#define NOTICES 4
#define APPS 8
#define RECORD 16384
#define ORCA_WAYLAND "/usr/lib/nexus/orca-wayland"
/* Object indices: -2 the desktop, -1 the application "Aurora", [0, MAX_NODES) the shell's tree (aurora_reader.c),
 * APP_INDEX(k, i) element i of aurora-app window k (aurora_a11y_client.c), NOTICE_INDEX(k) banner k. */
#define APP_INDEX(k, i) ((1 + (k)) * 65536 + (i))
#define NOTICE_INDEX(k) (1048576 + (k))

enum { ROLE_ALERT = 2, ROLE_CHECK_BOX = 7, ROLE_DIALOG = 16, ROLE_FRAME = 23, ROLE_LABEL = 29, ROLE_LIST = 31, ROLE_LIST_ITEM = 32,
       ROLE_MENU = 33, ROLE_MENU_BAR = 34, ROLE_MENU_ITEM = 35, ROLE_PASSWORD_TEXT = 40, ROLE_PUSH_BUTTON = 43, ROLE_RADIO_BUTTON = 44,
       ROLE_SEPARATOR = 50, ROLE_TOOL_BAR = 63, ROLE_UNKNOWN = 67, ROLE_APPLICATION = 75, ROLE_ENTRY = 79, ROLE_HEADING = 83,
       ROLE_NOTIFICATION = 101 };
enum { ST_ACTIVE = 1, ST_CHECKED = 4, ST_COLLAPSED = 5, ST_EDITABLE = 7, ST_ENABLED = 8, ST_EXPANDABLE = 9, ST_EXPANDED = 10,
       ST_FOCUSABLE = 11, ST_FOCUSED = 12, ST_MODAL = 16, ST_SELECTABLE = 22, ST_SELECTED = 23, ST_SENSITIVE = 24, ST_SHOWING = 25,
       ST_SINGLE_LINE = 26, ST_VISIBLE = 30, ST_IS_DEFAULT = 39, ST_CHECKABLE = 41 };
static const struct { const char* name; int role; const char* text; } ROLES[] = {
    {"dialog", ROLE_DIALOG, "dialog"}, {"frame", ROLE_FRAME, "frame"}, {"list", ROLE_LIST, "list"}, {"list_item", ROLE_LIST_ITEM, "list item"},
    {"menu", ROLE_MENU, "menu"}, {"menu_bar", ROLE_MENU_BAR, "menu bar"}, {"menu_item", ROLE_MENU_ITEM, "menu item"},
    {"password_text", ROLE_PASSWORD_TEXT, "password text"}, {"push_button", ROLE_PUSH_BUTTON, "push button"}, {"separator", ROLE_SEPARATOR, "separator"},
    {"tool_bar", ROLE_TOOL_BAR, "tool bar"}, {"entry", ROLE_ENTRY, "entry"}, {"notification", ROLE_NOTIFICATION, "notification"},
    {"alert", ROLE_ALERT, "alert"}, {"application", ROLE_APPLICATION, "application"}, {"label", ROLE_LABEL, "label"},
    {"heading", ROLE_HEADING, "heading"}, {"check_box", ROLE_CHECK_BOX, "check box"}, {"radio_button", ROLE_RADIO_BUTTON, "radio button"},
};
static const struct { const char* name; int bit; } STATES[] = {
    {"active", ST_ACTIVE}, {"collapsed", ST_COLLAPSED}, {"editable", ST_EDITABLE}, {"expandable", ST_EXPANDABLE}, {"expanded", ST_EXPANDED},
    {"focusable", ST_FOCUSABLE}, {"focused", ST_FOCUSED}, {"modal", ST_MODAL}, {"selectable", ST_SELECTABLE}, {"selected", ST_SELECTED},
    {"single_line", ST_SINGLE_LINE}, {"checked", ST_CHECKED}, {"checkable", ST_CHECKABLE}, {"default", ST_IS_DEFAULT},
};

/* text: the content of an entry (never of a password field: Aurora and aurora-app do not send it) */
struct node { char key[96]; unsigned id; int parent; int role; uint64_t states; char name[160], description[200], text[256]; };
struct tree { struct node nodes[MAX_NODES]; int count; char focus[96]; };
static struct tree trees[2]; static struct tree* now = &trees[0];
static struct tree* view = &trees[0];   /* the shell tree a D-Bus call reads: now, or the previous one for an element that
                                          just went (clients look at a window:deactivate source after it left the screen) */
struct app { bool used, active, announced; int fd; long pid; struct tree tree; };   /* an aurora-app window (aurora_a11y_client.h) */
static struct app apps[APPS];
static int apps_listener = -1;
struct notice { unsigned id; char text[400]; uint64_t until; };
static struct notice notices[NOTICES];
static struct { char key[96]; unsigned id; } ids[MAX_IDS]; static int id_count; static unsigned next_id = 1;
static DBusConnection* bus;          /* the accessibility bus */
static DBusConnection* session;      /* the session bus: org.freedesktop.a11y.Manager */
static char key_watcher[256];        /* the unique name watching the keyboard ("" none) */
static int shell = -1;               /* the watch descriptor (Aurora) */
static char desktop_name[256] = "org.a11y.atspi.Registry", desktop_path[256] = ROOT_PATH;
static bool trace; static pid_t orca = -1; static char** orca_args; static int child_pipe[2] = {-1, -1};

static void note(const char* format, ...) {
    char line[700]; int n = snprintf(line, sizeof(line), "[64] AURORA A11Y BRIDGE "); va_list list; va_start(list, format);
    int m = vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, list); va_end(list);
    size_t length = m < 0 ? (size_t)n : strlen(line); if (length > sizeof(line) - 2) length = sizeof(line) - 2; line[length++] = '\n';
    if (write(2, line, length) < 0) {}
}
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000; }
static unsigned id_of(const char* key) {   /* the same element keeps its object path across updates */
    for (int i = 0; i < id_count; i++) if (!strcmp(ids[i].key, key)) return ids[i].id;
    if (id_count == MAX_IDS) id_count = 0;   /* forget the oldest names (their objects are gone by now) */
    snprintf(ids[id_count].key, sizeof(ids[id_count].key), "%s", key); ids[id_count].id = next_id++; return ids[id_count++].id;
}
static int characters(const char* s) { int n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static const char* at_character(const char* s, int offset) { for (int n = 0; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80 && n++ == offset) return s; return s; }

/* ---------------------------------------------------------------- the trees */
static int find_key(const struct tree* t, const char* key) { for (int i = 0; i < t->count; i++) if (!strcmp(t->nodes[i].key, key)) return i; return -1; }
static int find_id(const struct tree* t, unsigned id) { for (int i = 0; i < t->count; i++) if (t->nodes[i].id == id) return i; return -1; }
/* "focus PATH" and "node\tPATH\tPARENT\tROLE\tSTATES\tNAME\tDESCRIPTION[\tTEXT]" lines; keys get the prefix */
static bool parse(const char* text, size_t size, struct tree* t, const char* prefix) {
    memset(t, 0, sizeof(*t)); strcpy(t->focus, "-");
    static char parents[MAX_NODES][96]; char* copy = malloc(size + 1); if (!copy) return false;
    memcpy(copy, text, size); copy[size] = 0;
    for (char* save = NULL, *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "focus ", 6)) { if (strcmp(line + 6, "-")) snprintf(t->focus, sizeof(t->focus), "%s%s", prefix, line + 6); continue; }
        if (strncmp(line, "node\t", 5) || t->count == MAX_NODES) continue;
        char* field[7] = {0}; char* p = line + 5; int got = 0;
        for (; got < 7 && p; got++) { field[got] = p; char* tab = strchr(p, '\t'); if (tab) { *tab = 0; p = tab + 1; } else p = NULL; }
        if (got < 6 || !field[0][0] || strlen(field[0]) >= 64 || strlen(field[1]) >= 64) continue;
        struct node* n = &t->nodes[t->count]; snprintf(n->key, sizeof(n->key), "%s%s", prefix, field[0]);
        if (field[1][0]) snprintf(parents[t->count], sizeof(parents[0]), "%s%s", prefix, field[1]); else parents[t->count][0] = 0;
        n->role = ROLE_UNKNOWN; for (size_t r = 0; r < sizeof(ROLES) / sizeof(*ROLES); r++) if (!strcmp(ROLES[r].name, field[2])) n->role = ROLES[r].role;
        n->states = 1ULL << ST_SHOWING | 1ULL << ST_VISIBLE | 1ULL << ST_ENABLED | 1ULL << ST_SENSITIVE;
        char states[200]; snprintf(states, sizeof(states), "%s", field[3]);
        for (char* s2 = NULL, *word = strtok_r(states, ",", &s2); word; word = strtok_r(NULL, ",", &s2)) {
            if (!strcmp(word, "disabled")) { n->states &= ~(1ULL << ST_ENABLED | 1ULL << ST_SENSITIVE); continue; }
            for (size_t s = 0; s < sizeof(STATES) / sizeof(*STATES); s++) if (!strcmp(STATES[s].name, word)) n->states |= 1ULL << STATES[s].bit;
        }
        snprintf(n->name, sizeof(n->name), "%s", field[4]); snprintf(n->description, sizeof(n->description), "%s", field[5]);
        snprintf(n->text, sizeof(n->text), "%s", got >= 7 && n->role != ROLE_PASSWORD_TEXT ? field[6] : "");
        n->id = id_of(n->key); t->count++;
    }
    free(copy);
    for (int i = 0; i < t->count; i++) { t->nodes[i].parent = parents[i][0] ? find_key(t, parents[i]) : -1; if (t->nodes[i].parent == i) t->nodes[i].parent = -1; }
    return true;
}
static int app_of(int index) { return index >= 65536 && index < NOTICE_INDEX(0) ? index / 65536 - 1 : -1; }
static struct node* node_at(int index) {
    if (index >= 0 && index < MAX_NODES) return index < view->count ? &view->nodes[index] : NULL;
    int k = app_of(index), i = index % 65536;
    return k >= 0 && k < APPS && apps[k].used && i < apps[k].tree.count ? &apps[k].tree.nodes[i] : NULL;
}
/* the application's children: the shell window (nodes without a parent), the aurora-app windows, the banners */
static int top_children(int* list, int max) {
    int n = 0; for (int i = 0; i < view->count && n < max; i++) if (view->nodes[i].parent < 0) list[n++] = i;
    for (int k = 0; k < APPS; k++) if (apps[k].used && apps[k].announced) for (int i = 0; i < apps[k].tree.count && n < max; i++) if (apps[k].tree.nodes[i].parent < 0) list[n++] = APP_INDEX(k, i);
    for (int i = 0; view == now && i < NOTICES && n < max; i++) if (notices[i].id) list[n++] = NOTICE_INDEX(i);
    return n;
}
static int children(int index, int* list, int max) {
    if (index == -1) return top_children(list, max);
    int n = 0, k = app_of(index);
    if (index >= 0 && index < MAX_NODES) { for (int i = 0; i < view->count && n < max; i++) if (view->nodes[i].parent == index) list[n++] = i; }
    else if (k >= 0 && apps[k].used) { int own = index % 65536; for (int i = 0; i < apps[k].tree.count && n < max; i++) if (apps[k].tree.nodes[i].parent == own) list[n++] = APP_INDEX(k, i); }
    return n;
}
static void path_of(int index, char* out, size_t size) {
    if (index < 0) { snprintf(out, size, ROOT_PATH); return; }
    if (index >= NOTICE_INDEX(0)) { snprintf(out, size, NODE_PREFIX "%u", notices[index - NOTICE_INDEX(0)].id); return; }
    struct node* n = node_at(index); snprintf(out, size, NODE_PREFIX "%u", n ? n->id : 0);
}
static bool resolve(const char* path, int* index) {
    if (!strcmp(path, ROOT_PATH)) { *index = -1; return true; }
    if (strncmp(path, NODE_PREFIX, sizeof(NODE_PREFIX) - 1)) return false;
    char* end; unsigned long id = strtoul(path + sizeof(NODE_PREFIX) - 1, &end, 10); if (*end || !id) return false;
    int i = find_id(now, (unsigned)id); if (i >= 0) { *index = i; return true; }
    for (int k = 0; k < NOTICES; k++) if (notices[k].id == id) { *index = NOTICE_INDEX(k); return true; }
    for (int k = 0; k < APPS; k++) if (apps[k].used && (i = find_id(&apps[k].tree, (unsigned)id)) >= 0) { *index = APP_INDEX(k, i); return true; }
    struct tree* previous = now == &trees[0] ? &trees[1] : &trees[0];
    i = find_id(previous, (unsigned)id); if (i >= 0) { view = previous; *index = i; return true; }   /* gone: read as hidden */
    return false;
}
static int role_of(int index) { if (index < 0) return ROLE_APPLICATION; if (index >= NOTICE_INDEX(0)) return ROLE_NOTIFICATION; struct node* n = node_at(index); return n ? n->role : ROLE_UNKNOWN; }
static const char* name_of(int index) { if (index < 0) return "Aurora"; if (index >= NOTICE_INDEX(0)) return notices[index - NOTICE_INDEX(0)].text; struct node* n = node_at(index); return n ? n->name : ""; }
static const char* description_of(int index) { struct node* n = index >= 0 && index < NOTICE_INDEX(0) ? node_at(index) : NULL; return n ? n->description : ""; }
static const char* text_of(int index) { struct node* n = index >= 0 && index < NOTICE_INDEX(0) ? node_at(index) : NULL; return n ? n->text : ""; }
static bool has_text(int index) { int r = role_of(index); return index >= 0 && (r == ROLE_ENTRY || r == ROLE_PASSWORD_TEXT); }
static uint64_t states_of(int index) {
    if (index < 0) return 0;
    if (index >= NOTICE_INDEX(0)) return 1ULL << ST_SHOWING | 1ULL << ST_VISIBLE | 1ULL << ST_ENABLED | 1ULL << ST_SENSITIVE;
    struct node* n = node_at(index); if (!n) return 0;
    if (index < MAX_NODES && view != now) return n->states & ~(1ULL << ST_ACTIVE | 1ULL << ST_FOCUSED | 1ULL << ST_SHOWING | 1ULL << ST_VISIBLE);
    return n->states;
}
static int parent_of(int index) {   /* -2: the desktop */
    if (index < 0) return -2; if (index >= NOTICE_INDEX(0)) return -1;
    struct node* n = node_at(index); if (!n || n->parent < 0) return -1;
    int k = app_of(index); return k >= 0 ? APP_INDEX(k, n->parent) : n->parent;
}
static const char* role_text(int role) { for (size_t r = 0; r < sizeof(ROLES) / sizeof(*ROLES); r++) if (ROLES[r].role == role) return ROLES[r].text; return "unknown"; }

/* ---------------------------------------------------------------- D-Bus replies */
static void append_ref(DBusMessageIter* it, int index) {
    DBusMessageIter s; char path[96]; const char* name = dbus_bus_get_unique_name(bus); const char* p = path;
    if (index == -2) { name = desktop_name; p = desktop_path; } else path_of(index, path, sizeof(path));
    dbus_message_iter_open_container(it, DBUS_TYPE_STRUCT, NULL, &s);
    dbus_message_iter_append_basic(&s, DBUS_TYPE_STRING, &name); dbus_message_iter_append_basic(&s, DBUS_TYPE_OBJECT_PATH, &p);
    dbus_message_iter_close_container(it, &s);
}
static void append_variant(DBusMessageIter* it, int type, const char* signature, const void* value, int ref) {
    DBusMessageIter v; dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, signature, &v);
    if (ref != -3) append_ref(&v, ref); else dbus_message_iter_append_basic(&v, type, value);
    dbus_message_iter_close_container(it, &v);
}
static bool property(DBusMessageIter* it, int index, const char* interface, const char* name) {   /* appends one variant */
    if (!strcmp(interface, ACCESSIBLE)) {
        if (!strcmp(name, "Name")) { const char* s = name_of(index); append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "Description")) { const char* s = description_of(index); append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "HelpText") || !strcmp(name, "AccessibleId")) { const char* s = ""; append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "Locale")) { const char* s = "en_US"; append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "Parent")) { append_variant(it, 0, "(so)", NULL, parent_of(index)); return true; }
        if (!strcmp(name, "ChildCount")) { int list[MAX_NODES + APPS + NOTICES]; dbus_int32_t n = children(index, list, MAX_NODES + APPS + NOTICES); append_variant(it, DBUS_TYPE_INT32, "i", &n, -3); return true; }
    }
    if (!strcmp(interface, TEXT) && has_text(index)) {
        if (!strcmp(name, "CharacterCount") || !strcmp(name, "CaretOffset")) { dbus_int32_t n = characters(text_of(index)); append_variant(it, DBUS_TYPE_INT32, "i", &n, -3); return true; }
    }
    if (!strcmp(interface, APPLICATION) && index == -1) {
        if (!strcmp(name, "ToolkitName")) { const char* s = "Aurora"; append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "Version")) { const char* s = "5.1"; append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "AtspiVersion")) { const char* s = "2.1"; append_variant(it, DBUS_TYPE_STRING, "s", &s, -3); return true; }
        if (!strcmp(name, "Id")) { static dbus_int32_t app_id; append_variant(it, DBUS_TYPE_INT32, "i", &app_id, -3); return true; }
    }
    return false;
}
static DBusHandlerResult reply_error(DBusMessage* m, const char* name, const char* text) {
    DBusMessage* r = dbus_message_new_error(m, name, text); if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); }
    return DBUS_HANDLER_RESULT_HANDLED;
}
static DBusHandlerResult send_reply(DBusMessage* r) { if (r) { dbus_connection_send(bus, r, NULL); dbus_message_unref(r); } return DBUS_HANDLER_RESULT_HANDLED; }
static DBusHandlerResult handle_call(DBusMessage* m);
static DBusHandlerResult handle(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; (void)data; view = now; DBusHandlerResult result = handle_call(m); view = now; return result;
}
static DBusHandlerResult cache(DBusConnection* c, DBusMessage* m, void* data) {   /* org.a11y.atspi.Cache: nothing cached (libatspi asks) */
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !dbus_message_is_method_call(m, "org.a11y.atspi.Cache", "GetItems")) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    DBusMessage* r = dbus_message_new_method_return(m); DBusMessageIter it, a; dbus_message_iter_init_append(r, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "((so)(so)(so)iiassusau)", &a); dbus_message_iter_close_container(&it, &a);
    dbus_connection_send(bus, r, NULL); dbus_message_unref(r); return DBUS_HANDLER_RESULT_HANDLED;
}
/* org.a11y.atspi.Text, read only, for entries and password fields (whose text is always empty here) */
static DBusHandlerResult text_call(DBusMessage* m, int index, const char* member) {
    const char* all = text_of(index); dbus_int32_t length = characters(all), a = 0, b = 0, zero = 0; dbus_uint32_t kind = 0;
    DBusMessage* r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    DBusMessageIter it, d; dbus_message_iter_init_append(r, &it);
    if (!strcmp(member, "GetText")) {
        dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &a, DBUS_TYPE_INT32, &b, DBUS_TYPE_INVALID);
        if (a < 0) a = 0; if (b < 0 || b > length) b = length; if (a > b) a = b;
        char piece[256]; const char* from = at_character(all, a); const char* to = at_character(all, b);
        snprintf(piece, sizeof(piece), "%.*s", (int)(to - from), from); const char* s = piece; dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s);
    } else if (!strcmp(member, "GetCharacterAtOffset")) {
        dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &a, DBUS_TYPE_INVALID);
        dbus_int32_t c = a >= 0 && a < length ? (unsigned char)*at_character(all, a) : 0; dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &c);
    } else if (!strcmp(member, "GetTextAtOffset") || !strcmp(member, "GetTextBeforeOffset") || !strcmp(member, "GetTextAfterOffset") || !strcmp(member, "GetStringAtOffset")) {
        dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &a, DBUS_TYPE_UINT32, &kind, DBUS_TYPE_INVALID);   /* the line is the whole field */
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &all); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &zero); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &length);
    } else if (!strcmp(member, "GetAttributeRun") || !strcmp(member, "GetAttributes")) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &d); dbus_message_iter_close_container(&it, &d);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &zero); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &length);
    } else if (!strcmp(member, "GetDefaultAttributes") || !strcmp(member, "GetDefaultAttributeSet")) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &d); dbus_message_iter_close_container(&it, &d);
    } else if (!strcmp(member, "GetNSelections") || !strcmp(member, "GetOffsetAtPoint")) {
        dbus_int32_t v = !strcmp(member, "GetNSelections") ? 0 : -1; dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &v);
    } else if (!strcmp(member, "GetSelection")) {
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &zero); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &zero);
    } else if (!strcmp(member, "GetCharacterExtents") || !strcmp(member, "GetRangeExtents")) {
        for (int i = 0; i < 4; i++) dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &zero);
    } else if (!strcmp(member, "SetCaretOffset") || !strcmp(member, "AddSelection") || !strcmp(member, "RemoveSelection") || !strcmp(member, "SetSelection")) {
        dbus_bool_t no = FALSE; dbus_message_iter_append_basic(&it, DBUS_TYPE_BOOLEAN, &no);
    } else { dbus_message_unref(r); return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); }
    return send_reply(r);
}
static DBusHandlerResult handle_call(DBusMessage* m) {
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* path = dbus_message_get_path(m); const char* interface = dbus_message_get_interface(m); const char* member = dbus_message_get_member(m);
    if (!path || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    int index; if (!resolve(path, &index)) return reply_error(m, "org.freedesktop.DBus.Error.UnknownObject", "that element is gone");
    if (trace) note("call %s %s.%s", path, interface ? interface : "-", member);
    DBusMessage* r = NULL; DBusMessageIter it, a;
    if (interface && !strcmp(interface, PROPERTIES)) {
        const char* want_interface = NULL; const char* want = NULL;
        if (!strcmp(member, "Get") && dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &want_interface, DBUS_TYPE_STRING, &want, DBUS_TYPE_INVALID)) {
            r = dbus_message_new_method_return(m); dbus_message_iter_init_append(r, &it);
            if (!property(&it, index, want_interface, want)) { dbus_message_unref(r); return reply_error(m, "org.freedesktop.DBus.Error.UnknownProperty", "no such property"); }
            return send_reply(r);
        }
        if (!strcmp(member, "GetAll") && dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &want_interface, DBUS_TYPE_INVALID)) {
            static const char* const accessible[] = {"Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId", "HelpText"};
            static const char* const application[] = {"ToolkitName", "Version", "AtspiVersion", "Id"};
            static const char* const text[] = {"CharacterCount", "CaretOffset"};
            bool app = !strcmp(want_interface, APPLICATION), txt = !strcmp(want_interface, TEXT);
            const char* const* names = app ? application : txt ? text : accessible; int count = app ? 4 : txt ? 2 : 7;
            bool ok = app ? index == -1 : txt ? has_text(index) : !strcmp(want_interface, ACCESSIBLE);
            r = dbus_message_new_method_return(m); dbus_message_iter_init_append(r, &it); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a);
            for (int i = 0; i < count && ok; i++) {
                DBusMessageIter e; dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
                dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &names[i]); property(&e, index, want_interface, names[i]); dbus_message_iter_close_container(&a, &e);
            }
            dbus_message_iter_close_container(&it, &a); return send_reply(r);
        }
        if (!strcmp(member, "Set")) return send_reply(dbus_message_new_method_return(m));   /* Application.Id: the registry's number, not needed */
        return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    }
    if (interface && !strcmp(interface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        const char* xml = "<node><interface name=\"" ACCESSIBLE "\"/></node>"; r = dbus_message_new_method_return(m);
        dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID); return send_reply(r);
    }
    if (interface && !strcmp(interface, APPLICATION)) {
        if (!strcmp(member, "GetLocale")) { const char* s = "en_US"; r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID); return send_reply(r); }
        if (!strcmp(member, "GetApplicationBusAddress")) { const char* s = ""; r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID); return send_reply(r); }
        return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    }
    if (interface && !strcmp(interface, TEXT)) {
        if (!has_text(index)) return reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "not a text field");
        return text_call(m, index, member);
    }
    if (interface && strcmp(interface, ACCESSIBLE)) return reply_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "not supported");
    r = dbus_message_new_method_return(m); if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    dbus_message_iter_init_append(r, &it); static int list[MAX_NODES + APPS + NOTICES]; const int max = MAX_NODES + APPS + NOTICES;
    if (!strcmp(member, "GetChildAtIndex")) {
        dbus_int32_t at = -1; dbus_message_get_args(m, NULL, DBUS_TYPE_INT32, &at, DBUS_TYPE_INVALID);
        int n = children(index, list, max);
        if (at < 0 || at >= n) { dbus_message_unref(r); return reply_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "no such child"); }
        append_ref(&it, list[at]);
    } else if (!strcmp(member, "GetChildren")) {
        int n = children(index, list, max); dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(so)", &a);
        for (int i = 0; i < n; i++) append_ref(&a, list[i]);
        dbus_message_iter_close_container(&it, &a);
    } else if (!strcmp(member, "GetIndexInParent")) {
        dbus_int32_t at = -1; int p = parent_of(index);
        if (p >= -1) { int n = children(p, list, max); for (int i = 0; i < n; i++) if (list[i] == index) at = i; }
        else at = 0;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &at);
    } else if (!strcmp(member, "GetRelationSet")) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(ua(so))", &a); dbus_message_iter_close_container(&it, &a);
    } else if (!strcmp(member, "GetRole")) {
        dbus_uint32_t role = (dbus_uint32_t)role_of(index); dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &role);
    } else if (!strcmp(member, "GetRoleName") || !strcmp(member, "GetLocalizedRoleName")) {
        const char* s = role_text(role_of(index)); dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s);
    } else if (!strcmp(member, "GetState")) {
        uint64_t s = states_of(index); dbus_uint32_t words[2] = {(dbus_uint32_t)s, (dbus_uint32_t)(s >> 32)}; const dbus_uint32_t* w = words;
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "u", &a); dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_UINT32, &w, 2); dbus_message_iter_close_container(&it, &a);
    } else if (!strcmp(member, "GetAttributes")) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{ss}", &a);
        DBusMessageIter e; const char* k = "toolkit"; const char* v = "Aurora";
        dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k); dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &v); dbus_message_iter_close_container(&a, &e);
        dbus_message_iter_close_container(&it, &a);
    } else if (!strcmp(member, "GetApplication")) {
        append_ref(&it, -1);
    } else if (!strcmp(member, "GetInterfaces")) {
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &a); const char* s = ACCESSIBLE; dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s);
        if (index == -1) { s = APPLICATION; dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s); }
        if (has_text(index)) { s = TEXT; dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s); }
        dbus_message_iter_close_container(&it, &a);
    } else { dbus_message_unref(r); return reply_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method"); }
    return send_reply(r);
}

/* ---------------------------------------------------------------- events */
/* any_data: a ref (child >= -2), a string (text != NULL) or the integer 0 */
static void emit_any(int index, const char* interface, const char* member, const char* detail, int one, int two, int child, const char* text) {
    char path[96]; path_of(index, path, sizeof(path));
    DBusMessage* s = dbus_message_new_signal(path, interface, member); if (!s) return;
    DBusMessageIter it, a; dbus_message_iter_init_append(s, &it); dbus_int32_t d1 = one, d2 = two, zero = 0;
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &detail); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &d1); dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &d2);
    if (text) append_variant(&it, DBUS_TYPE_STRING, "s", &text, -3);
    else if (child != -3) append_variant(&it, 0, "(so)", NULL, child); else append_variant(&it, DBUS_TYPE_INT32, "i", &zero, -3);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a); dbus_message_iter_close_container(&it, &a);
    dbus_connection_send(bus, s, NULL); dbus_message_unref(s);
    note("EVENT %s:%s:%s %d name=%s role=%s", !strcmp(interface, "org.a11y.atspi.Event.Window") ? "window" : "object", member, detail, one, name_of(index), role_text(role_of(index)));
}
static void emit(int index, const char* interface, const char* member, const char* detail, int one, int two, int child) { emit_any(index, interface, member, detail, one, two, child, NULL); }
#define OBJECT "org.a11y.atspi.Event.Object"
#define WINDOW "org.a11y.atspi.Event.Window"
static int top_of(const struct tree* t) { for (int i = 0; i < t->count; i++) if (t->nodes[i].parent < 0) return i; return -1; }
static int index_among_top(int index) { int list[MAX_NODES + APPS + NOTICES], n = top_children(list, MAX_NODES + APPS + NOTICES); for (int i = 0; i < n; i++) if (list[i] == index) return i; return 0; }
/* entries whose text changed: text-changed:delete/insert of the differing tail, then the caret at the end */
static void text_changes(const struct tree* old, const struct tree* new_tree, int app) {
    for (int i = 0; i < new_tree->count; i++) {
        const struct node* n = &new_tree->nodes[i]; if (n->role != ROLE_ENTRY) continue;
        int o = find_id(old, n->id); if (o < 0 || !strcmp(old->nodes[o].text, n->text)) continue;
        const char* was = old->nodes[o].text; const char* is = n->text; int same = 0;
        while (was[same] && was[same] == is[same]) same++;
        while (same > 0 && ((unsigned char)is[same] & 0xC0) == 0x80) same--;
        int index = app >= 0 ? APP_INDEX(app, i) : i, from = characters(is) - characters(is + same);
        if (was[same]) emit_any(index, OBJECT, "TextChanged", "delete", from, characters(was + same), -3, was + same);
        if (is[same]) emit_any(index, OBJECT, "TextChanged", "insert", from, characters(is + same), -3, is + same);
        emit(index, OBJECT, "TextCaretMoved", "", characters(is), 0, -3);
    }
}
static void update(const char* text, size_t size) {   /* the shell's state (aurora_reader.c) */
    struct tree* old = now; struct tree* next = now == &trees[0] ? &trees[1] : &trees[0];
    view = now; if (!parse(text, size, next, "")) return;
    int old_top = top_of(old), new_top = top_of(next);
    bool same_window = old_top >= 0 && new_top >= 0 && old->nodes[old_top].id == next->nodes[new_top].id;
    if (old_top >= 0 && !same_window) {   /* the shell window that had the keyboard goes */
        emit(old_top, OBJECT, "StateChanged", "active", 0, 0, -3); emit(old_top, WINDOW, "Deactivate", "", 0, 0, -3);
        emit(-1, OBJECT, "ChildrenChanged", "remove", index_among_top(old_top), 0, old_top);
    }
    now = next; view = now;
    if (new_top >= 0 && !same_window) {
        emit(-1, OBJECT, "ChildrenChanged", "add", index_among_top(new_top), 0, new_top);
        emit(new_top, WINDOW, "Activate", "", 0, 0, -3); emit(new_top, OBJECT, "StateChanged", "active", 1, 0, -3);
    }
    int was = find_key(old, old->focus), is = find_key(now, now->focus);
    bool moved = is >= 0 && (was < 0 || old->nodes[was].id != now->nodes[is].id || strcmp(old->nodes[was].name, now->nodes[is].name) || !same_window);
    if (moved) {
        if (was >= 0 && find_id(now, old->nodes[was].id) >= 0 && old->nodes[was].id != now->nodes[is].id) emit(find_id(now, old->nodes[was].id), OBJECT, "StateChanged", "focused", 0, 0, -3);
        emit(is, OBJECT, "StateChanged", "focused", 1, 0, -3);
    } else if (is >= 0 && was >= 0 && strcmp(old->nodes[was].description, now->nodes[is].description) && now->nodes[is].description[0])
        emit(is, OBJECT, "PropertyChange", "accessible-description", 0, 0, -3);
    if (same_window) text_changes(old, now, -1);
    if (is < 0 && was >= 0) note("FOCUS left the shell");
}
/* an aurora-app window's record ("app\tPID\tAPP\tTITLE\tACTIVE" + focus + nodes) */
static void app_update(int k, const char* text, size_t size) {
    struct app* a = &apps[k]; long pid = 0; char title[160] = ""; int active = 0;
    if (strncmp(text, "app\t", 4) || sscanf(text + 4, "%ld\t%*[^\t]\t%159[^\t]\t%d", &pid, title, &active) < 3) return;
    static struct tree old; old = a->tree; char prefix[32]; snprintf(prefix, sizeof(prefix), "app%ld:", pid);
    if (!parse(text, size, &a->tree, prefix)) return;
    a->pid = pid; int frame = top_of(&a->tree); if (frame < 0) return;
    if (active) a->tree.nodes[frame].states |= 1ULL << ST_ACTIVE; else a->tree.nodes[frame].states &= ~(1ULL << ST_ACTIVE);
    if (!a->announced) { a->announced = true; emit(-1, OBJECT, "ChildrenChanged", "add", index_among_top(APP_INDEX(k, frame)), 0, APP_INDEX(k, frame)); note("APP window pid=%ld title=%s", pid, title); }
    bool was_active = a->active; a->active = active;
    if (was_active && !active) { emit(APP_INDEX(k, frame), OBJECT, "StateChanged", "active", 0, 0, -3); emit(APP_INDEX(k, frame), WINDOW, "Deactivate", "", 0, 0, -3); }
    if (active && !was_active) { emit(APP_INDEX(k, frame), WINDOW, "Activate", "", 0, 0, -3); emit(APP_INDEX(k, frame), OBJECT, "StateChanged", "active", 1, 0, -3); }
    int was = find_key(&old, old.focus), is = find_key(&a->tree, a->tree.focus);
    if (active && is >= 0 && is != frame && (!was_active || was < 0 || old.nodes[was].id != a->tree.nodes[is].id || strcmp(old.nodes[was].name, a->tree.nodes[is].name)))
        emit(APP_INDEX(k, is), OBJECT, "StateChanged", "focused", 1, 0, -3);
    text_changes(&old, &a->tree, k);
}
static void app_gone(int k) {
    struct app* a = &apps[k]; int frame = top_of(&a->tree);
    if (a->announced && frame >= 0) {
        if (a->active) emit(APP_INDEX(k, frame), WINDOW, "Deactivate", "", 0, 0, -3);
        emit(-1, OBJECT, "ChildrenChanged", "remove", index_among_top(APP_INDEX(k, frame)), 0, APP_INDEX(k, frame));
    }
    note("APP gone pid=%ld", a->pid); close(a->fd); memset(a, 0, sizeof(*a)); a->fd = -1;
}
static void show_notice(const char* text) {
    int k = 0; for (int i = 0; i < NOTICES; i++) if (!notices[i].id || notices[i].until < notices[k].until) { k = i; if (!notices[i].id) break; }
    if (notices[k].id) { int index = NOTICE_INDEX(k); emit(-1, OBJECT, "ChildrenChanged", "remove", index_among_top(index), 0, index); }
    notices[k].id = next_id++; snprintf(notices[k].text, sizeof(notices[k].text), "%s", text); notices[k].until = now_ms() + 10000;
    int index = NOTICE_INDEX(k);
    emit(-1, OBJECT, "ChildrenChanged", "add", index_among_top(index), 0, index);
    emit(index, OBJECT, "StateChanged", "showing", 1, 0, -3);
}
static void expire_notices(void) {
    for (int k = 0; k < NOTICES; k++) if (notices[k].id && notices[k].until <= now_ms()) {
        int index = NOTICE_INDEX(k); emit(-1, OBJECT, "ChildrenChanged", "remove", index_among_top(index), 0, index); notices[k].id = 0;
    }
}

/* ---------------------------------------------------------------- org.freedesktop.a11y.KeyboardMonitor */
#define MANAGER_NAME "org.freedesktop.a11y.Manager"
#define MANAGER_PATH "/org/freedesktop/a11y/Manager"
#define MONITOR "org.freedesktop.a11y.KeyboardMonitor"
static void tell_aurora(const char* text) { if (shell >= 0 && send(shell, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) < 0) note("request lost: %s", strerror(errno)); }
static long sender_pid(DBusMessage* m) {
    const char* sender = dbus_message_get_sender(m); if (!sender) return -1;
    DBusMessage* q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "GetConnectionUnixProcessID");
    if (!q) return -1;
    dbus_message_append_args(q, DBUS_TYPE_STRING, &sender, DBUS_TYPE_INVALID);
    DBusError e; dbus_error_init(&e); DBusMessage* r = dbus_connection_send_with_reply_and_block(session, q, 5000, &e); dbus_message_unref(q);
    dbus_uint32_t pid = 0; long result = -1;
    if (r && dbus_message_get_args(r, &e, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID)) result = (long)pid;
    if (r) dbus_message_unref(r); dbus_error_free(&e); return result;
}
static void forget_watcher(void) { if (!key_watcher[0]) return; note("KEYS released by %s", key_watcher); key_watcher[0] = 0; tell_aurora("grab-all 0\n"); tell_aurora("grabs - -\n"); tell_aurora("keys 0\n"); }
static DBusHandlerResult session_reply(DBusMessage* r) { if (r) { dbus_connection_send(session, r, NULL); dbus_message_unref(r); } return DBUS_HANDLER_RESULT_HANDLED; }
static DBusHandlerResult session_error(DBusMessage* m, const char* name, const char* text) { return session_reply(dbus_message_new_error(m, name, text)); }
static DBusHandlerResult monitor(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; (void)data;
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char* interface = dbus_message_get_interface(m); const char* member = dbus_message_get_member(m); const char* sender = dbus_message_get_sender(m);
    if (!member || !sender) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (interface && !strcmp(interface, PROPERTIES) && !strcmp(member, "GetAll")) {   /* no properties */
        DBusMessage* r = dbus_message_new_method_return(m); DBusMessageIter it, a; dbus_message_iter_init_append(r, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &a); dbus_message_iter_close_container(&it, &a); return session_reply(r);
    }
    if (interface && !strcmp(interface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
        const char* xml = "<node><interface name=\"" MONITOR "\"><method name=\"GrabKeyboard\"/><method name=\"UngrabKeyboard\"/>"
                          "<method name=\"WatchKeyboard\"/><method name=\"UnwatchKeyboard\"/><method name=\"SetKeyGrabs\"><arg type=\"au\" direction=\"in\"/>"
                          "<arg type=\"a(uu)\" direction=\"in\"/></method><signal name=\"KeyEvent\"><arg type=\"b\"/><arg type=\"u\"/><arg type=\"u\"/>"
                          "<arg type=\"u\"/><arg type=\"q\"/></signal></interface></node>";
        DBusMessage* r = dbus_message_new_method_return(m); dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID); return session_reply(r);
    }
    if (interface && strcmp(interface, MONITOR)) return session_error(m, "org.freedesktop.DBus.Error.UnknownInterface", "not supported");
    long pid = sender_pid(m);
    if (orca <= 0 || pid != (long)orca) { note("KEYS refused %s pid=%ld", member, pid); return session_error(m, "org.freedesktop.DBus.Error.AccessDenied", "only the screen reader started with it may use the keyboard monitor"); }
    if (key_watcher[0] && strcmp(key_watcher, sender)) forget_watcher();
    if (!strcmp(member, "WatchKeyboard")) { snprintf(key_watcher, sizeof(key_watcher), "%s", sender); tell_aurora("keys 1\n"); note("KEYS watched by %s", sender); }
    else if (!strcmp(member, "UnwatchKeyboard")) forget_watcher();
    else if (!strcmp(member, "GrabKeyboard") || !strcmp(member, "UngrabKeyboard")) tell_aurora(member[0] == 'G' ? "grab-all 1\n" : "grab-all 0\n");
    else if (!strcmp(member, "SetKeyGrabs")) {
        DBusMessageIter it, a, st; char request[8192]; size_t used = (size_t)snprintf(request, sizeof(request), "grabs "); int count = 0;
        if (!dbus_message_iter_init(m, &it) || strcmp(dbus_message_get_signature(m), "aua(uu)")) return session_error(m, "org.freedesktop.DBus.Error.InvalidArgs", "SetKeyGrabs(au, a(uu))");
        dbus_message_iter_recurse(&it, &a);
        while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_UINT32 && used + 24 < sizeof(request)) { dbus_uint32_t v; dbus_message_iter_get_basic(&a, &v); used += (size_t)snprintf(request + used, sizeof(request) - used, "%s%u", count++ ? "," : "", v); dbus_message_iter_next(&a); }
        if (!count) used += (size_t)snprintf(request + used, sizeof(request) - used, "-");
        dbus_message_iter_next(&it); dbus_message_iter_recurse(&it, &a); count = 0; used += (size_t)snprintf(request + used, sizeof(request) - used, " ");
        while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRUCT && used + 32 < sizeof(request)) {
            dbus_uint32_t keysym = 0, modifiers = 0; dbus_message_iter_recurse(&a, &st); dbus_message_iter_get_basic(&st, &keysym); dbus_message_iter_next(&st); dbus_message_iter_get_basic(&st, &modifiers);
            used += (size_t)snprintf(request + used, sizeof(request) - used, "%s%u:%u", count++ ? "," : "", keysym, modifiers); dbus_message_iter_next(&a);
        }
        if (!count) used += (size_t)snprintf(request + used, sizeof(request) - used, "-");
        snprintf(request + used, sizeof(request) - used, "\n"); tell_aurora(request);
    } else return session_error(m, "org.freedesktop.DBus.Error.UnknownMethod", "unknown method");
    return session_reply(dbus_message_new_method_return(m));
}
static DBusHandlerResult session_filter(DBusConnection* c, DBusMessage* m, void* data) {
    (void)c; (void)data; const char *name, *old_owner, *new_owner;
    if (key_watcher[0] && dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged") &&
        dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner, DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
        !strcmp(name, key_watcher) && !new_owner[0]) forget_watcher();
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
static void key_event(char* record) {   /* "key\tPRESSED\tSTATE\tKEYSYM\tUNICHAR\tKEYCODE" from Aurora */
    unsigned pressed, state, keysym, unichar, keycode;
    if (!key_watcher[0] || sscanf(record, "key\t%u\t%u\t%u\t%u\t%u", &pressed, &state, &keysym, &unichar, &keycode) != 5) return;
    DBusMessage* s = dbus_message_new_signal(MANAGER_PATH, MONITOR, "KeyEvent"); if (!s) return;
    dbus_bool_t released = !pressed; dbus_uint32_t st = state, ks = keysym, uc = unichar; dbus_uint16_t kc = (dbus_uint16_t)keycode;
    dbus_message_set_destination(s, key_watcher);
    dbus_message_append_args(s, DBUS_TYPE_BOOLEAN, &released, DBUS_TYPE_UINT32, &st, DBUS_TYPE_UINT32, &ks, DBUS_TYPE_UINT32, &uc, DBUS_TYPE_UINT16, &kc, DBUS_TYPE_INVALID);
    dbus_connection_send(session, s, NULL); dbus_message_unref(s);
    if (trace) note("KEY %s keysym=0x%x state=0x%x", pressed ? "press" : "release", keysym, state);
}
static bool keyboard_monitor(void) {
    DBusError e; dbus_error_init(&e);
    session = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!session) { note("no session bus: %s", e.message ? e.message : "?"); dbus_error_free(&e); return false; }
    dbus_connection_set_exit_on_disconnect(session, FALSE);
    int owned = -1;
    for (int tries = 0; tries < 40; tries++) {   /* a screen reader that is just being switched off may still own it */
        owned = dbus_bus_request_name(session, MANAGER_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
        if (owned != DBUS_REQUEST_NAME_REPLY_EXISTS) break;
        usleep(250000);
    }
    if (owned != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) { note("%s is taken (%s): no keyboard monitor", MANAGER_NAME, e.message ? e.message : "owned"); dbus_error_free(&e); return false; }
    static const DBusObjectPathVTable vtable = {.message_function = monitor};
    dbus_connection_register_object_path(session, MANAGER_PATH, &vtable, NULL);
    dbus_bus_add_match(session, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged'", NULL);
    dbus_connection_add_filter(session, session_filter, NULL, NULL);
    return true;
}

/* ---------------------------------------------------------------- set-up */
static DBusConnection* a11y_bus(void) {
    DBusError e; dbus_error_init(&e);
    DBusConnection* session = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!session) { note("no session bus: %s", e.message ? e.message : "?"); dbus_error_free(&e); return NULL; }
    DBusMessage* m = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    DBusMessage* r = m ? dbus_connection_send_with_reply_and_block(session, m, 30000, &e) : NULL; if (m) dbus_message_unref(m);
    const char* address = NULL; char copy[512] = "";
    if (r && dbus_message_get_args(r, &e, DBUS_TYPE_STRING, &address, DBUS_TYPE_INVALID) && address) snprintf(copy, sizeof(copy), "%s", address);
    if (r) dbus_message_unref(r);
    dbus_connection_close(session); dbus_connection_unref(session);
    if (!copy[0]) { note("no accessibility bus: %s", e.message ? e.message : "?"); dbus_error_free(&e); return NULL; }
    DBusConnection* c = dbus_connection_open_private(copy, &e);
    if (!c || !dbus_bus_register(c, &e)) { note("accessibility bus %s: %s", copy, e.message ? e.message : "?"); dbus_error_free(&e); if (c) { dbus_connection_close(c); dbus_connection_unref(c); } return NULL; }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    return c;
}
static bool embed(void) {
    DBusError e; dbus_error_init(&e);
    DBusMessage* m = dbus_message_new_method_call("org.a11y.atspi.Registry", ROOT_PATH, "org.a11y.atspi.Socket", "Embed"); if (!m) return false;
    DBusMessageIter it; dbus_message_iter_init_append(m, &it); append_ref(&it, -1);
    DBusMessage* r = NULL;   /* the registry is started on demand with the accessibility bus: a slow first start */
    for (int attempt = 1; !r && attempt <= 3; attempt++) {
        r = dbus_connection_send_with_reply_and_block(bus, m, 20000, &e);
        if (!r) { note("Embed attempt %d failed: %s", attempt, e.message ? e.message : "?"); dbus_error_free(&e); dbus_error_init(&e); }
    }
    dbus_message_unref(m);
    if (!r) return false;
    DBusMessageIter ri, s; const char* name = NULL; const char* path = NULL;
    if (dbus_message_iter_init(r, &ri) && dbus_message_iter_get_arg_type(&ri) == DBUS_TYPE_STRUCT) {
        dbus_message_iter_recurse(&ri, &s); if (dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_STRING) dbus_message_iter_get_basic(&s, &name);
        if (dbus_message_iter_next(&s) && dbus_message_iter_get_arg_type(&s) == DBUS_TYPE_OBJECT_PATH) dbus_message_iter_get_basic(&s, &path);
    }
    if (name && path) { snprintf(desktop_name, sizeof(desktop_name), "%s", name); snprintf(desktop_path, sizeof(desktop_path), "%s", path); }
    dbus_message_unref(r); return true;
}
static int watch_shell(void) {   /* "a11y-watch" with one end of a SEQPACKET pair: Aurora writes the shell's state into it */
    const char* run = getenv("XDG_RUNTIME_DIR"); if (!run || run[0] != '/') { note("no XDG_RUNTIME_DIR"); return -1; }
    int pair[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair)) return -1;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-control", run);
    bool ok = false; char reply[512] = "";   /* (no SO_RCVTIMEO on NexusOS unix sockets: poll() bounds the wait) */
    if (s < 0 || connect(s, (struct sockaddr*)&a, sizeof(a))) snprintf(reply, sizeof(reply), "connect %s: %s", a.sun_path, strerror(errno));
    else {
        const char request[] = "a11y-watch\n"; struct iovec iov = {(void*)request, sizeof(request) - 1};
        char control[CMSG_SPACE(sizeof(int))]; memset(control, 0, sizeof(control));
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        struct cmsghdr* c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(c), &pair[1], sizeof(int));
        if (sendmsg(s, &m, MSG_NOSIGNAL) == (ssize_t)iov.iov_len) {
            size_t used = 0; ssize_t n = -1; struct pollfd w = {s, POLLIN, 0};
            while (used < sizeof(reply) - 1 && poll(&w, 1, 5000) > 0 && (n = read(s, reply + used, sizeof(reply) - 1 - used)) > 0) { used += (size_t)n; reply[used] = 0; if (strstr(reply, "OK\n") || strstr(reply, "ERROR")) break; }
            ok = strstr(reply, "OK\n") != NULL;
            if (!reply[0]) snprintf(reply, sizeof(reply), "%s", n == 0 ? "closed without a reply" : "no reply within 5 s");
        } else snprintf(reply, sizeof(reply), "sendmsg: %s", strerror(errno));
    }
    if (s >= 0) close(s); close(pair[1]);
    if (!ok) { note("a11y-watch refused: %s", reply[0] ? reply : "no answer"); close(pair[0]); return -1; }
    return pair[0];
}
static void on_signal(int number) { (void)number; int saved = errno; if (child_pipe[1] >= 0 && write(child_pipe[1], "x", 1) < 0) {} errno = saved; }
/* speech-dispatcher plays through its server's audio output; Debian's default is PulseAudio, which NexusOS does not
 * run (no sound at all, and "spd-say -w" never returns). Without a user configuration and without a PulseAudio socket,
 * the user's ~/.config/speech-dispatcher/speechd.conf becomes the system one with AudioOutputMethod "alsa". */
static void speech_config(void) {
    const char* home = getenv("HOME"); const char* run = getenv("XDG_RUNTIME_DIR"); char path[512], temporary[560], pulse[300];
    if (!home || home[0] != '/' || snprintf(path, sizeof(path), "%s/.config/speech-dispatcher/speechd.conf", home) >= (int)sizeof(path)) return;
    if (run && snprintf(pulse, sizeof(pulse), "%s/pulse/native", run) < (int)sizeof(pulse) && !access(pulse, F_OK)) return;
    if (!access(path, F_OK)) return;
    FILE* in = fopen("/etc/speech-dispatcher/speechd.conf", "re"); if (!in) return;
    char folder[512]; snprintf(folder, sizeof(folder), "%s/.config", home); mkdir(folder, 0700);
    snprintf(folder, sizeof(folder), "%s/.config/speech-dispatcher", home); mkdir(folder, 0700);
    snprintf(temporary, sizeof(temporary), "%s.nexus-tmp", path);
    FILE* out = fopen(temporary, "wxe"); if (!out) { fclose(in); return; }
    char line[1024]; bool set = false;
    while (fgets(line, sizeof(line), in)) {
        const char* p = line; while (*p == '#' || *p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "AudioOutputMethod", 17)) { if (!set) fputs("AudioOutputMethod \"alsa\"   # NexusOS: no PulseAudio\n", out); set = true; continue; }
        fputs(line, out);
    }
    if (!set) fputs("AudioOutputMethod \"alsa\"   # NexusOS: no PulseAudio\n", out);
    fclose(in);
    if (fclose(out) || rename(temporary, path)) { unlink(temporary); return; }
    note("SPEECH configured speech-dispatcher for ALSA (%s)", path);
}
static pid_t start_orca(void) {
    speech_config();
    pid_t pid = fork(); if (pid) return pid;
    signal(SIGCHLD, SIG_DFL); signal(SIGTERM, SIG_DFL);
    char* command[64]; int n = 0; bool wayland = !getenv("DISPLAY") && !access(ORCA_WAYLAND, R_OK);
    if (wayland) { command[n++] = (char*)"python3"; command[n++] = (char*)ORCA_WAYLAND; } else command[n++] = (char*)"orca";
    for (int i = 0; orca_args && orca_args[i] && n < 63; i++) command[n++] = orca_args[i];
    command[n] = NULL; execvp(wayland ? "/usr/bin/python3" : "orca", command);
    _exit(127);
}
/* ---------------------------------------------------------------- aurora-app windows */
static char apps_path[108];
static void apps_listen(void) {
    const char* run = getenv("XDG_RUNTIME_DIR"); if (!run || run[0] != '/') return;
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    if (snprintf(a.sun_path, sizeof(a.sun_path), "%s/aurora-a11y-apps", run) >= (int)sizeof(a.sun_path)) return;
    unlink(a.sun_path);
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0); if (s < 0) return;
    mode_t mask = umask(077);
    if (bind(s, (struct sockaddr*)&a, sizeof(a)) || listen(s, APPS)) { note("apps socket %s: %s", a.sun_path, strerror(errno)); umask(mask); close(s); return; }
    umask(mask); chmod(a.sun_path, 0600);
    apps_listener = s; snprintf(apps_path, sizeof(apps_path), "%s", a.sun_path);
    for (int k = 0; k < APPS; k++) apps[k].fd = -1;
}
static void apps_accept(void) {   /* only the session user's own processes */
    int c = accept4(apps_listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK); if (c < 0) return;
    struct ucred cred; socklen_t size = sizeof(cred); int k = -1;
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid != getuid()) { note("APP refused (uid %d)", size == sizeof(cred) ? (int)cred.uid : -1); close(c); return; }
    for (int i = 0; i < APPS && k < 0; i++) if (!apps[i].used) k = i;
    if (k < 0) { note("APP refused: %d windows already", APPS); close(c); return; }
    memset(&apps[k], 0, sizeof(apps[k])); apps[k].used = true; apps[k].fd = c; apps[k].pid = cred.pid;
}
static void apps_read(int k) {
    static char record[RECORD + 1];
    for (;;) {
        ssize_t n = recv(apps[k].fd, record, RECORD, MSG_DONTWAIT);
        if (n > 0) { record[n] = 0; app_update(k, record, (size_t)n); continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        app_gone(k); return;
    }
}
static volatile sig_atomic_t stopping;
static void on_term(int number) { (void)number; stopping = 1; if (child_pipe[1] >= 0 && write(child_pipe[1], "x", 1) < 0) {} }
int main(int argc, char** argv) {
    bool want_orca = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--orca")) want_orca = true; else if (!strcmp(argv[i], "--trace")) trace = true;
        else if (!strcmp(argv[i], "--")) { orca_args = argv + i + 1; break; }
        else { fprintf(stderr, "usage: aurora-a11y-bridge [--orca] [--trace] [-- ORCA-ARGUMENTS]\n"); return 2; }
    }
    if (!getuid() || !geteuid()) { note("refusing to run as root"); return 1; }
    if (pipe2(child_pipe, O_CLOEXEC | O_NONBLOCK)) return 1;
    struct sigaction sa = {0}; sa.sa_handler = on_signal; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_term; sa.sa_flags = SA_RESTART; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); signal(SIGPIPE, SIG_IGN);
    strcpy(trees[0].focus, "-"); strcpy(trees[1].focus, "-");
    bus = a11y_bus(); if (!bus) return 1;
    static const DBusObjectPathVTable vtable = {.message_function = handle};
    if (!dbus_connection_register_fallback(bus, "/org/a11y/atspi/accessible", &vtable, NULL)) return 1;
    static const DBusObjectPathVTable cache_vtable = {.message_function = cache};
    dbus_connection_register_object_path(bus, "/org/a11y/atspi/cache", &cache_vtable, NULL);
    if (!embed()) return 1;
    shell = watch_shell(); if (shell < 0) return 1;
    apps_listen();
    bool keys = keyboard_monitor();
    note("READY bus=%s keyboard_monitor=%d", dbus_bus_get_unique_name(bus), keys);
    if (want_orca) { orca = start_orca(); note("ORCA pid=%ld", (long)orca); }
    static char buffer[RECORD + 1]; int status = 0;
    while (!stopping) {
        while (dbus_connection_dispatch(bus) == DBUS_DISPATCH_DATA_REMAINS) {}
        while (session && dbus_connection_dispatch(session) == DBUS_DISPATCH_DATA_REMAINS) {}
        int fd = -1, sfd = -1; dbus_connection_get_unix_fd(bus, &fd); if (session) dbus_connection_get_unix_fd(session, &sfd);
        struct pollfd p[5 + APPS] = {{fd, POLLIN, 0}, {shell, POLLIN, 0}, {child_pipe[0], POLLIN, 0}, {sfd, POLLIN, 0}, {apps_listener, POLLIN, 0}};
        for (int k = 0; k < APPS; k++) p[5 + k] = (struct pollfd){apps[k].used ? apps[k].fd : -1, POLLIN, 0};
        if (poll(p, 5 + APPS, 1000) < 0 && errno != EINTR) break;
        if (session && (p[3].revents & (POLLIN | POLLHUP | POLLERR)) && (!dbus_connection_read_write(session, 0) || !dbus_connection_get_is_connected(session))) { note("session bus closed"); break; }
        expire_notices();
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) { if (!dbus_connection_read_write(bus, 0) || !dbus_connection_get_is_connected(bus)) { note("accessibility bus closed"); break; } }
        if (p[1].revents & POLLIN) {
            ssize_t n = recv(shell, buffer, RECORD, 0);
            if (n <= 0) { note("Aurora closed the watch"); break; }
            buffer[n] = 0;
            if (!strncmp(buffer, "quit\n", 5)) { note("switched off"); break; }
            if (!strncmp(buffer, "key\t", 4)) { key_event(buffer); dbus_connection_flush(session); continue; }
            if (!strncmp(buffer, "shell\n", 6)) update(buffer, (size_t)n);
            else if (!strncmp(buffer, "notice\t", 7)) { char* end = strchr(buffer, '\n'); if (end) *end = 0; if (buffer[7]) show_notice(buffer + 7); }
        } else if (p[1].revents & (POLLHUP | POLLERR)) { note("Aurora closed the watch"); break; }
        if (apps_listener >= 0 && (p[4].revents & POLLIN)) apps_accept();
        for (int k = 0; k < APPS; k++) if (apps[k].used && p[5 + k].revents) apps_read(k);
        if (p[2].revents & POLLIN) {
            char drain[64]; while (read(child_pipe[0], drain, sizeof(drain)) > 0) {}
            int result; pid_t done;
            while ((done = waitpid(-1, &result, WNOHANG)) > 0) if (done == orca) { note("ORCA EXIT status=%d", result); orca = -1; status = 0; stopping = 1; }
        }
        dbus_connection_flush(bus); if (session) dbus_connection_flush(session);
    }
    if (orca > 0) { kill(orca, SIGTERM); for (int i = 0; i < 50 && waitpid(orca, NULL, WNOHANG) == 0; i++) usleep(100000); if (waitpid(orca, NULL, WNOHANG) == 0) { kill(orca, SIGKILL); waitpid(orca, NULL, 0); } }
    if (apps_path[0]) unlink(apps_path);
    note("EXIT"); close(shell); dbus_connection_close(bus); dbus_connection_unref(bus);
    if (session) { dbus_connection_close(session); dbus_connection_unref(session); }
    return status;
}

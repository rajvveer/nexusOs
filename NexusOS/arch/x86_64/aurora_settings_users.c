/* Settings > Users: the accounts (/etc/passwd: uid 1000 and up), their password and administrator state, adding and
 * deleting accounts, changing passwords, and the login options (automatic login, lock after idle time, lock before
 * sleep: AURORA_LOGIN_CONF, re-read by the desktop through "reload-login"). Account changes go through the helper
 * /usr/libexec/nexus-auth (set-user-ID root on ext4 roots): on a FAT root /etc cannot be written and the page says so.
 * With the authorization broker running (nexus-polkitd, aurora_authdialog.h) adding, deleting and promoting accounts
 * first asks it through the authentication dialog (action org.nexus.accounts.manage, else pkexec's action): the forms
 * then have no "Your password" field; the password the dialog verified is handed to nexus-auth, which checks it too. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "aurora_settings.h"
#include "aurora_auth.h"
#include "aurora_authdialog.h"
#include "../../kernel/macui.h"
#include "../../kernel/appui.h"

#define USERS 16
static struct aurora_user users[USERS]; static int count, selected;
static struct aurora_login_conf conf;
static bool writable, helper, agent;   /* agent: the broker answers, admin actions go through the dialog */
static const char* agent_action = AURORA_ACTION_ACCOUNTS;
static pid_t agent_pid = -1; static int agent_fd = -1; static char agent_reply[256]; static size_t agent_used; static bool agent_done;
enum { FORM_NONE, FORM_PASSWORD, FORM_ADD, FORM_ADMIN, FORM_DELETE };
static int form, field;              /* the open form and its focused field */
static int form_fields(void) { return form == FORM_PASSWORD ? 3 : form == FORM_ADD ? (agent ? 3 : 4) : form == FORM_NONE ? 0 : agent ? 0 : 1; }
static char inputs[4][64];           /* password: current, new, confirm | add: full name, account, password, your password */
static bool add_admin;
struct rect { int x, y, w, h; };
static struct rect hit_users[USERS], hit_password, hit_admin, hit_delete, hit_add, hit_autologin, hit_lock_after, hit_sleep, hit_fields[4], hit_ok, hit_cancel, hit_form_admin;
static int page_x, page_y;
static const int LOCK_AFTER[4] = {0, 60, 300, 900};

static bool inside(int x, int y, struct rect r) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static void clear_inputs(void) { memset(inputs, 0, sizeof(inputs)); field = 0; }
static void load(void) {
    count = aurora_users(users, USERS, false); if (selected >= count) selected = 0;
    aurora_login_conf(&conf);
    helper = !access(AURORA_HELPER, X_OK); writable = false;
    if (helper) {   /* "posix=1 writable=1": an ext4 root and a set-user-ID helper */
        int out[2]; if (!pipe2(out, O_CLOEXEC)) {
            pid_t pid = fork();
            if (!pid) { dup2(out[1], 1); execl(AURORA_HELPER, "nexus-auth", "status", (char*)NULL); _exit(127); }
            close(out[1]); char text[128] = ""; ssize_t n = pid > 0 ? read(out[0], text, sizeof(text) - 1) : -1; close(out[0]);
            if (pid > 0) waitpid(pid, NULL, 0);
            if (n > 0) { text[n] = 0; writable = strstr(text, "writable=1") != NULL; }
        }
    }
    agent = writable && aurora_authz_available();
    if (agent) agent_action = aurora_authz_known(AURORA_ACTION_ACCOUNTS) ? AURORA_ACTION_ACCOUNTS : AURORA_ACTION_EXEC;
    printf("[64] SETTINGS USERS count=%d writable=%d helper=%d autologin=%d lock_after=%d agent=%d\n", count, writable, helper, conf.autologin, conf.lock_after, agent); fflush(stdout);
    settings_redraw();
}
/* Run the helper with stdin lines; its status (AURORA_AUTH_*) and first output line. */
static int run_helper(const char* const* argv, const char* const* lines, int line_count, char* output, size_t size) {
    output[0] = 0; if (!helper) return AURORA_AUTH_UNAVAILABLE;
    int in[2], out[2]; if (pipe2(in, O_CLOEXEC)) return AURORA_AUTH_UNAVAILABLE;
    if (pipe2(out, O_CLOEXEC)) { close(in[0]); close(in[1]); return AURORA_AUTH_UNAVAILABLE; }
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return AURORA_AUTH_UNAVAILABLE; }
    if (!pid) { dup2(in[0], 0); dup2(out[1], 1); execv(AURORA_HELPER, (char* const*)argv); _exit(AURORA_AUTH_UNAVAILABLE); }
    close(in[0]); close(out[1]);
    for (int i = 0; i < line_count; i++) { (void)!write(in[1], lines[i], strlen(lines[i])); (void)!write(in[1], "\n", 1); }
    close(in[1]);
    ssize_t n = read(out[0], output, size - 1); close(out[0]); if (n > 0) { output[n] = 0; output[strcspn(output, "\n")] = 0; }
    int status = 0; while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : AURORA_AUTH_UNAVAILABLE;
}
static void result(const char* what, int code, const char* output) {
    char text[200];
    if (code == AURORA_AUTH_OK) snprintf(text, sizeof(text), "%s", output[0] ? output : what);
    else snprintf(text, sizeof(text), "%s: %s", what, aurora_auth_message(code));
    settings_message(text); printf("[64] SETTINGS USERS %s code=%d\n", what, code); fflush(stdout);
}
static void save_conf(void) {
    if (!aurora_login_conf_save(&conf)) { settings_message("Could not save the login options."); return; }
    char reply[128]; settings_request("reload-login\n", reply, sizeof(reply));
    printf("[64] SETTINGS LOGIN autologin=%d lock_after=%d lock_on_sleep=%d\n", conf.autologin, conf.lock_after, conf.lock_on_sleep); fflush(stdout);
    settings_message("Login options saved.");
}
/* The authentication dialog in its own process (aurora-app authenticate): the verified password comes back on a pipe. */
static void begin_auth(const char* description) {
    int p[2]; if (pipe2(p, O_CLOEXEC)) { settings_message("Could not start the authentication dialog."); return; }
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); settings_message("Could not start the authentication dialog."); return; }
    if (!pid) {
        if (dup2(p[1], 3) < 0) _exit(2);
        char* argv[] = {(char*)"aurora-app", (char*)"authenticate", (char*)agent_action, (char*)"--app", (char*)"System Settings",
                        (char*)"--description", (char*)description, (char*)"--passfd", (char*)"3", NULL};
        execv("/proc/self/exe", argv); execv("/bin/aurora-app", argv); _exit(2);
    }
    close(p[1]); agent_fd = p[0]; fcntl(agent_fd, F_SETFL, O_NONBLOCK); agent_pid = pid; agent_used = 0; agent_reply[0] = 0;
    settings_message("Authenticate in the dialog to continue."); printf("[64] SETTINGS USERS AUTH started action=%s\n", agent_action); fflush(stdout);
}
static void submit(void);
static bool tick(void) {   /* the dialog's verdict */
    if (agent_pid <= 0) return false;
    ssize_t n; while (agent_used < sizeof(agent_reply) - 1 && (n = read(agent_fd, agent_reply + agent_used, sizeof(agent_reply) - 1 - agent_used)) > 0) agent_used += (size_t)n;
    agent_reply[agent_used] = 0;
    int status; if (waitpid(agent_pid, &status, WNOHANG) != agent_pid) return false;
    close(agent_fd); agent_fd = -1; agent_pid = -1;
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 2; char* nl = strchr(agent_reply, '\n');
    printf("[64] SETTINGS USERS AUTH result=%d\n", code); fflush(stdout);
    if (code == 0 && nl) {
        *nl = 0; char* slot = form == FORM_ADD ? inputs[3] : inputs[0]; snprintf(slot, sizeof(inputs[0]), "%s", agent_reply);
        memset(agent_reply, 0, sizeof(agent_reply)); agent_done = true; submit(); agent_done = false; return true;
    }
    memset(agent_reply, 0, sizeof(agent_reply));
    settings_message(code == 1 ? "Authentication cancelled." : code == 3 ? "You are not allowed to change user accounts (administrators only)." :
                     code == 4 ? "Too many failed attempts: try again later." : "The authorization service is not available.");
    return true;
}
static void submit(void) {
    struct aurora_user* u = count ? &users[selected] : NULL; char output[160]; int code;
    if (agent && !agent_done && (form == FORM_ADD || ((form == FORM_ADMIN || form == FORM_DELETE) && u))) {   /* ask the broker first */
        if (agent_pid > 0) return;
        if (form == FORM_ADD && (!inputs[1][0] || !inputs[2][0])) { settings_message("An account name and a password are needed."); return; }
        char description[160];
        if (form == FORM_ADD) snprintf(description, sizeof(description), "create the account %s", inputs[1]);
        else if (form == FORM_DELETE) snprintf(description, sizeof(description), "delete the account %s", u->name);
        else snprintf(description, sizeof(description), "%s %s as an administrator", u->admin ? "remove" : "make", u->name);
        begin_auth(description); return;
    }
    if (form == FORM_PASSWORD && u) {
        if (strcmp(inputs[1], inputs[2])) { settings_message("The new passwords do not match."); return; }
        if (!inputs[1][0]) { settings_message("Type a new password."); return; }
        const char* argv[] = {"nexus-auth", "passwd", u->name, NULL}; const char* lines[] = {inputs[0], inputs[1]};
        code = run_helper(argv, lines, 2, output, sizeof(output)); result("Change password", code, output);
    } else if (form == FORM_ADD) {
        if (!inputs[1][0] || !inputs[2][0]) { settings_message("An account name and a password are needed."); return; }
        const char* argv[] = {"nexus-auth", "useradd", inputs[1], inputs[0][0] ? inputs[0] : inputs[1], add_admin ? "1" : "0", NULL}; const char* lines[] = {inputs[3], inputs[2]};
        code = run_helper(argv, lines, 2, output, sizeof(output)); result("Add user", code, output);
    } else if ((form == FORM_ADMIN || form == FORM_DELETE) && u) {
        const char* admin_argv[] = {"nexus-auth", "admin", u->name, u->admin ? "0" : "1", NULL}; const char* delete_argv[] = {"nexus-auth", "userdel", u->name, NULL};
        const char* lines[] = {inputs[0]};
        code = run_helper(form == FORM_ADMIN ? admin_argv : delete_argv, lines, 1, output, sizeof(output));
        result(form == FORM_ADMIN ? "Administrator" : "Delete user", code, output);
    } else return;
    clear_inputs(); if (code == AURORA_AUTH_OK) form = FORM_NONE;
    load();
}
static void enter(void) { form = FORM_NONE; clear_inputs(); load(); }
static void draw(int x, int y, int w, int h) {
    (void)h; page_x = x; page_y = y;
    appui_px_text(x + 20, y + 14, "Users", APPUI_F_TITLE, APPUI_TEXT);
    if (!writable) appui_px_text(x + 20, y + 44, helper ? "Accounts are read-only on this disk (FAT root): sign-in and the lock screen use them as they are."
                                                         : "The account helper (/usr/libexec/nexus-auth) is not installed: accounts are read-only.", APPUI_F_SMALL, APPUI_SUBTLE);
    int ly = y + 66;
    for (int i = 0; i < count && i < USERS; i++) {
        struct aurora_user* u = &users[i]; struct rect r = {x + 20, ly, w - 40, 52};
        appui_px_card(r.x, r.y, r.w, r.h, 8, i == selected ? 0x2F4F86 : APPUI_PANEL); hit_users[i] = (struct rect){r.x - x, r.y - y, r.w, r.h};
        ui_avatar(r.x + 28, r.y + 26, 18);
        appui_px_text(r.x + 56, r.y + 8, u->gecos[0] ? u->gecos : u->name, APPUI_F_BOLD, APPUI_TEXT);
        char line[160]; snprintf(line, sizeof(line), "%s  -  %s  -  %s", u->name, u->admin ? "Administrator" : "Standard",
                                 u->password == AURORA_PW_SET ? "password set" : u->password == AURORA_PW_NONE ? "no password" : u->password == AURORA_PW_LOCKED ? "locked" : "password hidden");
        appui_px_text(r.x + 56, r.y + 28, line, APPUI_F_SMALL, APPUI_SUBTLE);
        ly += 60;
    }
    int by = ly + 6;
    hit_password = (struct rect){x + 20 - x, by - y, 170, 28}; appui_px_button(x + 20, by, 170, 28, "Change Password...", false);
    hit_admin = (struct rect){x + 200 - x, by - y, 150, 28}; appui_px_button(x + 200, by, 150, 28, count && users[selected].admin ? "Remove Admin" : "Make Admin", false);
    hit_delete = (struct rect){x + 360 - x, by - y, 130, 28}; appui_px_button(x + 360, by, 130, 28, "Delete User", false);
    hit_add = (struct rect){x + 500 - x, by - y, 120, 28}; appui_px_button(x + 500, by, 120, 28, "Add User...", true);
    int oy = by + 48;
    appui_px_text(x + 20, oy + 4, "Automatic login", APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + 250, oy, conf.autologin); hit_autologin = (struct rect){250, oy - y, 52, 28};
    oy += 40; appui_px_text(x + 20, oy + 6, "Lock the screen after", APPUI_F_BODY, APPUI_TEXT);
    static const char* const after[4] = {"Never", "1 min", "5 min", "15 min"}; int current = 0; for (int i = 0; i < 4; i++) if (LOCK_AFTER[i] == conf.lock_after) current = i;
    appui_px_segment(x + 250, oy, 320, 28, after, 4, current); hit_lock_after = (struct rect){250, oy - y, 320, 28};
    oy += 40; appui_px_text(x + 20, oy + 4, "Lock before sleep", APPUI_F_BODY, APPUI_TEXT); appui_px_toggle(x + 250, oy, conf.lock_on_sleep); hit_sleep = (struct rect){250, oy - y, 52, 28};
    memset(hit_fields, 0, sizeof(hit_fields)); memset(&hit_ok, 0, sizeof(hit_ok)); memset(&hit_cancel, 0, sizeof(hit_cancel)); memset(&hit_form_admin, 0, sizeof(hit_form_admin));
    if (form != FORM_NONE) {   /* the open form, below the options */
        int fy = oy + 48; static const char* const password_labels[3] = {"Current password", "New password", "Confirm"};
        static const char* const add_labels[4] = {"Full name", "Account name", "Password", "Your password"};
        int fields = form_fields(); struct aurora_user* u = &users[selected];
        char title[96]; snprintf(title, sizeof(title), "%s", form == FORM_PASSWORD ? "Change password" : form == FORM_ADD ? "New account" : "");
        if (form == FORM_ADMIN) snprintf(title, sizeof(title), "%s %s as administrator", u->admin ? "Remove" : "Make", u->name);
        if (form == FORM_DELETE) snprintf(title, sizeof(title), "Delete %s (the home folder is kept)", u->name);
        appui_px_card(x + 20, fy, w - 40, 30 + fields * 36 + (form == FORM_ADD ? 36 : 0) + 40, 10, APPUI_PANEL);
        appui_px_text(x + 34, fy + 8, title, APPUI_F_BOLD, APPUI_TEXT); fy += 32;
        for (int i = 0; i < fields; i++) {
            const char* label = form == FORM_PASSWORD ? password_labels[i] : form == FORM_ADD ? add_labels[i] : "Your password";
            bool secret = form != FORM_ADD || i >= 2; char shown[64]; size_t n = strlen(inputs[i]);
            if (secret) { memset(shown, '*', n < sizeof(shown) - 1 ? n : sizeof(shown) - 1); shown[n < sizeof(shown) - 1 ? n : sizeof(shown) - 1] = 0; } else snprintf(shown, sizeof(shown), "%s", inputs[i]);
            appui_px_text(x + 34, fy + 6, label, APPUI_F_BODY, APPUI_SUBTLE); if (secret) appui_px_secret_field(x + 200, fy, 260, 28, shown, i == field); else appui_px_field(x + 200, fy, 260, 28, shown, i == field);
            hit_fields[i] = (struct rect){200, fy - y, 260, 28}; fy += 36;
        }
        if (form == FORM_ADD) { appui_px_text(x + 34, fy + 4, "Administrator", APPUI_F_BODY, APPUI_SUBTLE); appui_px_toggle(x + 200, fy, add_admin); hit_form_admin = (struct rect){200, fy - y, 52, 28}; fy += 36; }
        appui_px_button(x + 200, fy, 100, 28, "Cancel", false); hit_cancel = (struct rect){200, fy - y, 100, 28};
        appui_px_button(x + 310, fy, 100, 28, form == FORM_PASSWORD ? "Change" : form == FORM_ADD ? "Create" : form == FORM_DELETE ? "Delete" : "Apply", true); hit_ok = (struct rect){310, fy - y, 100, 28};
    }
    static char reported[600]; char controls[600];   /* window-local control rectangles (tests drive the page by them) */
    snprintf(controls, sizeof(controls), "[64] SETTINGS USERS CONTROLS password=%d,%d add=%d,%d autologin=%d,%d lock_after=%d,%d,%d sleep=%d,%d field0=%d,%d field1=%d,%d field2=%d,%d field3=%d,%d ok=%d,%d\n",
             page_x + hit_password.x + 85, page_y + hit_password.y + 14, page_x + hit_add.x + 60, page_y + hit_add.y + 14, page_x + hit_autologin.x + 26, page_y + hit_autologin.y + 14,
             page_x + hit_lock_after.x, page_y + hit_lock_after.y + 14, hit_lock_after.w, page_x + hit_sleep.x + 26, page_y + hit_sleep.y + 14,
             page_x + hit_fields[0].x + 130, page_y + hit_fields[0].y + 14, page_x + hit_fields[1].x + 130, page_y + hit_fields[1].y + 14,
             page_x + hit_fields[2].x + 130, page_y + hit_fields[2].y + 14, page_x + hit_fields[3].x + 130, page_y + hit_fields[3].y + 14, page_x + hit_ok.x + 50, page_y + hit_ok.y + 14);
    if (strcmp(controls, reported)) { strcpy(reported, controls); fputs(controls, stdout); fflush(stdout); }
}
static void click(int x, int y) {
    for (int i = 0; i < count; i++) if (inside(x, y, hit_users[i])) { selected = i; form = FORM_NONE; clear_inputs(); settings_redraw(); return; }
    if (form != FORM_NONE) {
        for (int i = 0; i < 4; i++) if (hit_fields[i].w && inside(x, y, hit_fields[i])) { field = i; settings_redraw(); return; }
        if (inside(x, y, hit_form_admin)) { add_admin = !add_admin; settings_redraw(); return; }
        if (inside(x, y, hit_cancel)) { form = FORM_NONE; clear_inputs(); settings_redraw(); return; }
        if (inside(x, y, hit_ok)) { submit(); return; }
    }
    if (inside(x, y, hit_password)) { form = FORM_PASSWORD; clear_inputs(); if (count && users[selected].password == AURORA_PW_NONE) field = 1; settings_redraw(); return; }
    if (inside(x, y, hit_add)) { form = FORM_ADD; clear_inputs(); add_admin = false; settings_redraw(); return; }
    if (inside(x, y, hit_admin) && count) { form = FORM_ADMIN; clear_inputs(); settings_redraw(); return; }
    if (inside(x, y, hit_delete) && count) { form = FORM_DELETE; clear_inputs(); settings_redraw(); return; }
    if (inside(x, y, hit_autologin)) { conf.autologin = !conf.autologin; save_conf(); settings_redraw(); return; }
    if (inside(x, y, hit_sleep)) { conf.lock_on_sleep = !conf.lock_on_sleep; save_conf(); settings_redraw(); return; }
    if (inside(x, y, hit_lock_after)) { int i = (x - hit_lock_after.x) * 4 / hit_lock_after.w; conf.lock_after = LOCK_AFTER[i < 0 ? 0 : i > 3 ? 3 : i]; save_conf(); settings_redraw(); return; }
}
static void key(char k) {
    if (form == FORM_NONE) return;
    int fields = form_fields();
    if (!fields) { if (k == '\n') submit(); else if (k == 27) { form = FORM_NONE; clear_inputs(); } settings_redraw(); return; }
    if (k == '\t') { field = (field + 1) % fields; settings_redraw(); return; }
    if (k == '\n') { if (field + 1 < fields) field++; else submit(); settings_redraw(); return; }
    if (k == 27) { form = FORM_NONE; clear_inputs(); settings_redraw(); return; }
    char* text = inputs[field]; size_t n = strlen(text);
    if (k == '\b') { if (n) text[n - 1] = 0; settings_redraw(); return; }
    if ((unsigned char)k >= 32 && (unsigned char)k < 127 && n < sizeof(inputs[0]) - 1) { text[n] = k; text[n + 1] = 0; settings_redraw(); }
}
const struct settings_page settings_users = {"users", "Users", ICON_CONTACTS, enter, draw, click, NULL, key, tick};

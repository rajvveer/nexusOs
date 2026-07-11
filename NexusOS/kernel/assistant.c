/* ============================================================================
 * NexusOS — AI Assistant — Phase 46
 * ============================================================================
 * Offline, rule-based assistant. See assistant.h for the four pillars. No LLM,
 * no libc, no 64-bit math, no float — everything is integer keyword scoring
 * over small static tables. Text helpers (lowercase, tokenize, classify) are
 * implemented locally since the kernel's string.c has none.
 * ============================================================================ */

#include "assistant.h"
#include "shell.h"
#include "vfs.h"
#include "vga.h"
#include "string.h"

/* ==========================================================================
 * Local text helpers (no libc)
 * ========================================================================== */
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static bool is_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
/* case-insensitive substring test */
static bool contains_ci(const char* hay, const char* needle) {
    if (!needle[0]) return true;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && lc(hay[i + j]) == lc(needle[j])) j++;
        if (!needle[j]) return true;
    }
    return false;
}
/* case-insensitive whole-string compare */
static bool eq_ci(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (lc(a[i]) != lc(b[i])) return false; i++; }
    return a[i] == b[i];
}
/* subsequence match: do needle's chars appear in order within hay? (fuzzy) */
static bool subseq_ci(const char* hay, const char* needle) {
    int j = 0;
    for (int i = 0; hay[i] && needle[j]; i++)
        if (lc(hay[i]) == lc(needle[j])) j++;
    return needle[j] == '\0';
}

/* Tokenize `s` into lowercased alnum words. Returns word count; fills
 * words[][] (each <= TOK_MAX-1 chars). Stop at MAX_TOK words. */
#define MAX_TOK   24
#define TOK_MAX   24
static int tokenize(const char* s, char words[MAX_TOK][TOK_MAX]) {
    int n = 0, k = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (is_alnum(c)) {
            if (k < TOK_MAX - 1) words[n][k++] = lc(c);
        } else {
            if (k > 0) { words[n][k] = '\0'; n++; k = 0; if (n >= MAX_TOK) break; }
            if (c == '\0') break;
        }
    }
    return n;
}

/* Filler / stop words ignored when matching intents and extracting args. */
static const char* STOPWORDS[] = {
    "the","a","an","to","my","me","i","please","can","you","could","would",
    "want","need","some","is","it","my","for","of","on","off","do","up",
    "this","that","with","and","get","go","let","us","how","what","s",0
};
static bool is_stop(const char* w) {
    for (int i = 0; STOPWORDS[i]; i++) if (eq_ci(w, STOPWORDS[i])) return true;
    return false;
}

/* ==========================================================================
 * Command knowledge base (the machine-readable command list the shell lacks)
 * ========================================================================== */
typedef struct {
    const char* name;
    const char* usage;
    const char* desc;
    const char* group;
} kb_entry_t;

static const kb_entry_t KB[] = {
    {"help",     "help",            "List all shell commands by category",          "System"},
    {"clear",    "clear",           "Clear the screen",                             "System"},
    {"date",     "date",            "Show the current date and time",               "System"},
    {"uptime",   "uptime",          "Show how long the system has been running",    "System"},
    {"meminfo",  "meminfo",         "Show free / total memory",                     "System"},
    {"reboot",   "reboot",          "Restart the computer",                         "System"},
    {"ls",       "ls",              "List files in the filesystem",                 "Files"},
    {"cat",      "cat <file>",      "Print a file's contents",                      "Files"},
    {"touch",    "touch <file>",    "Create a new empty file",                      "Files"},
    {"write",    "write <file> <text>","Write text into a file",                    "Files"},
    {"rm",       "rm <file>",       "Delete a file (destructive)",                  "Files"},
    {"edit",     "edit <file>",     "Open a file in the text editor",               "Files"},
    {"find",     "find <query>",    "Smart search of file names and contents",      "Files"},
    {"ps",       "ps",              "List running processes",                       "Processes"},
    {"kill",     "kill <pid>",      "Stop a process by id (destructive)",           "Processes"},
    {"ifconfig", "ifconfig",        "Show network interface / IP address",          "Network"},
    {"ping",     "ping <ip>",       "Test reachability of a host",                  "Network"},
    {"netstat",  "netstat",         "Show network connections",                     "Network"},
    {"dns",      "dns <host>",      "Resolve a hostname to an IP",                  "Network"},
    {"wget",     "wget <url>",      "Download a file over HTTP",                     "Network"},
    {"browse",   "browse <url>",    "Open a page in the text web browser",          "Network"},
    {"vnc",      "vnc",             "Start the VNC remote-desktop server",          "Cloud"},
    {"sync",     "sync serve",      "Start / use the cloud file-sync service",      "Cloud"},
    {"clipsync", "clipsync",        "Show or set the shared clipboard",             "Cloud"},
    {"gui",      "gui",             "Switch to the graphical desktop",              "Desktop"},
    {"theme",    "theme <name>",    "Change the color theme",                       "Desktop"},
    {"calc",     "calc",            "Open the calculator",                          "Desktop"},
    {"files",    "files",           "Open the file manager",                        "Desktop"},
    {"sysmon",   "sysmon",          "Open the system monitor",                      "Desktop"},
    {"settings", "settings",        "Open system settings",                         "Desktop"},
    {"notepad",  "notepad",         "Open the notepad editor",                      "Apps"},
    {"calendar", "calendar",        "Open the calendar",                            "Apps"},
    {"music",    "music",           "Open the music player",                        "Apps"},
    {"paint",    "paint",           "Open the paint app",                           "Apps"},
    {"snake",    "snake",           "Play Snake",                                   "Games"},
    {"tetris",   "tetris",          "Play Tetris",                                  "Games"},
    {"pong",     "pong",            "Play Pong",                                    "Games"},
    {"doom",     "doom",            "Play NEXUSDOOM",                               "Games"},
    {"breakout", "breakout",        "Play Breakout",                                "Games"},
    {"minesweeper","minesweeper",   "Play Minesweeper",                             "Games"},
    {"play",     "play <file.wav>", "Play a WAV sound file",                        "Sound"},
    {"volume",   "volume <0-100>",  "Set the audio volume",                         "Sound"},
    {"tone",     "tone <hz> <ms>",  "Play a tone",                                  "Sound"},
    {"view",     "view <file>",     "View an image (BMP/PNG/JPEG/GIF)",             "Images"},
    {"fontsize", "fontsize <1-4>",  "Set the UI text scale (accessibility)",        "Access"},
    {"contrast", "contrast",        "Toggle the high-contrast theme",               "Access"},
    {"reader",   "reader <on|off>", "Toggle the screen reader",                     "Access"},
    {"say",      "say <text>",      "Speak text via the PC speaker",                "Access"},
    {"whoami",   "whoami",          "Show the current user",                        "Security"},
    {"id",       "id",              "Show user / group ids",                        "Security"},
    {"users",    "users",           "List user accounts",                           "Security"},
    {"passwd",   "passwd",          "Change a password",                            "Security"},
    {"chmod",    "chmod <mode> <f>","Change file permissions (destructive)",        "Security"},
    {"firewall", "firewall list",   "Manage the packet-filter firewall",            "Security"},
    {"synccfg",  "synccfg save",    "Save / load / show settings",                  "Cloud"},
    {"ai",       "ai [topic]",      "This assistant: explain a command or topic",   "AI"},
    {"ask",      "ask <text>",      "Tell the assistant what you want in English",  "AI"},
};
#define KB_COUNT ((int)(sizeof(KB) / sizeof(KB[0])))

static const kb_entry_t* kb_find(const char* name) {
    for (int i = 0; i < KB_COUNT; i++) if (eq_ci(KB[i].name, name)) return &KB[i];
    return 0;
}

/* ==========================================================================
 * Intent table: natural language -> command template
 *   kw   = space-separated trigger words (any hit scores; more hits = better)
 *   cmd  = command, with "%s" where an extracted argument goes (else "")
 *   arg  = how to fill %s: "" none, "file" a filename-ish token, "num" a
 *          number, "url" a url/host, "rest" the remaining meaningful words
 *   dgr  = destructive (never auto-run on a weak match)
 * ========================================================================== */
typedef struct {
    const char* kw;
    const char* cmd;
    const char* arg;
    bool        dgr;
} intent_t;

static const intent_t INTENTS[] = {
    /* files */
    {"list show files file directory folder documents dir contents",      "ls",            "",    false},
    {"read show open view display print contents file what inside",       "cat %s",        "file",false},
    {"create make new file touch empty called named",                     "touch %s",      "file",false},
    {"write put text save into file",                                     "write %s",      "rest",false},
    {"delete remove erase trash destroy file get rid",                    "rm %s",         "file",true},
    {"edit open editor modify change file text",                          "edit %s",       "file",false},
    {"find search look locate file files for where",                      "find %s",       "rest",false},
    /* system */
    {"time date day today clock what",                                    "date",          "",    false},
    {"uptime long running been on awake",                                 "uptime",        "",    false},
    {"memory ram free much available used",                              "meminfo",       "",    false},
    {"clear clean wipe screen",                                          "clear",         "",    false},
    {"restart reboot reset computer system",                            "reboot",        "",    true},
    {"help commands what can do list available",                        "help",          "",    false},
    /* processes */
    {"processes running tasks list jobs programs",                       "ps",            "",    false},
    {"kill stop terminate end process task",                            "kill %s",       "num", true},
    /* network */
    {"network ip address interface connection ifconfig connected",      "ifconfig",      "",    false},
    {"ping reach test connect host server",                             "ping %s",       "url", false},
    {"connections sockets netstat ports",                              "netstat",       "",    false},
    {"resolve dns lookup hostname domain name address",                "dns %s",        "url", false},
    {"download fetch get wget url web internet file",                   "wget %s",       "url", false},
    {"browse web page website internet open url",                      "browse %s",     "url", false},
    {"internet online connect web",                                     "ifconfig",      "",    false},
    /* desktop / apps */
    {"desktop gui graphical interface window",                         "gui",           "",    false},
    {"theme color scheme dark light ocean retro hicon mode appearance","theme %s",      "theme",false},
    {"calculator calc math compute add",                              "calc",          "",    false},
    {"file manager browser explorer finder",                          "files",         "",    false},
    {"system monitor performance cpu sysmon",                         "sysmon",        "",    false},
    {"settings preferences config options",                           "settings",      "",    false},
    {"notepad note write document text",                              "notepad",       "",    false},
    {"calendar date schedule month",                                  "calendar",      "",    false},
    {"music song audio player tunes",                                 "music",         "",    false},
    {"paint draw drawing picture art",                                "paint",         "",    false},
    /* games */
    {"game play fun bored",                                            "doom",          "",    false},
    {"snake game",                                                     "snake",         "",    false},
    {"tetris blocks game",                                             "tetris",        "",    false},
    {"pong game ball paddle",                                          "pong",          "",    false},
    {"doom shooter fps demons game",                                   "doom",          "",    false},
    {"breakout bricks game",                                           "breakout",      "",    false},
    {"minesweeper mines game",                                         "minesweeper",   "",    false},
    /* sound */
    {"play sound wav audio music file",                               "play %s",       "file",false},
    {"volume loud quiet sound level set",                             "volume %s",     "num", false},
    {"beep tone sound frequency",                                      "tone 440 300",  "",    false},
    /* images */
    {"view show image picture photo open display",                    "view %s",       "file",false},
    /* accessibility */
    {"font text bigger larger smaller size scale zoom",               "fontsize %s",   "num", false},
    {"contrast high accessibility visibility",                        "contrast",      "",    false},
    {"reader screen speak voice accessibility read",                  "reader on",     "",    false},
    {"say speak voice talk announce",                                 "say %s",        "rest",false},
    /* security */
    {"who am whoami user current logged",                             "whoami",        "",    false},
    {"users accounts list people",                                    "users",         "",    false},
    {"password passwd change credentials",                            "passwd",        "",    false},
    {"firewall block allow packet filter security",                  "firewall list", "",    false},
    /* cloud */
    {"vnc remote desktop screen share control",                      "vnc",           "",    false},
    {"sync cloud share files server",                                "sync serve",    "",    false},
    {"clipboard clip copy paste shared",                             "clipsync",      "",    false},
};
#define INTENT_COUNT ((int)(sizeof(INTENTS) / sizeof(INTENTS[0])))

/* Score word w against the space-separated keyword list kw:
 *   exact token match            -> 10
 *   w starts-with a keyword>=4   ->  6  (stem, e.g. "documents" ~ "document")
 *   keyword starts-with w, w>=4  ->  6  (stem the other way)
 *   no match                     ->  0
 * Returns the best single match (no double-count). The first keyword token is
 * the canonical verb; `first_is_verb` is set if w equals it. */
static int kw_score(const char* kw, const char* w, bool* first_is_verb) {
    int best = 0, idx = 0;
    int i = 0;
    while (kw[i]) {
        /* extract token [i..te) */
        int te = i; while (kw[te] && kw[te] != ' ') te++;
        int klen = te - i;
        /* exact? */
        int j = 0;
        while (j < klen && w[j] && lc(kw[i + j]) == lc(w[j])) j++;
        bool exact = (j == klen && w[j] == '\0');
        if (exact) {
            best = 10;
            if (idx == 0 && first_is_verb) *first_is_verb = true;
            return best; /* exact is max, stop */
        }
        /* stem: w starts with keyword (keyword len >=4) */
        if (klen >= 4) {
            int p = 0; while (p < klen && w[p] && lc(kw[i + p]) == lc(w[p])) p++;
            if (p == klen) { if (best < 6) best = 6; }
        }
        /* stem: keyword starts with w (w len >=4) */
        int wl = 0; while (w[wl]) wl++;
        if (wl >= 4 && klen >= wl) {
            int p = 0; while (p < wl && lc(kw[i + p]) == lc(w[p])) p++;
            if (p == wl) { if (best < 6) best = 6; }
        }
        i = te; while (kw[i] == ' ') i++; idx++;
    }
    return best;
}

/* ==========================================================================
 * Argument extraction
 * ========================================================================== */
static bool looks_num(const char* w)  { for (int i=0; w[i]; i++) if (w[i]<'0'||w[i]>'9') return false; return w[0]!=0; }
static bool looks_url(const char* w)  { return contains_ci(w, ".") || contains_ci(w, "http") || contains_ci(w, "/"); }

/* Pull the original (case-preserving) word/argument out of `query` for the
 * given arg-type. Returns true and fills out[] if found. */
static bool extract_arg(const char* query, const char* arg_type, char* out, int out_max) {
    if (!arg_type[0]) { out[0] = '\0'; return false; }

    /* split original query into raw (case-preserving) words */
    char raw[MAX_TOK][TOK_MAX];
    int nraw = 0, k = 0;
    for (int i = 0; ; i++) {
        char c = query[i];
        /* keep dots/slashes/dashes for url/filenames */
        if (is_alnum(c) || c=='.' || c=='/' || c=='-' || c=='_' || c==':') {
            if (k < TOK_MAX - 1) raw[nraw][k++] = c;
        } else {
            if (k > 0) { raw[nraw][k] = '\0'; nraw++; k = 0; if (nraw >= MAX_TOK) break; }
            if (c == '\0') break;
        }
    }

    /* "called/named X" or "to X" pattern wins for files */
    for (int i = 0; i < nraw - 1; i++) {
        if (eq_ci(raw[i],"called") || eq_ci(raw[i],"named")) {
            strncpy(out, raw[i+1], out_max-1); out[out_max-1]='\0'; return true;
        }
    }

    if (eq_ci(arg_type, "theme")) {
        static const char* THEMES[] = {"dark","light","retro","ocean","hicon",0};
        for (int i = 0; i < nraw; i++)
            for (int t = 0; THEMES[t]; t++)
                if (eq_ci(raw[i], THEMES[t])) { strcpy(out, THEMES[t]); return true; }
        /* "dark mode" / "light mode" phrasing already covered by token match */
        return false;
    }
    if (eq_ci(arg_type, "num")) {
        for (int i = 0; i < nraw; i++) if (looks_num(raw[i])) { strncpy(out,raw[i],out_max-1); out[out_max-1]='\0'; return true; }
        /* fuzzy size words */
        for (int i=0;i<nraw;i++){ if(eq_ci(raw[i],"bigger")||eq_ci(raw[i],"larger")||eq_ci(raw[i],"up")){strcpy(out,"3");return true;}
                                  if(eq_ci(raw[i],"smaller")||eq_ci(raw[i],"down")){strcpy(out,"1");return true;}
                                  if(eq_ci(raw[i],"max")||eq_ci(raw[i],"loud")){strcpy(out,"100");return true;}
                                  if(eq_ci(raw[i],"mute")||eq_ci(raw[i],"quiet")){strcpy(out,"10");return true;} }
        return false;
    }
    if (eq_ci(arg_type, "url")) {
        for (int i = 0; i < nraw; i++) if (looks_url(raw[i])) { strncpy(out,raw[i],out_max-1); out[out_max-1]='\0'; return true; }
        return false;
    }
    if (eq_ci(arg_type, "file")) {
        /* prefer a token that looks like a filename (has a dot), else last
         * non-stop word */
        for (int i = nraw-1; i >= 0; i--) if (contains_ci(raw[i],".")) { strncpy(out,raw[i],out_max-1); out[out_max-1]='\0'; return true; }
        for (int i = nraw-1; i >= 0; i--) if (!is_stop(raw[i])) {
            char lw[TOK_MAX]; int j; for(j=0;raw[i][j];j++) lw[j]=lc(raw[i][j]); lw[j]='\0';
            /* skip if it's itself an intent keyword like "file"/"a" */
            if (eq_ci(lw,"file")||eq_ci(lw,"files")) continue;
            strncpy(out,raw[i],out_max-1); out[out_max-1]='\0'; return true;
        }
        return false;
    }
    if (eq_ci(arg_type, "rest")) {
        /* join all non-stop words that aren't the obvious trigger verbs */
        out[0]='\0'; int len=0;
        for (int i = 0; i < nraw; i++) {
            if (is_stop(raw[i])) continue;
            int wl = strlen(raw[i]);
            if (len + wl + 1 >= out_max) break;
            if (len) out[len++]=' ';
            strcpy(out+len, raw[i]); len += wl;
        }
        return out[0] != '\0';
    }
    return false;
}

/* ==========================================================================
 * Pillar 1: natural language -> command
 * ========================================================================== */
void assistant_ask(const char* query, bool run) {
    if (!query || !query[0]) {
        vga_print_color("  Ask me in plain English, e.g. ask \"show the files\"\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return;
    }

    char words[MAX_TOK][TOK_MAX];
    int nw = tokenize(query, words);

    /* score every intent: sum the best per-token keyword score, plus a
     * verb-at-front bonus and a multi-keyword bonus. */
    /* canonical command name for each intent (first word of the template) */
    int best = -1, best_score = 0, second = 0;
    for (int t = 0; t < INTENT_COUNT; t++) {
        char canon[32]; int z = 0;
        for (; INTENTS[t].cmd[z] && INTENTS[t].cmd[z] != ' ' && z < 31; z++) canon[z] = INTENTS[t].cmd[z];
        canon[z] = '\0';

        int score = 0, distinct = 0;
        bool verb_front = false;
        for (int w = 0; w < nw; w++) {
            if (is_stop(words[w])) continue;
            bool fv = false;
            int s = kw_score(INTENTS[t].kw, words[w], &fv);
            /* a token that IS the command name (e.g. user literally said "ls",
             * "doom", "calc") is a strong, unambiguous signal */
            if (eq_ci(words[w], canon)) { s = 12; fv = true; }
            if (s > 0) { score += s; distinct++; if (fv) verb_front = true; }
        }
        if (verb_front) score += 4;
        if (distinct >= 2) score += 5;
        if (score > best_score) { second = best_score; best_score = score; best = t; }
        else if (score > second) second = score;
    }

    vga_print_color("\n  Assistant: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("\""); vga_print(query); vga_print("\"\n");

    if (best < 0 || best_score == 0) {
        vga_print_color("  I'm not sure what you mean. Try 'ai' for what I can do,\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("  or 'help' for the full command list.\n\n");
        return;
    }

    /* build the resolved command, substituting an argument if the template
     * has %s */
    const intent_t* in = &INTENTS[best];
    char cmd[160];
    const char* tmpl = in->cmd;
    char arg[96]; arg[0] = '\0';
    bool need_arg = false;
    for (int i = 0; tmpl[i]; i++) if (tmpl[i]=='%' && tmpl[i+1]=='s') need_arg = true;

    bool have_arg = false;
    if (need_arg) have_arg = extract_arg(query, in->arg, arg, sizeof(arg));

    /* Clamp numeric args to each command's legal range. cmd_fontsize REJECTS
     * out-of-range (it doesn't clamp), so an unclamped value emits a no-op;
     * volume clamps internally but we belt-and-suspenders it too. */
    if (have_arg && eq_ci(in->arg, "num")) {
        int v = 0; for (int i = 0; arg[i] >= '0' && arg[i] <= '9'; i++) v = v*10 + (arg[i]-'0');
        if (contains_ci(in->cmd, "fontsize")) { if (v < 1) v = 1; if (v > 4) v = 4; }
        else if (contains_ci(in->cmd, "volume")) { if (v < 0) v = 0; if (v > 100) v = 100; }
        int_to_str(v, arg);
    }

    /* substitute */
    {
        int o = 0;
        for (int i = 0; tmpl[i] && o < (int)sizeof(cmd)-1; i++) {
            if (tmpl[i]=='%' && tmpl[i+1]=='s') {
                for (int j = 0; arg[j] && o < (int)sizeof(cmd)-1; j++) cmd[o++] = arg[j];
                i++;
            } else cmd[o++] = tmpl[i];
        }
        cmd[o] = '\0';
        /* trim a trailing space if arg was empty */
        while (o>0 && cmd[o-1]==' ') cmd[--o]='\0';
    }

    /* confidence gates (integer, per design):
     *   RUN requires best >= 12 AND (best - second) >= 4
     *   destructive needs best >= 20 AND margin >= 6 AND a real arg. */
    #define RUN_MIN 12
    #define RUN_MARGIN 4
    #define DESTRUCT_MIN 20
    #define DESTRUCT_MARGIN 6
    int margin = best_score - second;

    /* missing required argument -> show the stub, run nothing */
    if (need_arg && !have_arg) {
        vga_print("  Looks like: ");
        vga_print_color(in->cmd, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print_color("   (tell me the argument)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        char nm[32]; int z=0; for(;in->cmd[z]&&in->cmd[z]!=' '&&z<31;z++)nm[z]=in->cmd[z]; nm[z]='\0';
        const kb_entry_t* k = kb_find(nm);
        if (k) { vga_print("  Usage: "); vga_print(k->usage); vga_print("\n"); }
        vga_print("\n");
        return;
    }

    vga_print("  -> ");
    vga_print_color(cmd, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("\n");

    /* too weak to act on */
    if (best_score < RUN_MIN) {
        vga_print_color("  (not confident — type the command yourself if that's right,\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("   or try 'ai' for what I can do)\n\n");
        return;
    }
    /* ambiguous: a close runner-up exists */
    if (margin < RUN_MARGIN) {
        vga_print_color("  (ambiguous — not running; rephrase or run it yourself)\n\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return;
    }

    /* destructive guard: never auto-run without strong signal + a real arg */
    if (in->dgr) {
        bool strong = (best_score >= DESTRUCT_MIN && margin >= DESTRUCT_MARGIN && have_arg);
        if (!run || !strong) {
            vga_print_color("  That command is destructive — run it yourself if it's right:\n",
                            VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            vga_print("    "); vga_print(cmd); vga_print("\n\n");
            return;
        }
    }

    if (!run) { vga_print("  (explain only — not executed)\n\n"); return; }
    vga_print("\n");
    shell_exec_line(cmd);
}

/* ==========================================================================
 * Pillar 2: help / knowledge base
 * ========================================================================== */
static void kb_overview(void) {
    vga_print_color("\n  NexusOS AI Assistant\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ====================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  An offline, rule-based helper (no internet model).\n\n");
    vga_print_color("  ask ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("\"<plain English>\"   - I pick & run the matching command\n");
    vga_print_color("  ai <command>", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("            - explain a command\n");
    vga_print_color("  find <query>", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("            - smart search of files (names + contents)\n\n");
    vga_print_color("  Try: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("ask \"what time is it\"   ask \"show the files\"\n");
    vga_print("       ask \"how much memory is free\"   ai ping   find welcome\n\n");
}

void assistant_help(const char* topic) {
    if (!topic || !topic[0]) { kb_overview(); return; }

    /* exact command? */
    const kb_entry_t* k = kb_find(topic);
    if (!k) {
        /* maybe the topic has args, take first word */
        char first[32]; int i=0; for(;topic[i]&&topic[i]!=' '&&i<31;i++) first[i]=topic[i]; first[i]='\0';
        k = kb_find(first);
    }
    if (k) {
        vga_print_color("\n  ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print_color(k->name, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print_color("  [", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print(k->group); vga_print_color("]\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("  "); vga_print(k->desc); vga_print("\n");
        vga_print_color("  Usage: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print(k->usage); vga_print("\n");
        /* related: same group */
        vga_print_color("  Related: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        int shown = 0;
        for (int j = 0; j < KB_COUNT && shown < 6; j++)
            if (j < KB_COUNT && eq_ci(KB[j].group, k->group) && !eq_ci(KB[j].name, k->name)) {
                vga_print(KB[j].name); vga_print(" "); shown++;
            }
        vga_print("\n\n");
        return;
    }

    /* free text: find the KB entry whose desc/name best matches the words */
    char words[MAX_TOK][TOK_MAX];
    int nw = tokenize(topic, words);
    int best = -1, bs = 0;
    for (int e = 0; e < KB_COUNT; e++) {
        int s = 0;
        for (int w = 0; w < nw; w++) {
            if (is_stop(words[w])) continue;
            if (contains_ci(KB[e].name, words[w])) s += 3;
            if (contains_ci(KB[e].desc, words[w])) s += 1;
        }
        if (s > bs) { bs = s; best = e; }
    }
    if (best >= 0 && bs > 0) {
        vga_print_color("\n  Closest match: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        assistant_help(KB[best].name);
        return;
    }
    vga_print_color("\n  No matching command. Type 'ai' for an overview.\n\n",
                    VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* ==========================================================================
 * Pillar 3: smart file search
 * ========================================================================== */
void assistant_find(const char* query) {
    if (!query || !query[0]) {
        vga_print_color("  Usage: find <query>   (searches file names and contents)\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        return;
    }
    fs_node_t* root = vfs_get_root();
    if (!root) return;

    vga_print_color("\n  Search results for \"", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print(query);
    vga_print_color("\":\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));

    int hits = 0;
    static uint8_t buf[4096];   /* RAMFS_MAX_FILE_SIZE; static to avoid big stack */
    for (uint32_t i = 0; ; i++) {
        fs_node_t* f = vfs_readdir(root, i);
        if (!f) break;
        if (f->type != FS_FILE) continue;

        bool name_hit = contains_ci(f->name, query) || subseq_ci(f->name, query);

        /* content grep */
        bool content_hit = false;
        char snippet[60]; snippet[0] = '\0';
        uint32_t sz = f->size; if (sz > sizeof(buf)-1) sz = sizeof(buf)-1;
        if (sz > 0) {
            int32_t r = vfs_read(f, 0, sz, buf);
            if (r > 0) {
                buf[r] = '\0';
                if (contains_ci((const char*)buf, query)) {
                    content_hit = true;
                    /* grab the line around the first match for a snippet */
                    int qpos = -1;
                    for (int p = 0; buf[p]; p++) {
                        int j = 0; while (query[j] && lc(buf[p+j])==lc(query[j])) j++;
                        if (!query[j]) { qpos = p; break; }
                    }
                    if (qpos >= 0) {
                        int s = qpos; while (s > 0 && buf[s-1] != '\n' && qpos - s < 20) s--;
                        int o = 0;
                        for (int p = s; buf[p] && buf[p] != '\n' && o < (int)sizeof(snippet)-1; p++)
                            snippet[o++] = (buf[p] >= 32 && buf[p] < 127) ? buf[p] : ' ';
                        snippet[o] = '\0';
                    }
                }
            }
        }

        if (name_hit || content_hit) {
            hits++;
            vga_print("    ");
            vga_print_color(f->name, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            if (name_hit)    vga_print_color("  [name]", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            if (content_hit) {
                vga_print_color("  [text] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
                vga_print(snippet);
            }
            vga_print("\n");
        }
    }
    if (!hits) vga_print_color("    (no files matched)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    char n[12]; int_to_str(hits, n);
    vga_print_color("  ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print(n); vga_print(" match(es)\n\n");
}

/* ==========================================================================
 * Pillar 4: editor autocomplete candidate provider
 * ========================================================================== */
static const char* KEYWORDS[] = {
    /* C */
    "int","char","void","return","while","for","if","else","struct","static",
    "const","unsigned","switch","case","break","continue","sizeof","typedef",
    "uint8_t","uint16_t","uint32_t","bool","true","false","include","define",
    /* NexusScript */
    "let","print","elif","end","then","and","or","not","func","do",
    /* common shell-ish words also useful in notes */
    "NexusOS","assistant","function","return","value",
    0
};

int assistant_complete(const char* prefix, int nth, const char* buffer_words,
                       char* out, int out_max) {
    if (!prefix || !prefix[0]) return 0;
    int total = 0;
    int plen = strlen(prefix);

    /* helper: consider one candidate word; if it starts with prefix
     * (case-insensitive) and isn't equal to it, it's a match. We collect by
     * index `nth` (wrap handled by caller via total). */
    #define CONSIDER(word) do { \
        const char* _w = (word); \
        bool pref = true; \
        for (int _i = 0; _i < plen; _i++) if (lc(_w[_i]) != lc(prefix[_i])) { pref = false; break; } \
        if (pref && _w[plen] != '\0') { \
            if (total == nth) { strncpy(out, _w, out_max-1); out[out_max-1]='\0'; } \
            total++; \
        } \
    } while(0)

    for (int i = 0; KEYWORDS[i]; i++) CONSIDER(KEYWORDS[i]);

    /* words harvested from the editor buffer */
    if (buffer_words) {
        char w[TOK_MAX]; int k = 0;
        for (int i = 0; ; i++) {
            char c = buffer_words[i];
            if (is_alnum(c) || c=='_') { if (k < TOK_MAX-1) w[k++]=c; }
            else { if (k>0){ w[k]='\0'; CONSIDER(w); k=0; } if (c=='\0') break; }
        }
    }
    #undef CONSIDER
    return total;
}

/* ==========================================================================
 * Init
 * ========================================================================== */
void assistant_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("AI assistant ready (ask / ai / find)\n");
}

/* ============================================================================
 * NexusOS - Interactive Shell v36.0
 * ============================================================================
 * Phase 51: NPFS journaling filesystem (write-ahead journal + crash recovery,
 * direct+indirect blocks). 142 commands. ROADMAP COMPLETE (51/51).
 * ============================================================================ */

#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "string.h"
#include "memory.h"
#include "heap.h"
#include "vfs.h"
#include "ramfs.h"
#include "process.h"
#include "scheduler.h"
#include "rtc.h"
#include "speaker.h"
#include "audio.h"
#include "ac97.h"
#include "image.h"
#include "video.h"
#include "gpu.h"
#include "sprite.h"
#include "game.h"
#include "doom.h"
#include "breakout.h"
#include "accessibility.h"
#include "font.h"
#include "users.h"
#include "firewall.h"
#include "framebuffer.h"
#include "gfx.h"
#include "editor.h"
#include "snake.h"
#include "desktop.h"
#include "theme.h"
#include "calculator.h"
#include "filemgr.h"
#include "sysmon.h"
#include "settings.h"
#include "notepad.h"
#include "taskmgr.h"
#include "calendar.h"
#include "music.h"
#include "x11.h"
#include "help.h"
#include "paint.h"
#include "minesweeper.h"
#include "recycle.h"
#include "sysinfo.h"
#include "todo.h"
#include "clock.h"
#include "pong.h"
#include "search.h"
#include "tetris.h"
#include "hexview.h"
#include "contacts.h"
#include "colorpick.h"
#include "syslog.h"
#include "clipmgr.h"
#include "appearance.h"
#include "fileops.h"
#include "net.h"
#include "rtl8139.h"
#include "arp.h"
#include "ip.h"
#include "icmp.h"
#include "udp.h"
#include "tcp.h"
#include "socket.h"
#include "dns.h"
#include "http.h"
#include "browser.h"
#include "dhcp.h"
#include "win32.h"
#include "pe.h"
#include "registry.h"
#include "ntp.h"
#include "httpd.h"
#include "rshell.h"
#include "vnc.h"
#include "sync.h"
#include "clipboard.h"
#include "assistant.h"
#include "mobile.h"
#include "perf.h"
#include "appstore.h"
#include "finale.h"
#include "npfs.h"
#include "procfs.h"
#include "termios.h"
#include "posix.h"
#include "dynlink.h"
#include "libc.h"
#include "pe.h"
#include "registry.h"
#include "win32.h"
#include "pkg.h"
#include "script.h"
#include "macho.h"
#include "cocoa.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* Shell constants */
#define INPUT_MAX 256
#define MAX_ARGS  16
#define HISTORY_SIZE 16

/* Command history */
static char history[HISTORY_SIZE][INPUT_MAX];
static int history_count = 0;
static int history_pos = 0;   /* Next write position */

/* --------------------------------------------------------------------------
 * history_add: Store a command in history ring buffer
 * -------------------------------------------------------------------------- */
static void history_add(const char* cmd) {
    if (cmd[0] == '\0') return;
    /* Don't add duplicates of last command */
    int last = (history_pos - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    if (history_count > 0 && strcmp(history[last], cmd) == 0) return;

    strcpy(history[history_pos], cmd);
    history_pos = (history_pos + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) history_count++;
}

/* --------------------------------------------------------------------------
 * shell_readline: Enhanced readline with history (up/down arrows)
 * -------------------------------------------------------------------------- */
/* Replace the current input with `buffer` (length `len`), redrawn from the
 * prompt origin. Backspacing `i` chars one at a time keeps cursor_row/col in
 * sync with any scroll the previous echo caused, so we never strand text on a
 * stale row (the old code reset to vga_get_cursor_row()+start_col after the
 * backspaces, which drifted a whole line whenever the echo had wrapped). */
static void readline_replace(const char* buffer, int len, int* i) {
    while (*i > 0) { vga_backspace(); (*i)--; }
    for (int j = 0; j < len; j++) vga_putchar(buffer[j]);
    *i = len;
}

static int shell_readline(char* buffer, int max_len) {
    int i = 0;
    int hist_idx = history_count;  /* Start at "current" (no history selected) */
    char saved[INPUT_MAX];
    saved[0] = '\0';

    while (i < max_len - 1) {
        vga_flush();
        char c = keyboard_getchar();

        /* Up arrow — prev history */
        if ((unsigned char)c == 0x80) {
            if (hist_idx > 0) {
                /* Save current input on first up press */
                if (hist_idx == history_count) {
                    buffer[i] = '\0';
                    strcpy(saved, buffer);
                }
                hist_idx--;
                int actual = (history_pos - history_count + hist_idx + HISTORY_SIZE) % HISTORY_SIZE;
                strcpy(buffer, history[actual]);
                readline_replace(buffer, strlen(buffer), &i);
            }
            continue;
        }

        /* Down arrow — next history */
        if ((unsigned char)c == 0x81) {
            if (hist_idx < history_count) {
                hist_idx++;
                if (hist_idx == history_count) {
                    strcpy(buffer, saved);
                } else {
                    int actual = (history_pos - history_count + hist_idx + HISTORY_SIZE) % HISTORY_SIZE;
                    strcpy(buffer, history[actual]);
                }
                readline_replace(buffer, strlen(buffer), &i);
            }
            continue;
        }

        /* Enter */
        if (c == '\n') {
            buffer[i] = '\0';
            vga_putchar('\n');
            return i;
        }

        /* Backspace */
        if (c == '\b') {
            if (i > 0) {
                i--;
                vga_backspace();
            }
            continue;
        }

        /* Normal character */
        if (c >= 32 && c < 127) {
            buffer[i++] = c;
            vga_putchar(c);
        }
    }
    buffer[i] = '\0';
    return i;
}

/* --------------------------------------------------------------------------
 * print_prompt: Display shell prompt with PID
 * -------------------------------------------------------------------------- */
static void print_prompt(void) {
    process_t* current = process_get_current();
    char pid_str[8];
    int_to_str(current ? current->pid : 0, pid_str);

    vga_print_color("[", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color(pid_str, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("] ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("NexusOS", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print_color("> ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * parse_args: Split input string into arguments
 * -------------------------------------------------------------------------- */
static int parse_args(char* input, char* argv[]) {
    int argc = 0;
    char* p = input;

    while (*p && argc < MAX_ARGS) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* --------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------- */

static void cmd_help(void) {
    vga_print_color("\n  NexusOS Shell Commands v36.0\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ===========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    vga_print_color("  System:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("help  clear  about  meminfo  heapinfo  uptime  date  reboot\n");
    vga_print_color("  Files:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("ls  cat <f>  touch <f>  write <f> <text>  rm <f>  edit <f>\n");
    vga_print_color("  Processes:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("ps  kill <pid>  run <name>\n");
    vga_print_color("  Network:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("net  ifconfig  ping <ip>  netstat  arp  dns <host>  wget <url>  browse <url>\n");
    vga_print_color("  Desktop:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gui  theme <name>  calc  files  sysmon  settings\n");
    vga_print_color("  Apps:       ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("notepad  taskmgr  calendar  music  paint  help\n");
    vga_print_color("  Games:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("snake  minesweeper  pong  tetris\n");
    vga_print_color("  System:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("sysinfo  trash  restore  todo  search  log\n");
    vga_print_color("  Dev:        ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("hexview  colors  contacts\n");
    vga_print_color("  Fun:        ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("snake  beep  echo <text>  color <0-15>  history\n");
    vga_print_color("  DynLink:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("ldd  ldconfig  dlopen <lib>  dlsym <handle> <sym>\n");
    vga_print_color("  Win32:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("runexe <file>  regedit  win32info\n");
    vga_print_color("  Packages:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("npkg list  npkg install <pkg>  npkg remove <pkg>  npkg info <pkg>\n");
    vga_print_color("  Scripting:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("script <file.ns>   (try: script demo.ns)\n");
    vga_print_color("  macOS:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("machoinfo  runmacho <file>  cocoademo\n");
    vga_print_color("  Sound:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("sndinfo  play <file.wav>  volume <0-100>  tone <hz> <ms>  mixer\n");
    vga_print_color("  Images:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("imginfo  view <file>   (BMP/PNG/JPEG/GIF)\n");
    vga_print_color("  Video:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("vidinfo  mplay [file]   (AVI/MJPEG + audio)\n");
    vga_print_color("  GPU:        ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gpuinfo  gpubench  sprites   (VirtIO-GPU)\n");
    vga_print_color("  Gaming:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gameinfo  gamepad  doom  breakout   (NexusSDL)\n");
    vga_print_color("  Access:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("accinfo  fontsize <1-4>  contrast  reader <on|off>  say <text>\n");
    vga_print_color("  Security:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("id  users  passwd  useradd  userdel  chmod  chown  firewall\n");
    vga_print_color("  Cloud:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("vnc  sync  synccfg  clipsync   (remote desktop + file/settings/clipboard sync)\n");
    vga_print_color("  AI:         ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("ask <english>  ai [cmd]  find <query>   (offline assistant)\n");
    vga_print_color("  Mobile:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gesture  orientation  lowpower  arm  mobileinfo\n");
    vga_print_color("  Perf:       ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("perf  smp  preempt   (benchmarks + boot time + honest SMP status)\n");
    vga_print_color("  App Store:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("store [list|info|install|update|upgrade|sdk]   (ecosystem over npkg)\n");
    vga_print_color("  Finale:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("urun <file>  install [disk yes]  finale   (v5.0: universal bins + installer)\n");
    vga_print_color("  NPFS:       ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("npfs [format|mount|ls|write|cat|rm|stat|journal|crashtest]   (journaling FS)\n\n");

    vga_print_color("  Tip: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("Use Up/Down arrows for command history\n\n");
}

static void cmd_clear(void) {
    vga_clear();
}

static void cmd_about(void) {
    vga_print_color("\n  NexusOS v1.3.0\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  The Hybrid Operating System\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Built from scratch in C and Assembly\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

static void cmd_meminfo(void) {
    char buf[16];
    uint32_t total = pmm_get_total_pages();
    uint32_t used  = pmm_get_used_pages();
    uint32_t free  = pmm_get_free_pages();

    vga_print_color("\n  Memory Info\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("  Total: "); int_to_str(total * 4, buf); vga_print(buf); vga_print(" KB\n");
    vga_print("  Used:  "); int_to_str(used * 4, buf);  vga_print(buf); vga_print(" KB\n");
    vga_print("  Free:  "); int_to_str(free * 4, buf);  vga_print(buf); vga_print(" KB\n\n");
}

static void cmd_heapinfo(void) {
    char buf[16];
    uint32_t used = heap_get_used();
    uint32_t free_mem = heap_get_free();

    vga_print_color("\n  Heap Info\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print("  Total:  "); int_to_str(used + free_mem, buf); vga_print(buf); vga_print(" bytes\n");
    vga_print("  Used:   "); int_to_str(used, buf);  vga_print(buf); vga_print(" bytes\n");
    vga_print("  Free:   "); int_to_str(free_mem, buf);   vga_print(buf); vga_print(" bytes\n\n");
}

static void cmd_uptime(void) {
    uint32_t ticks = system_ticks;
    uint32_t seconds = ticks / 18;
    uint32_t minutes = seconds / 60;
    seconds %= 60;

    char buf[16];
    vga_print("  Uptime: ");
    int_to_str(minutes, buf); vga_print(buf); vga_print("m ");
    int_to_str(seconds, buf); vga_print(buf); vga_print("s (");
    int_to_str(ticks, buf); vga_print(buf); vga_print(" ticks)\n");
}

static void cmd_date(void) {
    rtc_time_t t;
    rtc_read(&t);
    char time_buf[9], date_buf[11];
    rtc_format_time(&t, time_buf);
    rtc_format_date(&t, date_buf);

    vga_print("  ");
    vga_print_color(date_buf, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print(" ");
    vga_print_color(time_buf, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    vga_print("\n");
}

static void cmd_reboot(void) {
    vga_print_color("  Rebooting...\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    /* Triple-fault reboot */
    uint8_t good = 0x02;
    while (good & 0x02)
        __asm__ volatile("inb $0x64, %0" : "=a"(good));
    __asm__ volatile("outb %0, $0x64" : : "a"((uint8_t)0xFE));
}

static void cmd_echo(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) vga_putchar(' ');
        /* Support $VAR expansion */
        if (argv[i][0] == '$') {
            const char* val = env_get(argv[i] + 1);
            if (val) vga_print(val);
            else vga_print(argv[i]);
        } else {
            vga_print(argv[i]);
        }
    }
    vga_putchar('\n');
}

static void cmd_color(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: color <0-15>\n");
        return;
    }
    int c = 0;
    for (int i = 0; argv[1][i]; i++) {
        c = c * 10 + (argv[1][i] - '0');
    }
    if (c >= 0 && c <= 15) {
        vga_set_color(VGA_COLOR(c, VGA_BLACK));
        vga_print("  Color changed.\n");
    }
}

static void cmd_history_show(void) {
    vga_print_color("\n  Command History\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (int i = 0; i < history_count; i++) {
        int idx = (history_pos - history_count + i + HISTORY_SIZE) % HISTORY_SIZE;
        char num[8];
        int_to_str(i + 1, num);
        vga_print("  ");
        vga_print_color(num, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print(") ");
        vga_print(history[idx]);
        vga_print("\n");
    }
    vga_print("\n");
}

/* --- File commands --- */

static void cmd_ls(int argc, char* argv[]) {
    fs_node_t* root = vfs_get_root();
    if (!root) { vga_print("  No filesystem mounted.\n"); return; }

    /* Phase 44: 'ls -l' shows mode/owner/size like Unix. */
    bool long_fmt = (argc >= 2 && strcmp(argv[1], "-l") == 0);

    fs_node_t* entry;
    uint32_t index = 0;
    vga_print_color("\n  Files:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    while ((entry = vfs_readdir(root, index)) != NULL) {
        vga_print("  ");
        if (long_fmt) {
            char modes[12];
            vfs_mode_string(entry, modes);
            vga_print_color(modes, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            vga_print("  ");
            /* owner name, padded to 8 */
            const char* owner = users_name_of(entry->uid);
            vga_print_color(owner, VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            int olen = strlen(owner);
            for (int s = olen; s < 8; s++) vga_print(" ");
            vga_print(" ");
            char sb[16]; int_to_str(entry->size, sb);
            for (int s = strlen(sb); s < 6; s++) vga_print(" ");
            vga_print(sb); vga_print("  ");
        }
        if (entry->type & FS_DIRECTORY) {
            vga_print_color(entry->name, VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK));
            if (!long_fmt) vga_print_color("/", VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK));
        } else {
            vga_print_color(entry->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        }
        if (!long_fmt) {
            char buf[16];
            vga_print("  (");
            int_to_str(entry->size, buf);
            vga_print(buf);
            vga_print(" bytes)");
        }
        vga_print("\n");
        index++;
    }
    vga_print("\n");
}

static void cmd_cat(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: cat [-n] <filename>\n"); return; }
    bool line_nums = false;
    int file_arg = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) { line_nums = true; file_arg = 2; }
    const char* fname = argv[file_arg];

    /* Check /proc paths */
    if (procfs_is_proc(fname)) {
        char pbuf[512];
        int len = procfs_read(fname, pbuf, sizeof(pbuf));
        if (len < 0) {
            vga_print_color("  Not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print(fname); vga_print("\n"); return;
        }
        if (line_nums) {
            int ln = 1; char nb[12];
            char* p = pbuf;
            while (*p) {
                int_to_str(ln, nb); vga_print("  ");
                vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
                vga_print("  ");
                while (*p && *p != '\n') { vga_putchar(*p++); }
                vga_putchar('\n');
                if (*p == '\n') p++;
                ln++;
            }
        } else {
            vga_print("  "); vga_print(pbuf);
        }
        return;
    }

    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, fname);
    if (!node) {
        vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(fname); vga_print("\n"); return;
    }
    char buf[4096];
    uint32_t bytes = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)buf);
    buf[bytes] = '\0';
    if (line_nums) {
        int ln = 1; char nb[12]; char* p = buf;
        while (*p) {
            int_to_str(ln, nb); vga_print("  ");
            vga_print_color(nb, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            vga_print("  ");
            while (*p && *p != '\n') { vga_putchar(*p++); }
            vga_putchar('\n');
            if (*p == '\n') p++;
            ln++;
        }
    } else {
        vga_print("  "); vga_print(buf); vga_print("\n");
    }
}

static void cmd_touch(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: touch <filename>\n"); return; }
    fs_node_t* node = ramfs_create(argv[1], FS_FILE);
    if (node) {
        vga_print_color("  Created: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
    } else {
        vga_print_color("  Failed to create file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
}

static void cmd_write(int argc, char* argv[]) {
    if (argc < 3) { vga_print("  Usage: write <filename> <content>\n"); return; }
    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, argv[1]);
    if (!node) {
        node = ramfs_create(argv[1], FS_FILE);
    }
    if (!node) { vga_print("  Error creating file.\n"); return; }

    /* Concatenate remaining args */
    char content[1024];
    content[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) strcat(content, " ");
        strcat(content, argv[i]);
    }
    vfs_write(node, 0, strlen(content), (uint8_t*)content);
    vga_print_color("  Written to: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(argv[1]); vga_print("\n");
}

static void cmd_rm(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: rm <filename>\n"); return; }
    int rc = ramfs_delete(argv[1]);
    if (rc == 0) {
        vga_print_color("  Deleted: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
    } else if (rc == -2) {
        vga_print_color("  Permission denied: not the owner.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    } else {
        vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
    }
}

static void cmd_edit(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: edit <filename>\n"); return; }
    editor_run(argv[1]);
    /* After editor exits, redraw prompt area */
    vga_print_color("\n  Returned from editor.\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

/* --- Process commands --- */

static void cmd_ps(void) {
    vga_print_color("\n  PID  STATE     NAME\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ---  --------  ----\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t* p = process_get_by_pid(i);
        if (!p || p->state == PROC_UNUSED || p->state == PROC_TERMINATED) continue;

        char buf[8];
        int_to_str(p->pid, buf);
        vga_print("  ");
        vga_print(buf);
        /* Pad */
        int len = strlen(buf);
        for (int j = len; j < 5; j++) vga_putchar(' ');

        const char* state_str;
        switch (p->state) {
            case PROC_READY:    state_str = "READY   "; break;
            case PROC_RUNNING:  state_str = "RUNNING "; break;
            case PROC_BLOCKED:  state_str = "BLOCKED "; break;
            default:            state_str = "UNKNOWN "; break;
        }
        vga_print(state_str);
        vga_print("  ");
        vga_print(p->name);
        vga_print("\n");
    }
    vga_print("\n");
}

static void demo_process(void) {
    for (int i = 0; i < 100; i++) {
        __asm__ volatile("hlt");
    }
    process_exit();
}

static void cmd_run(int argc, char* argv[]) {
    const char* name = (argc >= 2) ? argv[1] : "worker";
    process_t* p = process_create(name, demo_process);
    if (p) {
        char buf[8];
        int_to_str(p->pid, buf);
        vga_print_color("  Started process '", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(name);
        vga_print("' (PID ");
        vga_print(buf);
        vga_print(")\n");
    }
}

static void cmd_kill(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: kill <pid>\n"); return; }
    int pid = 0;
    for (int i = 0; argv[1][i]; i++) {
        pid = pid * 10 + (argv[1][i] - '0');
    }
    if (pid <= 1) {
        vga_print_color("  Cannot kill system process.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    /* Phase 44: permission-checked kill (owner or root only). */
    int rc = process_terminate_as(pid, users_current_uid());
    if (rc == -2) {
        vga_print_color("  Permission denied: not the process owner.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    if (rc == -1) {
        vga_print_color("  No such process.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    vga_print_color("  Terminated PID ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print(argv[1]); vga_print("\n");
}

/* --- Network commands (Phase 26) --- */

static void cmd_net(void) {
    char buf[20];
    vga_print_color("\n  Network Interfaces\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    int count = net_device_count();
    if (count == 0) {
        vga_print("  No network devices detected.\n\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        net_device_t* dev = net_get_device(i);
        if (!dev) continue;

        /* Device name */
        vga_print_color("  ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        vga_print_color(dev->name, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(": ");

        /* Link status */
        if (dev->link_up) {
            vga_print_color("UP", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        } else {
            vga_print_color("DOWN", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        }
        vga_print("\n");

        /* MAC address */
        char mac_str[18];
        net_mac_to_string(&dev->mac, mac_str);
        vga_print("    MAC:   ");
        vga_print_color(mac_str, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print("\n");

        /* TX stats */
        vga_print("    TX:    ");
        int_to_str(dev->stats.tx_packets, buf);
        vga_print(buf);
        vga_print(" packets, ");
        int_to_str(dev->stats.tx_bytes, buf);
        vga_print(buf);
        vga_print(" bytes");
        if (dev->stats.tx_errors > 0) {
            vga_print(", ");
            int_to_str(dev->stats.tx_errors, buf);
            vga_print_color(buf, VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print_color(" errors", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        }
        vga_print("\n");

        /* RX stats */
        vga_print("    RX:    ");
        int_to_str(dev->stats.rx_packets, buf);
        vga_print(buf);
        vga_print(" packets, ");
        int_to_str(dev->stats.rx_bytes, buf);
        vga_print(buf);
        vga_print(" bytes");
        if (dev->stats.rx_errors > 0) {
            vga_print(", ");
            int_to_str(dev->stats.rx_errors, buf);
            vga_print_color(buf, VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print_color(" errors", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        }
        if (dev->stats.rx_dropped > 0) {
            vga_print(", ");
            int_to_str(dev->stats.rx_dropped, buf);
            vga_print_color(buf, VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            vga_print_color(" dropped", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        }
        vga_print("\n");
    }
    vga_print("\n");
}

/* --- Phase 27: TCP/IP commands --- */

static void cmd_ifconfig(void) {
    char buf[20];
    const net_config_t* cfg = ip_get_config();

    vga_print_color("\n  Network Configuration\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ====================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* Show each device */
    int count = net_device_count();
    for (int i = 0; i < count; i++) {
        net_device_t* dev = net_get_device(i);
        if (!dev) continue;

        vga_print_color("  ", VGA_COLOR(VGA_WHITE, VGA_BLACK));
        vga_print_color(dev->name, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(": ");
        if (dev->link_up) {
            vga_print_color("UP", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        } else {
            vga_print_color("DOWN", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        }
        vga_print("\n");

        /* MAC */
        char mac_str[18];
        net_mac_to_string(&dev->mac, mac_str);
        vga_print("    MAC:     ");
        vga_print_color(mac_str, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print("\n");

        /* IP config */
        char ip_str[16];
        ip_to_string(cfg->ip, ip_str);
        vga_print("    IPv4:    ");
        vga_print_color(ip_str, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        vga_print("\n");

        ip_to_string(cfg->subnet_mask, ip_str);
        vga_print("    Mask:    ");
        vga_print(ip_str);
        vga_print("\n");

        ip_to_string(cfg->gateway, ip_str);
        vga_print("    Gateway: ");
        vga_print(ip_str);
        vga_print("\n");

        ip_to_string(cfg->dns, ip_str);
        vga_print("    DNS:     ");
        vga_print(ip_str);
        vga_print("\n");

        /* Stats */
        vga_print("    TX:      ");
        int_to_str(dev->stats.tx_packets, buf); vga_print(buf);
        vga_print(" pkts, ");
        int_to_str(dev->stats.tx_bytes, buf); vga_print(buf);
        vga_print(" bytes\n");

        vga_print("    RX:      ");
        int_to_str(dev->stats.rx_packets, buf); vga_print(buf);
        vga_print(" pkts, ");
        int_to_str(dev->stats.rx_bytes, buf); vga_print(buf);
        vga_print(" bytes\n");
    }

    if (count == 0) {
        vga_print("  No network devices detected.\n");
    }
    vga_print("\n");
}

static void cmd_ping(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: ping <ip>\n");
        return;
    }

    uint32_t target = ip_parse(argv[1]);
    if (target == 0) {
        vga_print_color("  Invalid IP address.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }

    char ip_str[16];
    ip_to_string(target, ip_str);
    vga_print("  PING ");
    vga_print(ip_str);
    vga_print("\n");

    /* Set up ping state */
    ping_state_t* ps = icmp_get_ping_state();
    ps->active = true;
    ps->target_ip = target;
    ps->id = (uint16_t)(system_ticks & 0xFFFF);
    ps->seq_next = 1;
    ps->replies = 0;
    ps->sent = 0;
    ps->got_reply = false;

    /* Send 4 pings */
    for (int i = 0; i < 4; i++) {
        ps->got_reply = false;
        icmp_send_echo(target, ps->id, ps->seq_next++);

        /* Wait for reply (up to ~2 seconds = 36 ticks) */
        uint32_t start = system_ticks;
        while (!ps->got_reply && (system_ticks - start) < 36) {
            net_poll();
            __asm__ volatile("hlt");
        }

        if (ps->got_reply) {
            char num[12];
            vga_print("  Reply from ");
            vga_print(ip_str);
            vga_print(": time=");
            uint32_t rtt_ms = (ps->last_rtt_ticks * 1000) / 18;
            int_to_str(rtt_ms, num);
            vga_print(num);
            vga_print("ms\n");
        } else {
            vga_print_color("  Request timed out.\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        }
    }

    /* Stats summary */
    char num[12];
    vga_print("\n  --- ");
    vga_print(ip_str);
    vga_print(" ping statistics ---\n  ");
    int_to_str(ps->sent, num); vga_print(num);
    vga_print(" sent, ");
    int_to_str(ps->replies, num); vga_print(num);
    vga_print(" received, ");
    int lost = ps->sent - ps->replies;
    int_to_str(lost, num); vga_print(num);
    vga_print(" lost\n\n");

    ps->active = false;
}

static void cmd_netstat(void) {
    char buf[20];
    vga_print_color("\n  Active Connections\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    /* TCP connections */
    int tcp_count;
    const tcp_conn_t* tcp_conns = tcp_get_connections(&tcp_count);
    bool any_tcp = false;

    vga_print_color("  Proto  Local Port   Remote IP        Port   State\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print_color("  -----  ----------   ---------        ----   -----\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < tcp_count; i++) {
        if (!tcp_conns[i].active) continue;
        any_tcp = true;

        vga_print("  TCP    ");
        int_to_str(tcp_conns[i].local_port, buf); vga_print(buf);
        /* Pad */
        int len = strlen(buf);
        for (int j = len; j < 13; j++) vga_putchar(' ');

        char ip_str[16];
        ip_to_string(tcp_conns[i].remote_ip, ip_str);
        vga_print(ip_str);
        len = strlen(ip_str);
        for (int j = len; j < 17; j++) vga_putchar(' ');

        int_to_str(tcp_conns[i].remote_port, buf); vga_print(buf);
        len = strlen(buf);
        for (int j = len; j < 7; j++) vga_putchar(' ');

        vga_print_color(tcp_state_name(tcp_conns[i].state),
                        VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("\n");
    }

    /* UDP bindings */
    int udp_count;
    const udp_binding_t* udp_binds = udp_get_bindings(&udp_count);
    for (int i = 0; i < udp_count; i++) {
        if (!udp_binds[i].active) continue;
        any_tcp = true;

        vga_print("  UDP    ");
        int_to_str(udp_binds[i].port, buf); vga_print(buf);
        int len = strlen(buf);
        for (int j = len; j < 13; j++) vga_putchar(' ');
        vga_print("*                *      ");
        vga_print_color("BOUND", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print("\n");
    }

    if (!any_tcp) {
        vga_print("  No active connections.\n");
    }
    vga_print("\n");
}

/* ============================================================================
 * Phase 44: Firewall command
 * ============================================================================ */

static uint8_t fw_proto_from_str(const char* s) {
    if (strcmp(s, "tcp") == 0) return FW_PROTO_TCP;
    if (strcmp(s, "udp") == 0) return FW_PROTO_UDP;
    if (strcmp(s, "icmp") == 0) return FW_PROTO_ICMP;
    if (strcmp(s, "any") == 0) return FW_PROTO_ANY;
    return 0xFF;  /* invalid */
}

static const char* fw_proto_str(uint8_t p) {
    switch (p) {
        case FW_PROTO_TCP:  return "tcp";
        case FW_PROTO_UDP:  return "udp";
        case FW_PROTO_ICMP: return "icmp";
        default:            return "any";
    }
}

static void cmd_firewall_list(void) {
    char b[20];
    vga_print_color("\n  Firewall rules", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print(firewall_enabled() ? "  [ENABLED]" : "  [disabled]");
    vga_print("   default=");
    vga_print(firewall_default() == FW_DROP ? "DROP" : "ACCEPT");
    vga_print("\n");
    vga_print_color("  ===============================================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  #   ACTION  DIR   PROTO  ADDRESS          PORT\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int shown = 0;
    for (int i = 0; i < FW_MAX_RULES; i++) {
        const fw_rule_t* r = firewall_rule(i);
        if (!r) continue;
        shown++;
        vga_print("  ");
        int_to_str(i, b); vga_print(b); for (int s = strlen(b); s < 4; s++) vga_print(" ");
        vga_print(r->action == FW_DROP ? "DROP    " : "ACCEPT  ");
        vga_print(r->direction == FW_IN ? "in    " : r->direction == FW_OUT ? "out   " : "both  ");
        vga_print(fw_proto_str(r->proto)); for (int s = strlen(fw_proto_str(r->proto)); s < 7; s++) vga_print(" ");
        if (r->addr == 0) { vga_print("any            "); }
        else { char ip[16]; ip_to_string(r->addr, ip); vga_print(ip); for (int s = strlen(ip); s < 15; s++) vga_print(" "); }
        vga_print("  ");
        if (r->port == 0) vga_print("any"); else { int_to_str(r->port, b); vga_print(b); }
        vga_print("\n");
    }
    if (!shown) vga_print("  (no rules)\n");
    vga_print("  dropped="); int_to_str((int)firewall_dropped(), b); vga_print(b);
    vga_print("  passed=");  int_to_str((int)firewall_passed(), b);  vga_print(b);
    vga_print("\n\n");
}

static void cmd_firewall(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage:\n");
        vga_print("    firewall list\n");
        vga_print("    firewall on | off\n");
        vga_print("    firewall default <accept|drop>\n");
        vga_print("    firewall block|allow <proto> <ip|any> [port] [in|out]\n");
        vga_print("    firewall del <#>   |   firewall clear\n");
        vga_print("    (proto = tcp|udp|icmp|any)\n");
        return;
    }
    /* Mutating the firewall requires root. */
    bool is_query = (strcmp(argv[1], "list") == 0);
    if (!is_query && !users_current_is_root()) {
        vga_print_color("  Permission denied: only root may change the firewall.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }

    if (strcmp(argv[1], "list") == 0) { cmd_firewall_list(); return; }
    if (strcmp(argv[1], "on") == 0)  { firewall_set_enabled(true);  vga_print_color("  Firewall ENABLED\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)); return; }
    if (strcmp(argv[1], "off") == 0) { firewall_set_enabled(false); vga_print_color("  Firewall disabled\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK)); return; }
    if (strcmp(argv[1], "clear") == 0) { firewall_clear(); vga_print("  All rules cleared.\n"); return; }
    if (strcmp(argv[1], "default") == 0) {
        if (argc < 3) { vga_print("  Usage: firewall default <accept|drop>\n"); return; }
        if (strcmp(argv[2], "drop") == 0) firewall_set_default(FW_DROP);
        else if (strcmp(argv[2], "accept") == 0) firewall_set_default(FW_ACCEPT);
        else { vga_print("  default must be 'accept' or 'drop'.\n"); return; }
        vga_print("  Default policy set.\n"); return;
    }
    if (strcmp(argv[1], "del") == 0) {
        if (argc < 3) { vga_print("  Usage: firewall del <#>\n"); return; }
        int idx = 0; for (int i = 0; argv[2][i]; i++) idx = idx * 10 + (argv[2][i] - '0');
        vga_print(firewall_del(idx) ? "  Rule removed.\n" : "  No such rule.\n");
        return;
    }
    if (strcmp(argv[1], "block") == 0 || strcmp(argv[1], "allow") == 0) {
        if (argc < 4) { vga_print("  Usage: firewall block|allow <proto> <ip|any> [port] [in|out]\n"); return; }
        uint8_t action = (strcmp(argv[1], "block") == 0) ? FW_DROP : FW_ACCEPT;
        uint8_t proto = fw_proto_from_str(argv[2]);
        if (proto == 0xFF) { vga_print("  proto must be tcp|udp|icmp|any.\n"); return; }
        uint32_t addr = (strcmp(argv[3], "any") == 0) ? 0 : ip_parse(argv[3]);
        uint16_t port = 0;
        uint8_t dir = FW_BOTH;
        for (int i = 4; i < argc; i++) {
            if (strcmp(argv[i], "in") == 0) dir = FW_IN;
            else if (strcmp(argv[i], "out") == 0) dir = FW_OUT;
            else { /* numeric port */ int p = 0; for (int j = 0; argv[i][j]; j++) p = p * 10 + (argv[i][j] - '0'); port = (uint16_t)p; }
        }
        int idx = firewall_add(action, dir, proto, addr, port);
        if (idx < 0) { vga_print_color("  Rule table full.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
        /* Auto-enable on first rule so a 'block' actually takes effect. */
        if (!firewall_enabled()) firewall_set_enabled(true);
        char b[12]; vga_print_color("  Rule #", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        int_to_str(idx, b); vga_print(b);
        vga_print(action == FW_DROP ? " (block) added; firewall enabled.\n" : " (allow) added; firewall enabled.\n");
        return;
    }
    vga_print("  Unknown firewall subcommand. Try 'firewall' for usage.\n");
}

static void cmd_arp_show(void) {
    char buf[20];
    vga_print_color("\n  ARP Cache\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =========\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    int count;
    const arp_entry_t* cache = arp_get_cache(&count);
    bool any = false;

    vga_print_color("  IP Address        MAC Address         Age(s)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print_color("  ---------------   -----------------   ------\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    for (int i = 0; i < count; i++) {
        if (!cache[i].valid) continue;
        any = true;

        char ip_str[16];
        ip_to_string(cache[i].ip, ip_str);
        vga_print("  ");
        vga_print(ip_str);
        int len = strlen(ip_str);
        for (int j = len; j < 18; j++) vga_putchar(' ');

        char mac_str[18];
        net_mac_to_string(&cache[i].mac, mac_str);
        vga_print(mac_str);
        vga_print("    ");

        uint32_t age = (system_ticks - cache[i].timestamp) / 18;
        int_to_str(age, buf);
        vga_print(buf);
        vga_print("\n");
    }

    if (!any) {
        vga_print("  Cache is empty.\n");
    }
    vga_print("\n");
}

/* --- Fun commands --- */

static void cmd_snake(void) {
    snake_run();
    vga_print_color("\n  Thanks for playing!\n\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
}

static void cmd_beep(void) {
    play_boot_sound();
}

static void cmd_gui(void) {
    vga_print_color("  Launching desktop...\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    desktop_run();
    vga_print_color("\n  Returned from desktop.\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

static void cmd_theme(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: theme <name>\n");
        vga_print("  Available: dark, light, retro, ocean, hicon\n");
        return;
    }
    if (theme_set_by_name(argv[1])) {
        vga_print_color("  Theme set to: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]);
        vga_print("\n");
    } else {
        vga_print_color("  Unknown theme: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(argv[1]);
        vga_print("\n  Available: dark, light, retro, ocean, hicon\n");
    }
}

/* --- Phase 31: New Unix commands --- */

static void cmd_head(int argc, char* argv[]) {
    int n = 10; int fi = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        n = 0; for (int i = 0; argv[2][i]; i++) n = n * 10 + (argv[2][i] - '0');
        fi = 3;
    }
    if (fi >= argc) { vga_print("  Usage: head [-n N] <file>\n"); return; }
    const char* fname = argv[fi];
    char buf[4096]; int len;
    if (procfs_is_proc(fname)) {
        len = procfs_read(fname, buf, sizeof(buf));
        if (len < 0) { vga_print("  Not found\n"); return; }
    } else {
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node) { vga_print("  Not found\n"); return; }
        len = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)buf);
        buf[len] = '\0';
    }
    char* p = buf; int ln = 0;
    while (*p && ln < n) {
        vga_print("  ");
        while (*p && *p != '\n') vga_putchar(*p++);
        vga_putchar('\n');
        if (*p == '\n') p++;
        ln++;
    }
}

static void cmd_tail(int argc, char* argv[]) {
    int n = 10; int fi = 1;
    if (argc >= 3 && strcmp(argv[1], "-n") == 0) {
        n = 0; for (int i = 0; argv[2][i]; i++) n = n * 10 + (argv[2][i] - '0');
        fi = 3;
    }
    if (fi >= argc) { vga_print("  Usage: tail [-n N] <file>\n"); return; }
    const char* fname = argv[fi];
    char buf[4096]; int len;
    if (procfs_is_proc(fname)) {
        len = procfs_read(fname, buf, sizeof(buf));
        if (len < 0) { vga_print("  Not found\n"); return; }
    } else {
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node) { vga_print("  Not found\n"); return; }
        len = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)buf);
        buf[len] = '\0';
    }
    /* Count total lines */
    int total = 0;
    for (int i = 0; buf[i]; i++) if (buf[i] == '\n') total++;
    if (buf[0] && buf[len-1] != '\n') total++;
    int skip = total > n ? total - n : 0;
    char* p = buf; int ln = 0;
    while (*p && ln < skip) { if (*p == '\n') ln++; p++; }
    while (*p) {
        vga_print("  ");
        while (*p && *p != '\n') vga_putchar(*p++);
        vga_putchar('\n');
        if (*p == '\n') p++;
    }
}

static void cmd_grep(int argc, char* argv[]) {
    if (argc < 3) { vga_print("  Usage: grep <pattern> <file>\n"); return; }
    const char* pattern = argv[1];
    const char* fname = argv[2];
    char buf[4096]; int len;
    if (procfs_is_proc(fname)) {
        len = procfs_read(fname, buf, sizeof(buf));
        if (len < 0) { vga_print("  Not found\n"); return; }
    } else {
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node) { vga_print("  Not found\n"); return; }
        len = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)buf);
        buf[len] = '\0';
    }
    int count = 0;
    char* p = buf;
    while (*p) {
        char line[256]; int li = 0;
        while (*p && *p != '\n' && li < 255) line[li++] = *p++;
        line[li] = '\0';
        if (*p == '\n') p++;
        /* Simple substring match */
        if (strstr(line, pattern)) {
            vga_print("  ");
            /* Highlight matches */
            char* lp = line;
            while (*lp) {
                char* m = strstr(lp, pattern);
                if (m) {
                    while (lp < m) vga_putchar(*lp++);
                    vga_print_color(pattern, VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                    lp = m + strlen(pattern);
                } else {
                    vga_print(lp); break;
                }
            }
            vga_putchar('\n');
            count++;
        }
    }
    if (count == 0) {
        vga_print_color("  No matches found\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    }
}

static void cmd_wc(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: wc <file>\n"); return; }
    const char* fname = argv[1];
    char buf[4096]; int len;
    if (procfs_is_proc(fname)) {
        len = procfs_read(fname, buf, sizeof(buf));
        if (len < 0) { vga_print("  Not found\n"); return; }
    } else {
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node) { vga_print("  Not found\n"); return; }
        len = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)buf);
        buf[len] = '\0';
    }
    int lines = 0, words = 0, chars = len;
    bool in_word = false;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t') {
            in_word = false;
        } else if (!in_word) {
            in_word = true; words++;
        }
    }
    char nb[12];
    vga_print("  ");
    int_to_str(lines, nb); vga_print(nb); vga_print(" lines  ");
    int_to_str(words, nb); vga_print(nb); vga_print(" words  ");
    int_to_str(chars, nb); vga_print(nb); vga_print(" chars  ");
    vga_print(fname); vga_print("\n");
}

static void cmd_env(void) {
    vga_print_color("\n  Environment Variables\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    /* Print common vars */
    const char* vars[] = {"PATH", "HOME", "USER", "SHELL", "TERM", "PWD", NULL};
    for (int i = 0; vars[i]; i++) {
        const char* val = env_get(vars[i]);
        if (val) {
            vga_print("  ");
            vga_print_color(vars[i], VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            vga_print("="); vga_print(val); vga_print("\n");
        }
    }
    vga_print("\n");
}

static void cmd_export(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: export KEY=VALUE\n"); return; }
    /* Find '=' in argv[1] */
    char key[64], val[256];
    char* eq = NULL;
    for (int i = 0; argv[1][i]; i++) {
        if (argv[1][i] == '=') { eq = &argv[1][i]; break; }
    }
    if (!eq) { vga_print("  Usage: export KEY=VALUE\n"); return; }
    int klen = eq - argv[1];
    if (klen <= 0 || klen > 63) { vga_print("  Invalid key\n"); return; }
    memcpy(key, argv[1], klen); key[klen] = '\0';
    strcpy(val, eq + 1);
    /* Append remaining args (for values with spaces) */
    for (int i = 2; i < argc; i++) { strcat(val, " "); strcat(val, argv[i]); }
    env_set(key, val);
    vga_print("  "); vga_print(key); vga_print("="); vga_print(val); vga_print("\n");
}

static void cmd_uname(void) {
    vga_print("  NexusOS v3.2.0 i386 Phase 32\n");
}

static void cmd_whoami(void) {
    /* Phase 44: the real logged-in user (not the USER env var). */
    vga_print("  "); vga_print((char*)users_current_name()); vga_print("\n");
}

/* ============================================================================
 * Phase 44: Security commands
 * ============================================================================ */

static void cmd_id(void) {
    char b[12];
    vga_print("  uid=");
    int_to_str((int)users_current_uid(), b); vga_print(b);
    vga_print("("); vga_print((char*)users_current_name()); vga_print(")");
    vga_print(users_current_is_root() ? "  [root/privileged]\n" : "  [standard user]\n");
}

static void cmd_users(void) {
    char b[12];
    vga_print_color("\n  Users\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =====\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  UID    NAME\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    user_t* t = users_table();
    for (int i = 0; i < USER_MAX; i++) {
        if (!t[i].used) continue;
        vga_print("  ");
        int_to_str((int)t[i].uid, b); vga_print(b);
        for (int s = strlen(b); s < 7; s++) vga_print(" ");
        vga_print(t[i].name);
        if (t[i].uid == users_current_uid()) vga_print_color("  (you)", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("\n");
    }
    vga_print("\n");
}

static void cmd_useradd(int argc, char* argv[]) {
    if (!users_current_is_root()) {
        vga_print_color("  Permission denied: only root may add users.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    if (argc < 3) { vga_print("  Usage: useradd <username> <password>\n"); return; }
    uint32_t uid = users_add(argv[1], argv[2]);
    if (uid == UID_INVALID) {
        vga_print_color("  Could not add user (exists or table full).\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    char b[12];
    vga_print_color("  Added user ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(argv[1]); vga_print(" (uid="); int_to_str((int)uid, b); vga_print(b); vga_print(")\n");
}

static void cmd_userdel(int argc, char* argv[]) {
    if (!users_current_is_root()) {
        vga_print_color("  Permission denied: only root may remove users.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    if (argc < 2) { vga_print("  Usage: userdel <username>\n"); return; }
    if (users_del(argv[1])) {
        vga_print_color("  Removed user ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
    } else {
        vga_print_color("  Could not remove (not found, is root, or is the current user).\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
}

static void cmd_passwd(int argc, char* argv[]) {
    /* passwd <newpass>          -> change own password
     * passwd <user> <newpass>   -> change another (root only) */
    const char* target;
    const char* newpass;
    if (argc == 2) {
        target = users_current_name();
        newpass = argv[1];
    } else if (argc >= 3) {
        if (!users_current_is_root()) {
            vga_print_color("  Permission denied: only root may change other users.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            return;
        }
        target = argv[1];
        newpass = argv[2];
    } else {
        vga_print("  Usage: passwd <newpass>   |   passwd <user> <newpass> (root)\n");
        return;
    }
    if (users_set_password(target, newpass)) {
        vga_print_color("  Password updated for ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print((char*)target); vga_print("\n");
    } else {
        vga_print_color("  No such user.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
}

static void cmd_chmod(int argc, char* argv[]) {
    if (argc < 3) { vga_print("  Usage: chmod <octal-mode> <file>   (e.g. chmod 600 secret)\n"); return; }
    /* parse octal mode */
    uint16_t mode = 0;
    for (int i = 0; argv[1][i]; i++) {
        if (argv[1][i] < '0' || argv[1][i] > '7') { vga_print("  Mode must be octal (0-7 digits).\n"); return; }
        mode = (uint16_t)(mode * 8 + (argv[1][i] - '0'));
    }
    fs_node_t* node = vfs_finddir(vfs_get_root(), argv[2]);
    if (!node) { vga_print_color("  No such file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
    if (vfs_chmod(node, mode, users_current_uid()) != 0) {
        vga_print_color("  Permission denied: not the owner.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    char ms[12]; vfs_mode_string(node, ms);
    vga_print("  ");
    vga_print_color(ms, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("  "); vga_print(argv[2]); vga_print("\n");
}

static void cmd_chown(int argc, char* argv[]) {
    if (argc < 3) { vga_print("  Usage: chown <user> <file>\n"); return; }
    user_t* u = users_find(argv[1]);
    if (!u) { vga_print_color("  No such user.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
    fs_node_t* node = vfs_finddir(vfs_get_root(), argv[2]);
    if (!node) { vga_print_color("  No such file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
    if (vfs_chown(node, u->uid, users_current_uid()) != 0) {
        vga_print_color("  Permission denied: only root may chown.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    vga_print_color("  Owner of ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(argv[2]); vga_print(" -> "); vga_print(argv[1]); vga_print("\n");
}

/* --- Phase 34: Win32 Layer Commands --- */

static void cmd_win32info(void) {
    vga_print_color("\n  Win32 Compatibility Shim - Phase 34\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  ");
    vga_print(win32_get_status());
    vga_print("\n");
    char buf[12];
    vga_print_color("  API Layer:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("kernel32.dll, user32.dll, gdi32.dll\n");
    vga_print_color("  Registry:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str((int)registry_get_entry_count(), buf);
    vga_print(buf); vga_print(" entries\n");
    vga_print_color("  System:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("PE32 Loader + Win32 API translation\n\n");
}

static void cmd_regedit(void) {
    syslog_add("cmd_regedit: started");
    vga_print_color("\n  Registry Editor (Console)\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    syslog_add("cmd_regedit: printing labels done");
    int count = registry_get_entry_count();
    syslog_add("cmd_regedit: count retrieved");
    if (count == 0) {
        vga_print("  Registry is empty.\n");
    } else {
        const reg_entry_t* entries = registry_get_entries();
        for (int i = 0; i < 64; i++) {
            if (!entries[i].active) continue;
            vga_print("  ");
            if (entries[i].hkey_root == (uint32_t)HKEY_LOCAL_MACHINE) vga_print("HKLM\\");
            else if (entries[i].hkey_root == (uint32_t)HKEY_CURRENT_USER) vga_print("HKCU\\");
            else if (entries[i].hkey_root == (uint32_t)HKEY_CLASSES_ROOT) vga_print("HKCR\\");
            else if (entries[i].hkey_root == (uint32_t)HKEY_USERS) vga_print("HKU\\");
            else vga_print("HK??\\");
            
            vga_print((char*)entries[i].path);
            vga_print("\\");
            vga_print_color((char*)entries[i].value_name, VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            vga_print(" = ");
            
            if (entries[i].type == REG_SZ) {
                vga_print("\"");
                vga_print((char*)entries[i].data);
                vga_print("\"");
            } else if (entries[i].type == REG_DWORD) {
                uint32_t val;
                memcpy(&val, entries[i].data, 4);
                char buf[16]; int_to_str(val, buf);
                vga_print(buf);
            } else {
                vga_print("<binary data>");
            }
            vga_print("\n");
        }
    }
    vga_print("\n");
}

static void cmd_runexe(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: runexe <file.exe>\n");
    } else {
        const char* fname = argv[1];
        fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
        if (!node || node->size == 0) {
            vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print((char*)fname); vga_print("\n");
        } else {
            uint8_t* buf = (uint8_t*)kmalloc(node->size);
            if (buf) {
                int32_t rd = vfs_read(node, 0, node->size, buf);
                if (rd == (int32_t)node->size) {
                    vga_print("  Loading PE32 executable...\n");
                    int res = pe_exec(buf, node->size, fname);
                    if (res != 0) {
                        vga_print_color("  Failed to execute PE file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                    }
                } else {
                    vga_print("  Read error.\n");
                }
                kfree(buf);
            } else {
                vga_print("  Out of memory.\n");
            }
        }
    }
}

/* --------------------------------------------------------------------------
 * cmd_npkg: NexusOS Package Manager front-end (Phase 35)
 * -------------------------------------------------------------------------- */

/* Case-insensitive substring test for `npkg search`. */
static bool npkg_contains_ci(const char* haystack, const char* needle) {
    if (!needle || !*needle) return true;
    for (const char* h = haystack; *h; h++) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return true;
    }
    return false;
}

static void npkg_usage(void) {
    vga_print_color("\n  npkg — NexusOS Package Manager (Phase 35)\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =========================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  npkg list", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("               List all packages in the repository\n");
    vga_print_color("  npkg search <term>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("      Search packages by name/description\n");
    vga_print_color("  npkg info <pkg>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("         Show package details and dependencies\n");
    vga_print_color("  npkg install <pkg>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("      Install a package (auto-resolves deps)\n");
    vga_print_color("  npkg remove <pkg>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("       Uninstall a package\n");
    vga_print_color("  npkg installed", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("          List installed packages\n");
    vga_print_color("  npkg update", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("             Refresh the package index\n\n");
}

static void npkg_list(void) {
    int n = pkg_repo_count();
    vga_print_color("\n  Available packages (", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    char buf[12]; int_to_str(n, buf); vga_print_color(buf, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("):\n\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (int i = 0; i < n; i++) {
        const pkg_def_t* p = pkg_repo_get(i);
        if (!p) continue;
        vga_print("  ");
        vga_print_color((char*)p->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        /* pad name column to ~12 chars */
        int pad = 12 - (int)strlen(p->name);
        for (int s = 0; s < pad; s++) vga_putchar(' ');
        vga_print_color((char*)p->version, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        pad = 9 - (int)strlen(p->version);
        for (int s = 0; s < pad; s++) vga_putchar(' ');
        if (pkg_is_installed(p->name))
            vga_print_color("[installed] ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        else
            vga_print("            ");
        vga_print_color((char*)p->description, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("\n");
    }
    vga_print("\n");
}

static void npkg_search(const char* term) {
    int n = pkg_repo_count();
    int hits = 0;
    vga_print_color("\n  Search results for '", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print((char*)term);
    vga_print_color("':\n\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    for (int i = 0; i < n; i++) {
        const pkg_def_t* p = pkg_repo_get(i);
        if (!p) continue;
        if (npkg_contains_ci(p->name, term) || npkg_contains_ci(p->description, term)) {
            vga_print("  ");
            vga_print_color((char*)p->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
            vga_print(" - ");
            vga_print_color((char*)p->description, VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            vga_print("\n");
            hits++;
        }
    }
    if (hits == 0) vga_print("  No matching packages.\n");
    vga_print("\n");
}

static void npkg_info(const char* name) {
    const pkg_def_t* p = pkg_repo_find(name);
    if (!p) {
        vga_print_color("  Package not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)name); vga_print("\n");
        return;
    }
    vga_print_color("\n  Package: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print_color((char*)p->name, VGA_COLOR(VGA_WHITE, VGA_BLACK)); vga_print("\n");
    vga_print_color("  Version: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)p->version); vga_print("\n");
    vga_print_color("  Author:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)p->author); vga_print("\n");
    vga_print_color("  Summary: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)p->description); vga_print("\n");
    vga_print_color("  Status:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    if (pkg_is_installed(p->name))
        vga_print_color("installed\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    else
        vga_print_color("not installed\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    vga_print_color("  Depends: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    if (!p->deps[0].name) {
        vga_print("(none)\n");
    } else {
        for (int i = 0; i < PKG_MAX_DEPS && p->deps[i].name; i++) {
            if (i > 0) vga_print(", ");
            vga_print((char*)p->deps[i].name);
            vga_print(" >="); vga_print((char*)p->deps[i].min_version);
        }
        vga_print("\n");
    }

    vga_print_color("  Files:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    for (int i = 0; i < PKG_MAX_FILES && p->files[i].name; i++) {
        if (i > 0) vga_print(", ");
        vga_print((char*)p->files[i].name);
    }
    vga_print("\n\n");
}

static void npkg_installed(void) {
    int n = pkg_installed_count();
    vga_print_color("\n  Installed packages (", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    char buf[12]; int_to_str(n, buf); vga_print_color(buf, VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("):\n\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    if (n == 0) {
        vga_print("  Nothing installed yet. Try 'npkg install hello'.\n\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        const pkg_installed_t* inst = pkg_installed_get(i);
        if (!inst) continue;
        vga_print("  ");
        vga_print_color((char*)inst->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        int pad = 12 - (int)strlen(inst->name);
        for (int s = 0; s < pad; s++) vga_putchar(' ');
        vga_print_color((char*)inst->version, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("  (");
        int_to_str((int)inst->file_count, buf); vga_print(buf);
        vga_print(" file(s), ");
        int_to_str((int)inst->install_size, buf); vga_print(buf);
        vga_print(" bytes)\n");
    }
    vga_print("\n");
}

static void cmd_npkg(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "help") == 0) { npkg_usage(); return; }

    const char* sub = argv[1];
    if (strcmp(sub, "list") == 0) {
        npkg_list();
    } else if (strcmp(sub, "installed") == 0) {
        npkg_installed();
    } else if (strcmp(sub, "update") == 0) {
        pkg_update();
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 3) { vga_print("  Usage: npkg search <term>\n"); return; }
        npkg_search(argv[2]);
    } else if (strcmp(sub, "info") == 0) {
        if (argc < 3) { vga_print("  Usage: npkg info <package>\n"); return; }
        npkg_info(argv[2]);
    } else if (strcmp(sub, "install") == 0) {
        if (argc < 3) { vga_print("  Usage: npkg install <package>\n"); return; }
        vga_print("\n");
        pkg_install(argv[2]);
    } else if (strcmp(sub, "remove") == 0) {
        if (argc < 3) { vga_print("  Usage: npkg remove <package>\n"); return; }
        vga_print("\n");
        pkg_remove(argv[2]);
    } else {
        vga_print_color("  Unknown npkg subcommand: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)sub); vga_print("\n");
        npkg_usage();
    }
}

/* --------------------------------------------------------------------------
 * cmd_script: Run a NexusScript file (Phase 36)
 * -------------------------------------------------------------------------- */
static void cmd_script(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: script <file.ns>\n");
        vga_print("  Try:   script demo.ns\n");
        return;
    }
    int rc = script_run_file(argv[1]);
    if (rc == SCRIPT_ERR_NOTFOUND) {
        vga_print_color("  Script not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
    } else if (rc != SCRIPT_OK && rc != SCRIPT_ERR_SYNTAX) {
        /* Syntax/runtime errors already printed a line diagnostic from the engine. */
        vga_print_color("  Script error: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)script_strerror(rc)); vga_print("\n");
    }
}

/* --------------------------------------------------------------------------
 * Phase 37: macOS Compatibility Shim commands
 * -------------------------------------------------------------------------- */
static void cmd_machoinfo(void) {
    vga_print_color("\n  macOS Compatibility Shim - Phase 37\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ===================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  "); vga_print((char*)macho_get_status()); vga_print("\n");
    vga_print("  "); vga_print((char*)cocoa_get_status()); vga_print("\n");
    char buf[12];
    vga_print_color("  CF Objects: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str(cocoa_object_count(), buf); vga_print(buf); vga_print(" live\n");
    vga_print_color("  Commands:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("runmacho <file>   cocoademo\n\n");
}

static void cmd_runmacho(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: runmacho <file>\n"); return; }
    const char* fname = argv[1];
    fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
    if (!node || node->size == 0) {
        vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)fname); vga_print("\n");
        return;
    }
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) { vga_print("  Out of memory.\n"); return; }
    int32_t rd = vfs_read(node, 0, node->size, buf);
    if (rd != (int32_t)node->size) { vga_print("  Read error.\n"); kfree(buf); return; }

    macho_info_t info;
    if (!macho_get_info(buf, node->size, &info)) {
        vga_print_color("  Not a valid i386 Mach-O executable.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        kfree(buf); return;
    }
    char b[12];
    vga_print_color("  Mach-O ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)); vga_print("i386 executable\n");
    vga_print("    segments: "); int_to_str(info.nsegments, b); vga_print(b);
    vga_print("   dylibs: ");    int_to_str(info.ndylibs, b);   vga_print(b);
    vga_print("   entry: 0x");   hex_to_str(info.entry, b);     vga_print(b); vga_print("\n");
    vga_print("  Loading and executing...\n");
    macho_exec(buf, node->size, fname);   /* irets to ring 3 on success */
    kfree(buf);
}

static void cmd_cocoademo(void) {
    vga_print_color("  Launching Cocoa demo...\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (cocoa_demo() == 0)
        vga_print("  NSWindow created. Type 'gui' to view it on the desktop.\n\n");
    else
        vga_print_color("  Cocoa demo failed (no free objects).\n\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
}

/* --------------------------------------------------------------------------
 * Phase 38: Sound — Audio Mixer + AC'97 commands
 * -------------------------------------------------------------------------- */
static int shell_atoi(const char* s) {
    int v = 0; int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return v * sign;
}

static void cmd_sndinfo(void) {
    char buf[12];
    vga_print_color("\n  Sound Subsystem - Phase 38\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Driver:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)ac97_status()); vga_print("\n");
    vga_print_color("  Mixer:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)audio_status()); vga_print("\n");
    vga_print_color("  Output:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str((int)audio_device_rate(), buf); vga_print(buf);
    vga_print(" Hz, 16-bit stereo\n");
    vga_print_color("  Voices:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str(audio_active_voices(), buf); vga_print(buf);
    vga_print(" active\n");
    vga_print_color("  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("play <file.wav>  volume <0-100>  tone <hz> <ms>  mixer\n\n");
}

static void cmd_volume(int argc, char* argv[]) {
    char buf[12];
    if (argc < 2) {
        vga_print_color("  Master volume: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        int_to_str(audio_get_master_volume(), buf); vga_print(buf);
        vga_print("%   (usage: volume <0-100>)\n");
        return;
    }
    int v = shell_atoi(argv[1]);
    if (v < 0) v = 0; if (v > 100) v = 100;
    audio_set_master_volume((uint8_t)v);
    vga_print_color("  Master volume set to ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    int_to_str(v, buf); vga_print(buf); vga_print("%\n");
}

static void cmd_tone(int argc, char* argv[]) {
    int hz = (argc >= 2) ? shell_atoi(argv[1]) : 440;
    int ms = (argc >= 3) ? shell_atoi(argv[2]) : 400;
    if (hz < 20 || hz > 20000) { vga_print("  Usage: tone <20-20000 hz> <ms>\n"); return; }
    if (ms < 1) ms = 400;
    vga_print_color("  Playing tone ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    char b[12]; int_to_str(hz, b); vga_print(b); vga_print(" Hz...\n");
    audio_play_tone((uint32_t)hz, (uint32_t)ms, 90);
    vga_print("  Done.\n");
}

static void cmd_play(int argc, char* argv[]) {
    const char* fname = (argc >= 2) ? argv[1] : "startup.wav";
    fs_node_t* node = vfs_finddir(vfs_get_root(), fname);
    if (!node || node->size == 0) {
        vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)fname);
        vga_print("\n  (try 'play startup.wav')\n");
        return;
    }
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) { vga_print("  Out of memory.\n"); return; }
    int32_t rd = vfs_read(node, 0, node->size, buf);
    if (rd != (int32_t)node->size) { vga_print("  Read error.\n"); kfree(buf); return; }

    wav_info_t w;
    if (!wav_parse(buf, node->size, &w)) {
        vga_print_color("  Not a valid PCM WAV file.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        kfree(buf); return;
    }
    char b[12];
    vga_print_color("  WAV ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)); vga_print((char*)fname);
    vga_print(": "); int_to_str((int)w.rate, b); vga_print(b); vga_print(" Hz, ");
    int_to_str(w.channels, b); vga_print(b); vga_print(" ch, ");
    int_to_str(w.bits, b); vga_print(b); vga_print("-bit, ");
    int_to_str((int)w.frames, b); vga_print(b); vga_print(" frames\n");
    if (!audio_have_device())
        vga_print_color("  (no AC'97 codec - playing to null sink)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Playing...\n");
    audio_play_wav(buf, node->size, audio_get_master_volume());
    vga_print("  Done.\n");
    kfree(buf);
}

/* Build a small loopable mono buffer of whole cycles for `freq`. */
static int16_t* mk_loop_tone(uint32_t freq, uint32_t rate, uint32_t* out_frames) {
    uint32_t period = rate / freq; if (period == 0) period = 1;
    uint32_t cycles = (rate / 20) / period; if (cycles == 0) cycles = 1;
    uint32_t frames = period * cycles;          /* ~50 ms, whole cycles */
    int16_t* b = (int16_t*)kmalloc(frames * sizeof(int16_t));
    if (b) audio_gen_tone(b, frames, freq, rate, 45);
    *out_frames = frames;
    return b;
}

static void cmd_mixer(void) {
    /* Demonstrate the software mixer: 3 simultaneous looping voices (C-E-G). */
    uint32_t rate = audio_device_rate();
    uint32_t fa, fb, fc;
    int16_t* a = mk_loop_tone(262, rate, &fa);   /* C4 */
    int16_t* b = mk_loop_tone(330, rate, &fb);   /* E4 */
    int16_t* c = mk_loop_tone(392, rate, &fc);   /* G4 */
    if (!a || !b || !c) {
        vga_print("  Out of memory.\n");
        if (a) kfree(a); if (b) kfree(b); if (c) kfree(c);
        return;
    }
    audio_voice_clear();
    audio_voice_add(a, fa, 1, rate, 90, true);
    audio_voice_add(b, fb, 1, rate, 90, true);
    audio_voice_add(c, fc, 1, rate, 90, true);

    vga_print_color("  Mixing 3 voices ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("(C-E-G major chord)...\n");
    audio_mix_run(2000);
    vga_print("  Done.\n");
    kfree(a); kfree(b); kfree(c);
}

/* --------------------------------------------------------------------------
 * Phase 39: Image Formats commands
 * -------------------------------------------------------------------------- */
static uint8_t* shell_read_file(const char* fname, uint32_t* out_size) {
    fs_node_t* node = vfs_finddir(vfs_get_root(), (char*)fname);
    if (!node || node->size == 0) return NULL;
    uint8_t* buf = (uint8_t*)kmalloc(node->size);
    if (!buf) return NULL;
    if (vfs_read(node, 0, node->size, buf) != (int32_t)node->size) { kfree(buf); return NULL; }
    *out_size = node->size;
    return buf;
}

static void cmd_imginfo(void) {
    static const char* names[] = { "icon.bmp", "logo.png", "photo.jpg", "anim.gif" };
    vga_print_color("\n  Image Subsystem - Phase 39\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Formats:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("BMP (24/32)   PNG (inflate+filters)   JPEG (baseline)   GIF (LZW, animated)\n\n");

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        uint32_t size = 0;
        uint8_t* buf = shell_read_file(names[i], &size);
        vga_print("  ");
        vga_print_color((char*)names[i], VGA_COLOR(VGA_WHITE, VGA_BLACK));
        if (!buf) { vga_print("  (missing)\n"); continue; }
        image_t img; char b[12];
        if (image_decode(buf, size, &img)) {
            vga_print("  "); vga_print((char*)img.format);
            vga_print("  "); int_to_str(img.width, b); vga_print(b);
            vga_print("x"); int_to_str(img.height, b); vga_print(b);
            if (img.frames > 1) { vga_print("  frames="); int_to_str(img.frames, b); vga_print(b); }
            uint32_t c = img.pixels[(img.height / 2) * img.width + (img.width / 2)];
            vga_print("  center=0x"); hex_to_str(c & 0xFFFFFF, b); vga_print(b);
            vga_print("\n");
            image_free(&img);
        } else {
            vga_print_color("  decode failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        }
        kfree(buf);
    }
    vga_print_color("\n  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("view <file>   (try: view logo.png)\n\n");
}

static void cmd_view(int argc, char* argv[]) {
    const char* fname = (argc >= 2) ? argv[1] : "logo.png";
    uint32_t size = 0;
    uint8_t* buf = shell_read_file(fname, &size);
    if (!buf) {
        vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)fname); vga_print("\n  (try 'view logo.png')\n");
        return;
    }
    vga_print_color("  Opening ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print((char*)fname); vga_print(" ("); vga_print((char*)image_format_name(buf, size));
    vga_print(")...\n");
    image_view(buf, size, fname);
    kfree(buf);
}

/* --------------------------------------------------------------------------
 * Phase 40: Video Playback commands
 * -------------------------------------------------------------------------- */
static void cmd_vidinfo(int argc, char* argv[]) {
    const uint8_t* d; uint32_t sz = 0; uint8_t* fbuf = NULL; const char* name;
    if (argc >= 2) {
        fbuf = shell_read_file(argv[1], &sz);
        if (!fbuf) { vga_print_color("  File not found.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); return; }
        d = fbuf; name = argv[1];
    } else { d = video_demo(&sz); name = "demo.avi"; }

    vga_print_color("\n  Video Subsystem - Phase 40\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ==========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Container: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("RIFF/AVI    Video: Motion-JPEG    Audio: PCM    Sync: audio clock\n\n");

    avi_info_t in; char b[12];
    vga_print("  "); vga_print_color((char*)name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    if (avi_parse(d, sz, &in)) {
        vga_print("  "); vga_print(in.vcodec);
        vga_print("  "); int_to_str(in.width, b); vga_print(b);
        vga_print("x"); int_to_str(in.height, b); vga_print(b);
        vga_print("  "); int_to_str(in.frame_count, b); vga_print(b); vga_print(" frames");
        uint32_t fps = in.us_per_frame ? 1000000u / in.us_per_frame : 0;
        vga_print("  "); int_to_str((int)fps, b); vga_print(b); vga_print(" fps\n");
        vga_print_color("  Audio:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        if (in.has_audio) {
            int_to_str(in.audio_rate, b); vga_print(b); vga_print(" Hz  ");
            int_to_str(in.audio_channels, b); vga_print(b); vga_print(" ch  ");
            int_to_str(in.audio_bits, b); vga_print(b); vga_print("-bit  ");
            int_to_str(in.audio_chunks, b); vga_print(b); vga_print(" chunks\n");
        } else { vga_print("none\n"); }
    } else {
        vga_print_color("  not a valid AVI\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    }
    vga_print_color("\n  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("mplay [file]   (no arg = bundled demo.avi)\n\n");
    if (fbuf) kfree(fbuf);
}

static void cmd_mplay(int argc, char* argv[]) {
    const uint8_t* d; uint32_t sz = 0; uint8_t* fbuf = NULL; const char* name;
    if (argc >= 2) {
        fbuf = shell_read_file(argv[1], &sz);
        if (!fbuf) {
            vga_print_color("  File not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print(argv[1]); vga_print("\n  (run 'mplay' alone for the demo)\n");
            return;
        }
        d = fbuf; name = argv[1];
    } else { d = video_demo(&sz); name = "demo.avi"; }

    vga_print_color("  Playing ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print((char*)name); vga_print("...\n");
    video_play(d, sz, name);
    vga_print("  Done.\n");
    if (fbuf) kfree(fbuf);
}

/* --------------------------------------------------------------------------
 * Phase 41: GPU Acceleration commands
 * -------------------------------------------------------------------------- */
static void print_label_num(const char* label, uint32_t v, const char* unit) {
    char b[12];
    vga_print((char*)label);
    int_to_str((int)v, b); vga_print(b);
    vga_print((char*)unit);
}

static void cmd_gpuinfo(void) {
    gpu_info_t gi;
    bool probed = gpu_get_info(&gi);
    char b[12];

    vga_print_color("\n  GPU Subsystem - Phase 41\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("  Driver:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("VirtIO-GPU, modern virtio-pci transport (polled controlq)\n");
    vga_print_color("  Device:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)gpu_status()); vga_print("\n");

    if (probed) {
        vga_print_color("  Scanout:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        int_to_str(gi.width, b); vga_print(b); vga_print("x");
        int_to_str(gi.height, b); vga_print(b);
        vga_print(" B8G8R8X8");
        print_label_num("  scanouts=", (uint32_t)gi.num_scanouts, "");
        print_label_num("  ctrlq=", gi.queue_size, " entries\n");
        vga_print_color("  Backing:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        hex_to_str(gi.backing_addr, b); vga_print(b);
        vga_print(" (kernel back buffer, zero-copy)");
        print_label_num("  presents=", gi.presents, "\n");
        vga_print_color("  Features: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        hex_to_str(gi.features_hi, b); vga_print(b); vga_print(":");
        hex_to_str(gi.features_lo, b); vga_print(b);
        vga_print(" VERSION_1");
        if (gi.features_lo & 0x2) vga_print(" EDID");
        vga_print("\n");
    }

    vga_print_color("  Mode:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print(gpu_active() ? "virtio scanout active (VGA compat retired)\n"
                           : "VESA framebuffer fallback\n");
    vga_print_color("\n  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gpubench   sprites\n\n");
}

/* Run fn() in a tight loop for at least min_ticks timer ticks (tick-edge
 * aligned). Returns the iteration count; *ms_out gets the elapsed time. */
static uint32_t bench_loop(void (*fn)(void), uint32_t min_ticks, uint32_t* ms_out) {
    uint32_t t0 = system_ticks;
    while (system_ticks == t0) { }
    uint32_t start = system_ticks, n = 0;
    while ((system_ticks - start) < min_ticks) { fn(); n++; }
    *ms_out = (system_ticks - start) * 55;
    if (*ms_out == 0) *ms_out = 1;
    return n;
}

static uint32_t bench_tint;
static void bf_present_full(void) { fb_flip(); }
static void bf_present_rect(void) {
    gpu_present((int)fb_get_width() / 2 - 32, (int)fb_get_height() / 2 - 32, 64, 64);
}
static void bf_legacy_flip(void)  { fb_flip_legacy(); }
static void bf_gpu_fill(void) {
    bench_tint ^= 0x003838;
    gpu_fill(0, 0, (int)fb_get_width(), (int)fb_get_height(), 0x102870 ^ bench_tint);
}
static void bf_sw_fill(void) {
    bench_tint ^= 0x003838;
    fb_fill_rect(0, 0, (int)fb_get_width(), (int)fb_get_height(), 0x701028 ^ bench_tint);
}
static void bf_gpu_blit(void) {
    gpu_blit((int)fb_get_width() / 4, (int)fb_get_height() / 4, 0, 0,
             (int)fb_get_width() / 2, (int)fb_get_height() / 2);
}

static void bench_report(const char* name, uint32_t n, uint32_t ms, uint32_t kb_per_op) {
    char b[12];
    vga_print("  ");
    vga_print_color((char*)name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
    int_to_str((int)n, b); vga_print("  "); vga_print(b); vga_print(" ops/");
    int_to_str((int)ms, b); vga_print(b); vga_print("ms = ");
    int_to_str((int)(n * 1000 / ms), b);
    vga_print_color(b, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    if (kb_per_op) {
        vga_print(" fps  (");
        int_to_str((int)(n * kb_per_op / 1024 * 1000 / ms), b); vga_print(b);
        vga_print(" MB/s)");
    } else {
        vga_print(" fps");
    }
    vga_print("\n");
}

static void cmd_gpubench(void) {
    if (!fb_is_vesa()) { vga_print("  gpubench needs VESA mode.\n"); return; }
    bool gpu_on = gpu_active();
    uint32_t sw = fb_get_width(), sh = fb_get_height();
    uint32_t frame_kb = sw * sh * 4 / 1024;          /* 3072 KB at 1024x768 */

    vga_print("  Benchmarking (screen will flicker)...\n");
    vga_flush();

    uint32_t n_full = 0,  ms_full = 1;
    uint32_t n_rect = 0,  ms_rect = 1;
    uint32_t n_leg  = 0,  ms_leg  = 1;
    uint32_t n_gf   = 0,  ms_gf   = 1;
    uint32_t n_sf   = 0,  ms_sf   = 1;
    uint32_t n_bl   = 0,  ms_bl   = 1;

    if (gpu_on) {
        n_full = bench_loop(bf_present_full, 9, &ms_full);
        n_rect = bench_loop(bf_present_rect, 9, &ms_rect);
    }
    n_leg = bench_loop(bf_legacy_flip, 9, &ms_leg);
    n_gf  = bench_loop(bf_gpu_fill,    9, &ms_gf);
    n_sf  = bench_loop(bf_sw_fill,     9, &ms_sf);
    n_bl  = bench_loop(bf_gpu_blit,    9, &ms_bl);

    vga_clear();
    vga_print_color("\n  GPU Benchmark - Phase 41\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    if (gpu_on) {
        bench_report("Present full screen (GPU) : ", n_full, ms_full, 0);
        bench_report("Present 64x64 dirty (GPU) : ", n_rect, ms_rect, 0);
    } else {
        vga_print("  GPU presents skipped (VirtIO-GPU inactive)\n");
    }
    bench_report("VESA flip (3MB memcpy)    : ", n_leg, ms_leg, 0);
    bench_report("Fill screen (rep stosl)   : ", n_gf, ms_gf, frame_kb);
    bench_report("Fill screen (per-pixel sw): ", n_sf, ms_sf, frame_kb);
    bench_report("Blit 512x384 (rep movsl)  : ", n_bl, ms_bl, frame_kb / 4);

    if (gpu_on) {
        uint32_t fps = n_full * 1000 / ms_full;
        vga_print_color("\n  60 fps desktop: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        if (fps >= 60) {
            char b[12]; int_to_str((int)fps, b);
            vga_print_color("YES", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            vga_print(" - full-frame present runs at "); vga_print(b); vga_print(" fps\n");
        } else {
            vga_print("below target\n");
        }
    }
    vga_print("\n");
}

static uint32_t demo_rng;
static uint32_t demo_rand(void) {
    demo_rng = demo_rng * 1664525u + 1013904223u;
    return demo_rng >> 8;
}

#define DEMO_BALLS 10
static void cmd_sprites(void) {
    if (!fb_is_vesa()) { vga_print("  sprites needs VESA mode.\n"); return; }
    int sw = (int)fb_get_width(), sh = (int)fb_get_height();
    static const uint32_t palette[DEMO_BALLS] = {
        0xE5484D, 0x46A758, 0x3E63DD, 0xF5A524, 0x8E4EC6,
        0x05A2C2, 0xE93D82, 0x99C24D, 0xF76B15, 0x00B4A0
    };

    int ids[DEMO_BALLS], px[DEMO_BALLS], py[DEMO_BALLS], vx[DEMO_BALLS], vy[DEMO_BALLS], dim[DEMO_BALLS];
    demo_rng = system_ticks * 2654435761u + 1;
    int made = 0;
    for (int i = 0; i < DEMO_BALLS; i++) {
        dim[made] = 32 + (int)(demo_rand() % 5) * 8;        /* 32..64 px */
        ids[made] = sprite_create(dim[made], dim[made]);
        if (ids[made] < 0) break;
        sprite_paint_ball(ids[made], palette[i]);
        sprite_set_z(ids[made], dim[made]);                  /* big = near = on top */
        px[made] = (int)(demo_rand() % (uint32_t)(sw - dim[made]));
        py[made] = (int)(demo_rand() % (uint32_t)(sh - dim[made]));
        vx[made] = ((int)(demo_rand() % 5) + 2) * ((demo_rand() & 1) ? 1 : -1);
        vy[made] = ((int)(demo_rand() % 5) + 2) * ((demo_rand() & 1) ? 1 : -1);
        made++;
    }
    if (made == 0) { vga_print("  sprite pool/heap exhausted\n"); return; }

    /* Animate until a key is pressed (or ~30s safety cap). */
    uint32_t frames = 0, fps = 0, win_frames = 0;
    uint32_t t_start = system_ticks, win_start = system_ticks;
    char hud[96], b[12];

    while ((system_ticks - t_start) < 545) {                 /* ~30 s cap */
        if (keyboard_has_key()) { keyboard_getchar(); break; }

        /* Vertical gradient backdrop in 4px bands (rep stosl fills). */
        for (int y = 0; y < sh; y += 4) {
            uint32_t r = 10 + (uint32_t)y * 50 / (uint32_t)sh;
            uint32_t g = 8  + (uint32_t)y * 14 / (uint32_t)sh;
            uint32_t bl = 42 + (uint32_t)y * 58 / (uint32_t)sh;
            gpu_fill(0, y, sw, 4, FB_RGB(r, g, bl));
        }

        for (int i = 0; i < made; i++) {
            px[i] += vx[i]; py[i] += vy[i];
            if (px[i] <= 0)             { px[i] = 0;             vx[i] = -vx[i]; }
            if (px[i] >= sw - dim[i])   { px[i] = sw - dim[i];   vx[i] = -vx[i]; }
            if (py[i] <= 0)             { py[i] = 0;             vy[i] = -vy[i]; }
            if (py[i] >= sh - dim[i])   { py[i] = sh - dim[i];   vy[i] = -vy[i]; }
            sprite_move(ids[i], px[i], py[i]);
        }
        sprite_composite();

        hud[0] = '\0';
        strcat(hud, "Phase 41 sprite engine   ");
        int_to_str(made, b); strcat(hud, b);
        strcat(hud, " sprites   ");
        int_to_str((int)fps, b); strcat(hud, b);
        strcat(hud, " fps   (any key to exit)");
        gfx_draw_text(16, 12, hud, FB_RGB(235, 235, 245), FB_RGB(10, 8, 42));

        fb_flip();
        frames++; win_frames++;

        /* Refresh the fps readout every ~0.5s. */
        if ((system_ticks - win_start) >= 9) {
            fps = win_frames * 1000 / ((system_ticks - win_start) * 55);
            win_frames = 0; win_start = system_ticks;
        }
    }

    uint32_t total_ms = (system_ticks - t_start) * 55;
    if (total_ms == 0) total_ms = 1;
    sprite_destroy_all();
    vga_clear();

    vga_print_color("\n  Sprite demo - Phase 41\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ======================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    print_label_num("  Sprites:  ", (uint32_t)made, " alpha-blended, z-ordered\n");
    print_label_num("  Frames:   ", frames, "");
    print_label_num("  in ", total_ms, " ms = ");
    char fb_[12]; int_to_str((int)(frames * 1000 / total_ms), fb_);
    vga_print_color(fb_, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(" fps average\n");
    vga_print_color("  Pipeline: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print(gpu_active() ? "gpu_fill -> sprite_composite -> VirtIO-GPU flush (zero-copy)\n\n"
                           : "gpu_fill -> sprite_composite -> VESA memcpy flip\n\n");
}

/* --------------------------------------------------------------------------
 * Phase 42: Gaming Framework commands
 * -------------------------------------------------------------------------- */
static void cmd_gameinfo(void) {
    char b[12];
    vga_print_color("\n  Gaming Framework - Phase 42\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ===========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    vga_print_color("  API:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("NexusSDL (game.h) - SDL-like kernel game API\n");
    vga_print_color("  Video:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("256-color surface up to 320x240, integer-scaled to\n");
    vga_print("            1024x768 via ");
    vga_print(gpu_active() ? "VirtIO-GPU dirty-rect presents\n"
                           : "VESA flip (GPU inactive)\n");
    vga_print_color("  Input:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)gamepad_name());
    vga_print(" - raw scancode hook, held-state + edges\n");
    vga_print_color("  Timing:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("PIT tick-locked frames (~18 fps, deterministic 55 ms)\n");
    vga_print_color("  Math:     ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("Q16.16 fixed point, idiv-based div, 1024-step LUT trig\n");
    vga_print_color("  Sound:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("non-blocking speaker sfx + AC'97 jingles (audio.h)\n");

    vga_print_color("\n  Pad map:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    for (int i = 0; i < PAD_BTN_COUNT; i++) {
        vga_print((char*)gamepad_btn_name(i));
        vga_print("=");
        vga_print((char*)gamepad_btn_keys(i));
        if (i == 5) vga_print("\n            ");
        else if (i < PAD_BTN_COUNT - 1) vga_print("  ");
    }
    vga_print("\n");

    vga_print_color("\n  Status:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print((char*)game_status()); vga_print("\n");
    vga_print_color("  Games:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str((int)game_sessions(), b); vga_print(b);
    vga_print(" played this boot\n");
    vga_print_color("\n  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("gamepad (input test)   doom (FPS)   breakout\n\n");
}

/* ============================================================================
 * Phase 43: Accessibility commands
 * ============================================================================ */

static void cmd_accinfo(void) {
    char b[12];
    vga_print_color("\n  Accessibility - Phase 43\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ========================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    vga_print_color("  Screen reader: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print(accessibility_reader_on() ? "ON  (PC-speaker earcons + spell-out)\n"
                                        : "off (PC-speaker earcons + spell-out)\n");
    vga_print_color("  High contrast: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print(accessibility_contrast_on() ? "ON  (hicon theme)\n" : "off (hicon theme)\n");
    vga_print_color("  Text scale:    ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    int_to_str(accessibility_font_scale(), b); vga_print(b); vga_print("x  (1-4)\n");

    vga_print_color("\n  Hotkeys (global):\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("    Alt+Shift+C  toggle high contrast\n");
    vga_print("    Alt+Shift+S  toggle screen reader\n");
    vga_print("    Alt+Shift+=  text larger    Alt+Shift+-  text smaller\n");
    vga_print("    Alt+Shift+A  announce status\n");
    vga_print_color("\n  Commands: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("fontsize <1-4>  contrast [on|off]  reader <on|off>  say <text>\n\n");
}

static void cmd_fontsize(int argc, char* argv[]) {
    if (argc < 2) {
        char b[12];
        vga_print("  Usage: fontsize <1-4>\n  Current: ");
        int_to_str(font_get_scale(), b); vga_print(b); vga_print("x\n");
        return;
    }
    int n = 0;
    for (int i = 0; argv[1][i]; i++) {
        if (argv[1][i] < '0' || argv[1][i] > '9') { n = -1; break; }
        n = n * 10 + (argv[1][i] - '0');
    }
    if (n < 1 || n > 4) {
        vga_print_color("  Scale must be 1-4.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        return;
    }
    font_set_scale(n);
    vga_print_color("  Text scale set to ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    char b[12]; int_to_str(n, b); vga_print(b); vga_print("x (affects desktop/GUI text)\n");
}

static void cmd_contrast(int argc, char* argv[]) {
    if (argc >= 2) {
        bool want_on = (strcmp(argv[1], "on") == 0);
        bool want_off = (strcmp(argv[1], "off") == 0);
        if (!want_on && !want_off) { vga_print("  Usage: contrast [on|off]\n"); return; }
        if (want_on != accessibility_contrast_on()) accessibility_toggle_contrast();
    } else {
        accessibility_toggle_contrast();
    }
    vga_print_color("  High contrast: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(accessibility_contrast_on() ? "ON\n" : "off\n");
}

static void cmd_reader(int argc, char* argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "on") == 0) accessibility_set_reader(true);
        else if (strcmp(argv[1], "off") == 0) accessibility_set_reader(false);
        else { vga_print("  Usage: reader <on|off>\n"); return; }
    } else {
        accessibility_set_reader(!accessibility_reader_on());
    }
    vga_print_color("  Screen reader: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print(accessibility_reader_on() ? "ON\n" : "off\n");
}

static void cmd_say(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: say <text>\n"); return; }
    /* Reassemble the words into one phrase, then spell it audibly. */
    static char phrase[128];
    int p = 0;
    for (int i = 1; i < argc && p < (int)sizeof(phrase) - 1; i++) {
        if (i > 1 && p < (int)sizeof(phrase) - 1) phrase[p++] = ' ';
        for (int j = 0; argv[i][j] && p < (int)sizeof(phrase) - 1; j++)
            phrase[p++] = argv[i][j];
    }
    phrase[p] = '\0';
    vga_print_color("  Speaking: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print(phrase); vga_print("\n");
    acc_say_force(phrase);   /* always demonstrates, even if reader master is off */
}

/* --- Phase 46: AI Assistant -------------------------------------------------
 * Reassemble argv[from..] into one phrase (the assistant takes free text). */
static void shell_join_args(int argc, char* argv[], int from, char* out, int max) {
    int p = 0;
    for (int i = from; i < argc && p < max - 1; i++) {
        if (i > from && p < max - 1) out[p++] = ' ';
        for (int j = 0; argv[i][j] && p < max - 1; j++) out[p++] = argv[i][j];
    }
    out[p] = '\0';
}

static void cmd_ai(int argc, char* argv[]) {
    static char topic[160];
    shell_join_args(argc, argv, 1, topic, sizeof(topic));
    assistant_help(topic[0] ? topic : 0);
}

static void cmd_ask(int argc, char* argv[]) {
    if (argc < 2) {
        vga_print("  Usage: ask <what you want in plain English>\n");
        vga_print("  e.g.  ask show me the files    ask what time is it\n");
        return;
    }
    static char q[200];
    shell_join_args(argc, argv, 1, q, sizeof(q));
    assistant_ask(q, true);   /* true = run the resolved command if confident */
}

static void cmd_find(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: find <query>\n"); return; }
    static char q[160];
    shell_join_args(argc, argv, 1, q, sizeof(q));
    assistant_find(q);
}

/* --- Phase 47: Mobile / Embedded mode -------------------------------------- */
static void cmd_gesture(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        mobile_set_touch(true);
        vga_print_color("  Touch mode ON", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(" — drag/tap/swipe with the mouse; run 'gesture' to see the last one.\n");
        if (!fb_is_vesa()) vga_print_color("  (best in the desktop/GUI; needs VESA mouse)\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        mobile_set_touch(false);
        vga_print_color("  Touch mode OFF\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    } else {
        char n[12];
        vga_print_color("\n  Touch / gestures\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print("  Mode: "); vga_print(mobile_touch_on() ? "ON" : "OFF");
        vga_print("   Last gesture: ");
        vga_print_color(mobile_gesture_name(mobile_last_gesture()), VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print("\n  Recognized this session: ");
        int_to_str((int)mobile_gesture_count(), n); vga_print(n);
        vga_print("\n  Usage: gesture on | off   (then tap/drag/swipe; gestures: tap,\n");
        vga_print("         long-press, drag, swipe-up/down/left/right)\n\n");
    }
}

static void cmd_orientation(int argc, char* argv[]) {
    if (argc >= 2 && (strcmp(argv[1],"portrait")==0 || strcmp(argv[1],"p")==0)) {
        mobile_set_orientation(ORIENT_PORTRAIT);
    } else if (argc >= 2 && (strcmp(argv[1],"landscape")==0 || strcmp(argv[1],"l")==0)) {
        mobile_set_orientation(ORIENT_LANDSCAPE);
    } else if (argc >= 2 && strcmp(argv[1],"toggle")==0) {
        mobile_set_orientation(mobile_is_portrait() ? ORIENT_LANDSCAPE : ORIENT_PORTRAIT);
    } else if (argc >= 2) {
        vga_print("  Usage: orientation portrait|landscape|toggle\n"); return;
    }
    char a[12], b[12];
    vga_print_color("\n  Orientation: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color(mobile_is_portrait() ? "PORTRAIT" : "LANDSCAPE",
                    VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("\n  Logical screen: ");
    int_to_str(mobile_screen_w(), a); int_to_str(mobile_screen_h(), b);
    vga_print(a); vga_print(" x "); vga_print(b); vga_print("\n");
    vga_print_color("  (logical model — layout-aware apps read mobile_screen_w/h;\n",
                    VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print_color("   the physical framebuffer is not rotated)\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
}

static void cmd_lowpower(int argc, char* argv[]) {
    if (argc >= 2 && strcmp(argv[1],"on")==0)  mobile_set_lowpower(true);
    else if (argc >= 2 && strcmp(argv[1],"off")==0) mobile_set_lowpower(false);
    else if (argc >= 2) { vga_print("  Usage: lowpower on|off\n"); return; }
    char n[12];
    vga_print_color("\n  Low-power mode: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color(mobile_lowpower_on() ? "ON" : "OFF",
                    mobile_lowpower_on() ? VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK)
                                         : VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("\n  Idle redraw interval: ");
    int_to_str(mobile_redraw_interval(), n); vga_print(n); vga_print(" ticks");
    vga_print(mobile_lowpower_on() ? "  (~4s — CPU stays in HLT longer)\n\n"
                                   : "  (~1s — normal)\n\n");
}

static void cmd_arm(void) { mobile_arm_status(); }

static void cmd_mobileinfo(void) {
    char a[12], b[12];
    vga_print_color("\n  Mobile / Embedded Mode (Phase 47)\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  =================================\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Touch mode:   "); vga_print(mobile_touch_on() ? "ON" : "OFF");
    vga_print("   (gestures: tap/long-press/drag/swipe)\n");
    vga_print("  Last gesture: "); vga_print(mobile_gesture_name(mobile_last_gesture()));
    vga_print("   count="); int_to_str((int)mobile_gesture_count(), a); vga_print(a); vga_print("\n");
    vga_print("  Orientation:  "); vga_print(mobile_is_portrait() ? "portrait" : "landscape");
    vga_print("   logical "); int_to_str(mobile_screen_w(), a); int_to_str(mobile_screen_h(), b);
    vga_print(a); vga_print("x"); vga_print(b); vga_print("\n");
    vga_print("  Low-power:    "); vga_print(mobile_lowpower_on() ? "ON" : "OFF");
    vga_print("   redraw every "); int_to_str(mobile_redraw_interval(), a); vga_print(a); vga_print(" ticks\n");
    vga_print("  Build target: i686-elf (x86-32) — see 'arm' for ARM port notes\n");
    vga_print_color("  Commands: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("gesture  orientation  lowpower  arm\n\n");
}

/* --- Phase 48: Performance ------------------------------------------------- */
static void cmd_perf(void)    { perf_run(); }
static void cmd_smp(void)     { perf_smp_status(); }
static void cmd_preempt(void) { perf_preempt_status(); }

/* --- Phase 49: App Store & Ecosystem --------------------------------------- *
 * Front-end over the package manager (pkg.c). Subcommands:
 *   store                 featured rail (default)
 *   store list            all apps, grouped by category
 *   store category <c>    system|productivity|games|other
 *   store search <term>   name/description/tagline match
 *   store info <app>      detail page (rating, deps, sandbox, status)
 *   store install <app>   install via the storefront (applies sandbox profile)
 *   store update          check for updates (re-probe + version compare)
 *   store upgrade         upgrade every outdated app
 *   store sandbox <app>   show the cooperative sandbox profile
 *   store sdk [install]   developer SDK docs / scaffold into the filesystem
 */
static void store_usage(void) {
    vga_print_color("\n  store — NexusOS App Store (Phase 49)\n",
                    VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  store", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("                  featured apps\n");
    vga_print_color("  store list", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("             browse all apps by category\n");
    vga_print_color("  store category <c>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("     system | productivity | games | other\n");
    vga_print_color("  store search <term>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("    search the catalog\n");
    vga_print_color("  store info <app>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("       detail page\n");
    vga_print_color("  store install <app>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("    install (applies sandbox profile)\n");
    vga_print_color("  store update", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("           check for app updates\n");
    vga_print_color("  store upgrade", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("          upgrade all outdated apps\n");
    vga_print_color("  store sandbox <app>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("    show the cooperative sandbox profile\n");
    vga_print_color("  store sdk [install]", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("    developer SDK docs / scaffold\n\n");
}

static void cmd_store(int argc, char* argv[]) {
    if (argc < 2) { appstore_show_featured(); return; }

    const char* sub = argv[1];
    if (strcmp(sub, "help") == 0) {
        store_usage();
    } else if (strcmp(sub, "list") == 0) {
        appstore_show_all();
    } else if (strcmp(sub, "featured") == 0) {
        appstore_show_featured();
    } else if (strcmp(sub, "category") == 0) {
        if (argc < 3) { vga_print("  Usage: store category <system|productivity|games|other>\n"); return; }
        const char* c = argv[2];
        if      (strcmp(c, "system") == 0)       appstore_show_category(APP_CAT_SYSTEM);
        else if (strcmp(c, "productivity") == 0) appstore_show_category(APP_CAT_PRODUCTIVITY);
        else if (strcmp(c, "games") == 0)        appstore_show_category(APP_CAT_GAMES);
        else if (strcmp(c, "other") == 0)        appstore_show_category(APP_CAT_OTHER);
        else vga_print("  Categories: system, productivity, games, other\n");
    } else if (strcmp(sub, "search") == 0) {
        if (argc < 3) { vga_print("  Usage: store search <term>\n"); return; }
        appstore_search(argv[2]);
    } else if (strcmp(sub, "info") == 0) {
        if (argc < 3) { vga_print("  Usage: store info <app>\n"); return; }
        appstore_show_detail(argv[2]);
    } else if (strcmp(sub, "install") == 0) {
        if (argc < 3) { vga_print("  Usage: store install <app>\n"); return; }
        vga_print("\n");
        appstore_install(argv[2]);
        vga_print("\n");
    } else if (strcmp(sub, "update") == 0) {
        appstore_check_updates();
    } else if (strcmp(sub, "upgrade") == 0) {
        appstore_upgrade_all();
    } else if (strcmp(sub, "sandbox") == 0) {
        if (argc < 3) { vga_print("  Usage: store sandbox <app>\n"); return; }
        appstore_sandbox_info(argv[2]);
    } else if (strcmp(sub, "sdk") == 0) {
        if (argc >= 3 && strcmp(argv[2], "install") == 0) appstore_sdk_install();
        else appstore_sdk_docs();
    } else {
        vga_print_color("  Unknown store subcommand: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)sub); vga_print("\n");
        store_usage();
    }
}

/* --- Phase 50: v5.0 Grand Finale ------------------------------------------- *
 * urun <file>      universal binary launcher (PE/ELF/Mach-O auto-detect)
 * install [disk yes]  disk installer (dry-run plan, or real MBR+system write)
 * finale | v5      the v5.0 release banner + footprint report card
 */
static void cmd_urun(int argc, char* argv[]) {
    if (argc < 2) { vga_print("  Usage: urun <file>   (PE/ELF/Mach-O)\n"); return; }
    finale_urun(argv[1]);
}

static void cmd_install(int argc, char* argv[]) {
    /* `install` or `install plan` = dry run; `install disk yes` = real write. */
    if (argc >= 3 && strcmp(argv[1], "disk") == 0 && strcmp(argv[2], "yes") == 0) {
        finale_install_disk(true);
    } else if (argc >= 2 && strcmp(argv[1], "disk") == 0) {
        vga_print_color("\n  Confirm with 'install disk yes' to write to the IDE disk.\n",
                        VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        finale_install_plan();
    } else {
        finale_install_plan();
    }
}

static void cmd_finale(void) { finale_show(); }

/* --- Phase 51: NPFS journaling filesystem ---------------------------------- *
 * npfs                 status / stats
 * npfs format          lay down a fresh filesystem (explicit; never automatic)
 * npfs mount           mount an existing filesystem + replay the journal
 * npfs ls              list files
 * npfs new <name>      create an empty file
 * npfs write <n> <txt> write text to a file (journaled)
 * npfs cat <name>      read a file back
 * npfs rm <name>       delete a file (journaled)
 * npfs stat            superblock + usage report
 * npfs journal         journal head/tail + pending-txn status
 * npfs crashtest       prove the journal recovers from a simulated crash
 */
static void npfs_usage(void) {
    vga_print_color("\n  npfs — NexusOS Persistent File System (Phase 51)\n",
                    VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  npfs format", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("            create a fresh journaling FS (explicit)\n");
    vga_print_color("  npfs mount", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("             mount existing FS + replay journal\n");
    vga_print_color("  npfs ls", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("                list files\n");
    vga_print_color("  npfs new <name>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("        create an empty file\n");
    vga_print_color("  npfs write <n> <txt>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("   write text (journaled)\n");
    vga_print_color("  npfs cat <name>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("        read a file\n");
    vga_print_color("  npfs rm <name>", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("         delete a file (journaled)\n");
    vga_print_color("  npfs stat", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("              superblock + usage report\n");
    vga_print_color("  npfs journal", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("           journal status (pending txn?)\n");
    vga_print_color("  npfs crashtest", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
    vga_print("         prove journal crash recovery\n\n");
}

static void cmd_npfs(int argc, char* argv[]) {
    if (argc < 2) {
        if (npfs_is_mounted()) npfs_show_stats();
        else npfs_usage();
        return;
    }
    const char* sub = argv[1];

    if (strcmp(sub, "help") == 0) {
        npfs_usage();
    } else if (strcmp(sub, "format") == 0) {
        int r = npfs_format();
        if (r == NPFS_OK) {
            vga_print_color("\n  NPFS formatted. ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            vga_print("Journaling filesystem ready.\n\n");
        } else {
            vga_print_color("  format failed: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print((char*)npfs_strerror(r)); vga_print("\n");
        }
    } else if (strcmp(sub, "mount") == 0) {
        int r = npfs_mount();
        if (r == NPFS_OK) vga_print_color("  NPFS mounted.\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        else { vga_print_color("  mount failed: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
               vga_print((char*)npfs_strerror(r)); vga_print("\n"); }
    } else if (strcmp(sub, "ls") == 0) {
        npfs_list();
    } else if (strcmp(sub, "stat") == 0) {
        npfs_show_stats();
    } else if (strcmp(sub, "journal") == 0) {
        npfs_journal_status();
    } else if (strcmp(sub, "crashtest") == 0) {
        npfs_crashtest();
    } else if (strcmp(sub, "new") == 0) {
        if (argc < 3) { vga_print("  Usage: npfs new <name>\n"); return; }
        int r = npfs_create(argv[2]);
        if (r == NPFS_OK) { vga_print("  Created "); vga_print(argv[2]); vga_print("\n"); }
        else { vga_print_color("  error: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
               vga_print((char*)npfs_strerror(r)); vga_print("\n"); }
    } else if (strcmp(sub, "write") == 0) {
        if (argc < 4) { vga_print("  Usage: npfs write <name> <text>\n"); return; }
        char text[256];
        shell_join_args(argc, argv, 3, text, sizeof(text));
        /* Auto-create if missing, then write. */
        if (npfs_create(argv[2]) == NPFS_ERR_TOOBIG) {
            vga_print("  name too long.\n"); return;
        }
        int r = npfs_write(argv[2], (const uint8_t*)text, (uint32_t)strlen(text));
        if (r == NPFS_OK) {
            vga_print("  Wrote "); { char b[12]; int_to_str((int)strlen(text), b); vga_print(b); }
            vga_print(" bytes to "); vga_print(argv[2]); vga_print(" (journaled)\n");
        } else { vga_print_color("  error: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                 vga_print((char*)npfs_strerror(r)); vga_print("\n"); }
    } else if (strcmp(sub, "cat") == 0) {
        if (argc < 3) { vga_print("  Usage: npfs cat <name>\n"); return; }
        static uint8_t buf[8192];
        uint32_t got = 0;
        int r = npfs_read(argv[2], buf, sizeof(buf) - 1, &got);
        if (r != NPFS_OK) { vga_print_color("  error: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                            vga_print((char*)npfs_strerror(r)); vga_print("\n"); return; }
        buf[got] = '\0';
        vga_print("\n"); vga_print((char*)buf); vga_print("\n\n");
    } else if (strcmp(sub, "rm") == 0) {
        if (argc < 3) { vga_print("  Usage: npfs rm <name>\n"); return; }
        int r = npfs_delete(argv[2]);
        if (r == NPFS_OK) { vga_print("  Deleted "); vga_print(argv[2]); vga_print("\n"); }
        else { vga_print_color("  error: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
               vga_print((char*)npfs_strerror(r)); vga_print("\n"); }
    } else {
        vga_print_color("  Unknown npfs subcommand: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print((char*)sub); vga_print("\n");
        npfs_usage();
    }
}

static void cmd_gamepad(void) {
    if (!fb_is_vesa()) { vga_print("  gamepad needs VESA mode.\n"); return; }
    if (!game_open(320, 200, "Game Controller Test - Phase 42 (Esc quits)")) {
        vga_print("  gamepad: could not open game surface.\n");
        return;
    }

    /* button box layout: x, y, w, h per PAD_* id (SNES-ish arrangement) */
    static const int box[PAD_BTN_COUNT][4] = {
        {  52,  62, 28, 22 },   /* UP     */
        {  52, 110, 28, 22 },   /* DOWN   */
        {  20,  86, 28, 22 },   /* LEFT   */
        {  84,  86, 28, 22 },   /* RIGHT  */
        { 272,  86, 28, 22 },   /* A      */
        { 238, 110, 28, 22 },   /* B      */
        { 238,  62, 28, 22 },   /* X      */
        { 204,  86, 28, 22 },   /* Y      */
        {  20,  30, 44, 16 },   /* L      */
        { 256,  30, 44, 16 },   /* R      */
        { 174, 150, 52, 16 },   /* START  */
        {  94, 150, 52, 16 },   /* SELECT */
    };

    uint32_t events = 0;
    uint32_t t_start = game_ms();
    pad_event_t ev;
    char b[12], line[36];

    while ((game_ms() - t_start) < 60000) {
        gamepad_poll();
        if (gamepad_key_held(0x01, false)) break;      /* Esc exits */
        while (gamepad_next_event(&ev)) events++;

        game_clear(game_ramp(RAMP_GRAY, 15));
        game_text_center(8, "12-BUTTON VIRTUAL GAME PAD", 15);
        game_text_center(18, "PRESS KEYS - BOXES LIGHT UP - ESC EXITS", 7);

        for (int i = 0; i < PAD_BTN_COUNT; i++) {
            bool on = gamepad_held(i);
            uint8_t fill = on ? game_ramp(RAMP_GREEN, 2) : game_ramp(RAMP_GRAY, 12);
            uint8_t edge = on ? game_ramp(RAMP_GREEN, 0) : game_ramp(RAMP_GRAY, 8);
            game_fill(box[i][0], box[i][1], box[i][2], box[i][3], fill);
            game_rect(box[i][0], box[i][1], box[i][2], box[i][3], edge);
            int tx = box[i][0] + (box[i][2] - (int)strlen(gamepad_btn_name(i)) * 8) / 2;
            game_text(tx, box[i][1] + (box[i][3] - 8) / 2,
                      gamepad_btn_name(i), on ? 0 : 15);
        }

        strcpy(line, "HELD ");
        int held = 0;
        for (int i = 0; i < PAD_BTN_COUNT; i++) if (gamepad_held(i)) held++;
        int_to_str(held, b); strcat(line, b);
        strcat(line, "   EVENTS "); int_to_str((int)events, b); strcat(line, b);
        game_text(20, 180, line, 14);
        strcpy(line, "FPS "); int_to_str((int)game_fps(), b); strcat(line, b);
        game_text(264, 180, line, 7);

        game_present();
        game_sync();
    }

    game_close();
    vga_clear();
    vga_print_color("\n  Game controller test - Phase 42\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    vga_print_color("  ===============================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
    vga_print("  Source:   "); vga_print((char*)gamepad_name()); vga_print("\n");
    print_label_num("  Events:   ", events, " press/release transitions captured\n");
    vga_print("\n");
}

/* --------------------------------------------------------------------------
 * shell_pipe_redirect: Handle |, >, < operators
 * Returns true if handled (caller should not execute_command)
 * -------------------------------------------------------------------------- */
static bool shell_pipe_redirect(char* input) {
    /* Check for > redirect */
    char* redir = NULL;
    for (int i = 0; input[i]; i++) {
        if (input[i] == '>' && (i == 0 || input[i-1] != '\\')) { redir = &input[i]; break; }
    }
    if (redir) {
        /* Split at > */
        *redir = '\0';
        char* filename = redir + 1;
        while (*filename == ' ') filename++;
        /* Trim trailing spaces from filename */
        int flen = strlen(filename);
        while (flen > 0 && filename[flen-1] == ' ') filename[--flen] = '\0';
        if (!*filename) { vga_print("  No output file specified\n"); return true; }

        /* Capture command output */
        posix_capture_start();
        /* Execute the left side command by temporarily restoring vga_print behavior */
        /* We use a trick: run the command, and intercept its vga_print output */
        char* argv[MAX_ARGS];
        int argc = parse_args(input, argv);
        if (argc > 0) {
            /* Simple: re-execute just the echo part into capture */
            for (int i = 0; i < argc; i++) {
                if (i > 0) posix_capture_write(" ");
                posix_capture_write(argv[i]);
            }
        }
        posix_capture_stop();

        /* Write captured output to file */
        int cap_len;
        const char* cap = posix_capture_get(&cap_len);
        fs_node_t* root = vfs_get_root();
        fs_node_t* node = vfs_finddir(root, filename);
        if (!node) node = ramfs_create(filename, FS_FILE);
        if (node && cap_len > 0) {
            vfs_write(node, 0, cap_len, (uint8_t*)cap);
            vga_print_color("  Written to ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            vga_print(filename); vga_print("\n");
        }
        posix_capture_clear();
        return true;
    }

    /* Check for | pipe (simplified: only handles cmd | grep pattern) */
    char* pipe = NULL;
    for (int i = 0; input[i]; i++) {
        if (input[i] == '|' && (i == 0 || input[i-1] != '\\')) { pipe = &input[i]; break; }
    }
    if (pipe) {
        *pipe = '\0';
        char* right = pipe + 1;
        while (*right == ' ') right++;

        /* For now: capture left side output, then grep it with right side */
        /* Only supports: <cmd> | grep <pattern> */
        char* rargv[MAX_ARGS];
        int rargc = parse_args(right, rargv);
        if (rargc < 2 || strcmp(rargv[0], "grep") != 0) {
            vga_print("  Pipe only supports: <cmd> | grep <pattern>\n");
            return true;
        }
        const char* pattern = rargv[1];

        /* We can't easily capture vga_print output without modifying vga.c,
         * so we handle known commands specially */
        char cap[4096]; int clen = 0;

        char* largv[MAX_ARGS];
        int largc = parse_args(input, largv);
        if (largc > 0 && strcmp(largv[0], "ls") == 0) {
            fs_node_t* root = vfs_get_root();
            fs_node_t* entry; uint32_t idx = 0;
            while ((entry = vfs_readdir(root, idx)) != NULL) {
                int nl = strlen(entry->name);
                if (clen + nl + 1 < 4095) {
                    memcpy(cap + clen, entry->name, nl); clen += nl;
                    cap[clen++] = '\n';
                }
                idx++;
            }
            cap[clen] = '\0';
        } else if (largc > 0 && strcmp(largv[0], "ps") == 0) {
            process_t* table = process_get_table();
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (table[i].state != PROC_UNUSED) {
                    char nb[12]; int_to_str(table[i].pid, nb);
                    int nl = strlen(nb); memcpy(cap + clen, nb, nl); clen += nl;
                    cap[clen++] = ' ';
                    nl = strlen(table[i].name); memcpy(cap + clen, table[i].name, nl); clen += nl;
                    cap[clen++] = '\n';
                }
            }
            cap[clen] = '\0';
        } else if (largc > 0 && strcmp(largv[0], "env") == 0) {
            const char* vars[] = {"PATH", "HOME", "USER", "SHELL", "TERM", "PWD", NULL};
            for (int i = 0; vars[i]; i++) {
                const char* val = env_get(vars[i]);
                if (val) {
                    int kl = strlen(vars[i]); memcpy(cap + clen, vars[i], kl); clen += kl;
                    cap[clen++] = '=';
                    int vl = strlen(val); memcpy(cap + clen, val, vl); clen += vl;
                    cap[clen++] = '\n';
                }
            }
            cap[clen] = '\0';
        } else if (largc > 1 && strcmp(largv[0], "cat") == 0) {
            const char* fn = largv[1];
            if (procfs_is_proc(fn)) {
                clen = procfs_read(fn, cap, sizeof(cap));
                if (clen < 0) clen = 0;
            } else {
                fs_node_t* node = vfs_finddir(vfs_get_root(), fn);
                if (node) {
                    clen = vfs_read(node, 0, node->size < 4095 ? node->size : 4095, (uint8_t*)cap);
                    cap[clen] = '\0';
                }
            }
        } else {
            vga_print("  Pipe: unsupported left command\n");
            return true;
        }

        /* Now grep the captured output */
        char* p = cap;
        int count = 0;
        while (*p) {
            char line[256]; int li = 0;
            while (*p && *p != '\n' && li < 255) line[li++] = *p++;
            line[li] = '\0';
            if (*p == '\n') p++;
            if (strstr(line, pattern)) {
                vga_print("  ");
                char* lp = line;
                while (*lp) {
                    char* m = strstr(lp, pattern);
                    if (m) {
                        while (lp < m) vga_putchar(*lp++);
                        vga_print_color(pattern, VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                        lp = m + strlen(pattern);
                    } else { vga_print(lp); break; }
                }
                vga_putchar('\n');
                count++;
            }
        }
        if (count == 0) vga_print_color("  No matches\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        return true;
    }

    return false; /* Not a pipe/redirect — execute normally */
}

/* --------------------------------------------------------------------------
 * execute_command: Parse and execute a command
 * -------------------------------------------------------------------------- */
static void execute_command(char* input) {
    /* Check for pipe/redirect operators first */
    char saved[INPUT_MAX]; strcpy(saved, input);
    if (shell_pipe_redirect(saved)) return;

    char* argv[MAX_ARGS];
    int argc = parse_args(input, argv);
    if (argc == 0) return;

    if      (strcmp(argv[0], "help")     == 0) cmd_help();
    else if (strcmp(argv[0], "clear")    == 0) cmd_clear();
    else if (strcmp(argv[0], "about")    == 0) cmd_about();
    else if (strcmp(argv[0], "meminfo")  == 0) cmd_meminfo();
    else if (strcmp(argv[0], "heapinfo") == 0) cmd_heapinfo();
    else if (strcmp(argv[0], "uptime")   == 0) cmd_uptime();
    else if (strcmp(argv[0], "date")     == 0) cmd_date();
    else if (strcmp(argv[0], "reboot")   == 0) cmd_reboot();
    else if (strcmp(argv[0], "echo")     == 0) cmd_echo(argc, argv);
    else if (strcmp(argv[0], "color")    == 0) cmd_color(argc, argv);
    else if (strcmp(argv[0], "history")  == 0) cmd_history_show();
    else if (strcmp(argv[0], "ls")       == 0) cmd_ls(argc, argv);
    else if (strcmp(argv[0], "cat")      == 0) cmd_cat(argc, argv);
    else if (strcmp(argv[0], "head")     == 0) cmd_head(argc, argv);
    else if (strcmp(argv[0], "tail")     == 0) cmd_tail(argc, argv);
    else if (strcmp(argv[0], "grep")     == 0) cmd_grep(argc, argv);
    else if (strcmp(argv[0], "wc")       == 0) cmd_wc(argc, argv);
    else if (strcmp(argv[0], "touch")    == 0) cmd_touch(argc, argv);
    else if (strcmp(argv[0], "write")    == 0) cmd_write(argc, argv);
    else if (strcmp(argv[0], "rm")       == 0) cmd_rm(argc, argv);
    else if (strcmp(argv[0], "edit")     == 0) cmd_edit(argc, argv);
    else if (strcmp(argv[0], "ps")       == 0) cmd_ps();
    else if (strcmp(argv[0], "run")      == 0) cmd_run(argc, argv);
    else if (strcmp(argv[0], "kill")     == 0) cmd_kill(argc, argv);
    else if (strcmp(argv[0], "snake")    == 0) cmd_snake();
    else if (strcmp(argv[0], "beep")     == 0) cmd_beep();
    else if (strcmp(argv[0], "gui")      == 0) cmd_gui();
    else if (strcmp(argv[0], "theme")    == 0) cmd_theme(argc, argv);
    else if (strcmp(argv[0], "calc")     == 0) cmd_gui();
    else if (strcmp(argv[0], "files")    == 0) cmd_gui();
    else if (strcmp(argv[0], "sysmon")   == 0) cmd_gui();
    else if (strcmp(argv[0], "settings") == 0) cmd_gui();
    else if (strcmp(argv[0], "notepad")  == 0) cmd_gui();
    else if (strcmp(argv[0], "taskmgr")  == 0) cmd_gui();
    else if (strcmp(argv[0], "calendar") == 0) cmd_gui();
    else if (strcmp(argv[0], "music")    == 0) cmd_gui();
    else if (strcmp(argv[0], "paint")    == 0) cmd_gui();
    else if (strcmp(argv[0], "help2")    == 0) cmd_gui();
    else if (strcmp(argv[0], "minesweeper") == 0) cmd_gui();
    else if (strcmp(argv[0], "sysinfo")  == 0) cmd_gui();
    else if (strcmp(argv[0], "todo")     == 0) cmd_gui();
    else if (strcmp(argv[0], "pong")     == 0) cmd_gui();
    else if (strcmp(argv[0], "search")   == 0) cmd_gui();
    else if (strcmp(argv[0], "tetris")   == 0) cmd_gui();
    else if (strcmp(argv[0], "hexview")  == 0) cmd_gui();
    else if (strcmp(argv[0], "contacts") == 0) cmd_gui();
    else if (strcmp(argv[0], "colors")   == 0) cmd_gui();
    else if (strcmp(argv[0], "env")      == 0) cmd_env();
    else if (strcmp(argv[0], "export")   == 0) cmd_export(argc, argv);
    else if (strcmp(argv[0], "uname")    == 0) cmd_uname();
    else if (strcmp(argv[0], "whoami")   == 0) cmd_whoami();
    else if (strcmp(argv[0], "log") == 0) {
        vga_print_color("  System Log: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        char n[4]; int_to_str(syslog_count(), n); vga_print(n); vga_print(" entries\n");
    }
    else if (strcmp(argv[0], "trash") == 0) {
        vga_print_color("  Recycle Bin: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        char n[4]; int_to_str(recycle_count(), n); vga_print(n); vga_print(" items\n");
    }
    else if (strcmp(argv[0], "restore") == 0) {
        if (recycle_count() > 0) { recycle_restore(0); vga_print("  Restored.\n"); }
        else vga_print("  Trash empty.\n");
    }
    else if (strcmp(argv[0], "net") == 0) {
        cmd_net();
    }
    else if (strcmp(argv[0], "ifconfig") == 0) {
        cmd_ifconfig();
    }
    else if (strcmp(argv[0], "ping") == 0) {
        cmd_ping(argc, argv);
    }
    else if (strcmp(argv[0], "netstat") == 0) {
        cmd_netstat();
    }
    else if (strcmp(argv[0], "arp") == 0) {
        cmd_arp_show();
    }
    else if (strcmp(argv[0], "dns") == 0) {
        if (argc < 2) {
            vga_print("  Usage: dns <hostname>\n");
        } else {
            vga_print("  Resolving ");
            vga_print(argv[1]);
            vga_print("...\n");
            uint32_t ip = dns_resolve(argv[1]);
            if (ip) {
                char ip_str[16];
                ip_to_string(ip, ip_str);
                vga_print("  ");
                vga_print(argv[1]);
                vga_print(" -> ");
                vga_print_color(ip_str, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                vga_print("\n");
            } else {
                vga_print_color("  DNS lookup failed.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            }
        }
    }
    else if (strcmp(argv[0], "wget") == 0) {
        if (argc < 2) {
            vga_print("  Usage: wget <url>\n");
        } else {
            vga_print("  Fetching ");
            vga_print(argv[1]);
            vga_print("...\n");
            http_response_t resp = http_get(argv[1]);
            if (resp.success) {
                char buf[12];
                vga_print_color("  HTTP ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                int_to_str(resp.status_code, buf);
                vga_print_color(buf, VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                vga_print(" (");
                int_to_str(resp.body_len, buf);
                vga_print(buf);
                vga_print(" bytes)\n\n");
                /* Print body (truncated) */
                int show = resp.body_len < 1024 ? resp.body_len : 1024;
                for (int i = 0; i < show; i++) {
                    if (resp.body[i] >= 32 || resp.body[i] == '\n' || resp.body[i] == '\r') {
                        vga_putchar(resp.body[i]);
                    }
                }
                if (resp.body_len > 1024) {
                    vga_print("\n... (truncated)\n");
                }
                vga_print("\n");
            } else {
                vga_print_color("  Request failed.", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                if (resp.status_code > 0) {
                    char buf[12];
                    vga_print(" (HTTP ");
                    int_to_str(resp.status_code, buf);
                    vga_print(buf);
                    vga_print(")");
                }
                vga_print("\n");
            }
        }
    }
    else if (strcmp(argv[0], "browse") == 0) {
        if (argc < 2) {
            vga_print("  Usage: browse <url>\n");
        } else {
            vga_print("  Browser requires GUI desktop.\n");
            vga_print("  Type 'gui' first, then use 'browse' in desktop terminal.\n");
        }
    }
    else if (strcmp(argv[0], "dhcp") == 0) {
        vga_print_color("\n  DHCP Client\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        dhcp_request();
        vga_print("\n");
    }
    else if (strcmp(argv[0], "ntp") == 0) {
        vga_print_color("\n  NTP Time Sync\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        ntp_sync();
        vga_print("\n");
    }
    else if (strcmp(argv[0], "httpd") == 0) {
        if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
            httpd_stop();
        } else {
            httpd_start();
        }
    }
    else if (strcmp(argv[0], "rshell") == 0) {
        if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
            rshell_stop();
        } else {
            rshell_start();
        }
    }
    /* ===================== Phase 45: Cloud & Sync ===================== */
    else if (strcmp(argv[0], "vnc") == 0) {
        if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
            vnc_stop();
        } else if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
            /* Interactive serve loop: start the server and pump it until Esc,
             * exactly like the ping/doom blocking-command pattern. */
            if (!vnc_is_running()) vnc_start();
            vga_print_color("\n  VNC server running on port 5900.\n",
                            VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print("  Connect from the host:  vncviewer 127.0.0.1:5900\n");
            vga_print_color("  Press Esc to return to the shell (server keeps running).\n\n",
                            VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            for (;;) {
                net_poll();
                vnc_poll();
                if (keyboard_has_key()) {
                    char k = keyboard_getchar();
                    if (k == 27) break;        /* Esc */
                }
                __asm__ volatile("hlt");
            }
            vga_print("  (VNC still serving in the background.)\n");
        } else if (argc >= 2 && strcmp(argv[1], "status") == 0) {
            vga_print("\n  VNC: ");
            vga_print(vnc_is_running() ? "running" : "stopped");
            if (vnc_is_running()) {
                vga_print(vnc_client_connected() ? ", client connected" : ", waiting for client");
                char num[12]; int_to_str((int)vnc_frames_sent(), num);
                vga_print("\n  Frames sent: "); vga_print(num);
            }
            vga_print("\n\n");
        } else {
            vnc_start();
        }
    }
    else if (strcmp(argv[0], "sync") == 0) {
        if (argc >= 2 && strcmp(argv[1], "serve") == 0) {
            sync_server_start();
        } else if (argc >= 2 && strcmp(argv[1], "stop") == 0) {
            sync_server_stop();
        } else if (argc >= 3 && strcmp(argv[1], "list") == 0) {
            sync_client_list(ip_parse(argv[2]));
        } else if (argc >= 4 && strcmp(argv[1], "pull") == 0) {
            sync_client_pull(ip_parse(argv[2]), argv[3]);
        } else if (argc >= 4 && strcmp(argv[1], "push") == 0) {
            sync_client_push(ip_parse(argv[2]), argv[3]);
        } else if (argc >= 3 && strcmp(argv[1], "pullall") == 0) {
            sync_client_pull_all(ip_parse(argv[2]));
        } else if (argc >= 2 && strcmp(argv[1], "status") == 0) {
            char num[12];
            vga_print("\n  Sync server: ");
            vga_print(sync_server_running() ? "running (port 7070)" : "stopped");
            vga_print("\n  Files sent: "); int_to_str((int)sync_files_sent(), num); vga_print(num);
            vga_print("   recv: ");        int_to_str((int)sync_files_recv(), num); vga_print(num);
            vga_print("\n\n");
        } else {
            vga_print_color("\n  Cloud Sync (NexusOS file sync, port 7070)\n",
                            VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print("  Usage:\n");
            vga_print("    sync serve              start the sync server\n");
            vga_print("    sync stop               stop it\n");
            vga_print("    sync list <ip>          list a peer's files\n");
            vga_print("    sync pull <ip> <file>   download one file\n");
            vga_print("    sync push <ip> <file>   upload one file\n");
            vga_print("    sync pullall <ip>       download every file\n");
            vga_print("    sync status             show stats\n\n");
            vga_print_color("  Tip: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            vga_print("loopback peer is 10.0.2.15 (or host via hostfwd)\n\n");
        }
    }
    else if (strcmp(argv[0], "synccfg") == 0) {
        if (argc >= 2 && strcmp(argv[1], "save") == 0) {
            int n = settings_save(argc >= 3 ? argv[2] : NULL);
            if (n >= 0) {
                char num[12]; int_to_str(n, num);
                vga_print_color("  Settings saved (", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                vga_print(num); vga_print(" bytes) to settings.cfg\n");
            } else vga_print_color("  Save failed\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        } else if (argc >= 2 && strcmp(argv[1], "load") == 0) {
            if (settings_load(argc >= 3 ? argv[2] : NULL) == 0)
                vga_print_color("  Settings loaded and applied\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
            else vga_print_color("  No settings.cfg (run 'synccfg save' first)\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        } else {
            char buf[256];
            settings_serialize(buf, sizeof(buf));
            vga_print_color("\n  Current settings:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print(buf);
            vga_print("\n  (synccfg save | synccfg load)\n\n");
        }
    }
    else if (strcmp(argv[0], "clipsync") == 0) {
        /* Direct clipboard get/set — the local half of clipboard sync. The
         * VNC link carries it automatically; this exposes it on the CLI. */
        if (argc >= 3 && strcmp(argv[1], "set") == 0) {
            clipboard_copy(argv[2]);
            vga_print_color("  Clipboard set\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        } else {
            const char* c = clipboard_paste();
            vga_print_color("\n  Clipboard: ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
            vga_print((c && *c) ? c : "(empty)");
            vga_print("\n\n");
        }
    }
    /* ===================== Phase 46: AI Assistant ===================== */
    else if (strcmp(argv[0], "ai")   == 0) cmd_ai(argc, argv);
    else if (strcmp(argv[0], "ask")  == 0) cmd_ask(argc, argv);
    else if (strcmp(argv[0], "find") == 0) cmd_find(argc, argv);
    /* ===================== Phase 47: Mobile / Embedded ===================== */
    else if (strcmp(argv[0], "gesture")     == 0) cmd_gesture(argc, argv);
    else if (strcmp(argv[0], "orientation") == 0) cmd_orientation(argc, argv);
    else if (strcmp(argv[0], "lowpower")    == 0) cmd_lowpower(argc, argv);
    else if (strcmp(argv[0], "arm")         == 0) cmd_arm();
    else if (strcmp(argv[0], "mobileinfo")  == 0) cmd_mobileinfo();
    /* ===================== Phase 48: Performance ===================== */
    else if (strcmp(argv[0], "perf")        == 0) cmd_perf();
    else if (strcmp(argv[0], "smp")         == 0) cmd_smp();
    else if (strcmp(argv[0], "preempt")     == 0) cmd_preempt();
    /* ===================== Phase 49: App Store ====================== */
    else if (strcmp(argv[0], "store")       == 0) cmd_store(argc, argv);
    /* ===================== Phase 50: Grand Finale =================== */
    else if (strcmp(argv[0], "urun")        == 0) cmd_urun(argc, argv);
    else if (strcmp(argv[0], "install")     == 0) cmd_install(argc, argv);
    else if (strcmp(argv[0], "finale")      == 0 || strcmp(argv[0], "v5") == 0) cmd_finale();
    /* ===================== Phase 51: NPFS journaling FS ============= */
    else if (strcmp(argv[0], "npfs")        == 0) cmd_npfs(argc, argv);
    /* Phase 32: Dynamic linking commands */
    else if (strcmp(argv[0], "ldd") == 0) {
        int count = dynlink_get_lib_count();
        vga_print_color("\n  Loaded Shared Libraries\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print_color("  =======================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        if (count == 0) {
            vga_print("  No shared libraries loaded.\n");
        } else {
            const loaded_lib_t* libs = dynlink_get_libs();
            char buf[12];
            vga_print_color("  Name                  Base         Size\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
            vga_print_color("  --------------------  ----------   --------\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
            for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
                if (!libs[i].active) continue;
                vga_print("  ");
                vga_print(libs[i].name);
                int pad = 22 - (int)strlen(libs[i].name);
                for (int j = 0; j < pad && j < 22; j++) vga_putchar(' ');
                vga_print("0x");
                hex_to_str(libs[i].load_base, buf);
                vga_print(buf);
                vga_print("   ");
                int_to_str(libs[i].load_size, buf);
                vga_print(buf);
                vga_print(" bytes\n");
            }
        }
        vga_print_color("\n  Built-in libc: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        char nb[12]; int_to_str(dynlink_get_builtin_count(), nb); vga_print(nb);
        vga_print(" symbols\n\n");
    }
    else if (strcmp(argv[0], "ldconfig") == 0) {
        int bcount = dynlink_get_builtin_count();
        const builtin_sym_t* syms = dynlink_get_builtins();
        vga_print_color("\n  Built-in Symbol Table\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print_color("  =====================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print_color("  Symbol              Address\n", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print_color("  ------------------  ----------\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        char buf[12];
        for (int i = 0; i < bcount; i++) {
            vga_print("  ");
            vga_print(syms[i].name);
            int pad = 20 - (int)strlen(syms[i].name);
            for (int j = 0; j < pad && j < 20; j++) vga_putchar(' ');
            vga_print("0x");
            hex_to_str(syms[i].addr, buf);
            vga_print(buf);
            vga_print("\n");
        }
        vga_print_color("\n  Total: ", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        int_to_str(bcount, buf); vga_print(buf);
        vga_print(" symbols\n\n");
    }
    else if (strcmp(argv[0], "dlopen") == 0) {
        if (argc < 2) {
            vga_print("  Usage: dlopen <library.so>\n");
        } else {
            uint32_t handle = dl_open(argv[1], 0);
            if (handle) {
                vga_print_color("  Library loaded, handle=", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                char buf[12]; int_to_str(handle, buf); vga_print(buf); vga_print("\n");
            } else {
                vga_print_color("  Failed to load library.\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            }
        }
    }
    else if (strcmp(argv[0], "dlsym") == 0) {
        if (argc < 3) {
            vga_print("  Usage: dlsym <handle|0> <symbol_name>\n");
            vga_print("  Handle 0 searches all libraries + builtins.\n");
        } else {
            uint32_t handle = 0;
            for (int i = 0; argv[1][i]; i++) handle = handle * 10 + (argv[1][i] - '0');
            uint32_t addr = dl_sym(handle, argv[2]);
            if (addr) {
                vga_print("  ");
                vga_print_color(argv[2], VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
                vga_print(" = 0x");
                char buf[12]; hex_to_str(addr, buf); vga_print(buf); vga_print("\n");
            } else {
                vga_print_color("  Symbol not found: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
                vga_print(argv[2]); vga_print("\n");
            }
        }
    }
    /* === Phase 33: X11 Shim commands === */
    else if (strcmp(argv[0], "xinfo") == 0) {
        vga_print_color("\n  X11 Compatibility Shim — Phase 33\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print_color("  ================================\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));
        vga_print("  "); vga_print(x11_get_status()); vga_print("\n");
        char buf[12];
        vga_print_color("  Display:  ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        int_to_str((int)fb_get_width(), buf); vga_print(buf); vga_print("x");
        int_to_str((int)fb_get_height(), buf); vga_print(buf); vga_print("x24 TrueColor\n");
        vga_print_color("  Protocol: ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("X11R6 compatible (kernel shim)\n");
        vga_print_color("  Server:   ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("NexusOS X11 v1.0\n");
        vga_print_color("  API:      ", VGA_COLOR(VGA_YELLOW, VGA_BLACK));
        vga_print("XOpenDisplay, XCreateSimpleWindow, XDrawLine,\n");
        vga_print("            XDrawRectangle, XFillRectangle, XDrawArc,\n");
        vga_print("            XDrawString, XNextEvent, XInternAtom\n\n");
    }
    else if (strcmp(argv[0], "xdemo") == 0) {
        vga_print_color("  Launching X11 demo...\n", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        Display* dpy = XOpenDisplay(NULL);
        if (!dpy) { vga_print_color("  Failed to open display\n", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK)); }
        else {
            int scr = XDefaultScreen(dpy);
            Window w = XCreateSimpleWindow(dpy, XRootWindow(dpy, scr),
                50, 50, 300, 200, 1, XBlackPixel(dpy, scr), 0x1E1E32);
            XStoreName(dpy, w, "X11 Demo");
            XSelectInput(dpy, w, ExposureMask | KeyPressMask);
            XMapWindow(dpy, w);
            /* Draw demo content */
            GC gc = XCreateGC(dpy, w, 0, NULL);
            XSetForeground(dpy, gc, 0xFF6464);
            XFillRectangle(dpy, w, gc, 10, 10, 80, 40);
            XSetForeground(dpy, gc, 0x64FF64);
            XFillRectangle(dpy, w, gc, 100, 10, 80, 40);
            XSetForeground(dpy, gc, 0x6464FF);
            XFillRectangle(dpy, w, gc, 190, 10, 80, 40);
            XSetForeground(dpy, gc, 0xFFFF00);
            XDrawLine(dpy, w, gc, 10, 70, 270, 70);
            XSetForeground(dpy, gc, 0x00FFFF);
            XFillArc(dpy, w, gc, 100, 80, 80, 80, 0, 360*64);
            XSetForeground(dpy, gc, 0xFFFFFF);
            XDrawString(dpy, w, gc, 10, 180, "X11 on NexusOS!", 15);
            XFlush(dpy);
            XFreeGC(dpy, gc);
            vga_print("  X11 window created. Close from desktop.\n");
        }
    }
    /* === Phase 34: Win32 Shim commands === */
    else if (strcmp(argv[0], "win32info") == 0) cmd_win32info();
    else if (strcmp(argv[0], "regedit") == 0) cmd_regedit();
    else if (strcmp(argv[0], "runexe") == 0) cmd_runexe(argc, argv);
    /* === Phase 35: Package Manager === */
    else if (strcmp(argv[0], "npkg") == 0) cmd_npkg(argc, argv);
    /* === Phase 36: Scripting Engine === */
    else if (strcmp(argv[0], "script") == 0) cmd_script(argc, argv);
    /* === Phase 37: macOS Compatibility Shim === */
    else if (strcmp(argv[0], "machoinfo") == 0) cmd_machoinfo();
    else if (strcmp(argv[0], "runmacho") == 0) cmd_runmacho(argc, argv);
    else if (strcmp(argv[0], "cocoademo") == 0) cmd_cocoademo();
    /* === Phase 38: Sound — Audio Mixer + AC'97 === */
    else if (strcmp(argv[0], "sndinfo") == 0) cmd_sndinfo();
    else if (strcmp(argv[0], "play") == 0) cmd_play(argc, argv);
    else if (strcmp(argv[0], "volume") == 0) cmd_volume(argc, argv);
    else if (strcmp(argv[0], "tone") == 0) cmd_tone(argc, argv);
    else if (strcmp(argv[0], "mixer") == 0) cmd_mixer();
    /* === Phase 39: Image Formats === */
    else if (strcmp(argv[0], "imginfo") == 0) cmd_imginfo();
    else if (strcmp(argv[0], "view") == 0) cmd_view(argc, argv);
    /* === Phase 40: Video Playback === */
    else if (strcmp(argv[0], "vidinfo") == 0) cmd_vidinfo(argc, argv);
    else if (strcmp(argv[0], "mplay") == 0) cmd_mplay(argc, argv);
    /* === Phase 41: GPU Acceleration === */
    else if (strcmp(argv[0], "gpuinfo") == 0) cmd_gpuinfo();
    else if (strcmp(argv[0], "gpubench") == 0) cmd_gpubench();
    else if (strcmp(argv[0], "sprites") == 0) cmd_sprites();
    /* === Phase 42: Gaming Framework === */
    else if (strcmp(argv[0], "gameinfo") == 0) cmd_gameinfo();
    else if (strcmp(argv[0], "gamepad") == 0) cmd_gamepad();
    else if (strcmp(argv[0], "doom") == 0) doom_run();
    else if (strcmp(argv[0], "breakout") == 0) breakout_run();
    /* === Phase 43: Accessibility === */
    else if (strcmp(argv[0], "accinfo") == 0) cmd_accinfo();
    else if (strcmp(argv[0], "fontsize") == 0) cmd_fontsize(argc, argv);
    else if (strcmp(argv[0], "contrast") == 0) cmd_contrast(argc, argv);
    else if (strcmp(argv[0], "reader") == 0) cmd_reader(argc, argv);
    else if (strcmp(argv[0], "say") == 0) cmd_say(argc, argv);
    /* === Phase 44: Security === */
    else if (strcmp(argv[0], "id") == 0) cmd_id();
    else if (strcmp(argv[0], "users") == 0) cmd_users();
    else if (strcmp(argv[0], "useradd") == 0) cmd_useradd(argc, argv);
    else if (strcmp(argv[0], "userdel") == 0) cmd_userdel(argc, argv);
    else if (strcmp(argv[0], "passwd") == 0) cmd_passwd(argc, argv);
    else if (strcmp(argv[0], "chmod") == 0) cmd_chmod(argc, argv);
    else if (strcmp(argv[0], "chown") == 0) cmd_chown(argc, argv);
    else if (strcmp(argv[0], "firewall") == 0 || strcmp(argv[0], "fw") == 0) cmd_firewall(argc, argv);
    else {
        int len = strlen(argv[0]);
        if (len > 4 && 
            (strcmp(argv[0] + len - 4, ".exe") == 0 || strcmp(argv[0] + len - 4, ".EXE") == 0)) {
            char* runexe_args[] = { "runexe", argv[0] };
            cmd_runexe(2, runexe_args);
        } else {
            vga_print_color("  Unknown command: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
            vga_print(argv[0]);
            vga_print("\n  Type 'help' for available commands.\n");
        }
    }
}

/* --------------------------------------------------------------------------
 * shell_run: Main shell loop
 * -------------------------------------------------------------------------- */
/* Phase 45: pump backgrounded network servers while the shell idles at the
 * prompt (installed as the keyboard idle hook). Keeps a VNC remote desktop or
 * the cloud-sync server alive without the GUI desktop running. */
static void shell_idle_pump(void) {
    net_poll();
    vnc_poll();
    sync_poll();
    mobile_poll();   /* Phase 47: recognize touch gestures while idle */
}

void shell_run(void) {
    char input[INPUT_MAX];

    vga_print("\n");
    vga_print_color("  Welcome to NexusOS Shell v36.0!\n", VGA_COLOR(VGA_WHITE, VGA_BLACK));
    vga_print_color("  Type 'help' for commands. 'gui' for desktop.\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    keyboard_set_idle_hook(shell_idle_pump);

    while (1) {
        print_prompt();
        vga_flush();
        shell_readline(input, INPUT_MAX);
        if (input[0] != '\0') {
            history_add(input);
            execute_command(input);
            vga_flush();
        }
    }
}

/* --------------------------------------------------------------------------
 * shell_exec_line: Execute one command line programmatically.
 * Used by the scripting engine's `run` statement. Copies into a mutable
 * buffer because execute_command() tokenizes in place.
 * -------------------------------------------------------------------------- */
void shell_exec_line(const char* line) {
    if (!line) return;
    char buf[INPUT_MAX];
    int i = 0;
    while (line[i] && i < INPUT_MAX - 1) { buf[i] = line[i]; i++; }
    buf[i] = '\0';
    execute_command(buf);
}

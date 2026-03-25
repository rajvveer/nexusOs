/* ============================================================================
 * NexusOS - Interactive Shell v21.0
 * ============================================================================
 * Phase 33: X11 Compatibility Shim. 79 commands total.
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
#include "ntp.h"
#include "httpd.h"
#include "rshell.h"
#include "procfs.h"
#include "termios.h"
#include "posix.h"
#include "dynlink.h"
#include "libc.h"

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
static int shell_readline(char* buffer, int max_len) {
    int i = 0;
    int hist_idx = history_count;  /* Start at "current" (no history selected) */
    char saved[INPUT_MAX];
    saved[0] = '\0';
    int cursor_start_col = vga_get_cursor_col();

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

                /* Clear current line */
                while (i > 0) { vga_backspace(); i--; }

                /* Print history entry */
                strcpy(buffer, history[actual]);
                i = strlen(buffer);
                vga_set_cursor(vga_get_cursor_row(), cursor_start_col);
                /* Clear rest of line */
                for (int j = 0; j < max_len && j < VGA_WIDTH - cursor_start_col; j++)
                    vga_putchar(' ');
                vga_set_cursor(vga_get_cursor_row(), cursor_start_col);
                for (int j = 0; j < i; j++)
                    vga_putchar(buffer[j]);
            }
            continue;
        }

        /* Down arrow — next history */
        if ((unsigned char)c == 0x81) {
            if (hist_idx < history_count) {
                hist_idx++;

                while (i > 0) { vga_backspace(); i--; }

                if (hist_idx == history_count) {
                    strcpy(buffer, saved);
                } else {
                    int actual = (history_pos - history_count + hist_idx + HISTORY_SIZE) % HISTORY_SIZE;
                    strcpy(buffer, history[actual]);
                }
                i = strlen(buffer);
                vga_set_cursor(vga_get_cursor_row(), cursor_start_col);
                for (int j = 0; j < max_len && j < VGA_WIDTH - cursor_start_col; j++)
                    vga_putchar(' ');
                vga_set_cursor(vga_get_cursor_row(), cursor_start_col);
                for (int j = 0; j < i; j++)
                    vga_putchar(buffer[j]);
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
    vga_print_color("\n  NexusOS Shell Commands v17.0\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
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
    vga_print("ldd  ldconfig  dlopen <lib>  dlsym <handle> <sym>\n\n");

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

static void cmd_ls(void) {
    fs_node_t* root = vfs_get_root();
    if (!root) { vga_print("  No filesystem mounted.\n"); return; }

    fs_node_t* entry;
    uint32_t index = 0;
    vga_print_color("\n  Files:\n", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
    while ((entry = vfs_readdir(root, index)) != NULL) {
        vga_print("  ");
        if (entry->type & FS_DIRECTORY) {
            vga_print_color(entry->name, VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK));
            vga_print_color("/", VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK));
        } else {
            vga_print_color(entry->name, VGA_COLOR(VGA_WHITE, VGA_BLACK));
        }
        char buf[16];
        vga_print("  (");
        int_to_str(entry->size, buf);
        vga_print(buf);
        vga_print(" bytes)");
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
    if (ramfs_delete(argv[1]) == 0) {
        vga_print_color("  Deleted: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]); vga_print("\n");
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
    process_terminate(pid);
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
        vga_print("  Available: dark, light, retro, ocean\n");
        return;
    }
    if (theme_set_by_name(argv[1])) {
        vga_print_color("  Theme set to: ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
        vga_print(argv[1]);
        vga_print("\n");
    } else {
        vga_print_color("  Unknown theme: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(argv[1]);
        vga_print("\n  Available: dark, light, retro, ocean\n");
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
    const char* user = env_get("USER");
    vga_print("  "); vga_print(user ? user : "root"); vga_print("\n");
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
    else if (strcmp(argv[0], "ls")       == 0) cmd_ls();
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
    else {
        vga_print_color("  Unknown command: ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
        vga_print(argv[0]);
        vga_print("\n  Type 'help' for available commands.\n");
    }
}

/* --------------------------------------------------------------------------
 * shell_run: Main shell loop
 * -------------------------------------------------------------------------- */
void shell_run(void) {
    char input[INPUT_MAX];

    vga_print("\n");
    vga_print_color("  Welcome to NexusOS Shell v20.0!\n", VGA_COLOR(VGA_WHITE, VGA_BLACK));
    vga_print_color("  Type 'help' for commands. 'gui' for desktop.\n\n", VGA_COLOR(VGA_DARK_GREY, VGA_BLACK));

    while (1) {
        print_prompt();
        shell_readline(input, INPUT_MAX);
        if (input[0] != '\0') {
            history_add(input);
            execute_command(input);
        }
    }
}

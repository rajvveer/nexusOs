/* ============================================================================
 * NexusOS — /proc Filesystem — Phase 31
 * ============================================================================
 * Virtual filesystem that generates content on-the-fly from kernel state.
 * Supports: /proc/uptime, /proc/meminfo, /proc/cpuinfo, /proc/version,
 *           /proc/net, /proc/<pid>/status
 * ============================================================================ */

#include "procfs.h"
#include "process.h"
#include "memory.h"
#include "heap.h"
#include "ip.h"
#include "net.h"
#include "rtc.h"
#include "vga.h"
#include "string.h"

/* System tick counter */
extern volatile uint32_t system_ticks;

/* --------------------------------------------------------------------------
 * Helper: append string to buffer
 * -------------------------------------------------------------------------- */
static int pappend(char* buf, int pos, int max, const char* s) {
    while (*s && pos < max - 1) buf[pos++] = *s++;
    buf[pos] = '\0';
    return pos;
}

static int pappend_num(char* buf, int pos, int max, uint32_t n) {
    char tmp[12];
    int_to_str(n, tmp);
    return pappend(buf, pos, max, tmp);
}

/* --------------------------------------------------------------------------
 * procfs_init
 * -------------------------------------------------------------------------- */
void procfs_init(void) {
    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("procfs initialized (/proc)\n");
}

/* --------------------------------------------------------------------------
 * procfs_is_proc: Check if path starts with /proc
 * -------------------------------------------------------------------------- */
bool procfs_is_proc(const char* path) {
    return (path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
            path[3] == 'o' && path[4] == 'c' &&
            (path[5] == '/' || path[5] == '\0'));
}

/* --------------------------------------------------------------------------
 * parse_pid: Extract PID from /proc/<pid>/...
 * -------------------------------------------------------------------------- */
static int parse_pid(const char* path) {
    /* path should be "/proc/<pid>/..." */
    if (!procfs_is_proc(path) || path[5] != '/') return -1;
    const char* p = path + 6;
    int pid = 0;
    while (*p >= '0' && *p <= '9') {
        pid = pid * 10 + (*p - '0');
        p++;
    }
    if (p == path + 6) return -1;  /* No digits */
    return pid;
}

/* --------------------------------------------------------------------------
 * procfs_read: Generate content for a /proc path
 * -------------------------------------------------------------------------- */
int procfs_read(const char* path, char* buf, int max) {
    int pos = 0;

    /* /proc/uptime */
    if (strcmp(path, "/proc/uptime") == 0) {
        uint32_t secs = system_ticks / 18;
        uint32_t mins = secs / 60;
        uint32_t hrs  = mins / 60;
        secs %= 60;
        mins %= 60;
        pos = pappend(buf, pos, max, "Uptime: ");
        pos = pappend_num(buf, pos, max, hrs);
        pos = pappend(buf, pos, max, "h ");
        pos = pappend_num(buf, pos, max, mins);
        pos = pappend(buf, pos, max, "m ");
        pos = pappend_num(buf, pos, max, secs);
        pos = pappend(buf, pos, max, "s\n");
        pos = pappend(buf, pos, max, "Ticks: ");
        pos = pappend_num(buf, pos, max, system_ticks);
        pos = pappend(buf, pos, max, "\n");
        return pos;
    }

    /* /proc/meminfo */
    if (strcmp(path, "/proc/meminfo") == 0) {
        uint32_t total = pmm_get_total_pages() * 4;
        uint32_t used  = pmm_get_used_pages() * 4;
        uint32_t free  = pmm_get_free_pages() * 4;
        pos = pappend(buf, pos, max, "MemTotal: ");
        pos = pappend_num(buf, pos, max, total);
        pos = pappend(buf, pos, max, " KB\nMemUsed:  ");
        pos = pappend_num(buf, pos, max, used);
        pos = pappend(buf, pos, max, " KB\nMemFree:  ");
        pos = pappend_num(buf, pos, max, free);
        pos = pappend(buf, pos, max, " KB\n");
        return pos;
    }

    /* /proc/cpuinfo */
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        pos = pappend(buf, pos, max, "Architecture: i386\n");
        pos = pappend(buf, pos, max, "CPU: Intel 386 compatible\n");
        pos = pappend(buf, pos, max, "Timer: 18.2 Hz (PIT)\n");
        pos = pappend(buf, pos, max, "Ticks: ");
        pos = pappend_num(buf, pos, max, system_ticks);
        pos = pappend(buf, pos, max, "\n");
        return pos;
    }

    /* /proc/version */
    if (strcmp(path, "/proc/version") == 0) {
        pos = pappend(buf, pos, max, "NexusOS v3.1.0\n");
        pos = pappend(buf, pos, max, "Phase 31: POSIX compatibility\n");
        pos = pappend(buf, pos, max, "Compiler: i686-elf-gcc\n");
        return pos;
    }

    /* /proc/net */
    if (strcmp(path, "/proc/net") == 0) {
        const net_config_t* cfg = ip_get_config();
        char ip[16];
        ip_to_string(cfg->ip, ip);
        pos = pappend(buf, pos, max, "IP:      ");
        pos = pappend(buf, pos, max, ip);
        pos = pappend(buf, pos, max, "\n");
        ip_to_string(cfg->subnet_mask, ip);
        pos = pappend(buf, pos, max, "Mask:    ");
        pos = pappend(buf, pos, max, ip);
        pos = pappend(buf, pos, max, "\n");
        ip_to_string(cfg->gateway, ip);
        pos = pappend(buf, pos, max, "Gateway: ");
        pos = pappend(buf, pos, max, ip);
        pos = pappend(buf, pos, max, "\n");
        ip_to_string(cfg->dns, ip);
        pos = pappend(buf, pos, max, "DNS:     ");
        pos = pappend(buf, pos, max, ip);
        pos = pappend(buf, pos, max, "\n");

        net_device_t* dev = net_get_device(0);
        if (dev) {
            char ms[18];
            net_mac_to_string(&dev->mac, ms);
            pos = pappend(buf, pos, max, "Device:  ");
            pos = pappend(buf, pos, max, dev->name);
            pos = pappend(buf, pos, max, dev->link_up ? " (UP)\n" : " (DOWN)\n");
            pos = pappend(buf, pos, max, "MAC:     ");
            pos = pappend(buf, pos, max, ms);
            pos = pappend(buf, pos, max, "\n");
            pos = pappend(buf, pos, max, "TX:      ");
            pos = pappend_num(buf, pos, max, dev->stats.tx_packets);
            pos = pappend(buf, pos, max, " pkts\nRX:      ");
            pos = pappend_num(buf, pos, max, dev->stats.rx_packets);
            pos = pappend(buf, pos, max, " pkts\n");
        }
        return pos;
    }

    /* /proc (directory listing) */
    if (strcmp(path, "/proc") == 0 || strcmp(path, "/proc/") == 0) {
        pos = pappend(buf, pos, max, "uptime\nmeminfo\ncpuinfo\nversion\nnet\n");
        /* List PIDs */
        process_t* table = process_get_table();
        for (int i = 0; i < MAX_PROCESSES; i++) {
            if (table[i].state != PROC_UNUSED) {
                pos = pappend_num(buf, pos, max, table[i].pid);
                pos = pappend(buf, pos, max, "\n");
            }
        }
        return pos;
    }

    /* /proc/<pid>/status */
    int pid = parse_pid(path);
    if (pid >= 0) {
        /* Check if path ends with /status or just the pid */
        process_t* proc = process_get_by_pid(pid);
        if (!proc || proc->state == PROC_UNUSED) return -1;

        pos = pappend(buf, pos, max, "Name: ");
        pos = pappend(buf, pos, max, proc->name);
        pos = pappend(buf, pos, max, "\nPID:  ");
        pos = pappend_num(buf, pos, max, proc->pid);
        pos = pappend(buf, pos, max, "\nPPID: ");
        pos = pappend_num(buf, pos, max, proc->ppid);
        pos = pappend(buf, pos, max, "\nState: ");
        const char* states[] = {"unused","ready","running","blocked","stopped","terminated"};
        if (proc->state <= PROC_TERMINATED)
            pos = pappend(buf, pos, max, states[proc->state]);
        pos = pappend(buf, pos, max, "\nTicks: ");
        pos = pappend_num(buf, pos, max, proc->ticks);
        pos = pappend(buf, pos, max, "\n");
        return pos;
    }

    return -1;  /* Not found */
}

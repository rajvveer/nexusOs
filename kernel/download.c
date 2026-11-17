/* Shared bounded-memory file sink for browser and wget downloads. */
#include "download.h"
#include "crypto.h"
#include "string.h"
#include "port.h"

static void serial(const char* s) {
    while (*s) { while (!(port_byte_in(0x3FD) & 0x20)) {} port_byte_out(0x3F8, (uint8_t)*s++); }
}

static int failed(download_t* d, const char* error) {
    strncpy(d->error, error, sizeof(d->error) - 1);
    d->error[sizeof(d->error) - 1] = 0;
    return -1;
}
static int hex(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static void filename(const char* url, const char* hint, char* name) {
    bool from_hint = hint && hint[0];
    const char* p = hint && hint[0] ? hint : url;
    const char* leaf = p;
    if (!(hint && hint[0])) {
        const char* scheme = strstr(p, "://");
        if (scheme) { p = scheme + 3; while (*p && *p != '/') p++; leaf = p; }
    }
    for (const char* q = p; *q && (from_hint || (*q != '?' && *q != '#')); q++)
        if (*q == '/' || *q == '\\') leaf = q + 1;
    int n = 0;
    while (*leaf && (from_hint || (*leaf != '?' && *leaf != '#')) && n < FAT32_LFN_MAX) {
        uint8_t c = (uint8_t)*leaf++;
        if (c == '%' && leaf[0] && leaf[1] && hex(leaf[0]) >= 0 && hex(leaf[1]) >= 0) {
            c = (uint8_t)((hex(leaf[0]) << 4) | hex(leaf[1])); leaf += 2;
        }
        if (c < 32 || c == 127 || c == '"' || c == '*' || c == '/' || c == ':' ||
            c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') c = '_';
        name[n++] = (char)c;
    }
    while (n && (name[n - 1] == '.' || name[n - 1] == ' ')) n--;
    name[n] = 0;
    if (!n) strcpy(name, "download");
}

int download_begin(download_t* d, const char* url, const char* hint, const char* destination) {
    if (d->active) return failed(d, "A download is already active");
    memset(d, 0, sizeof(*d)); d->crc = 0xFFFFFFFFu;
    if (!fat32_is_mounted()) return failed(d, "Storage disk is not mounted");
    char path[FAT32_PATH_MAX];
    if (destination && destination[0] == '/') {
        if (strncmp(destination, "/disk/", 6)) return failed(d, "Save persistent downloads under /disk/");
        if (strlen(destination + 6) >= sizeof(path)) return failed(d, "Destination path is too long");
        strcpy(path, destination + 6);
    } else {
        fat32_file_t dir;
        if (!fat32_lookup("Downloads", &dir) && fat32_create_path("Downloads", true, &dir) != 0)
            return failed(d, "Cannot create Downloads (disk full or I/O error)");
        if (!dir.is_dir) return failed(d, "Downloads is not a directory");
        char name[FAT32_LFN_MAX + 1];
        if (destination && destination[0]) {
            if (strlen(destination) > FAT32_LFN_MAX) return failed(d, "Filename is too long");
            strcpy(name, destination);
        } else filename(url, hint, name);
        strcpy(path, "Downloads/"); strcat(path, name);
        if (!destination) {
            fat32_file_t existing;
            char original[FAT32_LFN_MAX + 1]; strcpy(original, name);
            int dot = (int)strlen(original);
            for (int i = 1; original[i]; i++) if (original[i] == '.') dot = i;
            for (uint32_t suffix = 1; fat32_lookup(path, &existing); suffix++) {
                char number[12]; int_to_str((int)suffix, number);
                int reserve = (int)strlen(number) + 3 + (int)strlen(original + dot);
                int base = dot;
                if (base + reserve > FAT32_LFN_MAX) base = FAT32_LFN_MAX - reserve;
                while (base > 0 && ((uint8_t)original[base] & 0xC0) == 0x80) base--;
                if (base < 1 || suffix > 65536) return failed(d, "No available download filename");
                memcpy(name, original, base); name[base] = 0;
                strcat(name, " ("); strcat(name, number); strcat(name, ")"); strcat(name, original + dot);
                strcpy(path, "Downloads/"); strcat(path, name);
            }
        }
    }
    strcpy(d->path, "/disk/"); strcat(d->path, path);
    int r = fat32_begin_download(path, &d->file);
    if (r != 0) return failed(d, r == -2 ? "Destination already exists" : r == -3 ? "Destination folder does not exist" : r == -4 ? "Disk is full" : "Cannot create download file");
    d->active = true;
    return 0;
}
int download_write(download_t* d, const void* data, uint32_t size) {
    if (!d->active) return failed(d, "Download is not active");
    if (size > 0xFFFFFFFFu - d->written) return failed(d, "File exceeds the FAT32 4 GiB file limit");
    int r = fat32_write_file(&d->file, d->written, size, data);
    if (r < 0 || (uint32_t)r != size) return failed(d, r == -2 || r >= 0 ? "Disk is full" : "Disk write failed");
    d->crc = crc32_update(d->crc, data, size); d->written += size;
    return 0;
}
int download_finish(download_t* d) {
    if (!d->active) return failed(d, "Download is not active");
    int r = fat32_finish_download(&d->file);
    if (r != 0) return failed(d, r == -2 ? "Destination already exists" : "Could not publish the completed download");
    d->active = false;
    char number[12]; uint_to_str(d->written, number);
    serial("[DL] saved bytes="); serial(number);
    hex_to_str(~d->crc, number); serial(" crc="); serial(number + 2);
    serial(" path="); serial(d->path); serial("\n");
    return 0;
}
void download_cancel(download_t* d) {
    if (d->file.pending && fat32_abort_download(&d->file) != 0) failed(d, "Partial download cleanup failed");
    d->active = false;
}
uint32_t download_percent(uint32_t done, uint32_t total) {
    if (!total) return 0;
    if (done >= total) return 100;
    return total >= 512 ? (done / 512) * 100 / (total / 512) : done * 100 / total;
}

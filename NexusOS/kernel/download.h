#ifndef DOWNLOAD_H
#define DOWNLOAD_H
#include "fat32.h"

typedef struct {
    fat32_file_t file;
    char path[FAT32_PATH_MAX + 6];
    char error[96];
    uint32_t written, crc;
    bool active;
} download_t;

/* NULL destination chooses a unique name under /disk/Downloads. */
int download_begin(download_t* d, const char* url, const char* suggested_name, const char* destination);
int download_write(download_t* d, const void* data, uint32_t size);
int download_finish(download_t* d);
void download_cancel(download_t* d);
uint32_t download_percent(uint32_t done, uint32_t total);
#endif

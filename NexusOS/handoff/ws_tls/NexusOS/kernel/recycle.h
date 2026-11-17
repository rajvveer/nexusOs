/* NexusOS — Recycle Bin (Header) */
#ifndef RECYCLE_H
#define RECYCLE_H
#include "types.h"
#define TRASH_MAX 8
#define TRASH_NAME_MAX 32
#define TRASH_DATA_MAX 256
void recycle_init(void);
bool recycle_delete(const char* name, const uint8_t* data, uint32_t size);
bool recycle_restore(int index);
void recycle_empty(void);
int  recycle_count(void);
const char* recycle_get_name(int index);
int  recycle_open(void);
#endif

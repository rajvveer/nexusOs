/* NexusOS — File Operations (Header) */
#ifndef FILEOPS_H
#define FILEOPS_H
int fileops_open(void);
int fileops_rename(const char* old_name, const char* new_name);
int fileops_duplicate(const char* name);
#endif

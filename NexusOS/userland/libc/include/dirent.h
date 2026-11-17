#ifndef NEXUS_DIRENT_H
#define NEXUS_DIRENT_H
#define DT_DIR 4
#define DT_REG 8
struct dirent { unsigned int d_ino, d_size; unsigned short d_type; char d_name[38]; };
typedef struct { int fd; struct dirent entry; } DIR;
DIR *opendir(const char *);
struct dirent *readdir(DIR *);
int closedir(DIR *);
#endif

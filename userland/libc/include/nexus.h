#ifndef NEXUS_USER_H
#define NEXUS_USER_H
#include <sys/types.h>
pid_t spawn(const char *path, char *const argv[], char *const envp[]);
#endif

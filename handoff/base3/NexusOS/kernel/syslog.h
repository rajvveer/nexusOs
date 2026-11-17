/* NexusOS — System Log (Header) */
#ifndef SYSLOG_H
#define SYSLOG_H
#include "types.h"
#define SLOG_MAX 20
#define SLOG_MSG_MAX 40
void syslog_init(void);
void syslog_add(const char* msg);
int  syslog_open(void);
int  syslog_count(void);
#endif

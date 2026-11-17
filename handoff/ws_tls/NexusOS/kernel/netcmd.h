/* ============================================================================
 * NexusOS — Phase 57 network shell commands: wget / curl / tlsinfo / netstat+
 * ============================================================================ */
#ifndef NETCMD_H
#define NETCMD_H

#include "types.h"

void netcmd_wget(int argc, char** argv);      /* wget <url> [path]              */
void netcmd_curl(int argc, char** argv);      /* curl [-I] [-q] [-k] <url>       */
void netcmd_tlsinfo(int argc, char** argv);   /* tlsinfo <host[:port]>           */
void netcmd_netstat_extra(void);              /* streams in use + TCP counters   */
void netcmd_cryptotest(void);                 /* cryptotest: known-answer tests  */

#endif /* NETCMD_H */

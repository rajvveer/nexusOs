/* Host and NIS domain name (uname nodename/domainname): sethostname(2), setdomainname(2), /proc/sys/kernel/hostname.
 * One system-wide pair (no UTS namespaces); changing it needs CAP_SYS_ADMIN, as on Linux. nexusd sets the host name
 * from /etc/hostname at boot. */
#include "native.h"
#include "../../kernel/string.h"

static char hostname[65] = "nexus", domainname[65] = "(none)";
const char* uts_hostname(void) { return hostname; }
const char* uts_domainname(void) { return domainname; }
int64_t uts_call(struct registers* r) {   /* 170 sethostname, 171 setdomainname */
    char* target = r->rax == 170 ? hostname : domainname;
    uint64_t length = r->rsi;
    if (length > 64) return -22;
    if (!credentials_capable(credentials_current(), NULL, 21)) return -1;
    if (length && !accessible(r->rdi, length, false)) return -14;
    char copy[65]; copy_from_user(copy, (const void*)r->rdi, length); copy[length] = 0;
    memcpy(target, copy, length + 1);
    return 0;
}

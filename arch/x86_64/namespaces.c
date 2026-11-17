/* Network and IPC namespaces (CLONE_NEWNET / CLONE_NEWIPC, unshare, setns) and setns(2) itself.
 *
 * A network or IPC namespace is a reference-counted slot here; each process holds one slot of each type
 * (process.c ns_slot[]), inherited on fork/clone and replaced by CLONE_NEW* / unshare / setns. Slot 0 is the initial
 * namespace (owned by the initial user namespace, never freed). Every namespace records the user namespace that owns
 * it (the creator's, or the new one when CLONE_NEWUSER is combined), which is what setns(2) checks CAP_SYS_ADMIN
 * against - so a sandboxed process that is root in its own user namespace still cannot setns() back into the
 * initial network namespace (Linux netns_install / ipcns_install).
 *
 * What a namespace isolates: the network namespace is enforced by inet.c (a non-initial netns has only loopback:
 * connect/bind to any other address fail with ENETUNREACH). The IPC namespace scopes System V IPC (sysvipc.c: its
 * objects and limits; they go when the namespace goes) and POSIX message queues (not implemented).
 *
 * /proc/PID/ns/{net,ipc} (proc.c) are nsfs magic links named "net:[inode]" / "ipc:[inode]"; an opened one keeps its
 * namespace alive (proc handle reference) and can be passed to setns(2). Inode numbers follow Linux: the initial
 * IPC namespace is PROC_IPC_INIT_INO 0xEFFFFFFF, dynamically created ones come from 0xF0000000 upwards. */
#include "native.h"

#define NS_SLOTS 256
struct nsobj { uint32_t references; uint32_t inode; struct user_namespace* owner; };
static struct nsobj table[2][NS_SLOTS] = {
    [NSOBJ_NET] = {[0] = {1, 0xF0000000U, NULL}},
    [NSOBJ_IPC] = {[0] = {1, 0xEFFFFFFFU, NULL}},
};
static uint32_t next_inode = 0xF0000001U;

static struct nsobj* slot_of(unsigned type, unsigned slot) {
    if (type > NSOBJ_IPC || slot >= NS_SLOTS || !table[type][slot].references) panic("invalid namespace slot");
    return &table[type][slot];
}
int nsobj_create(unsigned type, struct user_namespace* owner) {
    for (unsigned slot = 1; slot < NS_SLOTS; slot++) {
        struct nsobj* ns = &table[type][slot];
        if (ns->references) continue;
        ns->references = 1; ns->inode = next_inode++; ns->owner = owner;
        if (owner) namespace_retain(owner);
        return (int)slot;
    }
    return -28;   /* ENOSPC, as Linux when max_{net,ipc}_namespaces is exceeded */
}
void nsobj_retain(unsigned type, unsigned slot) {
    struct nsobj* ns = slot_of(type, slot);
    if (ns->references == 0xFFFFFFFFU) panic("namespace reference overflow");
    if (slot) ns->references++;
}
void nsobj_release(unsigned type, unsigned slot) {
    struct nsobj* ns = slot_of(type, slot);
    if (!slot || --ns->references) return;
    if (type == NSOBJ_IPC) sysvipc_namespace_gone(slot);
    if (ns->owner) namespace_release(ns->owner);
    ns->owner = NULL;
}
uint64_t nsobj_inode(unsigned type, unsigned slot) { return slot_of(type, slot)->inode; }

/* setns(2) on an opened /proc/PID/ns/<type> file; proc.c resolves the handle to what it names. type is the CLONE_NEW*
 * bit of the file's namespace, nstype the caller's filter (0 = any). Checks as Linux *_install(). */
int namespace_setns(unsigned long type, unsigned slot, struct user_namespace* user, struct pid_namespace* pid, uint64_t nstype) {
    if (nstype && nstype != type) return -22;
    struct credentials* self = credentials_current();
    struct user_namespace* mine = credentials_namespace(self);
    if (type == 0x40000000UL || type == 0x08000000UL) {   /* CLONE_NEWNET, CLONE_NEWIPC */
        unsigned kind = type == 0x40000000UL ? NSOBJ_NET : NSOBJ_IPC;
        if (!credentials_capable(self, slot_of(kind, slot)->owner, 21) || !credentials_capable(self, mine, 21)) return -1;
        uint32_t* held = task_nsobj_slot(kind); if (!held) return -22;
        nsobj_retain(kind, slot); nsobj_release(kind, *held); *held = slot;
        return 0;
    }
    if (type == 0x20000000UL) {   /* CLONE_NEWPID: pid_for_children (Linux pidns_install) */
        if (!pid) return -22;
        if (!credentials_capable(self, pid->owner, 21) || !credentials_capable(self, mine, 21)) return -1;
        /* only the active PID namespace or a descendant of it may be entered */
        if (!pid_namespace_contains(task_pid_namespace(), pid)) return -22;
        return task_set_children_namespace(pid);
    }
    if (type == 0x10000000UL) return task_enter_user_namespace(user);   /* CLONE_NEWUSER (Linux userns_install) */
    return -22;
}

/* System V IPC (kabi, wave 2): semaphores (semget, semop, semtimedop, semctl), message queues (msgget, msgsnd, msgrcv,
 * msgctl) and shared memory (shmget, shmat, shmdt, shmctl), per IPC namespace (namespaces.c slots), with Linux's
 * structures (semid64_ds, msqid64_ds, shmid64_ds, ipc64_perm), identifiers (seq << 15 | index), permission rules
 * (ipcperms with CAP_IPC_OWNER, owner/creator or CAP_SYS_ADMIN for IPC_SET/IPC_RMID), limits and errno order.
 *
 * Objects: one list per type, sorted by index, each object tagged with its IPC namespace; an IPC namespace that goes
 * away (namespaces.c nsobj_release) takes its objects with it. Blocked semop/msgsnd/msgrcv sleep on the object
 * (task_wait_until) and hold a reference: IPC_RMID wakes them with EIDRM and the object is freed after the last one.
 * They are never restarted after a signal handler (EINTR), as on Linux.
 * Semaphores: semop applies all operations or none (in array order; ERANGE above SEMVMX), SEM_UNDO adjustments are
 * kept per process and set (shared by the threads, not inherited by fork, kept over exec) and applied at exit
 * (sysvipc_exit, clamped to 0..SEMVMX); SETVAL/SETALL clear them. CLONE_SYSVSEM between processes is not shared.
 * Message queues: msg_qbytes bounds both bytes and messages; msgrcv by type (0, >0, <0 lowest <= |type|, MSG_EXCEPT),
 * MSG_NOERROR, MSG_COPY (CONFIG_CHECKPOINT_RESTORE semantics: IPC_NOWAIT required).
 * Shared memory: a segment is a shared memory file (shmem.c); shmat maps it MAP_SHARED (kernel.c native_map_shared),
 * shm_nattch is the number of mappings of that memory (forks and splits count, as Linux's VMA count), shmdt unmaps the
 * mappings of that segment within its size. A segment removed while attached (SHM_DEST, key IPC_PRIVATE) lives until
 * its last mapping goes (checked on every System V IPC call and /proc read). SHM_LOCK only sets SHM_LOCKED (the pages
 * may still be swapped), SHM_HUGETLB is ENOMEM (no huge pages).
 * /proc/sysvipc/{sem,msg,shm} and /proc/sys/kernel/{sem,msgmax,msgmnb,msgmni,shmmax,shmall,shmmni,shm_rmid_forced}
 * (procext.c) show and set the caller's namespace. Locking: the big kernel lock. */
#include "native.h"
#include "uaccess.h"
#include "swap.h"
#include "../../kernel/string.h"

bool credentials_in_group(struct credentials* credentials, uint32_t gid, bool real);
bool credentials_kernel_from_visible(uint32_t id, bool group, uint32_t* result);

#define IPC_CREAT 01000
#define IPC_EXCL 02000
#define IPC_NOWAIT 04000
#define IPC_RMID 0
#define IPC_SET 1
#define IPC_STAT 2
#define IPC_INFO 3
#define SEM_UNDO 0x1000
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16
#define SETALL 17
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20
#define MSG_STAT 11
#define MSG_INFO 12
#define MSG_STAT_ANY 13
#define MSG_NOERROR 010000
#define MSG_EXCEPT 020000
#define MSG_COPY 040000
#define SHM_LOCK 11
#define SHM_UNLOCK 12
#define SHM_STAT 13
#define SHM_INFO 14
#define SHM_STAT_ANY 15
#define SHM_HUGETLB 04000
#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000
#define SHM_DEST 01000
#define SHM_LOCKED 02000
#define SEMVMX 32767
#define SEMMSL_DEFAULT 32000
#define SEMMNS_DEFAULT 1024000000
#define SEMOPM_DEFAULT 500
#define SEMMNI_DEFAULT 32000
#define IPCMNI 32768
#define SEQ_MAX 65535
#define CAP_IPC_OWNER 15
#define CAP_SYS_ADMIN 21
#define CAP_SYS_RESOURCE 24
#define PAGE 4096ULL

enum { SEM, MSG, SHM, KINDS };
struct ipc_object {
    struct ipc_object* next;
    int32_t key, id; unsigned index; uint32_t ns;
    uint32_t uid, gid, cuid, cgid, mode; uint16_t seq;
    int64_t ctime;
    unsigned references;   /* the table's + one per sleeper */
    bool removed;
};
struct sem_value { int32_t value; uint32_t pid; uint32_t ncnt, zcnt; };
struct sem_undo { struct sem_undo* next; uint32_t pid; int16_t adjust[]; };
struct sem_set { struct ipc_object obj; unsigned count; struct sem_value* sems; int64_t otime; struct sem_undo* undos; };
struct message { struct message* next; int64_t type; uint64_t size; char text[]; };
struct msg_queue {
    struct ipc_object obj; struct message *head, *tail;
    uint64_t cbytes, qnum, qbytes; int64_t stime, rtime; uint32_t lspid, lrpid;
};
struct shm_segment {
    struct ipc_object obj; uint64_t size; struct memory_file* memory;
    int64_t atime, dtime; uint32_t cpid, lpid;
};
struct ipc_limits {   /* per IPC namespace (Linux ipc_namespace sc_* / msg_ctl* / shm_ctl*) */
    bool set; int32_t sem[4]; int32_t msgmax, msgmnb, msgmni, shmmni, shm_rmid_forced; uint64_t shmmax, shmall;
    uint64_t shm_pages; uint32_t used_sems; uint16_t next_seq[KINDS];
};
#define NS_SLOTS 256
static struct ipc_limits limits[NS_SLOTS];
static struct ipc_object* objects[KINDS];
static unsigned in_use[KINDS];

/* ---- namespaces, limits, identifiers ---- */
static uint32_t current_ns(void) { uint32_t* slot = task_nsobj_slot(NSOBJ_IPC); return slot && *slot < NS_SLOTS ? *slot : 0; }
static struct ipc_limits* ns_limits(uint32_t ns) {
    struct ipc_limits* l = &limits[ns < NS_SLOTS ? ns : 0];
    if (!l->set) {
        memset(l, 0, sizeof(*l)); l->set = true;
        l->sem[0] = SEMMSL_DEFAULT; l->sem[1] = SEMMNS_DEFAULT; l->sem[2] = SEMOPM_DEFAULT; l->sem[3] = SEMMNI_DEFAULT;
        l->msgmax = 8192; l->msgmnb = 16384; l->msgmni = 32000; l->shmmni = 4096;
        l->shmmax = l->shmall = ~0ULL - (1ULL << 24);
    }
    return l;
}
static int64_t now_seconds(void) { return (int64_t)(native_realtime_ns() / 1000000000ULL); }
static struct credentials* self(void) { return credentials_current(); }
static uint32_t euid(void) { return credentials_kernel_id(self(), false, 1); }
static uint32_t egid(void) { return credentials_kernel_id(self(), true, 1); }
static bool in_group(uint32_t gid) { return egid() == gid || credentials_in_group(self(), gid, false); }
static bool capable(unsigned capability) { return credentials_capable(self(), NULL, capability); }
/* Linux ipcperms: requested is a mode (0444 read, 0222 alter, 0111 exec, or the get call's permission bits) */
static bool permitted(const struct ipc_object* o, unsigned requested) {
    unsigned want = (requested >> 6 | requested >> 3 | requested) & 7, granted = o->mode;
    if (euid() == o->cuid || euid() == o->uid) granted >>= 6;
    else if (in_group(o->cgid) || in_group(o->gid)) granted >>= 3;
    return !(want & ~granted & 7) || capable(CAP_IPC_OWNER);
}
static bool owner_or_admin(const struct ipc_object* o) { return euid() == o->cuid || euid() == o->uid || capable(CAP_SYS_ADMIN); }
static struct ipc_object* by_index(unsigned kind, uint32_t ns, unsigned index) {
    for (struct ipc_object* o = objects[kind]; o; o = o->next) if (o->ns == ns && o->index == index && !o->removed) return o;
    return NULL;
}
static struct ipc_object* by_id(unsigned kind, int64_t id) {   /* EINVAL for any stale or foreign id */
    if (id < 0 || id > 0x7FFFFFFF) return NULL;
    struct ipc_object* o = by_index(kind, current_ns(), (unsigned)id % IPCMNI);
    return o && o->id == (int32_t)id ? o : NULL;
}
static struct ipc_object* by_key(unsigned kind, uint32_t ns, int32_t key) {
    for (struct ipc_object* o = objects[kind]; o; o = o->next) if (o->ns == ns && o->key == key && !o->removed) return o;
    return NULL;
}
static int max_index(unsigned kind, uint32_t ns) {
    int top = -1; for (struct ipc_object* o = objects[kind]; o; o = o->next) if (o->ns == ns && !o->removed && (int)o->index > top) top = (int)o->index;
    return top < 0 ? 0 : top;
}
static unsigned count_in(unsigned kind, uint32_t ns) { unsigned n = 0; for (struct ipc_object* o = objects[kind]; o; o = o->next) if (o->ns == ns && !o->removed) n++; return n; }
/* A new object of `kind` in the caller's namespace: lowest free index, the namespace's next sequence number. */
static bool publish(unsigned kind, struct ipc_object* o, int32_t key, unsigned mode) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns);
    unsigned index = 0;
    for (struct ipc_object* c = objects[kind]; c; c = c->next) if (c->ns == ns && c->index == index) index++;   /* sorted by index */
    if (index >= IPCMNI) return false;
    o->ns = ns; o->index = index; o->key = key; o->mode = mode & 0777; o->seq = l->next_seq[kind];
    l->next_seq[kind] = l->next_seq[kind] >= SEQ_MAX ? 0 : l->next_seq[kind] + 1;
    o->id = (int32_t)((uint32_t)o->seq << 15 | index);
    o->uid = o->cuid = euid(); o->gid = o->cgid = egid(); o->ctime = now_seconds(); o->references = 1;
    struct ipc_object** link = &objects[kind];
    while (*link && ((*link)->ns < ns || ((*link)->ns == ns && (*link)->index < index))) link = &(*link)->next;
    o->next = *link; *link = o; in_use[kind]++;
    return true;
}
static void unlink_object(unsigned kind, struct ipc_object* o) {
    struct ipc_object** link = &objects[kind]; while (*link && *link != o) link = &(*link)->next;
    if (*link) { *link = o->next; in_use[kind]--; }
    o->next = NULL;
}
/* sizes of the variable parts, for arena_free */
static uint64_t sem_bytes(unsigned count) { return sizeof(struct sem_set) + (uint64_t)count * sizeof(struct sem_value); }
static uint64_t undo_bytes(unsigned count) { return sizeof(struct sem_undo) + (uint64_t)count * sizeof(int16_t); }
static void object_free(unsigned kind, struct ipc_object* o) {
    if (kind == SEM) {
        struct sem_set* s = (struct sem_set*)o;
        while (s->undos) { struct sem_undo* u = s->undos; s->undos = u->next; kfree(u, undo_bytes(s->count)); }
        ns_limits(o->ns)->used_sems -= s->count;
        kfree(s, sem_bytes(s->count));
    } else if (kind == MSG) {
        struct msg_queue* q = (struct msg_queue*)o;
        while (q->head) { struct message* m = q->head; q->head = m->next; kfree(m, sizeof(*m) + m->size); }
        kfree(q, sizeof(*q));
    } else {
        struct shm_segment* g = (struct shm_segment*)o;
        struct ipc_limits* l = ns_limits(o->ns); uint64_t pages = (g->size + PAGE - 1) / PAGE;
        l->shm_pages = l->shm_pages > pages ? l->shm_pages - pages : 0;
        if (g->memory) memory_file_release(g->memory);
        kfree(g, sizeof(*g));
    }
}
static void object_put(unsigned kind, struct ipc_object* o) { if (!--o->references) object_free(kind, o); }
/* IPC_RMID: out of the table at once; sleepers see `removed` (EIDRM) and the last reference frees it. */
static void object_remove(unsigned kind, struct ipc_object* o) {
    o->removed = true; unlink_object(kind, o); task_wake_channel(o); object_put(kind, o);
}
static uint64_t segment_attaches(const struct shm_segment* g) { return g->memory ? g->memory->maps : 0; }
/* Removed segments whose last mapping went. */
static void shm_reap(void) {
    for (struct ipc_object *o = objects[SHM], *next; o; o = next) {
        next = o->next;
        if ((o->mode & SHM_DEST) && !segment_attaches((struct shm_segment*)o)) object_remove(SHM, o);
    }
}
static void shm_destroy_or_mark(struct shm_segment* g) {
    if (!segment_attaches(g)) { object_remove(SHM, &g->obj); return; }
    g->obj.mode |= SHM_DEST; g->obj.key = 0;   /* IPC_PRIVATE: no new shmget finds it */
}
/* namespaces.c: the IPC namespace `slot` went away with all its objects (Linux free_ipc_ns). */
void sysvipc_namespace_gone(uint32_t slot) {
    for (unsigned kind = 0; kind < KINDS; kind++)
        for (struct ipc_object *o = objects[kind], *next; o; o = next) {
            next = o->next;
            if (o->ns != slot) continue;
            if (kind == SHM) { unlink_object(SHM, o); o->removed = true; object_put(SHM, o); }   /* mappings keep the memory */
            else object_remove(kind, o);
        }
    if (slot < NS_SLOTS) limits[slot].set = false;
}

/* ---- the user-visible structures (x86-64) ---- */
struct perm_abi { int32_t key; uint32_t uid, gid, cuid, cgid, mode; uint16_t seq, pad2; uint32_t pad3; uint64_t unused1, unused2; };
struct semid_abi { struct perm_abi perm; int64_t otime; uint64_t unused1; int64_t ctime; uint64_t unused2; uint64_t nsems, unused3, unused4; };
struct msqid_abi { struct perm_abi perm; int64_t stime, rtime, ctime; uint64_t cbytes, qnum, qbytes; int32_t lspid, lrpid; uint64_t unused4, unused5; };
struct shmid_abi { struct perm_abi perm; uint64_t segsz; int64_t atime, dtime, ctime; int32_t cpid, lpid; uint64_t nattch, unused4, unused5; };
struct seminfo_abi { int32_t semmap, semmni, semmns, semmnu, semmsl, semopm, semume, semusz, semvmx, semaem; };
struct msginfo_abi { int32_t msgpool, msgmap, msgmax, msgmnb, msgmni, msgssz, msgtql; uint16_t msgseg, pad; };
struct shminfo_abi { uint64_t shmmax, shmmin, shmmni, shmseg, shmall, unused[4]; };
struct shm_info_abi { int32_t used_ids, pad; uint64_t shm_tot, shm_rss, shm_swp, swap_attempts, swap_successes; };
_Static_assert(sizeof(struct perm_abi) == 48 && sizeof(struct semid_abi) == 104 && sizeof(struct msqid_abi) == 120 &&
               sizeof(struct shmid_abi) == 112 && sizeof(struct seminfo_abi) == 40 && sizeof(struct msginfo_abi) == 32 &&
               sizeof(struct shminfo_abi) == 72 && sizeof(struct shm_info_abi) == 48, "System V IPC ABI layout");
static uint32_t visible_uid(uint32_t id) { return credentials_visible_id(id, false); }
static uint32_t visible_gid(uint32_t id) { return credentials_visible_id(id, true); }
static int32_t visible_pid(uint32_t pid) { return pid ? (int32_t)task_visible_pid(pid) : 0; }
static void perm_out(const struct ipc_object* o, struct perm_abi* p) {
    memset(p, 0, sizeof(*p)); p->key = o->key; p->uid = visible_uid(o->uid); p->gid = visible_gid(o->gid);
    p->cuid = visible_uid(o->cuid); p->cgid = visible_gid(o->cgid); p->mode = o->mode; p->seq = o->seq;
}
static int copy_out(uint64_t address, const void* data, uint64_t size) {
    if (!accessible(address, size, true)) return -14;
    copy_to_user((void*)address, data, size); return 0;
}
static int copy_in(void* data, uint64_t address, uint64_t size) {
    if (!accessible(address, size, false)) return -14;
    copy_from_user(data, (const void*)address, size); return 0;
}
/* IPC_SET: uid, gid and the permission bits from a user structure whose ipc64_perm comes first */
static int perm_set(struct ipc_object* o, const struct perm_abi* p) {
    uint32_t uid, gid;
    if (!credentials_kernel_from_visible(p->uid, false, &uid) || !credentials_kernel_from_visible(p->gid, true, &gid)) return -22;
    o->uid = uid; o->gid = gid; o->mode = (o->mode & ~0777U) | (p->mode & 0777); o->ctime = now_seconds(); return 0;
}
/* get calls: Linux ipcget (IPC_PRIVATE always new; a key: EEXIST, EACCES, then the type's own checks, ENOENT) */
static int lookup_key(unsigned kind, int32_t key, unsigned flags, struct ipc_object** found) {
    *found = NULL;
    if (!key) return 0;
    struct ipc_object* o = by_key(kind, current_ns(), key);
    if (!o) return flags & IPC_CREAT ? 0 : -2;
    if ((flags & IPC_CREAT) && (flags & IPC_EXCL)) return -17;
    if (!permitted(o, flags & 0777)) return -13;
    *found = o; return 0;
}

/* ---- semaphores ---- */
struct sembuf_abi { uint16_t num; int16_t op; int16_t flags; };
_Static_assert(sizeof(struct sembuf_abi) == 6, "struct sembuf");
static int64_t semget_call(int32_t key, int32_t count, uint32_t flags) {
    struct ipc_limits* l = ns_limits(current_ns());
    if (count < 0 || count > l->sem[0]) return -22;   /* checked first (Linux ksys_semget) */
    struct ipc_object* found; int error = lookup_key(SEM, key, flags, &found); if (error) return error;
    if (found) return count > (int32_t)((struct sem_set*)found)->count ? -22 : found->id;
    if (count == 0) return -22;
    if (l->used_sems + (uint32_t)count > (uint32_t)l->sem[1]) return -28;
    if (count_in(SEM, current_ns()) >= (unsigned)l->sem[3]) return -28;
    struct sem_set* s = kalloc(sem_bytes((unsigned)count)); if (!s) return -12;
    memset(s, 0, sem_bytes((unsigned)count)); s->count = (unsigned)count; s->sems = (struct sem_value*)(s + 1);
    if (!publish(SEM, &s->obj, key, flags)) { kfree(s, sem_bytes((unsigned)count)); return -28; }
    l->used_sems += (uint32_t)count;
    return s->obj.id;
}
static struct sem_undo* undo_of(struct sem_set* s, uint32_t pid, bool create) {
    for (struct sem_undo* u = s->undos; u; u = u->next) if (u->pid == pid) return u;
    if (!create) return NULL;
    struct sem_undo* u = kalloc(undo_bytes(s->count)); if (!u) return NULL;
    memset(u, 0, undo_bytes(s->count)); u->pid = pid; u->next = s->undos; s->undos = u; return u;
}
static void undo_clear(struct sem_set* s, int number) {   /* SETVAL (one semaphore) / SETALL (-1) */
    for (struct sem_undo* u = s->undos; u; u = u->next)
        if (number < 0) memset(u->adjust, 0, s->count * sizeof(int16_t)); else u->adjust[number] = 0;
}
/* All or nothing (Linux perform_atomic_semop): 0 applied, 1 would block (*blocker), -ERANGE. */
static int semop_try(struct sem_set* s, const struct sembuf_abi* ops, unsigned n, struct sem_undo* undo, unsigned* blocker) {
    for (unsigned pass = 0; pass < 2; pass++) {   /* 0: check, 1: apply */
        for (unsigned i = 0; i < n; i++) {
            struct sem_value* v = &s->sems[ops[i].num]; int32_t value = v->value;
            if (pass) {
                v->value += ops[i].op; v->pid = task_pid();
                if ((ops[i].flags & SEM_UNDO) && undo) undo->adjust[ops[i].num] = (int16_t)(undo->adjust[ops[i].num] - ops[i].op);
                continue;
            }
            /* the effect of the earlier operations on the same semaphore counts */
            for (unsigned k = 0; k < i; k++) if (ops[k].num == ops[i].num) value += ops[k].op;
            if (!ops[i].op && value) { *blocker = i; return 1; }
            if (value + ops[i].op < 0) { *blocker = i; return 1; }
            if (value + ops[i].op > SEMVMX) return -34;
            if ((ops[i].flags & SEM_UNDO) && undo) {
                int32_t adjust = undo->adjust[ops[i].num];
                for (unsigned k = 0; k < i; k++) if (ops[k].num == ops[i].num && (ops[k].flags & SEM_UNDO)) adjust -= ops[k].op;
                adjust -= ops[i].op;
                if (adjust < -SEMVMX - 1 || adjust > SEMVMX) return -34;
            }
        }
    }
    s->otime = now_seconds(); return 0;
}
static int64_t semop_call(int64_t id, uint64_t address, uint64_t count, uint64_t timeout) {
    struct ipc_limits* l = ns_limits(current_ns());
    if (count < 1 || id < 0) return -22;
    if (count > (uint64_t)l->sem[2]) return -7;
    struct sembuf_abi local[64], *ops = local; uint64_t bytes = count * sizeof(struct sembuf_abi);
    if (count > 64 && !(ops = kalloc(bytes))) return -12;
    int64_t result = copy_in(ops, address, bytes);
    uint64_t deadline = 0;
    if (!result && timeout) {
        int64_t ts[2]; result = copy_in(ts, timeout, sizeof(ts));
        if (!result && (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)) result = -22;
        if (!result) {
            uint64_t now = native_clock_ns(), span = (uint64_t)ts[0] > 0x7FFFFFFFFFFULL ? ~0ULL / 2 : (uint64_t)ts[0] * 1000000000ULL + (uint64_t)ts[1];
            deadline = now + span; if (!deadline) deadline = 1;
        }
    }
    unsigned top = 0; bool undo = false, alter = false;
    for (uint64_t i = 0; !result && i < count; i++) {
        if (ops[i].num > top) top = ops[i].num;
        if (ops[i].flags & SEM_UNDO) undo = true;
        if (ops[i].op) alter = true;
    }
    struct sem_set* s = result ? NULL : (struct sem_set*)by_id(SEM, id);
    if (!result && !s) result = -22;
    if (!result && top >= s->count) result = -27;   /* EFBIG */
    if (!result && !permitted(&s->obj, alter ? 0222 : 0444)) result = -13;
    struct sem_undo* u = NULL;
    if (!result && undo && !(u = undo_of(s, task_pid(), true))) result = -12;
    while (!result) {
        unsigned blocker = 0; int state = semop_try(s, ops, (unsigned)count, u, &blocker);
        if (state <= 0) { if (!state) task_wake_channel(&s->obj); result = state; break; }
        if (ops[blocker].flags & IPC_NOWAIT) { result = -11; break; }
        struct sem_value* v = &s->sems[ops[blocker].num]; bool zero = !ops[blocker].op;
        if (zero) v->zcnt++; else v->ncnt++;
        s->obj.references++;
        int error = task_wait_until(&s->obj, deadline);
        if (zero) v->zcnt--; else v->ncnt--;
        bool gone = s->obj.removed; object_put(SEM, &s->obj);
        if (gone) { result = -43; break; }
        if (error == -110) { result = -11; break; }   /* semtimedop: EAGAIN when the time ran out */
        if (error) {   /* a signal: one last try, then EINTR */
            int last = semop_try(s, ops, (unsigned)count, u, &blocker);
            if (!last) task_wake_channel(&s->obj);
            result = last == 1 ? -4 : last; break;
        }
    }
    if (ops != local) kfree(ops, bytes);
    return result;
}
static int64_t semctl_call(int64_t id, int64_t number, int64_t command, uint64_t argument) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns);
    if (id < 0) return -22;
    if (command == IPC_INFO || command == SEM_INFO) {
        struct seminfo_abi info = {SEMMNS_DEFAULT, l->sem[3], l->sem[1], SEMMNS_DEFAULT, l->sem[0], l->sem[2], l->sem[2], 20, SEMVMX, SEMVMX};
        if (command == SEM_INFO) { info.semusz = (int32_t)count_in(SEM, ns); info.semaem = (int32_t)l->used_sems; }
        int error = copy_out(argument, &info, sizeof(info)); return error ? error : max_index(SEM, ns);
    }
    struct sem_set* s;
    if (command == SEM_STAT || command == SEM_STAT_ANY) s = id >= IPCMNI ? NULL : (struct sem_set*)by_index(SEM, ns, (unsigned)id);
    else s = (struct sem_set*)by_id(SEM, id);
    if (!s) return -22;
    switch (command) {
    case IPC_STAT: case SEM_STAT: case SEM_STAT_ANY: {
        if (command != SEM_STAT_ANY && !permitted(&s->obj, 0444)) return -13;
        struct semid_abi out; memset(&out, 0, sizeof(out)); perm_out(&s->obj, &out.perm);
        out.otime = s->otime; out.ctime = s->obj.ctime; out.nsems = s->count;
        int error = copy_out(argument, &out, sizeof(out)); if (error) return error;
        return command == IPC_STAT ? 0 : s->obj.id;
    }
    case GETVAL: case GETPID: case GETNCNT: case GETZCNT: {
        if (number < 0 || number >= (int64_t)s->count) return -22;
        if (!permitted(&s->obj, 0444)) return -13;
        struct sem_value* v = &s->sems[number];
        return command == GETVAL ? v->value : command == GETPID ? visible_pid(v->pid) : command == GETNCNT ? (int64_t)v->ncnt : (int64_t)v->zcnt;
    }
    case GETALL: {
        if (!permitted(&s->obj, 0444)) return -13;
        if (!accessible(argument, (uint64_t)s->count * 2, true)) return -14;
        for (unsigned i = 0; i < s->count; i++) if (uput16(argument + (uint64_t)i * 2, (uint16_t)s->sems[i].value)) return -14;
        return 0;
    }
    case SETVAL: {
        if (number < 0 || number >= (int64_t)s->count) return -22;
        int32_t value = (int32_t)argument;
        if (value < 0 || value > SEMVMX) return -34;
        if (!permitted(&s->obj, 0222)) return -13;
        undo_clear(s, (int)number); s->sems[number].value = value; s->sems[number].pid = task_pid();
        s->obj.ctime = now_seconds(); task_wake_channel(&s->obj); return 0;
    }
    case SETALL: {
        if (!permitted(&s->obj, 0222)) return -13;
        uint64_t bytes = (uint64_t)s->count * 2; uint16_t* values = kalloc(bytes); if (!values) return -12;
        int error = copy_in(values, argument, bytes);
        for (unsigned i = 0; !error && i < s->count; i++) if (values[i] > SEMVMX) error = -34;
        if (!error) {
            undo_clear(s, -1);
            for (unsigned i = 0; i < s->count; i++) { s->sems[i].value = values[i]; s->sems[i].pid = task_pid(); }
            s->obj.ctime = now_seconds(); task_wake_channel(&s->obj);
        }
        kfree(values, bytes); return error;
    }
    case IPC_RMID: if (!owner_or_admin(&s->obj)) return -1; object_remove(SEM, &s->obj); return 0;
    case IPC_SET: {
        struct semid_abi in; int error = copy_in(&in, argument, sizeof(in)); if (error) return error;
        if (!owner_or_admin(&s->obj)) return -1;
        return perm_set(&s->obj, &in.perm);
    }
    }
    return -22;
}
/* process.c terminate: SEM_UNDO adjustments of the process are applied (Linux exit_sem) */
static void sem_exit(uint32_t pid) {
    for (struct ipc_object* o = objects[SEM]; o; o = o->next) {
        struct sem_set* s = (struct sem_set*)o;
        for (struct sem_undo** link = &s->undos; *link; link = &(*link)->next) {
            struct sem_undo* u = *link; if (u->pid != pid) continue;
            for (unsigned i = 0; i < s->count; i++) {
                if (!u->adjust[i]) continue;
                int32_t value = s->sems[i].value + u->adjust[i];
                s->sems[i].value = value < 0 ? 0 : value > SEMVMX ? SEMVMX : value; s->sems[i].pid = pid;
            }
            *link = u->next; kfree(u, undo_bytes(s->count));
            s->otime = now_seconds(); task_wake_channel(&s->obj);
            break;
        }
    }
}

/* ---- message queues ---- */
static int64_t msgget_call(int32_t key, uint32_t flags) {
    struct ipc_object* found; int error = lookup_key(MSG, key, flags, &found); if (error) return error;
    if (found) return found->id;
    struct ipc_limits* l = ns_limits(current_ns());
    if (count_in(MSG, current_ns()) >= (unsigned)l->msgmni) return -28;
    struct msg_queue* q = kalloc(sizeof(*q)); if (!q) return -12;
    memset(q, 0, sizeof(*q)); q->qbytes = (uint64_t)l->msgmnb;
    if (!publish(MSG, &q->obj, key, flags)) { kfree(q, sizeof(*q)); return -28; }
    return q->obj.id;
}
static int64_t msgsnd_call(int64_t id, uint64_t address, uint64_t size, uint32_t flags) {
    struct ipc_limits* l = ns_limits(current_ns());
    if (size > (uint64_t)l->msgmax || (int64_t)size < 0 || id < 0) return -22;
    int64_t type; int error = copy_in(&type, address, 8); if (error) return error;
    if (type < 1) return -22;
    struct message* m = kalloc(sizeof(*m) + size); if (!m) return -12;
    m->next = NULL; m->type = type; m->size = size;
    if ((error = copy_in(m->text, address + 8, size))) { kfree(m, sizeof(*m) + size); return error; }
    struct msg_queue* q = (struct msg_queue*)by_id(MSG, id);
    int64_t result = !q ? -22 : !permitted(&q->obj, 0222) ? -13 : 1;
    while (result == 1) {
        if (q->cbytes + size <= q->qbytes && q->qnum + 1 <= q->qbytes) {
            if (q->tail) q->tail->next = m; else q->head = m;
            q->tail = m; q->qnum++; q->cbytes += size; q->lspid = task_pid(); q->stime = now_seconds();
            m = NULL; task_wake_channel(&q->obj); result = 0; break;
        }
        if (flags & IPC_NOWAIT) { result = -11; break; }
        q->obj.references++;
        int wait = task_wait_until(&q->obj, 0);
        bool gone = q->obj.removed; object_put(MSG, &q->obj);
        if (gone) result = -43; else if (wait) result = -4;
    }
    if (m) kfree(m, sizeof(*m) + size);
    return result;
}
static int64_t msgrcv_call(int64_t id, uint64_t address, int64_t size, int64_t type, uint32_t flags) {
    struct ipc_limits* l = ns_limits(current_ns());
    if (id < 0 || size < 0) return -22;
    if ((flags & MSG_COPY) && ((flags & MSG_EXCEPT) || !(flags & IPC_NOWAIT))) return -22;
    if (flags & MSG_COPY) size = size < l->msgmax ? size : l->msgmax;
    enum { ANY, EQUAL, NOT_EQUAL, LESS_EQUAL, NUMBER } mode;
    if (flags & MSG_COPY) mode = NUMBER;
    else if (!type) mode = ANY;
    else if (type < 0) { type = type == (int64_t)0x8000000000000000ULL ? 0x7FFFFFFFFFFFFFFFLL : -type; mode = LESS_EQUAL; }
    else mode = flags & MSG_EXCEPT ? NOT_EQUAL : EQUAL;
    struct msg_queue* q = (struct msg_queue*)by_id(MSG, id);
    if (!q) return -22;
    for (;;) {
        if (!permitted(&q->obj, 0444)) return -13;
        struct message **link = &q->head, **chosen = NULL; int64_t index = 0;
        for (; *link; link = &(*link)->next, index++) {
            struct message* m = *link;
            if (mode == ANY || (mode == EQUAL && m->type == type) || (mode == NOT_EQUAL && m->type != type) ||
                (mode == NUMBER && index == type)) { chosen = link; break; }
            if (mode == LESS_EQUAL && m->type <= type && (!chosen || m->type < (*chosen)->type)) chosen = link;
        }
        if (chosen) {
            struct message* m = *chosen;
            if ((uint64_t)size < m->size && !(flags & MSG_NOERROR)) return -7;   /* E2BIG: it stays queued */
            uint64_t copied = m->size < (uint64_t)size ? m->size : (uint64_t)size;
            if (!(flags & MSG_COPY)) {
                *chosen = m->next; if (q->tail == m) { q->tail = NULL; for (struct message* t = q->head; t; t = t->next) q->tail = t; }
                q->qnum--; q->cbytes -= m->size; q->lrpid = task_pid(); q->rtime = now_seconds(); task_wake_channel(&q->obj);
            }
            int error = copy_out(address, &m->type, 8);
            if (!error) error = copy_out(address + 8, m->text, copied);
            if (!(flags & MSG_COPY)) kfree(m, sizeof(*m) + m->size);   /* gone even on EFAULT (Linux) */
            return error ? error : (int64_t)copied;
        }
        if (flags & IPC_NOWAIT) return -42;   /* ENOMSG */
        q->obj.references++;
        int wait = task_wait_until(&q->obj, 0);
        bool gone = q->obj.removed; object_put(MSG, &q->obj);
        if (gone) return -43;
        if (wait) return -4;
    }
}
static int64_t msgctl_call(int64_t id, int64_t command, uint64_t argument) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns);
    if (id < 0) return -22;
    if (command == IPC_INFO || command == MSG_INFO) {
        struct msginfo_abi info = {l->msgmni * (l->msgmnb / 1024), 16384, l->msgmax, l->msgmnb, l->msgmni, 16, 16384, 0xFFFF, 0};
        if (command == IPC_INFO) info.msgpool = (int32_t)((int64_t)l->msgmni * l->msgmnb / 1024);
        else {
            uint64_t messages = 0, bytes = 0;
            for (struct ipc_object* o = objects[MSG]; o; o = o->next) if (o->ns == ns && !o->removed) { messages += ((struct msg_queue*)o)->qnum; bytes += ((struct msg_queue*)o)->cbytes; }
            info.msgpool = (int32_t)count_in(MSG, ns); info.msgmap = messages > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)messages;
            info.msgtql = bytes > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)bytes;
        }
        int error = copy_out(argument, &info, sizeof(info)); return error ? error : max_index(MSG, ns);
    }
    struct msg_queue* q;
    if (command == MSG_STAT || command == MSG_STAT_ANY) q = id >= IPCMNI ? NULL : (struct msg_queue*)by_index(MSG, ns, (unsigned)id);
    else q = (struct msg_queue*)by_id(MSG, id);
    if (!q) return -22;
    switch (command) {
    case IPC_STAT: case MSG_STAT: case MSG_STAT_ANY: {
        if (command != MSG_STAT_ANY && !permitted(&q->obj, 0444)) return -13;
        struct msqid_abi out; memset(&out, 0, sizeof(out)); perm_out(&q->obj, &out.perm);
        out.stime = q->stime; out.rtime = q->rtime; out.ctime = q->obj.ctime; out.cbytes = q->cbytes; out.qnum = q->qnum;
        out.qbytes = q->qbytes; out.lspid = visible_pid(q->lspid); out.lrpid = visible_pid(q->lrpid);
        int error = copy_out(argument, &out, sizeof(out)); if (error) return error;
        return command == IPC_STAT ? 0 : q->obj.id;
    }
    case IPC_RMID: if (!owner_or_admin(&q->obj)) return -1; object_remove(MSG, &q->obj); return 0;
    case IPC_SET: {
        struct msqid_abi in; int error = copy_in(&in, argument, sizeof(in)); if (error) return error;
        if (!owner_or_admin(&q->obj)) return -1;
        if (in.qbytes > (uint64_t)l->msgmnb && !capable(CAP_SYS_RESOURCE)) return -1;
        if ((error = perm_set(&q->obj, &in.perm))) return error;
        q->qbytes = in.qbytes; task_wake_channel(&q->obj); return 0;
    }
    }
    return -22;
}

/* ---- shared memory ---- */
int64_t native_map_shared(struct memory_file* memory, uint64_t address, uint64_t length, unsigned prot, unsigned how, const char* path);
struct memory_file* native_area_memory(uint64_t address);
int native_unmap_memory(uint64_t address, uint64_t size, struct memory_file* memory);
static int64_t shmget_call(int32_t key, uint64_t size, uint32_t flags) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns);
    struct ipc_object* found; int error = lookup_key(SHM, key, flags, &found); if (error) return error;
    if (found) return size > ((struct shm_segment*)found)->size ? -22 : found->id;
    if (flags & SHM_HUGETLB) return -12;   /* no huge pages here */
    uint64_t pages = (size + PAGE - 1) / PAGE;
    if (size < 1 || size > l->shmmax) return -22;
    if (l->shm_pages + pages < l->shm_pages || l->shm_pages + pages > l->shmall) return -28;
    if (count_in(SHM, ns) >= (unsigned)l->shmmni) return -28;
    struct shm_segment* g = kalloc(sizeof(*g)); if (!g) return -12;
    memset(g, 0, sizeof(*g));
    if (!(g->memory = memory_file_create(0))) { kfree(g, sizeof(*g)); return -12; }
    g->memory->size = size; g->memory->mode = flags & 0777; memory_file_swappable(g->memory);
    g->size = size; g->cpid = task_pid();
    if (!publish(SHM, &g->obj, key, flags)) { memory_file_release(g->memory); kfree(g, sizeof(*g)); return -28; }
    l->shm_pages += pages;
    return g->obj.id;
}
static int64_t shmat_call(int64_t id, uint64_t address, uint32_t flags) {
    if (id < 0) return -22;
    unsigned how = 0;   /* 0 anywhere, 1 fixed (must be free), 2 fixed replacing (SHM_REMAP) */
    if (address) {
        if (address & (PAGE - 1)) {
            if (!(flags & SHM_RND)) return -22;
            address &= ~(PAGE - 1);
            if (!address && (flags & SHM_REMAP)) return -22;
        }
        how = flags & SHM_REMAP ? 2 : 1;
    } else if (flags & SHM_REMAP) return -22;
    shm_reap();
    struct shm_segment* g = (struct shm_segment*)by_id(SHM, id); if (!g) return -22;
    unsigned prot = 1, access = 0444; bool writable = !(flags & SHM_RDONLY);
    if (writable) { prot |= 2; access |= 0222; }
    if (flags & SHM_EXEC) { prot |= 4; access |= 0111; }
    if (!permitted(&g->obj, access)) return -13;
    char path[40]; kfmt(path, sizeof(path), 0, "/SYSV%08x (deleted)", (uint32_t)g->obj.key);
    uint64_t length = (g->size + PAGE - 1) & ~(PAGE - 1);
    int64_t result = native_map_shared(g->memory, address, length, prot, how, path);
    if (result == -17) return -22;   /* overlaps a mapping without SHM_REMAP */
    if (result < 0 && result > -4096) return result;
    g->atime = now_seconds(); g->lpid = task_pid();
    return result;
}
static int64_t shmdt_call(uint64_t address) {
    if (address & (PAGE - 1)) return -22;
    struct memory_file* memory = native_area_memory(address);
    struct shm_segment* g = NULL;
    for (struct ipc_object* o = objects[SHM]; memory && o && !g; o = o->next) if (((struct shm_segment*)o)->memory == memory) g = (struct shm_segment*)o;
    if (!g) return -22;
    int error = native_unmap_memory(address, (g->size + PAGE - 1) & ~(PAGE - 1), memory); if (error) return error;
    g->dtime = now_seconds(); g->lpid = task_pid();
    shm_reap(); return 0;
}
static int64_t shmctl_call(int64_t id, int64_t command, uint64_t argument) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns);
    if (id < 0) return -22;
    shm_reap();
    if (command == IPC_INFO) {
        struct shminfo_abi info; memset(&info, 0, sizeof(info));
        info.shmmax = l->shmmax; info.shmmin = 1; info.shmmni = info.shmseg = (uint64_t)l->shmmni; info.shmall = l->shmall;
        int error = copy_out(argument, &info, sizeof(info)); return error ? error : max_index(SHM, ns);
    }
    if (command == SHM_INFO) {
        struct shm_info_abi info; memset(&info, 0, sizeof(info)); info.used_ids = (int32_t)count_in(SHM, ns); info.shm_tot = l->shm_pages;
        for (struct ipc_object* o = objects[SHM]; o; o = o->next) if (o->ns == ns && !o->removed) info.shm_rss += ((struct shm_segment*)o)->memory->pages;
        int error = copy_out(argument, &info, sizeof(info)); return error ? error : max_index(SHM, ns);
    }
    struct shm_segment* g;
    if (command == SHM_STAT || command == SHM_STAT_ANY) g = id >= IPCMNI ? NULL : (struct shm_segment*)by_index(SHM, ns, (unsigned)id);
    else g = (struct shm_segment*)by_id(SHM, id);
    if (!g) return -22;
    switch (command) {
    case IPC_STAT: case SHM_STAT: case SHM_STAT_ANY: {
        if (command != SHM_STAT_ANY && !permitted(&g->obj, 0444)) return -13;
        struct shmid_abi out; memset(&out, 0, sizeof(out)); perm_out(&g->obj, &out.perm);
        out.segsz = g->size; out.atime = g->atime; out.dtime = g->dtime; out.ctime = g->obj.ctime;
        out.cpid = visible_pid(g->cpid); out.lpid = visible_pid(g->lpid); out.nattch = segment_attaches(g);
        int error = copy_out(argument, &out, sizeof(out)); if (error) return error;
        return command == IPC_STAT ? 0 : g->obj.id;
    }
    case IPC_RMID: if (!owner_or_admin(&g->obj)) return -1; shm_destroy_or_mark(g); return 0;
    case IPC_SET: {
        struct shmid_abi in; int error = copy_in(&in, argument, sizeof(in)); if (error) return error;
        if (!owner_or_admin(&g->obj)) return -1;
        return perm_set(&g->obj, &in.perm);
    }
    case SHM_LOCK: case SHM_UNLOCK:
        if (!capable(14) && (euid() != g->obj.uid && euid() != g->obj.cuid)) return -1;   /* CAP_IPC_LOCK or the owner */
        if (command == SHM_LOCK && !capable(14) && !task_rlimit_soft(8)) return -1;
        if (command == SHM_LOCK) g->obj.mode |= SHM_LOCKED; else g->obj.mode &= ~SHM_LOCKED;
        return 0;
    }
    return -22;
}
/* process.c terminate: SEM_UNDO; kernel.shm_rmid_forced removes the exiting process's unattached segments */
void sysvipc_exit(uint32_t pid) {
    if (!in_use[SEM] && !in_use[SHM]) return;
    sem_exit(pid);
    for (struct ipc_object *o = objects[SHM], *next; o; o = next) {
        next = o->next;
        struct shm_segment* g = (struct shm_segment*)o;
        if (g->cpid == pid && ns_limits(o->ns)->shm_rmid_forced) shm_destroy_or_mark(g);
    }
    shm_reap();
}

/* ---- /proc/sysvipc and /proc/sys/kernel (procext.c) ---- */
uint32_t sysvipc_proc_mode(const char* name) {   /* 0: not a System V IPC entry */
    static const char* const tables[] = {"sysvipc/shm", "sysvipc/sem", "sysvipc/msg"};
    static const char* const sysctls[] = {"sys/kernel/sem", "sys/kernel/msgmax", "sys/kernel/msgmnb", "sys/kernel/msgmni",
                                          "sys/kernel/shmmax", "sys/kernel/shmall", "sys/kernel/shmmni", "sys/kernel/shm_rmid_forced"};
    for (unsigned i = 0; i < sizeof(tables) / sizeof(tables[0]); i++) if (!strcmp(name, tables[i])) return 0100444;
    for (unsigned i = 0; i < sizeof(sysctls) / sizeof(sysctls[0]); i++) if (!strcmp(name, sysctls[i])) return 0100644;
    return 0;
}
int sysvipc_proc_text(const char* name, char* out, unsigned capacity) {
    uint32_t ns = current_ns(); struct ipc_limits* l = ns_limits(ns); unsigned used = 0;
    shm_reap();
    if (!strcmp(name, "sysvipc/shm")) {
        used = kfmt(out, capacity, 0, "       key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime                   rss                  swap\n");
        for (struct ipc_object* o = objects[SHM]; o; o = o->next) {
            if (o->ns != ns || o->removed) continue;
            struct shm_segment* g = (struct shm_segment*)o;
            used = kfmt(out, capacity, used, "%10d %10d  %4o %21lu %5u %5u  %5lu %5u %5u %5u %5u %10lld %10lld %10lld %21lu %21lu\n",
                        o->key, o->id, o->mode, g->size, (unsigned)visible_pid(g->cpid), (unsigned)visible_pid(g->lpid), segment_attaches(g),
                        visible_uid(o->uid), visible_gid(o->gid), visible_uid(o->cuid), visible_gid(o->cgid), g->atime, g->dtime, o->ctime,
                        g->memory->pages * PAGE, (uint64_t)0);
        }
    } else if (!strcmp(name, "sysvipc/sem")) {
        used = kfmt(out, capacity, 0, "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");
        for (struct ipc_object* o = objects[SEM]; o; o = o->next) {
            if (o->ns != ns || o->removed) continue;
            struct sem_set* s = (struct sem_set*)o;
            used = kfmt(out, capacity, used, "%10d %10d  %4o %10u %5u %5u %5u %5u %10lld %10lld\n", o->key, o->id, o->mode, s->count,
                        visible_uid(o->uid), visible_gid(o->gid), visible_uid(o->cuid), visible_gid(o->cgid), s->otime, o->ctime);
        }
    } else if (!strcmp(name, "sysvipc/msg")) {
        used = kfmt(out, capacity, 0, "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n");
        for (struct ipc_object* o = objects[MSG]; o; o = o->next) {
            if (o->ns != ns || o->removed) continue;
            struct msg_queue* q = (struct msg_queue*)o;
            used = kfmt(out, capacity, used, "%10d %10d  %4o  %10lu %10lu %5u %5u %5u %5u %5u %5u %10lld %10lld %10lld\n", o->key, o->id, o->mode,
                        q->cbytes, q->qnum, (unsigned)visible_pid(q->lspid), (unsigned)visible_pid(q->lrpid), visible_uid(o->uid), visible_gid(o->gid),
                        visible_uid(o->cuid), visible_gid(o->cgid), q->stime, q->rtime, o->ctime);
        }
    } else if (!strcmp(name, "sys/kernel/sem")) used = kfmt(out, capacity, 0, "%d\t%d\t%d\t%d\n", l->sem[0], l->sem[1], l->sem[2], l->sem[3]);
    else if (!strcmp(name, "sys/kernel/msgmax")) used = kfmt(out, capacity, 0, "%d\n", l->msgmax);
    else if (!strcmp(name, "sys/kernel/msgmnb")) used = kfmt(out, capacity, 0, "%d\n", l->msgmnb);
    else if (!strcmp(name, "sys/kernel/msgmni")) used = kfmt(out, capacity, 0, "%d\n", l->msgmni);
    else if (!strcmp(name, "sys/kernel/shmmax")) used = kfmt(out, capacity, 0, "%lu\n", l->shmmax);
    else if (!strcmp(name, "sys/kernel/shmall")) used = kfmt(out, capacity, 0, "%lu\n", l->shmall);
    else if (!strcmp(name, "sys/kernel/shmmni")) used = kfmt(out, capacity, 0, "%d\n", l->shmmni);
    else if (!strcmp(name, "sys/kernel/shm_rmid_forced")) used = kfmt(out, capacity, 0, "%d\n", l->shm_rmid_forced);
    else return -2;
    return (int)used;
}
static bool parse_numbers(const char* text, uint64_t* values, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        while (*text == ' ' || *text == '\t') text++;
        if (*text < '0' || *text > '9') return false;
        uint64_t v = 0;
        while (*text >= '0' && *text <= '9') { if (v > (~0ULL - 9) / 10) return false; v = v * 10 + (uint64_t)(*text++ - '0'); }
        values[i] = v;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n') text++;
    return !*text;
}
/* A sysctl write (proc.c checked root and the mode): Linux's ranges (proc_dointvec_minmax / proc_doulongvec_minmax). */
int64_t sysvipc_sysctl_write(const char* name, const char* value) {
    struct ipc_limits* l = ns_limits(current_ns()); uint64_t v[4];
    if (!strcmp(name, "sys/kernel/sem")) {
        if (!parse_numbers(value, v, 4)) return -22;
        for (unsigned i = 0; i < 4; i++) if (v[i] > 0x7FFFFFFF) return -22;
        if (v[3] > IPCMNI) return -34;   /* SEMMNI above IPCMNI: ERANGE (Linux proc_ipc_sem_dointvec) */
        for (unsigned i = 0; i < 4; i++) l->sem[i] = (int32_t)v[i];
        return 0;
    }
    if (!strcmp(name, "sys/kernel/shmmax") || !strcmp(name, "sys/kernel/shmall")) {
        if (!parse_numbers(value, v, 1)) return -22;
        if (name[11] == 'm') l->shmmax = v[0]; else l->shmall = v[0];
        return 0;
    }
    if (!parse_numbers(value, v, 1) || v[0] > 0x7FFFFFFF) return -22;
    if (!strcmp(name, "sys/kernel/msgmax")) l->msgmax = (int32_t)v[0];
    else if (!strcmp(name, "sys/kernel/msgmnb")) l->msgmnb = (int32_t)v[0];
    else if (!strcmp(name, "sys/kernel/msgmni")) { if (v[0] > IPCMNI) return -22; l->msgmni = (int32_t)v[0]; }
    else if (!strcmp(name, "sys/kernel/shmmni")) { if (v[0] > IPCMNI) return -22; l->shmmni = (int32_t)v[0]; }
    else if (!strcmp(name, "sys/kernel/shm_rmid_forced")) { if (v[0] > 1) return -22; l->shm_rmid_forced = (int32_t)v[0]; }
    else return -13;
    return 0;
}

/* ---- system calls: 29 shmget, 30 shmat, 31 shmctl, 64 semget, 65 semop, 66 semctl, 67 shmdt, 68 msgget, 69 msgsnd,
 * 70 msgrcv, 71 msgctl, 220 semtimedop ---- */
int64_t sysvipc_call(struct registers* r) {
    switch (r->rax) {
    case 64: return semget_call((int32_t)r->rdi, (int32_t)r->rsi, (uint32_t)r->rdx);
    case 65: return semop_call((int32_t)r->rdi, r->rsi, r->rdx, 0);
    case 220: return semop_call((int32_t)r->rdi, r->rsi, r->rdx, r->r10);
    case 66: return semctl_call((int32_t)r->rdi, (int32_t)r->rsi, (int32_t)r->rdx, r->r10);
    case 68: return msgget_call((int32_t)r->rdi, (uint32_t)r->rsi);
    case 69: return msgsnd_call((int32_t)r->rdi, r->rsi, r->rdx, (uint32_t)r->r10);
    case 70: return msgrcv_call((int32_t)r->rdi, r->rsi, (int64_t)r->rdx, (int64_t)r->r10, (uint32_t)r->r8);
    case 71: return msgctl_call((int32_t)r->rdi, (int32_t)r->rsi, r->rdx);
    case 29: return shmget_call((int32_t)r->rdi, r->rsi, (uint32_t)r->rdx);
    case 30: return shmat_call((int32_t)r->rdi, r->rsi, (uint32_t)r->rdx);
    case 31: return shmctl_call((int32_t)r->rdi, (int32_t)r->rsi, r->rdx);
    case 67: return shmdt_call(r->rdi);
    }
    return -38;
}
unsigned sysvipc_objects(void) { return in_use[SEM] + in_use[MSG] + in_use[SHM]; }   /* leak checks */

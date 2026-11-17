/* Stable process identities outlive tasks when held by groups or socket credentials. */
#include "native.h"
#include "../../kernel/vga.h"

static struct pid_namespace root;
static struct pid_identity* identities;
static uint64_t next_namespace;
struct pid_namespace* pid_namespace_root(void) { return &root; }
bool pid_namespace_contains(struct pid_namespace* ancestor, struct pid_namespace* space) {
    for (; space && space->depth >= ancestor->depth; space = space->parent) if (space == ancestor) return true;
    return false;
}
void pid_namespace_retain(struct pid_namespace* space) {
    if (space == &root) return;
    if (!space || !space->references || space->references == ~0u) panic("PID namespace reference overflow");
    space->references++;
}
void pid_namespace_release(struct pid_namespace* space) {
    while (space && space != &root) {
        if (!space->references) panic("unowned PID namespace");
        if (--space->references) return;
        struct pid_namespace* parent = space->parent;
        namespace_release(space->owner); page_free((uint64_t)space); space = parent;
    }
}
struct pid_namespace* pid_namespace_create(struct pid_namespace* parent, struct credentials* owner, int* error) {
    *error = -1;
    struct user_namespace* user = credentials_namespace(owner);
    if (!credentials_capable(owner, user, 21)) return NULL;
    *error = -28; if (parent->depth >= 32) return NULL;
    *error = -12;
    struct pid_namespace* space = (void*)page_alloc(); if (!space) return NULL;
    space->parent = parent; pid_namespace_retain(parent);
    space->owner = user; namespace_retain(user);
    space->depth = parent->depth + 1; space->references = 1; space->id = ++next_namespace;
    *error = 0; return space;
}
struct pid_identity* pid_allocate(struct pid_namespace* space) {
    /* shortcut: IDs grow monotonically; add reuse for sustained process churn. */
    for (struct pid_namespace* ns = space; ns; ns = ns->parent)
        if (ns->dead || ns->last == 0x7FFFFFFFU) return NULL;
    struct pid_identity* pid = (void*)page_alloc(); if (!pid) return NULL;
    pid->space = space; pid_namespace_retain(space); pid->references = 1;
    for (struct pid_namespace* ns = space; ns; ns = ns->parent) pid->numbers[ns->depth] = ++ns->last;
    pid->next = identities; identities = pid; return pid;
}
void pid_publish(struct pid_identity* pid) {
    if (pid->published) panic("PID published twice");
    pid->published = true;
    if (pid->numbers[pid->space->depth] == 1) pid->space->init = pid->numbers[0];
}
void pid_retain(struct pid_identity* pid) {
    if (!pid) return;
    if (!pid->references || pid->references == ~0u) panic("PID reference overflow");
    pid->references++;
}
void pid_release(struct pid_identity* pid) {
    if (!pid) return;
    if (!pid->references) panic("unowned PID identity");
    if (--pid->references) return;
    struct pid_identity** link = &identities; while (*link && *link != pid) link = &(*link)->next;
    if (!*link) panic("missing PID identity");
    *link = pid->next;
    if (!pid->published) for (struct pid_namespace* ns = pid->space; ns; ns = ns->parent) {
        if (ns->last != pid->numbers[ns->depth]) panic("unpublished PID allocation interleaved");
        ns->last--;
    }
    pid_namespace_release(pid->space); page_free((uint64_t)pid);
}
uint32_t pid_number(struct pid_identity* pid, struct pid_namespace* space) {
    return pid && pid_namespace_contains(space, pid->space) ? pid->numbers[space->depth] : 0;
}
struct pid_identity* pid_lookup(uint32_t number, struct pid_namespace* space) {
    if (!number) return NULL;
    for (struct pid_identity* pid = identities; pid; pid = pid->next) if (pid_number(pid, space) == number) return pid;
    return NULL;
}
void pid_reset(void) {
    if (identities) panic("PID identities leaked across process runner");
    root.last = root.init = 0; root.dead = false;
}
void pid_selftest(void) {
    uint64_t before = memory_used_pages(); int error;
    struct credentials* owner = credentials_clone(NULL, true, &error);
    if (!owner) panic("PID owner allocation");
    if (pid_namespace_create(&root, credentials_current(), &error) || error != -1) panic("unprivileged PID namespace creation");
    struct pid_namespace* a = pid_namespace_create(&root, owner, &error);
    struct pid_namespace* b = pid_namespace_create(&root, owner, &error);
    if (!a || !b) panic("PID namespace allocation");
    struct pid_namespace* nested = pid_namespace_create(a, owner, &error);
    if (!nested) panic("nested PID namespace allocation");
    struct pid_identity* first = pid_allocate(a); if (!first) panic("PID allocation"); pid_publish(first);
    struct pid_identity* other = pid_allocate(b); if (!other) panic("PID allocation"); pid_publish(other);
    struct pid_identity* child = pid_allocate(nested); if (!child) panic("PID allocation"); pid_publish(child);
    if (pid_number(first, a) != 1 || pid_number(other, b) != 1 || pid_number(child, nested) != 1 ||
        pid_number(child, a) != 2 || pid_number(first, b) || pid_number(first, nested) ||
        pid_number(child, &root) != 3 || pid_lookup(2, a) != child || pid_lookup(1, b) != other)
        panic("PID ancestor visibility or sibling isolation");
    pid_retain(child); pid_release(child); pid_namespace_release(nested);
    if (pid_number(child, a) != 2) panic("retained PID identity lost its namespace");
    struct pid_identity* aborted = pid_allocate(a); if (!aborted) panic("PID rollback allocation"); pid_release(aborted);
    aborted = pid_allocate(a); if (!aborted || pid_number(aborted, a) != 3) panic("failed PID allocation consumed an ID"); pid_release(aborted);
    a->dead = true; if (pid_allocate(a) || pid_allocate(child->space)) panic("dead PID namespace accepted process");
    pid_namespace_release(a); pid_namespace_release(b); credentials_destroy(owner);
    pid_release(first); pid_release(other); pid_release(child); pid_reset();
    if (memory_used_pages() != before) panic("PID namespace ownership leak");
    vga_print("[64] PASS PID identities: namespace visibility, retained mappings, rollback and ownership\n");
}

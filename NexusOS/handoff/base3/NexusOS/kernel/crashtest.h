/* ============================================================================
 * NexusOS — crashtest: deliberate kernel faults to exercise kguard — Phase 55
 * ============================================================================
 * Shell command:  crashtest <null|gpf|ud|div|wp|stack|app|stats>
 *   null   read *(uint32_t*)0           -> #PF (NULL page is unmapped)
 *   gpf    load a selector past the GDT -> #GP
 *   ud     execute ud2                  -> #UD
 *   div    divide by a volatile zero    -> #DE
 *   wp     write into kernel .text      -> #PF (W^X: image is read-only)
 *   stack  unbounded recursion          -> #PF on the stack guard -> #DF task
 *   app    open a window whose draw callback writes to NULL, inside the
 *          desktop: the window is closed, a toast appears, the desktop lives
 *   stats  show how many faults kguard has recovered so far
 * ============================================================================ */
#ifndef CRASHTEST_H
#define CRASHTEST_H

void cmd_crashtest(int argc, char* argv[]);

#endif /* CRASHTEST_H */

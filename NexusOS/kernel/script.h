/* ============================================================================
 * NexusOS — Scripting Engine (Header) — Phase 36
 * ============================================================================
 * NexusScript: a small, integer + string scripting language for OS automation
 * and macros. A real interpreter — lexer -> token stream -> recursive-descent
 * evaluator walked by a token cursor.
 *
 * Language summary:
 *   # comment
 *   let x = 2 + 3 * 4          variables (int or string)
 *   x = x + 1                  reassignment
 *   print "hello"             output (int -> decimal, string -> text)
 *   print x
 *   run "ls"                  execute a shell command line (automation)
 *   if a < b then ... elif ... else ... end
 *   while i <= 10 do ... end
 *   operators: + - * / %  == != < <= > >=  and or not
 *   builtins:  len(x)  str(x)  abs(x)
 *
 * Safety: every executed statement (and loop iteration) decrements a step
 * budget, so a runaway script can never hang the kernel.
 * ============================================================================ */

#ifndef SCRIPT_H
#define SCRIPT_H

#include "types.h"

/* Limits */
#define SCRIPT_MAX_SRC      8192     /* max source size we will load          */
#define SCRIPT_MAX_TOKENS   1024     /* max tokens per script                 */
#define SCRIPT_TOK_TEXT     64       /* max identifier / string token length  */
#define SCRIPT_STR_MAX      128      /* max runtime string value length       */
#define SCRIPT_MAX_VARS     32       /* max variables per run                 */
#define SCRIPT_MAX_STEPS    500000   /* statement/iteration budget            */
#define SCRIPT_MAX_DEPTH    64       /* expression recursion guard            */
#define SCRIPT_MAX_NEST     8        /* nested `run "script ..."` guard       */

/* Result codes */
#define SCRIPT_OK            0
#define SCRIPT_ERR_NOTFOUND  1       /* file not found                        */
#define SCRIPT_ERR_TOOBIG    2       /* source or token count exceeds limits  */
#define SCRIPT_ERR_NOMEM     3
#define SCRIPT_ERR_SYNTAX    4       /* lex/parse error                       */
#define SCRIPT_ERR_RUNTIME   5       /* runtime error (e.g. step budget)      */
#define SCRIPT_ERR_NEST      6       /* script nesting too deep               */

/* Initialize the scripting engine (installs a sample script). */
void script_init(void);

/* Run a script from a null-terminated source string. `name` is used only for
 * diagnostics. Returns a SCRIPT_* code. */
int  script_run_source(const char* src, const char* name);

/* Load a script file from the VFS and run it. Returns a SCRIPT_* code. */
int  script_run_file(const char* path);

/* Human-readable description of a SCRIPT_* code. */
const char* script_strerror(int code);

#endif /* SCRIPT_H */

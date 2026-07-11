/* ============================================================================
 * NexusOS — AI Assistant (Header) — Phase 46
 * ============================================================================
 * Era 6 (Polish). An OFFLINE, rule-based command assistant — NOT a neural
 * network. There is no internet LLM reachable from a freestanding kernel, so
 * this is an honest heuristic helper built on three small knowledge bases:
 *
 *   1. Natural-language -> shell command translation (`ask "<text>"`):
 *      tokenizes + normalizes the request, scores it against an intent table
 *      (keyword sets -> command templates), extracts a simple argument, and —
 *      if confident — runs the resolved command via shell_exec_line(). Honest
 *      when unsure: it shows the candidate command(s) and, for destructive
 *      commands, never auto-runs on a weak match.
 *
 *   2. CLI help / "what does X do" (`ai [topic]`): an embedded command
 *      knowledge base (the machine-readable command list the shell never had).
 *      `ai` = grouped overview, `ai <cmd>` = description + usage + related,
 *      `ai <free text>` = best-effort explanation + closest command.
 *
 *   3. Smart file search (`find <query>`): fuzzy filename match (case-folded
 *      substring + subsequence) AND content grep (reads each RAM-FS file),
 *      ranked, with the matching line as a snippet.
 *
 * A fourth pillar — editor autocomplete (Tab) — lives in editor.c and calls
 * assistant_complete() here for its candidate list.
 * ============================================================================ */

#ifndef ASSISTANT_H
#define ASSISTANT_H

#include "types.h"

/* One-time init at boot (prints a banner; tables are static). */
void assistant_init(void);

/* --- Pillar 1: natural language -> command -------------------------------- */
/* Interpret `query`, print the reasoning + resolved command, and run it when
 * confident. `run` = actually execute (false = dry-run / explain only). */
void assistant_ask(const char* query, bool run);

/* --- Pillar 2: help / knowledge base -------------------------------------- */
/* `topic` NULL/empty = grouped overview; a command name = its detail; any
 * other text = best-match explanation. */
void assistant_help(const char* topic);

/* --- Pillar 3: smart file search ------------------------------------------ */
/* Search filenames (fuzzy) and file contents (grep) for `query`; print ranked
 * results with snippets. */
void assistant_find(const char* query);

/* --- Pillar 4: editor autocomplete ---------------------------------------- */
/* Given a word prefix, fill `out` (size `out_max`) with the best completion
 * and return the number of total candidates. `nth` selects among candidates
 * (for Tab-cycling). `buffer_words` (may be NULL) is extra context words
 * (space/newline separated) harvested from the editor buffer to complete
 * against in addition to the built-in keyword list. Returns 0 if none. */
int assistant_complete(const char* prefix, int nth,
                       const char* buffer_words, char* out, int out_max);

#endif /* ASSISTANT_H */

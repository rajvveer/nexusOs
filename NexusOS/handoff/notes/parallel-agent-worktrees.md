---
name: parallel-agent-worktrees
description: "How to run several feature subagents on NexusOS in parallel (robocopy worktrees + tools junction, base snapshot for 3-way git merge-file, build cwd rule, port ranges) — used for Phase 54 and Phase 55"
metadata:
  node_type: memory
  type: project
  originSessionId: 37d1b0eb-50a3-46b5-aea1-6f0971a47e47
  modified: 2026-10-08T14:47:01.261Z
---

Recipe that worked (2026-10-08) for building big NexusOS features with 3-4 subagents at once:

- **Snapshot first:** `robocopy <repo> <scratch>/baseN/NexusOS /E /XD tools screenshots test_out __pycache__ /XF *.o *.png *.log ...`
  then one copy per agent (`ws_<name>/NexusOS`). baseN is the common ancestor for
  `git merge-file -L real -L base -L ws <real> <base> <ws>` after the agents finish
  (`scratchpad/mergeplan.py` classifies NEW/SINGLE/MULTI files and applies the merges).
- **tools (1.3 GB) must be a junction, not a copy:** Git Bash `ln -s` on a directory
  silently DEEP-COPIES it (no symlink privilege) — use `cmd /c mklink /J <ws>/NexusOS/tools <repo>/tools`.
- **build.bat needs cwd = the worktree** (`cd <ws>/NexusOS && cmd.exe //c "<full path>\\build.bat"`),
  otherwise `boot\boot.bin` "unable to open output file".
- Give each agent its own QEMU monitor-port range and a space-free work dir
  (`C:\nexustest\<tag>`), tell them never to `taskkill` all QEMU processes (others are testing).
- A shared brief file (`AGENT_COMMON.md`: build rules, no libgcc 64-bit div, 0 warnings,
  size cap, heredoc-backslash trap, harness usage, F12-to-console, report format) keeps
  the per-agent prompts short and consistent.
- Ask agents to keep shared-file edits (shell.c, kernel.c, build.bat, desktop.c, ramfs.c)
  in delimited blocks next to unique anchors so merge-file conflicts stay small.

See [[phase54-isolation-usb-folders]], [[headless-test-harness-gotchas]], [[nexusos-boot-and-test]].

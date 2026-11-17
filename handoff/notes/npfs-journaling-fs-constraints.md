---
name: npfs-journaling-fs-constraints
description: NPFS (Phase 51) journaling FS — disk window, journal/txn sizing limits, and the 3 bugs fixed in bring-up
metadata:
  type: project
---

NPFS (`kernel/npfs.c/h`, Phase 51) is a from-scratch journaling filesystem on
the **primary-master IDE disk** (the same disk FAT32 uses, via `ata.c`). Key
constraints to respect before changing it:

- **It lives in a reserved high-LBA window at `NPFS_WINDOW_LBA` (20480, =10 MiB
  in).** Layout: `[super][journal 192 blk][inodes 32 blk][bitmap 1 blk][data]`.
  It **never auto-formats** — `npfs_mount` only succeeds on a valid superblock
  magic (`0x4E504653`), so FAT32 (low LBAs) and the Phase-50 installer MBR
  (LBA 0/2048) are safe. Don't move the window without auditing both.
- **The block bitmap is ONE 512 B block = 4096 bits**, so `data_blocks` is
  CAPPED to 4096 at format (2 MiB of data). Any loop over data blocks must stay
  `< sb.data_blocks` or it over-reads the bitmap buffer (this was a real bug).
- **A single transaction is bounded by the journal descriptor (one 512 B block
  = 125 LBAs).** `NPFS_MAX_TXN_BLOCKS=125`; a single file write is capped to
  `NPFS_MAX_WRITE_BLOCKS=122` (122 blocks ≈ 61 KB) so data + indirect + bitmap +
  inode fit one txn. Inode = direct[12] + 1 single-indirect (128 ptrs).
- **The txn buffer is ~64 KB — it MUST be static, not on the stack.** There are
  two file-scope statics (`g_txn` for commits, `g_replay_txn` for mount replay);
  safe only because the kernel is single-threaded cooperative and ops never nest.
- **WAL ordering is the whole point**: journal descriptor + block images +
  COMMIT record (CRC32 over the txn) are written BEFORE checkpointing to real
  homes; `npfs_mount` replays a committed-but-uncheckpointed txn. `npfs
  crashtest` proves it via a `skip_next_checkpoint` hook. Keep that ordering.
- `txn_add` **replaces staged blocks by LBA** (not append) and allocators read
  the staged copy via `txn_find` — required so multiple allocs/inode updates in
  one txn compose instead of clobbering each other (was a bug).
- Reuses `pkg_crc32` (no duplicate CRC, no 64-bit math). Integer-only.
- Deferred: flat root namespace (no subdirs), no VFS integration (own `npfs`
  command namespace, separate from the RAM-FS behind `ls`/`cat`), not the boot
  FS. Related: [[nexusos-boot-and-test]].

# Download goal - completed

User requested removal of artificial download limits, and explicitly asked to set
a goal. Disk streaming, storage growth, failure handling and QEMU verification are complete.
No commit was made. The chronology below records intermediate states, not current limits.

Backup before edits: `C:\nexustest\before_downloads_20261009_231417`.

Scope: browser + wget write bounded chunks to FAT; downloads survive reboot and
can be found in the UI. Preserve existing files and remove failed partial files.
Keep RAM page/image rendering limits separate from the ability to save any URL.

Storage plan: support a FAT32 MBR partition starting at LBA 32768 (after the
existing 16 MiB image). A host grow tool rebuilds/migrates into a temporary larger
image, preserving NPFS/persistence at LBA 20480..32767 and existing app/file data,
then atomically replaces the image. Keep legacy superfloppy images supported.
Reuse/extend userland/fatput.py for geometry and migration. FAT32's actual per-file
limit is 0xFFFFFFFF bytes; ATA is LBA28. Do not claim infinite physical storage.

FAT driver needs volume bounds, allocation hint and sequential cursor (currently
it scans from cluster 2 and walks from the first cluster on every 4 KiB write),
checked metadata writes/mirrored FATs, and correct large-file overflow handling.
Long filenames currently do not work despite the old driver header claiming LFN.

Main file paths to touch: kernel/fat32.c/h, shared download sink, browser.c,
netcmd.c, netstream.c/h (content disposition and length checks), filemgr.c/h,
build.bat/Makefile, userland/fatput.py, storage grow script, tests and docs.

Implemented so far: bounded FAT partition I/O, allocation hint + sequential
cursors, mirrored FAT writes, long UTF-8 filenames, temporary-name download
publication via one SFN sector update, shared kernel/download.c sink, wget disk
streaming, browser disk streaming and Ctrl+S/Save, Downloads view/navigation in
File Manager, quoted arguments in the kernel shell, HTTP filename/length parsing.
`storage.py` grows/migrates to a FAT partition after LBA 32768 and verifies the
protected window. build.bat apps/run invoke its --ensure path (1 GiB default);
build.bat storage [MiB] is the explicit grow command. Main disk still 16 MiB;
only private test images have been expanded so far.

Verified: host FAT atomicity tests; 16->64->128 MiB migration including long/UTF-8
names and protected bytes; existing userland VM workflow on expanded partition;
HTTP and HTTPS wget 2 MiB, CRC 0E76E5FD verified from actual disk files.
Artifacts: C:\nexustest\downloads\smoke and \quoted. First smoke's quoted wget
timed out because the old kernel argument parser split on spaces; fixed and
quoted HTTPS run passed. Current test campaign: tests/test_downloads.py at
C:\nexustest\downloads\campaign (32 MiB CLI, chunked, error/overflow, GUI save).

Still to do: finish campaign; explicit cancellation/disk-full/reboot cases;
test official Chrome installer download if URL is reachable; migrate main disk;
rerun build/userland/boot/network checks appropriate to FAT/parser changes;
update HANDOFF_CODEX.md/README and usage docs; clean owned QEMU/server processes;
mark goal complete only after all of that. No commit.

Latest evidence: CLI saved 32 MiB, chunked 3 MiB+17, redirect and zero-byte files;
browser saved 8 MiB installer + full 1,200,070-byte HTML via Ctrl+S. Disk bytes
verified. Official Chrome offline installer (167,683,000 bytes) saved by the guest
with trusted HTTPS, disk SHA256 exactly matches the separately fetched Google
reference: e5d6e542768cd69637877a22b1a8772dacf3de83ecee7744425b9cdfac416d91.
Reference Authenticode signature is Valid, signer Google LLC. Artifacts in
C:\nexustest\downloads\chrome and chrome_official.*. Never executed the EXE.

The initial campaign's protected-byte assertion failed solely because NPFS
increments its mount counter and CRC (superblock offsets 52..59) each boot.
verify_protected now checks that exact increment + valid CRC and equality of all
other bytes. Rechecked existing artifacts across two boots: pass. Host migration
still requires byte-for-byte protected-region equality (no boot involved).

CLI disk-full, cancellation, early disconnect and destination-preservation tests
passed, including exact free-cluster count (no leak). Browser Escape initially
failed because desktop consumes Esc; browser_claims_key now claims Esc, and a
Cancel link/status page was added. Rebuild + test_download_failures.py currently
running at C:\nexustest\downloads\failures_fixed. Main image remains 16 MiB.

Owned fixture TLS server: handoff/ws_tls/tlsserver.py with work directory
C:\nexustest\downloads\servers, launcher PID 25916 (actual Python child also
exists). Match command line + workdir when stopping it, not all Python processes.


Final migration performed: main disk.img is now 1 GiB. 26 original files have
identical SHA-256 before/after, protected bytes were identical; manifest at
C:\nexustest\downloads\main_migration.json. Host storage/FAT tests passed again.
Final kernel build: 1,059,756 bytes, zero warnings. Main 1 GiB-layout userland
workflow passed all saved-file checks, TLS campaign 33/33 including crypto 44/44,
broad command campaign 66 commands no faults, boot loop 10/10 clean.
Remaining only duplicate-filename check and owned test-server cleanup, then goal
completion. Goal has no token budget. No staging/commit performed.

Final: duplicate names passed exact-byte checks; all owned QEMU and TLS fixture processes stopped. Implementation, documentation and verification complete. Main disk 1 GiB; native FAT32 per-file/ATA bounds documented.

#!/bin/dash
# FUSE on NexusOS with Debian's daemons (tests/test_x64_fstools.py, case fuse): sshfs (over a pipe to sftp-server),
# fuse2fs (ext4 image, checked by e2fsck), ntfs-3g (fuse on a file, fuseblk on a loop device, DESTROY at umount),
# a user mount through the set-user-ID fusermount3, and a daemon killed while mounted (ENOTCONN, umount still works).
pass() { echo "[64] FSTEST PASS $*"; }
fail() { echo "[64] FSTEST FAIL $*"; exit 1; }
run() { "$@" || fail "$* (status $?)"; }
# a FUSE daemon finishes its volume after umount returns (on a file there is no FUSE_DESTROY): wait for it to exit
wait_daemon() { i=0; while grep -qsx "$1" /proc/[0-9]*/comm && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done; }
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /mnt/ssh /mnt/e2 /mnt/ntfs /srv/share /tmp
[ -c /dev/fuse ] || fail "/dev/fuse"
grep -qw fuse /proc/filesystems || fail "/proc/filesystems has no fuse"

# ---- sshfs: SFTP over a pair of pipes to the real sftp-server ----
echo "remote file" > /srv/share/remote.txt
mkdir -p /srv/share/dir; dd if=/dev/urandom of=/srv/share/dir/blob bs=1k count=700 status=none
/bin/fstools-root connect /usr/lib/openssh/sftp-server -- /usr/bin/sshfs -f -o passive,cache=no localhost:/srv/share /mnt/ssh &
i=0; while ! grep -q " /mnt/ssh " /proc/mounts && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
echo "[64] FSTEST sshfs mounted after $i polls"
grep -q " /mnt/ssh fuse.sshfs " /proc/mounts || fail "/proc/mounts: $(grep /mnt/ssh /proc/mounts)"
[ "$(cat /mnt/ssh/remote.txt)" = "remote file" ] || fail "sshfs read"
cmp /srv/share/dir/blob /mnt/ssh/dir/blob || fail "sshfs 700 KiB read"
echo "written over sftp" > /mnt/ssh/new.txt || fail "sshfs create"
run mkdir /mnt/ssh/newdir
run mv /mnt/ssh/new.txt /mnt/ssh/newdir/moved.txt
run ln -s moved.txt /mnt/ssh/newdir/link
run sync
[ "$(cat /srv/share/newdir/moved.txt)" = "written over sftp" ] || fail "sshfs write reached the server"
[ "$(readlink /srv/share/newdir/link)" = moved.txt ] || fail "sshfs symlink"
[ "$(ls /mnt/ssh | wc -l)" = 3 ] || fail "sshfs listing: $(ls /mnt/ssh)"
run rm /mnt/ssh/newdir/link
df /mnt/ssh >/dev/null || fail "statfs over sftp"
run umount /mnt/ssh
pass "sshfs: read, 700 KiB file, create, mkdir, rename, symlink, unlink, statfs, umount (sftp-server over pipes)"

# ---- fuse2fs: ext4 image, POSIX semantics, e2fsck ----
run truncate -s 64M /tmp/e.img
run mkfs.ext4 -q -F -L NEXUSE2 /tmp/e.img
run fuse2fs /tmp/e.img /mnt/e2
grep -q " /mnt/e2 fuse" /proc/mounts || fail "fuse2fs mount"
run dd if=/dev/urandom of=/tmp/e2.src bs=1M count=5 status=none
run cp /tmp/e2.src /mnt/e2/big
run mkdir -p /mnt/e2/a/b/c
echo hi > /mnt/e2/a/b/c/file || fail "deep create"
run ln /mnt/e2/a/b/c/file /mnt/e2/hardlink
run ln -s a/b/c/file /mnt/e2/symlink
run chmod 0640 /mnt/e2/a/b/c/file
run chown 1234:5678 /mnt/e2/big
[ "$(stat -c %h /mnt/e2/hardlink)" = 2 ] || fail "hard link count $(stat -c %h /mnt/e2/hardlink)"
[ "$(stat -c %a /mnt/e2/a/b/c/file)" = 640 ] || fail "chmod $(stat -c %a /mnt/e2/a/b/c/file)"
[ "$(stat -c %u:%g /mnt/e2/big)" = 1234:5678 ] || fail "chown $(stat -c %u:%g /mnt/e2/big)"
[ "$(cat /mnt/e2/symlink)" = hi ] || fail "symlink"
run truncate -s 1000 /mnt/e2/hardlink
run umount /mnt/e2
wait_daemon fuse2fs
e2fsck -fn /tmp/e.img >/dev/null 2>&1 || fail "e2fsck after fuse2fs writes: $(e2fsck -fn /tmp/e.img 2>&1 | tail -5)"
run fuse2fs -o ro /tmp/e.img /mnt/e2
cmp /tmp/e2.src /mnt/e2/big || fail "fuse2fs content after remount"
[ "$(stat -c %s /mnt/e2/a/b/c/file)" = 1000 ] || fail "truncate through the hard link"
run umount /mnt/e2
pass "fuse2fs: 5 MiB file, deep dirs, hard link, symlink, chmod, chown, truncate, e2fsck clean, remount verified"

# ---- ntfs-3g: on a file (fuse) and on a loop device (fuseblk) ----
run truncate -s 48M /tmp/n.img
run mkntfs -q -F -Q -L NEXUSNTFS /tmp/n.img
[ "$(blkid -o value -s TYPE /tmp/n.img)" = ntfs ] || fail "blkid ntfs"
run ntfs-3g /tmp/n.img /mnt/ntfs
echo "ntfs text" > "/mnt/ntfs/Windows File.txt" || fail "ntfs create"
run mkdir "/mnt/ntfs/Program Files"
run cp /tmp/e2.src "/mnt/ntfs/Program Files/data.bin"
run umount /mnt/ntfs
wait_daemon ntfs-3g
ntfsfix -n /tmp/n.img >/dev/null 2>&1 || fail "NTFS left dirty by the first umount: $(cat /proc/[0-9]*/comm | tr '\n' ' ') $(ntfsfix -n /tmp/n.img 2>&1 | tail -2)"
L=$(losetup -f --show /tmp/n.img) || fail "losetup ntfs"
cmp /tmp/n.img $L || fail "the loop device shows other bytes than its file: $(cmp -l /tmp/n.img $L | head -3)"
/bin/fstools-root readcmp $L /tmp/n.img 400 || fail "random reads of the loop device differ from its file"
ntfsfix -n $L >/dev/null 2>&1 || fail "ntfsfix on the loop device: $(ntfsfix -n $L 2>&1 | tail -2)"
run ntfs-3g $L /mnt/ntfs
grep -q " /mnt/ntfs fuseblk " /proc/mounts || fail "fuseblk: $(grep /mnt/ntfs /proc/mounts)"
cmp /tmp/e2.src "/mnt/ntfs/Program Files/data.bin" || fail "ntfs content"
[ "$(cat "/mnt/ntfs/Windows File.txt")" = "ntfs text" ] || fail "ntfs text"
run rm "/mnt/ntfs/Windows File.txt"
run umount /mnt/ntfs
run losetup -d $L
ntfsfix -n /tmp/n.img >/dev/null 2>&1 || fail "ntfsfix -n after ntfs-3g writes"
rm -f /tmp/n.img
pass "ntfs-3g: mkntfs, fuse on a file, fuseblk on a loop device, write/read/remove, ntfsfix clean"

# ---- a user mount through fusermount3 (set-user-ID) ----
mkdir -p /home/nexus/fusemnt; chown 1000:1000 /home/nexus/fusemnt
run truncate -s 32M /home/nexus/user.img
run mkfs.ext4 -q -F -E root_owner=1000:1000 /home/nexus/user.img   # the user's own volume (fuse2fs checks modes itself)
chown 1000:1000 /home/nexus/user.img
[ "$(stat -c %a /usr/bin/fusermount3)" = 4755 ] || fail "fusermount3 is not set-user-ID"
/bin/fstools-root as 1000 1000 fuse2fs /home/nexus/user.img /home/nexus/fusemnt || fail "user fuse2fs via fusermount3"
grep -Eq " /home/nexus/fusemnt fuse\.(fuse2fs|ext4) .*user_id=1000" /proc/mounts || fail "user mount: $(grep fusemnt /proc/mounts)"   # fuse2fs names the subtype after the fs
/bin/fstools-root as 1000 1000 sh -c 'echo mine > /home/nexus/fusemnt/user.txt' || fail "user write"
ls /home/nexus/fusemnt >/dev/null 2>&1 && fail "root entered a FUSE mount without allow_other"
/bin/fstools-root as 1000 1000 fusermount3 -u /home/nexus/fusemnt || fail "fusermount3 -u"
grep -q fusemnt /proc/mounts && fail "still mounted after fusermount3 -u"
rm -f /home/nexus/user.img
pass "user mount: fusermount3 (set-user-ID, fd over SCM_RIGHTS), user_id policy (root kept out), fusermount3 -u"

# ---- the daemon dies while mounted ----
fuse2fs -f /tmp/e.img /mnt/e2 &
D=$!
i=0; while ! grep -q " /mnt/e2 " /proc/mounts && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
ls /mnt/e2 >/dev/null || fail "fuse2fs -f mount"
kill -9 $D; wait $D 2>/dev/null
ls /mnt/e2 2>/tmp/err.txt && fail "access after the daemon died"
grep -q "not connected" /tmp/err.txt || fail "error after daemon death: $(cat /tmp/err.txt)"
run umount /mnt/e2
pass "daemon killed: ENOTCONN (Transport endpoint is not connected), umount works"
rm -f /tmp/e.img /tmp/e2.src
echo "[64] FSTEST ALL PASSED"

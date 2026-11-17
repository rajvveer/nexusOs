#!/bin/dash
# Loop devices, exFAT, vfat and ISO 9660 on NexusOS, driven by Debian's own tools (tests/test_x64_fstools.py).
# Runs as root; every check prints "[64] FSTEST PASS ..." or stops with "[64] FSTEST FAIL ...".
pass() { echo "[64] FSTEST PASS $*"; }
fail() { echo "[64] FSTEST FAIL $*"; exit 1; }
run() { "$@" || fail "$* (status $?)"; }
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mkdir -p /mnt/ex /mnt/fat /mnt/iso /mnt/ex2 /tmp

# ---- loop devices (util-linux losetup/blockdev) ----
run truncate -s 64M /tmp/ex.img
L=$(losetup -f --show /tmp/ex.img) || fail "losetup -f --show"
case "$L" in /dev/loop[0-9]*) ;; *) fail "losetup printed '$L'" ;; esac
N=${L#/dev/}
losetup -l | grep -q "/tmp/ex.img" || fail "losetup -l does not list the backing file"
[ "$(cat /sys/block/$N/loop/backing_file)" = /tmp/ex.img ] || fail "sysfs backing_file: $(cat /sys/block/$N/loop/backing_file)"
[ "$(blockdev --getsize64 $L)" = 67108864 ] || fail "BLKGETSIZE64 $(blockdev --getsize64 $L)"
[ "$(blockdev --getss $L)" = 512 ] || fail "BLKSSZGET"
grep -q " $N\$" /proc/partitions || fail "/proc/partitions has no $N"
pass "loop: losetup -f --show, -l, sysfs backing_file, BLKGETSIZE64, BLKSSZGET, /proc/partitions"

# ---- exFAT made by mkfs.exfat, written by NexusOS, checked by fsck.exfat ----
run mkfs.exfat -L NEXUSEX $L
run fsck.exfat -n $L
[ "$(blkid -o value -s TYPE $L)" = exfat ] || fail "blkid TYPE $(blkid $L)"
[ "$(blkid -o value -s LABEL $L)" = NEXUSEX ] || fail "blkid LABEL $(blkid $L)"
run mount -t exfat $L /mnt/ex
grep -q " /mnt/ex exfat rw" /proc/mounts || fail "/proc/mounts: $(grep /mnt/ex /proc/mounts)"
run dd if=/dev/urandom of=/tmp/big.src bs=1M count=24 status=none
run cp /tmp/big.src /mnt/ex/big.bin
(cd /mnt/ex && sha256sum big.bin > /tmp/big.sum) || fail "sha256sum"
run mkdir -p "/mnt/ex/A Folder/sub dir"
echo hello > "/mnt/ex/A Folder/Hello World.TXT" || fail "create"
i=1; while [ $i -le 300 ]; do echo "file $i" > "/mnt/ex/A Folder/sub dir/f$i.txt" || fail "file $i"; i=$((i + 1)); done
run mv "/mnt/ex/A Folder/Hello World.TXT" /mnt/ex/renamed.txt
mv /mnt/ex/renamed.txt /mnt/ex/RENAMED.txt 2>/dev/null && fail "mv of a case-only rename (Linux: same file)"
run /bin/fstools-root rename /mnt/ex/renamed.txt /mnt/ex/RENAMED.txt
ls /mnt/ex | grep -qx RENAMED.txt || fail "case-only rename: $(ls /mnt/ex)"
echo "ünïcödé" > "/mnt/ex/日本語 ファイル.txt" || fail "unicode name"
run rm "/mnt/ex/A Folder/sub dir/f7.txt"
rmdir "/mnt/ex/A Folder/sub dir" 2>/dev/null && fail "rmdir of a full directory worked"
run truncate -s 5000 /mnt/ex/gap.bin
printf 'X' | dd of=/mnt/ex/gap.bin bs=1 seek=100000 conv=notrunc status=none || fail "write past ValidDataLength"
[ "$(stat -c %s /mnt/ex/gap.bin)" = 100001 ] || fail "gap size"
[ "$(dd if=/mnt/ex/gap.bin bs=1 skip=4000 count=96000 status=none | tr -d '\000' | wc -c)" = 0 ] || fail "gap not zero"
touch -d "2030-01-02 03:04:06" /mnt/ex/RENAMED.txt || fail "utimes"
chmod a-w /mnt/ex/RENAMED.txt || fail "chmod a-w"
[ "$(stat -c %A /mnt/ex/RENAMED.txt)" = "-r-xr-xr-x" ] || fail "read-only attribute: $(stat -c %A /mnt/ex/RENAMED.txt)"
chmod 4755 /mnt/ex/RENAMED.txt 2>/dev/null && fail "chmod 4755 worked"
df -k /mnt/ex | grep -q /mnt/ex || fail "df"
run umount /mnt/ex
fsck.exfat -n $L || fail "fsck.exfat after NexusOS writes"
run mount -t exfat -o ro $L /mnt/ex
cmp /tmp/big.src /mnt/ex/big.bin || { cmp -l /tmp/big.src /mnt/ex/big.bin | head -5; cmp -l /tmp/big.src /mnt/ex/big.bin | wc -l; umount /mnt/ex; losetup -d $L; fail "big file content after remount"; }
[ "$(ls "/mnt/ex/A Folder/sub dir" | wc -l)" = 299 ] || fail "directory listing: $(ls "/mnt/ex/A Folder/sub dir" | wc -l)"
[ "$(cat "/mnt/ex/A Folder/sub dir/f300.txt")" = "file 300" ] || fail "file 300"
[ "$(cat "/mnt/ex/日本語 ファイル.txt")" = "ünïcödé" ] || fail "unicode file"
[ "$(stat -c %y /mnt/ex/RENAMED.txt | cut -c1-19)" = "2030-01-02 03:04:06" ] || fail "mtime $(stat -c %y /mnt/ex/RENAMED.txt)"
[ -e /mnt/ex/renamed.TXT ] || fail "case-insensitive lookup"
run umount /mnt/ex
run losetup -d $L
pass "exFAT: mkfs.exfat, mount, 24 MiB file, 300-entry directory, rename (also case-only), unicode, unlink, ValidDataLength gap, times, read-only attribute, fsck.exfat clean, remount verified"

# ---- mount -o loop: autoclear ----
run mount -o loop /tmp/ex.img /mnt/ex2
losetup -j /tmp/ex.img | grep -q /tmp/ex.img || fail "mount -o loop: no loop device"
run umount /mnt/ex2
[ -z "$(losetup -j /tmp/ex.img)" ] || fail "loop not detached after umount (autoclear)"
pass "mount -o loop and autoclear at umount"

# ---- partitioned image: sfdisk, losetup -P, vfat (dosfstools) ----
run truncate -s 128M /tmp/disk.img
printf 'label: gpt\nsize=64M, type=U, name=esp\nsize=48M, type=L, name=data\ntype=L\n' | sfdisk -q /tmp/disk.img || fail "sfdisk"
L=$(losetup -fP --show /tmp/disk.img) || fail "losetup -P"
[ -b ${L}p1 ] && [ -b ${L}p2 ] && [ -b ${L}p3 ] || fail "partitions of $L: $(ls /dev | grep ${L#/dev/})"
run mkfs.vfat -F 32 -n NEXUSFAT ${L}p1
run mkfs.exfat -L DATA ${L}p2
[ "$(readlink -f /dev/disk/by-label/NEXUSFAT)" = ${L}p1 ] || fail "by-label: $(ls -l /dev/disk/by-label)"
lsblk -rno NAME,FSTYPE,LABEL $L | grep -q "${L#/dev/}p1 vfat NEXUSFAT" || fail "lsblk: $(lsblk -rno NAME,FSTYPE,LABEL $L)"
run mount ${L}p1 /mnt/fat
run cp /tmp/big.sum /mnt/fat/BIG.SUM
run mkdir /mnt/fat/dir
i=1; while [ $i -le 50 ]; do echo $i > "/mnt/fat/dir/a long file name $i.txt" || fail "vfat $i"; i=$((i + 1)); done
run mount ${L}p2 /mnt/ex
/bin/fstools-root open /mnt/ex/ 290000; /bin/fstools-root open /mnt/ex 290000; /bin/fstools-root open /mnt/ex/ 10000
run cp /mnt/fat/BIG.SUM /mnt/ex/
run umount /mnt/fat
run umount /mnt/ex
fsck.vfat -n ${L}p1 || fail "fsck.vfat after NexusOS writes"
fsck.exfat -n ${L}p2 || fail "fsck.exfat on a partition"
run losetup -d $L
rm -f /tmp/disk.img
pass "partitioned loop (sfdisk GPT, losetup -P), mkfs.vfat + fsck.vfat, mkfs.exfat on a partition, lsblk, /dev/disk/by-label"

# ---- GNU parted on a loop device: the kernel learns every change through BLKPG ----
run truncate -s 64M /tmp/pt.img
L=$(losetup -fP --show /tmp/pt.img) || fail "losetup -P (parted)"
run parted -s $L mklabel gpt mkpart first ext4 1MiB 17MiB mkpart second fat32 17MiB 33MiB
[ -b ${L}p1 ] && [ -b ${L}p2 ] || fail "parted partitions: $(ls /dev | grep ${L#/dev/})"
[ "$(blockdev --getsz ${L}p2)" = 32768 ] || fail "parted p2 size $(blockdev --getsz ${L}p2)"
run parted -s $L resizepart 2 49MiB
[ "$(blockdev --getsz ${L}p2)" = 65536 ] || fail "resizepart: $(blockdev --getsz ${L}p2)"
run parted -s $L rm 1
[ -e ${L}p1 ] && fail "${L}p1 still there after parted rm"
parted -s -m $L unit s print | grep -q "^2:34816s:100351s:65536s:" || fail "parted print: $(parted -s -m $L unit s print)"
sfdisk --verify $L >/dev/null || fail "sfdisk --verify after parted: $(sfdisk --verify $L 2>&1)"
run losetup -d $L
rm -f /tmp/pt.img
pass "parted: mklabel gpt, mkpart (BLKPG add), resizepart (BLKPG resize), rm (BLKPG delete), print; sfdisk --verify"

# ---- ISO 9660: xorriso -R -J, Rock Ridge / Joliet / plain names ----
rm -rf /tmp/isoroot; mkdir -p /tmp/isoroot/docs/deep/er/still
echo "iso content" > /tmp/isoroot/docs/ReadMe.txt; chmod 0640 /tmp/isoroot/docs/ReadMe.txt
ln -s docs/ReadMe.txt /tmp/isoroot/link-to-readme
echo deep > /tmp/isoroot/docs/deep/er/still/Deep_File.txt
dd if=/dev/urandom of=/tmp/isoroot/blob.bin bs=1k count=3000 status=none
(cd /tmp/isoroot && sha256sum blob.bin > /tmp/blob.sum)
run xorriso -as mkisofs -quiet -R -J -V NEXUSISO -o /tmp/test.iso /tmp/isoroot
[ "$(blkid -o value -s TYPE /tmp/test.iso)" = iso9660 ] || fail "blkid iso"
run mount -o loop /tmp/test.iso /mnt/iso
grep " /mnt/iso iso9660 ro" /proc/mounts >/dev/null || fail "iso mounted read-only: $(grep /mnt/iso /proc/mounts)"
[ "$(cat /mnt/iso/docs/ReadMe.txt)" = "iso content" ] || fail "rock ridge read"
[ "$(stat -c %a /mnt/iso/docs/ReadMe.txt)" = 640 ] || fail "rock ridge mode $(stat -c %a /mnt/iso/docs/ReadMe.txt)"
[ "$(readlink /mnt/iso/link-to-readme)" = docs/ReadMe.txt ] || fail "rock ridge symlink"
[ "$(cat /mnt/iso/link-to-readme)" = "iso content" ] || fail "following the symlink"
[ "$(cat /mnt/iso/docs/deep/er/still/Deep_File.txt)" = deep ] || fail "deep file"
(cd /mnt/iso && sha256sum -c /tmp/blob.sum >/dev/null) || fail "blob content"
touch /mnt/iso/new 2>/dev/null && fail "write to an ISO worked"
run umount /mnt/iso
run mount -o loop,norock /tmp/test.iso /mnt/iso
[ "$(cat /mnt/iso/docs/ReadMe.txt)" = "iso content" ] || fail "joliet names: $(ls /mnt/iso/docs)"
run umount /mnt/iso
run mount -o loop,norock,nojoliet /tmp/test.iso /mnt/iso
[ -e /mnt/iso/docs/readme.txt ] || fail "plain ISO names: $(ls /mnt/iso/docs)"
run umount /mnt/iso
[ -z "$(losetup -a)" ] || fail "loop devices left: $(losetup -a)"
rm -rf /tmp/isoroot /tmp/big.src /tmp/blob.sum
pass "ISO 9660 (xorriso -R -J): Rock Ridge names/modes/symlinks/deep dirs, Joliet, plain names, read-only, autoclear"
echo "[64] FSTEST ALL PASSED"

#!/bin/dash
# Device mapper and dm-crypt with Debian's dmsetup, keyctl and cryptsetup (tests/test_x64_fstools.py, case crypt):
# linear/zero/error tables, suspend/reload/resume, holders, EBUSY, IEEE 1619 known answers through dm-crypt (hex and
# keyring keys, IV offset, 4096-byte sectors), logon keys, LUKS2 (argon2id) format/open/status/addkey/wrong passphrase,
# a filesystem on the mapping with no plaintext on the disk, LUKS1, plain mode, non-root refused, CBC-ESSIV and
# CBC-plain64 known answers and a LUKS1 aes-cbc-essiv:sha256 volume, throughput over a zero target ("[64] FSTEST INFO").
pass() { echo "[64] FSTEST PASS $*"; }
fail() { echo "[64] FSTEST FAIL $*"; exit 1; }
run() { "$@" || fail "$* (status $?)"; }
hex() { od -An -tx1 -v -N "$2" "$1" | tr -d ' \n'; }   # FILE BYTES: the first BYTES bytes in hex
export PATH=/usr/sbin:/usr/bin:/sbin:/bin DM_DISABLE_UDEV=1
mkdir -p /mnt/c /run/cryptsetup
[ -c /dev/mapper/control ] || fail "/dev/mapper/control"
grep -q "device-mapper" /proc/misc || fail "/proc/misc"
dmsetup version | grep -q "Driver version: *4\." || fail "dmsetup version: $(dmsetup version 2>&1)"
T=$(dmsetup targets) || fail "dmsetup targets"
for t in linear crypt zero error; do echo "$T" | grep -q "^$t " || fail "target $t missing: $T"; done
run truncate -s 96M /tmp/d.img
L=$(losetup -f --show /tmp/d.img) || fail "losetup"
LN=$(basename $L)

# ---- linear, zero, error; suspend, reload, resume; holders; busy ----
run dmsetup create lin --table "0 32768 linear $L 2048"
[ -b /dev/mapper/lin ] || fail "/dev/mapper/lin"
[ "$(blockdev --getsz /dev/mapper/lin)" = 32768 ] || fail "size $(blockdev --getsz /dev/mapper/lin)"
D=dm-$(dmsetup info -c --noheadings -o minor lin | tr -d ' ')
[ "$(stat -c %t /dev/mapper/lin)" = fd ] || fail "/dev/mapper/lin is not a 253:N block device: $(stat -c '%F %t:%T' /dev/mapper/lin)"
[ "$(cat /sys/block/$D/dm/name)" = lin ] || fail "/sys/block/$D/dm/name: $(cat /sys/block/$D/dm/name)"
[ -e /sys/block/$LN/holders/$D ] || fail "holders of $LN: $(ls /sys/block/$LN/holders)"
[ -e /sys/block/$D/slaves/$LN ] || fail "slaves of $D: $(ls /sys/block/$D/slaves)"
printf 'NEXUSLINEAR' | dd of=/dev/mapper/lin bs=512 seek=10 conv=notrunc status=none || fail "write through linear"
[ "$(dd if=$L bs=512 skip=2058 count=1 status=none | head -c 11)" = NEXUSLINEAR ] || fail "linear offset"
dmsetup table lin | grep -q "^0 32768 linear 7:[0-9]* 2048$" || fail "table: $(dmsetup table lin)"
dmsetup info lin | grep -q "State: *ACTIVE" || fail "info: $(dmsetup info lin)"
lsblk -rno NAME,TYPE | grep -q "^lin dm$" || fail "lsblk: $(lsblk -rno NAME,TYPE)"
run dmsetup suspend lin
dmsetup info lin | grep -q "State: *SUSPENDED" || fail "suspended state"
printf '0 1024 linear %s 4096\n1024 1024 zero\n2048 8 error\n' $L | dmsetup reload lin || fail "reload"
dmsetup info lin | grep -q "Tables present: *LIVE & INACTIVE" || fail "inactive table: $(dmsetup info lin)"
run dmsetup resume lin
[ "$(blockdev --getsz /dev/mapper/lin)" = 2056 ] || fail "size after reload $(blockdev --getsz /dev/mapper/lin)"
[ "$(dd if=/dev/mapper/lin bs=512 skip=1024 count=1 status=none | tr -d '\0' | wc -c)" = 0 ] || fail "zero target"
dd if=/dev/mapper/lin of=/dev/null bs=512 skip=2048 count=1 status=none 2>/dev/null && fail "the error target read"
[ "$(dmsetup table lin | wc -l)" = 3 ] || fail "3-target table: $(dmsetup table lin)"
exec 3</dev/mapper/lin
dmsetup remove lin 2>/dev/null && fail "removed while open"
exec 3<&-
dmsetup create bad --table "0 999999999 linear $L 0" 2>/dev/null && fail "a target beyond its device"
dmsetup create bad2 --table "0 100 nosuchtarget" 2>/dev/null && fail "an unknown target"
/bin/fstools-root as 1000 1000 dmsetup ls 2>/dev/null && fail "non-root opened /dev/mapper/control"
dmsetup rename lin lin2 2>/tmp/rename.err || fail "rename: $(cat /tmp/rename.err)"
grep -v "System V IPC" /tmp/rename.err > /tmp/rename.out   # libdm notes that there are no SysV semaphores (no udev sync)
[ -s /tmp/rename.out ] && fail "rename complained: $(cat /tmp/rename.out) / old node: $(stat -c %F /dev/mapper/lin 2>&1)"
[ -b /dev/mapper/lin2 ] && [ ! -e /dev/mapper/lin ] || fail "rename: $(ls /dev/mapper)"
run dmsetup remove lin2
[ -e /dev/mapper/lin2 ] && fail "node left after remove"
[ "$(ls /sys/block/$LN/holders | wc -l)" = 0 ] || fail "holder left: $(ls /sys/block/$LN/holders)"
pass "dm linear/zero/error: create, sysfs name/holders/slaves, offsets, suspend/reload/resume, EBUSY, bad tables, non-root refused, rename, remove"

# ---- dm-crypt known answers (IEEE 1619 vectors 1 and 2, XTS-AES-128) ----
Z=0000000000000000000000000000000000000000000000000000000000000000
run dmsetup create v1 --table "0 2048 crypt aes-xts-plain64 $Z 0 $L 0"
run dd if=/dev/zero of=/dev/mapper/v1 bs=512 count=1 conv=notrunc,fsync status=none
[ "$(hex $L 32)" = 917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e ] || fail "IEEE 1619 vector 1: $(hex $L 32)"
[ "$(dd if=/dev/mapper/v1 bs=512 count=1 status=none | tr -d '\0' | wc -c)" = 0 ] || fail "vector 1 decrypt"
run dmsetup remove v1
K2=1111111111111111111111111111111122222222222222222222222222222222
head -c 512 /dev/zero | tr '\0' 'D' > /tmp/p44
run dmsetup create v2 --table "0 2048 crypt aes-xts-plain64 $K2 219902325555 $L 0"
run dd if=/tmp/p44 of=/dev/mapper/v2 bs=512 count=1 conv=notrunc,fsync status=none
[ "$(hex $L 32)" = c454185e6a16936e39334038acef838bfb186fff7480adc4289382ecd6d394f0 ] || fail "IEEE 1619 vector 2 (IV offset): $(hex $L 32)"
dmsetup table --showkeys v2 | grep -q " $K2 219902325555 7:" || fail "crypt table: $(dmsetup table --showkeys v2)"
run dmsetup remove v2
# the same key from the keyring (logon key), and 4096-byte sectors with large IVs
/usr/bin/printf "$(echo $K2 | sed 's/../\\x&/g')" > /tmp/k2.bin
[ "$(stat -c %s /tmp/k2.bin)" = 32 ] || fail "key file"
KID=$(keyctl padd logon nexus:k2 @u < /tmp/k2.bin) || fail "keyctl padd logon"
keyctl rdescribe $KID | grep -q "^logon;0;0;[0-9a-f]*;nexus:k2$" || fail "describe: $(keyctl rdescribe $KID)"
keyctl read $KID >/dev/null 2>&1 && fail "a logon key was readable"
run dmsetup create v3 --table "0 2048 crypt aes-xts-plain64 :32:logon:nexus:k2 219902325555 $L 0"
cmp -n 512 /dev/mapper/v3 /tmp/p44 || fail "keyring key decrypts vector 2"
dmsetup table --showkeys v3 | grep -q ":32:logon:nexus:k2 " || fail "keyring table: $(dmsetup table --showkeys v3)"
run dmsetup remove v3
run keyctl unlink $KID @u
dmsetup create v4 --table "0 2048 crypt aes-xts-plain64 :32:logon:nexus:k2 0 $L 0" 2>/dev/null && fail "an unlinked key still worked"
dmsetup remove v4 2>/dev/null
UK=$(printf userpayload | keyctl padd user nexus:u @s) || fail "user key"
[ "$(keyctl pipe $UK)" = userpayload ] || fail "user key read"
run keyctl revoke $UK
run dmsetup create s4k --table "0 4096 crypt aes-xts-plain64 $K2 0 $L 8192 2 sector_size:4096 iv_large_sectors"
[ "$(blockdev --getss /dev/mapper/s4k)" = 4096 ] || fail "4096-byte crypt sectors: $(blockdev --getss /dev/mapper/s4k)"
run dd if=/dev/urandom of=/tmp/r.bin bs=1M count=1 status=none
run dd if=/tmp/r.bin of=/dev/mapper/s4k bs=64k conv=fsync status=none
cmp -n 1048576 /tmp/r.bin /dev/mapper/s4k || fail "4096-byte sector round trip"
run dmsetup remove s4k
dmsetup create s4bad --table "0 4095 crypt aes-xts-plain64 $K2 0 $L 0 1 sector_size:4096" 2>/dev/null && fail "a crypt length not a multiple of the sector size"
pass "dm-crypt: IEEE 1619 vectors 1+2 (hex key, IV offset), logon keyring key (unreadable, unlink revokes access), user key, 4096-byte sectors"

# ---- cryptsetup LUKS2 (argon2id) ----
printf 'correct horse battery' > /tmp/pass1; printf 'second key' > /tmp/pass2; printf 'wrong' > /tmp/bad
P="--pbkdf argon2id --pbkdf-memory 32768 --pbkdf-parallel 1 --pbkdf-force-iterations 4"
run cryptsetup luksFormat -q --type luks2 $P --key-file /tmp/pass1 $L
cryptsetup isLuks $L || fail "isLuks"
cryptsetup luksDump $L | grep -q "PBKDF: *argon2id" || fail "luksDump: $(cryptsetup luksDump $L | head -30)"
[ "$(blkid -o value -s TYPE $L)" = crypto_LUKS ] || fail "blkid crypto_LUKS"
cryptsetup open --key-file /tmp/bad $L secret 2>/dev/null; [ $? = 2 ] || fail "wrong passphrase did not fail with 2"
run cryptsetup open --allow-discards --key-file /tmp/pass1 $L secret
S=$(cryptsetup status secret) || fail "status"
echo "$S" | grep -q "cipher: *aes-xts-plain64" && echo "$S" | grep -q "keysize: *512 bits" && echo "$S" | grep -q "discards" || fail "status: $S"
dmsetup table secret | grep -q " :64:logon:cryptsetup:" || fail "volume key not in the keyring: $(dmsetup table secret)"
run mkfs.vfat -n SECRET /dev/mapper/secret
run mount /dev/mapper/secret /mnt/c
yes NEXUS-PLAINTEXT-MARKER | head -c 2000000 > /mnt/c/marker.txt || fail "write"
run cp /tmp/r.bin /mnt/c/r.bin
run umount /mnt/c
run cryptsetup close secret
[ -e /dev/mapper/secret ] && fail "mapping left after close"
grep -c NEXUS-PLAINTEXT-MARKER $L >/dev/null && fail "plaintext on the encrypted disk"
run cryptsetup luksAddKey -q $P --key-file /tmp/pass1 $L /tmp/pass2
cryptsetup luksDump $L | grep -q "^  1: luks2" || fail "keyslot 1: $(cryptsetup luksDump $L | grep -A2 Keyslots)"
run cryptsetup open --key-file /tmp/pass2 $L secret
run fsck.vfat -n /dev/mapper/secret
run mount -o ro /dev/mapper/secret /mnt/c
cmp /tmp/r.bin /mnt/c/r.bin || fail "content after reopen with the second key"
cryptsetup close secret 2>/dev/null && fail "closed while mounted"
run umount /mnt/c
run cryptsetup close secret
run cryptsetup luksKillSlot -q --key-file /tmp/pass2 $L 0
cryptsetup open --test-passphrase --key-file /tmp/pass1 $L 2>/dev/null && fail "killed slot 0 still opened"
run cryptsetup open --test-passphrase --key-file /tmp/pass2 $L
pass "cryptsetup LUKS2: luksFormat argon2id, isLuks, blkid, wrong passphrase, open, status, keyring volume key, vfat on it, no plaintext on disk, luksAddKey, reopen, busy close, luksKillSlot"

# ---- LUKS2 with 4096-byte sectors, LUKS1, plain ----
run cryptsetup luksFormat -q --type luks2 $P --sector-size 4096 --key-file /tmp/pass1 $L
run cryptsetup open --key-file /tmp/pass1 $L s4
dmsetup table s4 | grep -q "sector_size:4096" || fail "LUKS2 4096 table: $(dmsetup table s4)"
run dd if=/tmp/r.bin of=/dev/mapper/s4 bs=1M conv=fsync status=none
cmp -n 1048576 /tmp/r.bin /dev/mapper/s4 || fail "LUKS2 4096 round trip"
run cryptsetup close s4
run cryptsetup luksFormat -q --type luks1 --pbkdf-force-iterations 1000 --key-file /tmp/pass1 $L
run cryptsetup open --key-file /tmp/pass1 $L l1
run dd if=/tmp/r.bin of=/dev/mapper/l1 bs=1M conv=fsync status=none
run cryptsetup close l1
run cryptsetup open --key-file /tmp/pass1 $L l1
cmp -n 1048576 /tmp/r.bin /dev/mapper/l1 || fail "LUKS1 round trip"
run cryptsetup close l1
run cryptsetup open --type plain -q --cipher aes-xts-plain64 --key-size 256 --key-file /tmp/k2.bin --keyfile-size 32 $L pl
dmsetup table --showkeys pl | grep -Eq "^0 [0-9]+ crypt aes-xts-plain64 [0-9a-f]{64} 0 7:[0-9]+ 0$" || fail "plain table: $(dmsetup table --showkeys pl)"
run cryptsetup close pl
pass "cryptsetup LUKS2 4096-byte sectors, LUKS1 (pbkdf2), plain mode"

# ---- CBC modes: known answers (from a pure-Python AES/SHA-256 reference), LUKS1 aes-cbc-essiv:sha256 ----
K16=000102030405060708090a0b0c0d0e0f
K32=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
run dmsetup create c1 --table "0 2048 crypt aes-cbc-essiv:sha256 $K16 0 $L 0"
run dd if=/tmp/p44 of=/dev/mapper/c1 bs=512 count=1 conv=notrunc,fsync status=none
[ "$(hex $L 32)" = 976cf775c76a2b989ee3aca34823582c29d2f5b329fe370237b0e2ad4c111917 ] || fail "aes-cbc-essiv:sha256 (128-bit key) known answer: $(hex $L 32)"
cmp -n 512 /dev/mapper/c1 /tmp/p44 || fail "cbc-essiv decrypt"
run dmsetup remove c1
run dmsetup create c2 --table "0 2048 crypt aes-cbc-essiv:sha256 $K32 0 $L 0"
run dd if=/tmp/p44 of=/dev/mapper/c2 bs=512 seek=5 count=1 conv=notrunc,fsync status=none
[ "$(dd if=$L bs=512 skip=5 count=1 status=none | od -An -tx1 -v -N 32 | tr -d ' \n')" = bb3a538aa63b9daad6a3cba70e27d6ab533ad92ef878fcdc52a5d7ac71638c77 ] || fail "aes-cbc-essiv:sha256 (256-bit key, sector 5) known answer"
run dmsetup remove c2
run dmsetup create c3 --table "0 2048 crypt aes-cbc-plain64 $K16 0 $L 0"
run dd if=/tmp/p44 of=/dev/mapper/c3 bs=512 count=1 conv=notrunc,fsync status=none
[ "$(hex $L 32)" = a72c90417ea282d473c3f61c745b32a1968453187c49bd9c9fcdd8392a00fd47 ] || fail "aes-cbc-plain64 known answer: $(hex $L 32)"
run dmsetup remove c3
run dd if=/dev/urandom of=/tmp/r.bin bs=1M count=1 status=none
run cryptsetup luksFormat -q --type luks1 --cipher aes-cbc-essiv:sha256 --key-size 256 --pbkdf-force-iterations 1000 --key-file /tmp/pass1 $L
run cryptsetup open --key-file /tmp/pass1 $L cbc
cryptsetup status cbc | grep -q "cipher: *aes-cbc-essiv:sha256" || fail "LUKS1 cbc status: $(cryptsetup status cbc)"
run dd if=/tmp/r.bin of=/dev/mapper/cbc bs=1M conv=fsync status=none
run cryptsetup close cbc
run cryptsetup open --key-file /tmp/pass1 $L cbc
cmp -n 1048576 /tmp/r.bin /dev/mapper/cbc || fail "LUKS1 aes-cbc-essiv round trip"
run cryptsetup close cbc
pass "dm-crypt CBC: aes-cbc-essiv:sha256 (128/256-bit keys) and aes-cbc-plain64 known answers, LUKS1 aes-cbc-essiv:sha256 round trip"

# ---- suspend holds I/O; luksSuspend/luksResume of a mounted volume ----
run truncate -s 96M /tmp/s.img
L2=$(losetup -f --show /tmp/s.img) || fail "losetup s.img"
run dmsetup create held --table "0 65536 linear $L2 0"
run dmsetup suspend held
( dd if=/dev/mapper/held of=/dev/null bs=4k count=1 iflag=direct status=none; echo done > /tmp/held.done ) &
H=$!; sleep 2
[ -e /tmp/held.done ] && fail "a read of a suspended device completed"
kill -0 $H || fail "the reader of the suspended device is gone"
run dmsetup resume held
wait $H; [ -e /tmp/held.done ] || fail "the held read did not finish after resume"
run dmsetup remove held
run cryptsetup luksFormat -q --type luks2 $P --key-file /tmp/pass1 $L2
run cryptsetup open --key-file /tmp/pass1 $L2 sus
run mkfs.vfat -n SUSPEND /dev/mapper/sus
run mount /dev/mapper/sus /mnt/c
echo before > /mnt/c/a.txt || fail "write before"
run sync
run cryptsetup luksSuspend sus
dmsetup info sus | grep -q "State: *SUSPENDED" || fail "luksSuspend state: $(dmsetup info sus)"
( echo during > /mnt/c/b.txt; echo done > /tmp/writer.done ) &
W=$!; sleep 2
[ -e /tmp/writer.done ] && fail "a write to the frozen filesystem completed while suspended"
kill -0 $W || fail "the writer is gone"
dmsetup resume sus 2>/dev/null && fail "resumed although the key was wiped"
cryptsetup luksResume --key-file /tmp/bad sus 2>/dev/null && fail "luksResume with a wrong passphrase"
run cryptsetup luksResume --key-file /tmp/pass1 sus
wait $W; [ -e /tmp/writer.done ] || fail "the held writer did not finish after luksResume"
[ "$(cat /mnt/c/b.txt)" = during ] && [ "$(cat /mnt/c/a.txt)" = before ] || fail "contents after luksResume"
run umount /mnt/c
run fsck.vfat -n /dev/mapper/sus
run cryptsetup close sus
run losetup -d $L2
rm -f /tmp/s.img /tmp/held.done /tmp/writer.done
pass "dm suspend holds I/O until resume; luksSuspend of a mounted volume: writer held, key wiped (plain resume refused, wrong passphrase refused), luksResume"

# ---- throughput: crypt over a zero target (no disk in the way) ----
MB=512; grep -qw noaesni /proc/cmdline && MB=8   # the constant-time software AES runs at ~1.6 MB/s
run dmsetup create z0 --table "0 1048576 zero"
run dmsetup create cz --table "0 1048576 crypt aes-xts-plain64 $K2$K2 0 /dev/mapper/z0 0"
R=$(dd if=/dev/mapper/cz of=/dev/null bs=1M count=$MB 2>&1 | tail -1)
W=$(dd if=/dev/zero of=/dev/mapper/cz bs=1M count=$MB 2>&1 | tail -1)
echo "[64] FSTEST INFO dm-crypt aes-xts-plain64 (512-bit) read: $R"
echo "[64] FSTEST INFO dm-crypt aes-xts-plain64 (512-bit) write: $W"
run dmsetup remove cz
run dmsetup remove z0
run losetup -d $L
rm -f /tmp/d.img /tmp/r.bin /tmp/p44 /tmp/k2.bin /tmp/pass1 /tmp/pass2 /tmp/bad
echo "[64] FSTEST ALL PASSED"

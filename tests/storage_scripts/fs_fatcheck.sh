#!/bin/dash
# Debian's fsck.fat (dosfstools) on three copies of the same damaged FAT32 image (tests/test_x64_fatcheck.py case
# dosfstools): /dev/nvme1n1p1 as damaged, /dev/nvme2n1p1 repaired by userland/fatfsck.py -a, /dev/nvme3n1p1 repaired by
# the NexusOS kernel's boot check. Every fsck.fat output line is echoed as "[64] FATFSCK <disk>: ..." for the host.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
fail() { echo "[64] FSTEST FAIL $*"; exit 1; }
for d in 1 2 3; do [ -b /dev/nvme${d}n1p1 ] || fail "no /dev/nvme${d}n1p1"; done
check() {   # $1 tag, $2 device, rest: fsck.fat options
    tag=$1; dev=$2; shift 2
    fsck.fat "$@" $dev > /tmp/fsck.out 2>&1; s=$?
    sed "s/^/[64] FATFSCK $tag: /" /tmp/fsck.out
    echo "[64] FATFSCK $tag status=$s"
}
check damaged-n /dev/nvme1n1p1 -n -v
check damaged-a /dev/nvme1n1p1 -a -w -v
check damaged-again /dev/nvme1n1p1 -n
check host-repaired /dev/nvme2n1p1 -n -v
check kernel-repaired /dev/nvme3n1p1 -n -v
sync
echo "[64] FSTEST ALL PASSED"

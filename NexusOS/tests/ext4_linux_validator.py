"""Independent ext4 validation with the real Linux kernel and e2fsprogs (Debian 13 packages).

    python -I tests/ext4_linux_validator.py <image> <work> [--script FILE | --mode check|replay|mount] [--whpx] [--rw]

Boots Debian's cloud kernel (ext4/jbd2/crc32c built in) with a busybox + e2fsprogs initramfs in QEMU;
the image is attached as virtio disk /dev/vda (the NexusOS layout puts the volume on /dev/vda1).
Modes: check = e2fsck -fn (read-only); replay = e2fsck -fy (journal replay/orphans/repairs) then
e2fsck -fn; mount = Linux mounts the volume read-write (kernel journal recovery), lists it, unmounts
and runs e2fsck -fn. The image is attached with snapshot=on unless --rw. Packages are downloaded from
deb.debian.org on first use (nexus_paths cache 'ext4-debs', NEXUS_EXT4_DEBS_CACHE / NEXUS_DEB_CACHE) and verified against the Debian index.
Exit status: 0 when every e2fsck run in the script reported a clean filesystem (exit 0).
"""
from pathlib import Path
import argparse
import hashlib
import io
import lzma
import os
import re
import subprocess
import sys
import tarfile
import time
import urllib.request
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
INDEX = ROOT / 'tools/chrome-packages/debian-Packages'
CACHE = nexus_paths.cache('ext4-debs')
QEMU = r'C:\Program Files\qemu\qemu-system-x86_64.exe'
KERNEL_PKG = 'linux-image-6.12.107+deb13-cloud-amd64'
PACKAGES = [KERNEL_PKG, 'e2fsprogs', 'libext2fs2t64', 'busybox-static', 'libss2', 'libuuid1', 'libblkid1', 'libcom-err2', 'libc6']
APPLETS = ['sh', 'mount', 'umount', 'mkdir', 'insmod', 'sleep', 'ls', 'md5sum', 'sha256sum', 'cat', 'poweroff', 'find',
           'stat', 'cp', 'dd', 'sync', 'head', 'tail', 'wc', 'echo', 'rm', 'mv', 'ln', 'touch', 'chmod', 'chown', 'mknod',
           'truncate', 'grep', 'sed', 'readlink', 'du', 'df', 'xargs', 'sort', 'cmp', 'printf', 'seq', 'uname', 'test', 'true', 'dmesg']


def fetch():
    CACHE.mkdir(parents=True, exist_ok=True)
    text = INDEX.read_text(encoding='utf-8', errors='replace')
    paths = []
    for rec in text.split('\n\n'):
        m = re.search(r'^Package: (.+)$', rec, re.M)
        if not m or m.group(1) not in PACKAGES:
            continue
        fn = re.search(r'^Filename: (.+)$', rec, re.M).group(1)
        sha = re.search(r'^SHA256: (.+)$', rec, re.M).group(1)
        out = CACHE / Path(fn).name
        if not out.exists():
            local = INDEX.parent / Path(fn).name
            data = local.read_bytes() if local.exists() else urllib.request.urlopen('https://deb.debian.org/debian/' + fn, timeout=300).read()
            if hashlib.sha256(data).hexdigest() != sha:
                raise SystemExit('checksum mismatch: ' + fn)
            out.write_bytes(data)
        paths.append(out)
    if len(paths) != len(PACKAGES):
        raise SystemExit('missing Debian packages in the index')
    return paths


def deb_files(deb):
    data = deb.read_bytes()
    off = 8
    while off < len(data):
        name = data[off:off + 16].decode().strip().rstrip('/')
        size = int(data[off + 48:off + 58].decode().strip())
        blob = data[off + 60:off + 60 + size]
        off += 60 + size + (size & 1)
        if name.startswith('data.tar'):
            with tarfile.open(fileobj=io.BytesIO(blob), mode='r:xz' if name.endswith('xz') else 'r:*') as t:
                for m in t.getmembers():
                    if m.isfile():
                        yield m.name.lstrip('./'), t.extractfile(m).read(), m.mode, None
                    elif m.issym():
                        yield m.name.lstrip('./'), None, 0o777, m.linkname


class Cpio:
    def __init__(self):
        self.out = io.BytesIO()
        self.ino = 1
        self.dirs = set()

    def entry(self, name, mode, data=b''):
        name = name.lstrip('/')
        hdr = '070701%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X%08X' % (
            self.ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name) + 1, 0)
        self.ino += 1
        self.out.write(hdr.encode() + name.encode() + b'\0')
        self.out.write(b'\0' * ((4 - self.out.tell() % 4) % 4))
        self.out.write(data)
        self.out.write(b'\0' * ((4 - self.out.tell() % 4) % 4))

    def mkdirs(self, path):
        parts = path.strip('/').split('/')[:-1]
        for i in range(1, len(parts) + 1):
            d = '/'.join(parts[:i])
            if d and d not in self.dirs:
                self.dirs.add(d)
                self.entry(d, 0o40755)

    def file(self, path, data, mode=0o100755):
        self.mkdirs(path)
        self.entry(path, mode, data)

    def symlink(self, path, target):
        self.mkdirs(path)
        self.entry(path, 0o120777, target.encode())

    def finish(self):
        self.entry('TRAILER!!!', 0)
        return self.out.getvalue()


def build_initramfs(script):
    debs = fetch()
    c = Cpio()
    kernel = None
    wanted = re.compile(r'^(etc/mke2fs[.]conf|usr/sbin/(e2fsck|debugfs|dumpe2fs|mke2fs|tune2fs)|usr/bin/busybox|usr/lib/x86_64-linux-gnu/(ld-linux-x86-64\.so\.2|libc\.so\.6|lib(ext2fs|com_err|e2p|ss|uuid|blkid)\.so.*))$')
    for deb in debs:
        for name, data, mode, link in deb_files(deb):
            if name.startswith('boot/vmlinuz'):
                kernel = data
            elif name.endswith('drivers/block/virtio_blk.ko.xz'):
                c.file('virtio_blk.ko', lzma.decompress(data), 0o100644)
            elif wanted.match(name):
                if link:
                    c.symlink(name, link)
                else:
                    c.file(name, data, 0o100755)
    c.symlink('lib', 'usr/lib')
    c.symlink('bin', 'usr/bin')
    c.symlink('sbin', 'usr/sbin')
    c.symlink('lib64', 'usr/lib/x86_64-linux-gnu')
    for applet in APPLETS:
        c.symlink('usr/bin/' + applet, 'busybox')
    init = '''#!/bin/sh
export PATH=/usr/sbin:/usr/bin LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu
mkdir -p /proc /sys /dev /mnt /tmp
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
insmod /virtio_blk.ko
n=0; while [ ! -b /dev/vda ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n+1)); done
D=/dev/vda1; [ -b /dev/vda1 ] || D=/dev/vda
echo "LXV START $(uname -r) device=$D"
fsck_n() { e2fsck -fn $D > /tmp/fsck.out 2>&1; rc=$?; cat /tmp/fsck.out; echo "LXV E2FSCK_N rc=$rc"; }
fsck_y() { e2fsck -fy $D > /tmp/fsck.out 2>&1; rc=$?; cat /tmp/fsck.out; echo "LXV E2FSCK_Y rc=$rc"; }
''' + script + '''
sync
echo "LXV DONE"
poweroff -f
'''
    c.file('init', init.encode(), 0o100755)
    return kernel, c.finish()


SCRIPTS = {
    'check': 'dumpe2fs -h $D 2>&1 | head -60\nfsck_n\n',
    'replay': 'fsck_y\nfsck_n\n',
    # Linux formats /dev/vdb1 (Debian mke2fs.conf defaults, 4 KiB blocks; full path: busybox prefers its own applets) and copies the NexusOS ext4 volume /dev/vda1 into it
    'linuxmade': 'mkdir -p /src /dst; mount -t ext4 -o ro /dev/vda1 /src || echo "LXV SRC MOUNT FAILED"\n'
                 '/usr/sbin/mke2fs -F -t ext4 -b 4096 -L linux-made /dev/vdb1 && mount -t ext4 /dev/vdb1 /dst || echo "LXV DST FAILED"\n'
                 'cp -a /src/. /dst/ && echo "LXV COPIED"\n'
                 'mkdir /dst/home/nexus/linux-index && i=0; while [ $i -lt 1500 ]; do echo $i > /dst/home/nexus/linux-index/linux-made-entry-name-$i; i=$((i+1)); done\n'
                 'chown -R 1000:1000 /dst/home/nexus/linux-index; echo 1 > /dst/nexus/test-writes; echo 1 > /dst/nexus/test-posix\n'
                 'ln -s /home/nexus/linux-index/linux-made-entry-name-7 /dst/home/nexus/linux-link\n'
                 'ln -s /home/nexus/linux-index/../linux-index/../linux-index/../linux-index/linux-made-entry-name-9 /dst/home/nexus/linux-slow-link\n'
                 'umount /dst && mount -t ext4 /dev/vdb1 /dst && ls -ld /dst/home/nexus/linux-index /dst/home/nexus/linux-link && echo "LXV REMOUNTED"\n'
                 'dumpe2fs -h /dev/vdb1 2>/dev/null | head -70\n'
                 'umount /dst && umount /src && echo "LXV LINUX MADE"\nD=/dev/vdb1\nfsck_n\n',
    'mount': 'mount -t ext4 $D /mnt && echo "LXV MOUNTED" || echo "LXV MOUNT FAILED"\n'
             'dmesg 2>/dev/null | tail -20\nls -la /mnt\nfind /mnt -xdev | head -400 | wc -l\n'
             'umount /mnt && echo "LXV UNMOUNTED"\nfsck_n\n',
}


def run(image, work, script, whpx=False, rw=False, timeout=600, second=None):
    """second: an extra image attached read-write as /dev/vdb (e.g. a target Linux formats and fills)."""
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    kernel, initrd = build_initramfs(script)
    (work / 'vmlinuz').write_bytes(kernel)
    (work / 'initrd.cpio').write_bytes(initrd)
    log = work / 'linux.log'
    log.unlink(missing_ok=True)
    accel = ['-accel', 'whpx,kernel-irqchip=off', '-cpu', 'qemu64,+rdrand'] if whpx else ['-cpu', 'max']
    cmd = [QEMU, *accel, '-m', '1024', '-kernel', str(work / 'vmlinuz'), '-initrd', str(work / 'initrd.cpio'),
           '-append', 'console=ttyS0 panic=-1 loglevel=4', '-drive',
           f'file={image},format=raw,if=virtio' + ('' if rw else ',snapshot=on'),
           *(['-drive', f'file={second},format=raw,if=virtio'] if second else []),
           '-display', 'none', '-vga', 'none', '-serial', f'file:{log}', '-no-reboot']
    t = time.time()
    try:
        subprocess.run(cmd, timeout=timeout, capture_output=True)
    finally:
        (work / 'vmlinuz').unlink(missing_ok=True); (work / 'initrd.cpio').unlink(missing_ok=True)
    text = log.read_text(encoding='utf-8', errors='replace')
    return text, time.time() - t


def verdict(text):
    rcs = [int(x) for x in re.findall(r'LXV E2FSCK_[NY] rc=(\d+)', text)]
    return rcs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('image'); ap.add_argument('work')
    ap.add_argument('--mode', default='check', choices=sorted(SCRIPTS))
    ap.add_argument('--script'); ap.add_argument('--whpx', action='store_true'); ap.add_argument('--rw', action='store_true')
    ap.add_argument('--second', help='extra image attached read-write as /dev/vdb (target of --mode linuxmade)')
    a = ap.parse_args()
    assert a.mode != 'linuxmade' or a.second, '--mode linuxmade needs --second TARGET.img (partitioned like apps64-ext4.img)'
    script = Path(a.script).read_text() if a.script else SCRIPTS[a.mode]
    text, seconds = run(a.image, a.work, script, a.whpx, a.rw, second=a.second)
    print(text[text.find('LXV START'):] if 'LXV START' in text else text[-6000:])
    rcs = verdict(text)
    ok = 'LXV DONE' in text and rcs and all(rc == 0 for rc in rcs[-1:]) and all(rc in (0, 1) for rc in rcs)
    print('[linux-validator] %s in %.0fs: e2fsck exit codes %s -> %s' % (a.image, seconds, rcs, 'CLEAN' if ok else 'PROBLEMS'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())

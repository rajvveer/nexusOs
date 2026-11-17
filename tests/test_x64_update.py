"""NexusOS core updates: signed bundles (arch/x86_64/update_bundle.py) into A/B slots (services/nexus-update), the
UEFI loader's slot choice, trial counter and rollback (uefi/abslot.c), the kernel's per-slot init (bootslot.c).

    python -I tests/test_x64_update.py WORK --base BASE [--accel auto|whpx|tcg] [--qmp-port 4676]

BASE is an installed system (the overlay a finished tests/test_x64_services.py --keep-disk run kept). Every boot is a
UEFI boot (OVMF, the build's ESP image out/nexus64-uefi.img, read only) of WORK/boot.qcow2 over WORK/stage.qcow2 over
BASE: the stage gets the worktree's services, this run's update signing key (keygen per run) as
/usr/share/keyrings/nexusos-update.pgp and TRIES=2 in /etc/nexus/update.conf.

  boot 1  bundles signed by another key, with a file changed after signing, with an extra member, with a member that is a
          symbolic link: refused, nothing
          installed; the good bundle (kernel + Aurora + loader, version 2026.10.18.2) goes into slot b on trial
  boot 2  the loader boots slot b on trial (attempt 1), starts slot b's own loader (chained), slot b's kernel runs slot
          b's Aurora as init; the boot check makes b the default; an older bundle is refused; a bundle whose kernel is
          not a NexusOS kernel goes into slot a
  boot 3  the loader rejects slot a's kernel and boots slot b with nexus.rollback=a; the boot check records it; a
          bundle with a good kernel but an init that cannot start the desktop goes into slot a
  boot 4, 5  slot a on trial: init exits, nothing marks it good (attempts 1 and 2)
  boot 6  the attempts are used up: the loader rolls back to slot b by itself; the boot check records it
Result: WORK/result.json, serial logs WORK/boot*.log.
"""
from pathlib import Path
import argparse
import io
import json
import re
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import pkg_vm  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4676)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
OUT = ROOT / 'arch/x86_64/out'
BUNDLER = ROOT / 'arch/x86_64/update_bundle.py'
results = {'test': 'update', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def bundler(*argv):
    subprocess.run([sys.executable, '-I', str(BUNDLER), *map(str, argv)], check=True, stdout=subprocess.DEVNULL)


def rewrite(source, target, change=None, extra=None, link=None):
    """A copy of a bundle with one member's content changed after signing, or an extra member."""
    src = tarfile.open(source); members = [(m, src.extractfile(m).read()) for m in src.getmembers()]
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode='w', format=tarfile.USTAR_FORMAT) as tar:
        for m, data in members + ([(tarfile.TarInfo(extra[0]), extra[1])] if extra else []):
            if change and m.name == change: data = data[:-1] + bytes([data[-1] ^ 1])
            if link and m.name == link[0]:   # the member becomes a symbolic link
                m.type = tarfile.SYMTYPE; m.linkname = link[1]; m.size = 0; tar.addfile(m); continue
            m.size = len(data); tar.addfile(m, io.BytesIO(data))
    target.write_bytes(buffer.getvalue())


# guest side: every check prints "UPD <name> PASS|FAIL <detail>"
COMMON = '''PATH=/usr/sbin:/usr/bin:/sbin:/bin
H=@HOST@/files
ok() { echo "UPD $1 PASS ${2:-}"; }
bad() { echo "UPD $1 FAIL ${2:-}"; }
check() { name=$1; shift; if "$@" >/tmp/upd.out 2>&1; then ok "$name" "$(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; else bad "$name" "$(tail -c 600 /tmp/upd.out | tr '\\n' ' ')"; fi; }
refused() { name=$1; shift; if "$@" >/tmp/upd.out 2>&1; then bad "$name" "accepted: $(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; else ok "$name" "$(tail -c 300 /tmp/upd.out | tr '\\n' ' ')"; fi; }
get() { wget -q -O "/tmp/$1" "$H/$1"; }
echo "UPD cmdline $(cat /proc/cmdline)"
nexusctl status nexus-update-check 2>&1 | head -5
nexus-update status 2>&1 | sed 's/^/UPD status| /'
'''
HOOKS = {
    1: COMMON + '''
check no-slots-yet sh -c '! [ -e /boot/nexus/bootctl ] && nexus-update status | grep -q "Booted slot:   none"'
get good.nxu; chmod 644 /tmp/good.nxu
check user-cannot-apply sh -c '! su nexus -s /bin/sh -c "nexus-update apply /tmp/good.nxu" && ! [ -e /boot/nexus/bootctl ]'
for b in badsig tampered extra symlink; do get $b.nxu; refused refused-$b nexus-update apply /tmp/$b.nxu; done
check nothing-installed-by-refused sh -c '! [ -e /boot/nexus/bootctl ] && ! [ -e /boot/nexus/slot-b ]'
get good.nxu; check apply-good nexus-update apply /tmp/good.nxu
check pending-b sh -c 'grep -qx "trial=b" /boot/nexus/bootctl && grep -qx "default=a" /boot/nexus/bootctl && [ -x /boot/nexus/slot-b/init ] && [ -s /boot/nexus/slot-b/kernel.bin ] && [ -s /boot/nexus/slot-b/BOOTX64.EFI ] && [ "$(wc -c < /boot/nexus/bootcount)" = 64 ]'
''',
    2: COMMON + '''
check booted-slot-b sh -c 'grep -q "nexus.slot=b" /proc/cmdline && grep -q "nexus.trial=" /proc/cmdline'
check init-from-slot sh -c '[ "$(readlink /proc/1/exe)" = /boot/nexus/slot-b/init ] || grep -q slot-b /proc/1/cmdline'
check marked-good sh -c 'grep -qx "default=b" /boot/nexus/bootctl && grep -qx "trial=none" /boot/nexus/bootctl && grep -q "^updated 2026.10.18.2" /var/lib/nexus/update/last'
check update-logged sh -c 'nexusctl logs -n 400 | grep -q "slot b (version 2026.10.18.2) booted"'
get older.nxu; refused refused-older nexus-update apply /tmp/older.nxu
get badkernel.nxu; check apply-badkernel nexus-update apply /tmp/badkernel.nxu
check pending-a sh -c 'grep -qx "trial=a" /boot/nexus/bootctl && grep -qx "default=b" /boot/nexus/bootctl'
''',
    3: COMMON + '''
check rolled-back-immediately sh -c 'grep -q "nexus.rollback=a" /proc/cmdline && grep -q "nexus.slot=b" /proc/cmdline'
check rollback-recorded sh -c 'grep -qx "default=b" /boot/nexus/bootctl && grep -qx "trial=none" /boot/nexus/bootctl && grep -q "^rolled-back 2026.10.18.3" /var/lib/nexus/update/last'
get badinit.nxu; check apply-badinit nexus-update apply /tmp/badinit.nxu
check pending-a-again sh -c 'grep -qx "trial=a" /boot/nexus/bootctl && grep -qx "tries=2" /boot/nexus/bootctl'
''',
    6: COMMON + '''
check rolled-back-after-tries sh -c 'grep -q "nexus.rollback=a" /proc/cmdline && grep -q "nexus.slot=b" /proc/cmdline && ! grep -q "nexus.trial=" /proc/cmdline'
check rollback-recorded-2 sh -c 'grep -qx "default=b" /boot/nexus/bootctl && grep -qx "trial=none" /boot/nexus/bootctl && grep -q "^rolled-back 2026.10.18.4" /var/lib/nexus/update/last'
check counter sh -c 'grep -q "count=2" /boot/nexus/bootcount'
check still-version-2 sh -c 'nexus-update status | grep -q "Default slot:  b (2026.10.18.2)"'
''',
}


started = time.time()
server = None
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    esp = OUT / 'nexus64-uefi.img'
    if not esp.is_file(): raise SystemExit('build first (out/nexus64-uefi.img)')
    keys, other = WORK / 'keys', WORK / 'otherkey'
    bundler('keygen', keys); bundler('keygen', other)
    b = WORK / 'bundles'; b.mkdir(exist_ok=True)
    bundler('build', b / 'good.nxu', '--version', '2026.10.18.2', '--key', keys / 'update-key.json')
    bundler('build', b / 'badsig.nxu', '--version', '2026.10.18.2', '--key', other / 'update-key.json')
    rewrite(b / 'good.nxu', b / 'tampered.nxu', change='init')
    rewrite(b / 'good.nxu', b / 'extra.nxu', extra=('evil.sh', b'#!/bin/sh\necho owned\n'))
    rewrite(b / 'good.nxu', b / 'symlink.nxu', link=('init', '/etc/shadow'))
    bundler('build', b / 'older.nxu', '--version', '2026.10.18.1', '--key', keys / 'update-key.json')
    junk = WORK / 'notakernel.bin'; junk.write_bytes(bytes(4096) + b'not a NexusOS kernel' * 1000)
    bundler('build', b / 'badkernel.nxu', '--version', '2026.10.18.3', '--key', keys / 'update-key.json', '--kernel', junk, '--no-loader')
    bundler('build', b / 'badinit.nxu', '--version', '2026.10.18.4', '--key', keys / 'update-key.json', '--init', OUT / 'svc_test_user.elf', '--no-loader')
    files = {p.name: p.read_bytes() for p in b.glob('*.nxu')}
    keyring = (keys / 'nexusos-update.pgp').read_bytes()

    def stage(fs):
        pkg_vm.refresh_services(fs)
        pkg_vm.put(fs, '/usr/share/keyrings/nexusos-update.pgp', keyring)
        pkg_vm.put(fs, '/etc/nexus/update.conf', 'TRIES=2\n')
    print('stage overlay: %d blocks' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', stage), flush=True)
    server = pkg_vm.HookServer(b'', files, WORK / 'uploads')
    logs = {}
    for n in range(1, 7):
        server.hook = (HOOKS.get(n) or '').encode()
        vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='boot%d' % n, accel=args.accel, smp=2, memory='2048',
                       fresh_overlay=(n == 1), overlay_name='boot.qcow2', uefi=esp, hook_port=server.port)
        try:
            vm.exited(900 if n in HOOKS else 420)
            logs[n] = vm.text()
        finally:
            vm.close(keep_overlay=True)
        log = logs[n]
        for m in re.finditer(r'^UPD (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check('boot%d %s' % (n, m.group(1)), m.group(2) == 'PASS', m.group(3))
        check('boot%d: no kernel fault' % n, 'KERNEL_FAULT' not in log, re.findall(r'.*KERNEL_FAULT.*', log)[:2])
        if n in HOOKS: check('boot%d: the guest checks ran' % n, 'TEST-HOOK finished' in log, log[-300:])
    L = logs
    check('boot1: no slot boot before an update (the ESP kernel, /sbin/init)', '[uefi] A/B' not in L[1] and 'INIT from the booted slot' not in L[1])
    check('boot2: the loader boots slot b on trial (attempt 1)', '[uefi] A/B trial slot=b attempt=1' in L[2])
    check('boot2: slot b\'s own loader takes over (chained)', '[uefi] A/B starting the slot loader' in L[2] and '[uefi] A/B chained loader slot=b' in L[2])
    check('boot2: slot b\'s kernel and Aurora run', '[uefi] A/B kernel from slot=b' in L[2] and '[64] INIT from the booted slot' in L[2])
    check('boot3: the trial of slot a starts, its kernel is rejected, slot b boots', '[uefi] A/B trial slot=a attempt=1' in L[3] and
          '[uefi] A/B unusable kernel in slot=a' in L[3] and '[uefi] A/B kernel from slot=b' in L[3])
    for n, attempt in ((4, 1), (5, 2)):
        check('boot%d: slot a on trial, attempt %d; its init cannot start the desktop' % (n, attempt), '[uefi] A/B trial slot=a attempt=%d' % attempt in L[n]
              and '[uefi] A/B kernel from slot=a' in L[n] and 'INIT from the booted slot' in L[n] and 'DESKTOP READY' not in L[n], L[n][-400:])
    check('boot6: the loader rolls back to slot b after 2 attempts', '[uefi] A/B rollback from slot=a to slot=b' in L[6] and '[uefi] A/B kernel from slot=b' in L[6])
finally:
    if server: server.close()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    for name in ('boot.qcow2', 'stage.qcow2'): (WORK / name).unlink(missing_ok=True)
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)

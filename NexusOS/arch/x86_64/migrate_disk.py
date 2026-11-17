#!/usr/bin/env python3
"""Upgrade a NexusOS FAT32 desktop disk to the full system (ext4 root + Debian base, apt, services, Software, printing,
core updates), keeping every file - and go back. The host side of System Settings > System Upgrade; the launcher
(build.py desktop) runs it between two sessions when the system asked for it.

    python -I arch/x86_64/migrate_disk.py status   [--disk D]
    python -I arch/x86_64/migrate_disk.py run      [--disk D] [--yes] [--size MiB] [--cache DIR] [--skip PATH ...]
    python -I arch/x86_64/migrate_disk.py rollback [--disk D] [--yes]
    python -I arch/x86_64/migrate_disk.py finish   [--disk D] [--yes]
    python -I arch/x86_64/migrate_disk.py pending  [--disk D]        what the system asked for: upgrade | rollback | finish

D defaults to the launcher's disk, arch/x86_64/out/desktop64.img. Files beside D (desktop64.img):
  D                    the disk the launcher boots
  D.upgrading          the new ext4 disk while it is being made (deleted on any failure; D is then unchanged)
  D-fat.img            after an upgrade: the old FAT32 disk, byte for byte as it was, kept for `rollback` until `finish`
  D-full.img           after a rollback: the full system as it was (with what changed there), until `finish`
  D.upgrade.json       the state (upgraded | rolled-back | finished | failed), times and the full report
In the guest, ~/.local/state/nexus/upgrade of the desktop account (/home/nexus: on a FAT32 root the desktop user can write
only there and in /tmp), written by System Settings > System Upgrade (aurora_settings_upgrade.c):
  FAT disk:  request.json                   {"version": 1, "user": "nexus", "password_hash": "$6$...", "hostname": ...,
                                           "timezone": ..., "locale": ...}; after a failed attempt it becomes
                                           request.failed.json beside failed.json (the reason)
  new disk:  result.json                  this tool's report (the page shows it); the page then writes
             rollback-request or finish-request for the next start

`run`: the disk must be a NexusOS FAT32 disk that no program has open, with room beside it for the new disk (its used
space + 1.5 GiB). The new ext4 disk gets every file (userland/fat2ext4.py: names, case restored, owners, times;
browser profiles, Wine prefixes, settings), then every file is compared with the original (size + SHA-256, links,
folders: sysinstall.py compare_trees), then the Debian base is staged (installed by dpkg at the first start) and the
services installed (sysinstall.py), the account gets the request's password (sudo, Software, printing need one),
the volume is checked (ext4 consistency), and only then are the files renamed: D -> D-fat.img, D.upgrading -> D. Any
failure leaves D as it was. `rollback` puts the FAT disk back (D -> D-full.img, D-fat.img -> D); `finish` deletes the
disk that is no longer used. --skip leaves a path out (tests: their disk copies' browser profile folders).
"""
from pathlib import Path
import argparse
import gc
import hashlib
import importlib.util
import json
import os
import re
import shutil
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
UPDIR = '/home/nexus/.local/state/nexus/upgrade'   # the desktop account of a FAT32 disk (kernel.c private_path)
FAT_UPDIR = UPDIR.lstrip('/')                     # fatput's spelling
REQUEST = UPDIR + '/request.json'
RESULT = UPDIR + '/result.json'
NAME_RE = re.compile(r'[a-z_][a-z0-9_-]{0,31}')
HASH_RE = re.compile(r'\$(6|y)\$[./A-Za-z0-9$=,]{8,240}')
HOST_RE = re.compile(r'[A-Za-z0-9][A-Za-z0-9-]{0,62}')
ZONE_RE = re.compile(r'[A-Za-z0-9_+-]+(/[A-Za-z0-9_+-]+){0,3}')
LOCALE_RE = re.compile(r'[A-Za-z0-9_.@-]{1,40}')
EXTRA_SPACE = 3 << 29   # 1.5 GiB: the Debian base (staged .debs + unpacked), journal, metadata


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec); spec.loader.exec_module(value); return value


f2e = module('migrate_fat2ext4', ROOT / 'userland/fat2ext4.py')
fatput, ext4lib = f2e.fatput, f2e.ext4lib


class Refused(Exception):
    pass


def _replace(src, dst, seconds=120):
    """Path.replace that waits out a short-lived open handle (a virus scanner reading the new disk, a file object not yet
    collected): Windows does not rename an open file."""
    gc.collect(); end = time.time() + seconds
    while True:
        try: return src.replace(dst)
        except PermissionError:
            if time.time() > end: raise
            time.sleep(1); gc.collect()


def _unlink(path, seconds=30):
    """Best effort (cleanup after a failure must not hide the failure's reason)."""
    end = time.time() + seconds
    while True:
        try: return path.unlink(missing_ok=True)
        except PermissionError:
            if time.time() > end: print('[upgrade] could not delete %s (in use): delete it by hand' % path, flush=True); return
            gc.collect(); time.sleep(1)
        except OSError as error: print('[upgrade] could not delete %s: %s' % (path, error), flush=True); return


def side(disk, suffix):
    disk = Path(disk); return disk.with_name(disk.stem + suffix) if suffix.startswith('-') else disk.with_name(disk.name + suffix)


def state_of(disk):
    try: return json.loads(side(disk, '.upgrade.json').read_text())
    except (OSError, ValueError): return {}


def save_state(disk, data):
    path = side(disk, '.upgrade.json'); part = path.with_name(path.name + '.part')
    part.write_text(json.dumps(data, indent=1)); part.replace(path)


def kind_of(disk):
    """'fat' (a NexusOS FAT32 disk), 'ext4', or None."""
    try:
        fat = fatput.Fat32(str(disk), readonly=True); base = fat.base; fat.close()
        if base: return 'fat'
    except (Exception, SystemExit): pass   # fatput/ext4lib end with SystemExit on what is not their file system
    try: ext4lib.Ext4(str(disk)).f.close(); return 'ext4'
    except (Exception, SystemExit): return None


def fat_read(disk, path):
    """A small file of a FAT32 disk, or None."""
    fat = fatput.Fat32(str(disk), readonly=True)
    try:
        parts = [p for p in path.split('/') if p]
        try: dirc = fat.lookup_dir(parts[:-1])
        except (Exception, SystemExit): return None
        hit = fat.find_name(dirc, parts[-1]) if dirc else None
        if not hit or hit[2][11] & 0x10: return None
        e = hit[2]; first = (int.from_bytes(e[20:22], 'little') << 16) | int.from_bytes(e[26:28], 'little'); size = int.from_bytes(e[28:32], 'little')
        if size > 1 << 20: return None
        # only the clusters the size needs (the guest wrote this disk: a long or looping chain must not be read whole)
        need, out, c, seen = -(-size // fat.cl_bytes), bytearray(), first, set()
        while need and 2 <= c < fat.nclusters + 2 and c not in seen:
            seen.add(c); out += fat.rd(fat.cl_off(c), fat.cl_bytes); need -= 1; c = fat.fat_get(c)
        return bytes(out[:size]) if len(out) >= size else None
    finally: fat.close()


def ext4_read(disk, path):
    fs = ext4lib.Ext4(str(disk))
    try:   # a request written just before power-off can still be in the journal: replayed in memory (the disk is not written)
        js = ext4lib.journal_super(fs) if fs.sb.compat & ext4lib.COMPAT_HAS_JOURNAL else None
        if js and js['start']:
            ext4lib.journal_replay(fs, apply_to_overlay=True); overlay = fs.overlay; fs.f.close(); fs = ext4lib.Ext4(str(disk), overlay=overlay)
    except Exception: pass   # noqa: BLE001 - read what the volume says without the journal
    try:
        try: ino = fs.lookup(path)
        except (KeyError, ValueError, FileNotFoundError): return None
        if not ino: return None
        inode = fs.read_inode(ino)
        return fs.read_file(inode) if (inode.mode & 0o170000) == 0o100000 and inode.size <= 1 << 20 else None
    finally: fs.f.close()


def fat_used_bytes(disk):
    fat = fatput.Fat32(str(disk), readonly=True)
    try: return sum(1 for c in range(2, fat.nclusters + 2) if fat.fat_get(c)) * fat.cl_bytes
    finally: fat.close()


def in_use(disk):
    """True while another program (QEMU, VMware) has the disk open: an exclusive open fails on Windows."""
    if sys.platform != 'win32': return False
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    handle = kernel32.CreateFileW(str(disk), 0x80000000, 0, None, 3, 0x80, None)   # GENERIC_READ, no sharing, OPEN_EXISTING
    if handle == wintypes.HANDLE(-1).value:
        return ctypes.get_last_error() == 32   # ERROR_SHARING_VIOLATION
    kernel32.CloseHandle(handle); return False


def free_bytes(path):
    override = os.environ.get('NEXUS_UPGRADE_FREE_BYTES')   # tests: a host that is short of space
    return int(override) if override else shutil.disk_usage(path).free


def validate_request(raw):
    """The guest's request (only these keys, each checked; the hash is stored as is). Raises Refused."""
    try: request = json.loads(raw.decode('utf-8'))
    except (UnicodeDecodeError, ValueError): raise Refused('the upgrade request is not valid JSON')
    if not isinstance(request, dict) or request.get('version') != 1: raise Refused('unknown upgrade request version')
    checks = {'user': NAME_RE, 'password_hash': HASH_RE, 'hostname': HOST_RE, 'timezone': ZONE_RE, 'locale': LOCALE_RE}
    clean = {}
    for key, value in request.items():
        if key in ('version', 'requested'): continue
        if key not in checks: raise Refused('unknown key %r in the upgrade request' % key)
        if not isinstance(value, str) or not checks[key].fullmatch(value) or '..' in value: raise Refused('bad %s in the upgrade request' % key)
        clean[key] = value
    if 'user' not in clean: raise Refused('the upgrade request names no user')
    return clean


def account(disk, user):
    """(uid, full name) of user on the FAT disk's /etc/passwd, or (1000, '') when the disk has no entry for it."""
    for line in (fat_read(disk, '/etc/passwd') or b'').decode('utf-8', 'replace').splitlines():
        f = line.split(':')
        if len(f) >= 7 and f[0] == user and f[2].isdigit(): return int(f[2]), f[4].split(',')[0]
    return 1000, ''


def settings_from_disk(disk, user):
    """What the old disk says about itself (host name, time zone, locale, the user's password hash), used when the
    request does not say it."""
    text = lambda p: (fat_read(disk, p) or b'').decode('utf-8', 'replace').strip()
    out = {}
    for line in text('/etc/shadow').splitlines():
        f = line.split(':')
        if len(f) >= 2 and f[0] == user and HASH_RE.fullmatch(f[1]): out['password_hash'] = f[1]
    host = text('/etc/hostname').splitlines()[0] if text('/etc/hostname') else ''
    if HOST_RE.fullmatch(host): out['hostname'] = host
    zone = text('/etc/timezone')
    if ZONE_RE.fullmatch(zone) and '..' not in zone: out['timezone'] = zone
    for line in text('/etc/default/locale').splitlines():
        if line.startswith('LANG='):
            value = line[5:].strip('"\'')
            if LOCALE_RE.fullmatch(value): out['locale'] = value
    return out


class Progress:
    def __init__(self, total):
        self.total = max(total, 1); self.last = 0; self.phase = None
    def __call__(self, phase, files, size):
        now = time.time()
        if phase != self.phase:
            self.phase = phase
            print({'copy': '[upgrade] 1/4 copying every file to the new disk', 'verify': '[upgrade] 2/4 comparing every file with the old disk',
                   'base': '[upgrade] 3/4 adding the Debian base (installed by dpkg at the first start)',
                   'services': '[upgrade] 3/4 installing the service manager, Software, printing and core updates'}.get(phase, phase), flush=True)
            self.last = 0
        if phase in ('copy', 'verify') and now - self.last >= 2:
            self.last = now
            print('[upgrade]   %s %3d%%  %d files, %.2f of %.2f GB' % ('copied ' if phase == 'copy' else 'compared', min(99, size * 100 // self.total),
                                                                       files, size / 1e9, self.total / 1e9), flush=True)


def ask_password():
    """The new system's password for the account (interactive use without an in-OS request): a SHA-512-crypt hash."""
    import getpass
    import secrets
    first = getpass.getpass('Password for the full system (administrator tasks: sudo, Software, printing): ')
    if len(first) < 6 or first != getpass.getpass('Again: '): raise Refused('the passwords are empty, too short (6) or differ')
    crypt = module('migrate_sha512_crypt', ROOT / 'tests/sha512_crypt.py')
    salt = ''.join(secrets.choice(crypt.B64) for _ in range(16))
    return crypt.sha512_crypt(first, salt)


def run(disk, yes=False, size_mib=None, cache=None, skips=(), request_required=False, password_hash=None):
    disk = Path(disk).resolve(); new = side(disk, '.upgrading'); kept = side(disk, '-fat.img'); lock = side(disk, '.upgrade.lock')
    if not disk.is_file(): raise Refused('no disk %s' % disk)
    kind = kind_of(disk)
    if kind == 'ext4': raise Refused('%s already runs the full system (ext4)' % disk.name)
    if kind != 'fat': raise Refused('%s is not a NexusOS FAT32 desktop disk' % disk.name)
    if kept.exists(): raise Refused('%s exists: an earlier upgrade is not finished (rollback or finish first)' % kept.name)
    if side(disk, '-full.img').exists(): raise Refused('%s exists: finish the earlier rollback first' % side(disk, '-full.img').name)
    if in_use(disk): raise Refused('%s is in use (NexusOS is running?): shut it down first' % disk.name)
    raw = fat_read(disk, REQUEST)
    if raw is None and request_required: raise Refused('the system did not ask for an upgrade')
    try: request = validate_request(raw) if raw is not None else {'user': 'nexus'}
    except Refused as error:   # a request this tool will never carry out: the page shows why (no retry at every start)
        mark_failed(disk, str(error)); save_state(disk, {'state': 'failed', 'reason': str(error), 'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())})
        raise
    used = fat_used_bytes(disk); need = used + EXTRA_SPACE; free = free_bytes(disk.parent)
    if free < need:
        reason = ('not enough free space beside %s: %.1f GB needed (its used space %.1f GB + 1.5 GB), %.1f GB free'
                  % (disk.name, need / 1e9, used / 1e9, free / 1e9))
        if raw is not None: mark_failed(disk, reason); save_state(disk, {'state': 'failed', 'reason': reason, 'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())})
        raise Refused(reason)
    settings = {**settings_from_disk(disk, request['user']), **request}
    if password_hash:
        if not HASH_RE.fullmatch(password_hash): raise Refused('--password-hash is not a $6$ or $y$ hash')
        settings['password_hash'] = password_hash
    if 'password_hash' not in settings:
        if yes: raise Refused('the full system needs a password for the account (administrator tasks): --password-hash, or the in-OS assistant')
        settings['password_hash'] = ask_password()
    if not yes:
        answer = input('Upgrade %s (%.1f GB used) to the full system? The old disk is kept as %s. [y/N] ' % (disk.name, used / 1e9, kept.name))
        if answer.strip().lower() not in ('y', 'yes'): raise Refused('cancelled')
    try:   # a lock left by a run that died (power loss) is taken over after 12 h
        if lock.exists() and time.time() - lock.stat().st_mtime > 12 * 3600: lock.unlink()
    except OSError: pass
    try: fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY); os.write(fd, str(os.getpid()).encode()); os.close(fd)
    except FileExistsError: raise Refused('another upgrade of this disk is running (%s)' % lock.name)
    t0 = time.time(); before = disk.stat().st_size
    try:
        new.unlink(missing_ok=True); Path(str(new) + '.migration.json').unlink(missing_ok=True)
        uid, gecos = account(disk, request['user'])
        config = {'USERNAME': request['user'], 'USERID': str(uid), 'FULLNAME': gecos or request['user'],
                  'HOSTNAME': settings.get('hostname', 'nexus'), 'TIMEZONE': settings.get('timezone', 'Etc/UTC'),
                  'LOCALES': ' '.join(dict.fromkeys(['C.UTF-8', settings.get('locale', 'en_US.UTF-8')])), 'LANG_DEFAULT': settings.get('locale', 'C.UTF-8')}
        config['PASSWORD_HASH'] = settings['password_hash']
        print('[upgrade] %s: %.2f GB used; user %s (%d); host %s; time zone %s' % (disk.name, used / 1e9, request['user'], uid, config['HOSTNAME'], config['TIMEZONE']), flush=True)
        sysinstall = module('migrate_sysinstall', HERE / 'sysinstall.py')
        report = sysinstall.new_system(new, disk, size_mib or (before >> 20), config, cache, migrate=True, skips=skips, progress=Progress(used))
        print('[upgrade] 4/4 checking the new disk', flush=True)
        errors, _, checker = ext4lib.check_image(str(new)); checker.fs.f.close()
        if errors: raise Refused('the new disk failed its consistency check: ' + '; '.join(errors[:5]))
        comparison = report.get('comparison', {})
        result = {'version': 1, 'upgraded': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'files': comparison.get('files'), 'bytes': comparison.get('bytes'),
                  'folders': comparison.get('dirs'), 'links': comparison.get('symlinks'), 'differences': len(comparison.get('differences', [])),
                  'missing': len(comparison.get('missing', [])), 'names_kept_lower_case': len(report.get('home_lowercased', [])),
                  'skipped': report.get('converted', {}).get('skipped', []), 'old_disk': kept.name, 'seconds': round(time.time() - t0)}
        fs = ext4lib.Writer(str(new))   # the report for the page; the request itself is not carried over
        try:
            fs.ensure_dir(fs.split(UPDIR), 0o755, uid, uid); fs.setattr(UPDIR, mode=0o755, uid=uid, gid=uid)   # the page writes its requests here
            for stale in (REQUEST, UPDIR + '/request.failed.json', UPDIR + '/failed.json'):
                try:
                    if fs.lookup(stale): fs.remove(stale)
                except (KeyError, ValueError, FileNotFoundError): pass
            fs.put_bytes(json.dumps(result, indent=1).encode(), RESULT, 0o644, uid, uid)
        finally: fs.close(backups=True)
        errors, _, checker = ext4lib.check_image(str(new)); checker.fs.f.close()
        if errors: raise Refused('the new disk failed its consistency check: ' + '; '.join(errors[:5]))
        if in_use(disk): raise Refused('%s was opened meanwhile: not switched' % disk.name)
        _replace(disk, kept)
        try: _replace(new, disk)
        except BaseException: _replace(kept, disk); raise   # the old disk goes back under its name before anything else
        side(disk, '.upgrading.migration.json').replace(side(disk, '.migration.json')) if side(disk, '.upgrading.migration.json').exists() else None
        save_state(disk, {'state': 'upgraded', **result, 'report': side(disk, '.migration.json').name})
        print('[upgrade] done in %d s: %s is the full system now (%s files, %.2f GB compared, %d differences); the old disk is %s until '
              '`finish` (or Keep in System Settings > System Upgrade). The first start installs the system packages (a few minutes).'
              % (result['seconds'], disk.name, result['files'], (result['bytes'] or 0) / 1e9, result['differences'], kept.name), flush=True)
        return result
    except BaseException as error:
        for path in (new, side(disk, '.upgrading.migration.json')): _unlink(path)
        reason = str(error) if isinstance(error, (Refused, SystemExit)) else '%s: %s' % (type(error).__name__, error)
        if kind_of(disk) == 'fat' and not kept.exists():
            mark_failed(disk, reason)
            save_state(disk, {'state': 'failed', 'reason': reason, 'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())})
        if isinstance(error, Refused): raise
        raise Refused(reason) from error
    finally:
        lock.unlink(missing_ok=True)


def mark_failed(disk, reason):
    """After a failed upgrade: the request becomes request.failed.json beside failed.json (the page shows the reason),
    so the next start does not try again by itself."""
    raw = fat_read(disk, REQUEST)
    if raw is None: return
    fat = fatput.Fat32(str(disk))
    try:
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            note = Path(tmp) / 'failed.json'; note.write_text(json.dumps({'reason': reason[:400], 'time': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}))
            old = Path(tmp) / 'request.failed.json'; old.write_bytes(raw)
            for name in ('failed.json', 'request.failed.json'):
                try: fat.rm(FAT_UPDIR + '/' + name)
                except SystemExit: pass
            fat.put(str(note), FAT_UPDIR + '/failed.json'); fat.put(str(old), FAT_UPDIR + '/request.failed.json'); fat.rm(FAT_UPDIR + '/request.json')
    finally: fat.close()


def rollback(disk, yes=False):
    disk = Path(disk).resolve(); kept = side(disk, '-fat.img'); full = side(disk, '-full.img')
    if not kept.is_file(): raise Refused('no %s: nothing to go back to' % kept.name)
    if kind_of(disk) != 'ext4' or kind_of(kept) != 'fat': raise Refused('%s is not an upgraded disk with its FAT32 original beside it' % disk.name)
    if full.exists(): raise Refused('%s exists already' % full.name)
    if in_use(disk) or in_use(kept): raise Refused('the disk is in use (NexusOS is running?): shut it down first')
    if not yes and input('Go back to the FAT32 disk? What changed on the full system stays in %s. [y/N] ' % full.name).strip().lower() not in ('y', 'yes'):
        raise Refused('cancelled')
    _replace(disk, full)
    try: _replace(kept, disk)
    except BaseException: _replace(full, disk); raise   # the full system stays the disk
    if fat_read(disk, REQUEST) is not None:   # the request was carried out: do not upgrade again at the next start
        fat = fatput.Fat32(str(disk))
        try: fat.rm(FAT_UPDIR + '/request.json')
        finally: fat.close()
    save_state(disk, {**state_of(disk), 'state': 'rolled-back', 'rolled_back': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'kept': full.name})
    print('[upgrade] %s is the FAT32 disk again; the full system is kept as %s until `finish`' % (disk.name, full.name), flush=True)


def finish(disk, yes=False):
    disk = Path(disk).resolve(); state = state_of(disk).get('state')
    target = side(disk, '-fat.img') if state == 'upgraded' else side(disk, '-full.img') if state == 'rolled-back' else None
    if target is None or not target.exists(): raise Refused('nothing to finish (state %s)' % (state or 'none'))
    if in_use(target): raise Refused('%s is in use' % target.name)
    if not yes and input('Delete %s (%.1f GB)? [y/N] ' % (target.name, target.stat().st_size / 1e9)).strip().lower() not in ('y', 'yes'): raise Refused('cancelled')
    gc.collect(); target.unlink()
    save_state(disk, {**state_of(disk), 'state': 'finished', 'finished': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'deleted': target.name})
    print('[upgrade] %s deleted' % target.name, flush=True)


def pending(disk):
    """What the system asked for at its last session: 'upgrade', 'rollback', 'finish' or None."""
    disk = Path(disk)
    if not disk.is_file(): return None
    kind = kind_of(disk)
    if kind == 'fat': return 'upgrade' if fat_read(disk, REQUEST) is not None and not side(disk, '-fat.img').exists() else None
    if kind == 'ext4':
        if ext4_read(disk, UPDIR + '/rollback-request') is not None and side(disk, '-fat.img').exists(): return 'rollback'
        if ext4_read(disk, UPDIR + '/finish-request') is not None and side(disk, '-fat.img').exists(): return 'finish'
    return None


def clear_request(disk, name):
    fs = ext4lib.Writer(str(disk))
    try:
        try:
            if fs.lookup(UPDIR + '/' + name): fs.remove(UPDIR + '/' + name)
        except (KeyError, ValueError, FileNotFoundError): pass
    finally: fs.close(backups=True)


def launcher_step(disk, cache=None):
    """build.py desktop, before a session: carry out what the system asked for. True when something was done. Never
    raises: whatever happens here, NexusOS still starts (from the disk as it is then)."""
    try: return _launcher_step(disk, cache)
    except (Exception, SystemExit) as error:   # noqa: BLE001
        print('[upgrade] the requested step failed: %s: %s; starting NexusOS as it is' % (type(error).__name__, error), flush=True)
        return False


def _launcher_step(disk, cache):
    action = pending(disk)
    if not action: return False
    print('[upgrade] NexusOS asked for: %s' % action, flush=True)
    try:
        if action == 'upgrade': run(disk, yes=True, cache=cache, request_required=True)
        elif action == 'rollback': rollback(disk, yes=True)
        else: clear_request(disk, 'finish-request'); finish(disk, yes=True)
    except Refused as error:
        print('[upgrade] not done: %s' % error, flush=True)
        if action != 'upgrade':   # a request that cannot be carried out is dropped (the page shows the state again)
            try: clear_request(disk, action + '-request')
            except Exception: pass
        return False
    return True


def status(disk):
    disk = Path(disk); state = state_of(disk)
    print('disk:     %s (%s)' % (disk, kind_of(disk) or 'missing or unknown'))
    print('state:    %s' % (state.get('state') or 'never upgraded'))
    for name in ('-fat.img', '-full.img', '.upgrading'):
        if side(disk, name).exists(): print('beside:   %s (%.1f GB)' % (side(disk, name).name, side(disk, name).stat().st_size / 1e9))
    print('pending:  %s' % (pending(disk) or 'nothing'))
    if state.get('reason'): print('failure:  %s' % state['reason'])


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=('status', 'run', 'rollback', 'finish', 'pending'))
    ap.add_argument('--disk', type=Path, default=OUT / 'desktop64.img'); ap.add_argument('--yes', action='store_true')
    ap.add_argument('--size', type=int, help='MiB of the new disk (default: the old disk\'s size)')
    ap.add_argument('--cache', type=Path, help='debian_base.py cache (default NEXUS_BASE_CACHE)')
    ap.add_argument('--skip', action='append', default=[], help='a path left out (never read)')
    ap.add_argument('--password-hash', help='the account\'s password hash on the new system ($6$/$y$; default: the request, the old disk, or asked)')
    a = ap.parse_args(argv)
    try:
        if a.action == 'status': status(a.disk)
        elif a.action == 'run': run(a.disk, a.yes, a.size, a.cache, a.skip, password_hash=a.password_hash)
        elif a.action == 'rollback': rollback(a.disk, a.yes)
        elif a.action == 'finish': finish(a.disk, a.yes)
        else:
            what = pending(a.disk); print(what or 'nothing'); return 0 if what else 1
    except Refused as error:
        print('[upgrade] refused: %s' % error, file=sys.stderr); return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

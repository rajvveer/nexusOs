#!/usr/bin/env python3
"""Install Wine (new WoW64 + Wayland driver), Xwayland, X11 test clients and wayland-info onto a NexusOS desktop disk.

    python -I arch/x86_64/wine_install.py <disk.img> [--only wine,xwayland,x11-tools,wayland-tools]
                                         [--grow MIB] [--cache DIR] [--report PATH]
    python -I arch/x86_64/wine_install.py lock [--update-keyring] [--allow-older]   # re-resolve wine-lock.json from signed indices

Groups (default: wine,xwayland):
  wine           Debian sid wine 10.0~repack-12(+b1): wine, wine64, libwine, wine-common, fonts-wine, wine64-preloader.
                 This is the first Debian build with new WoW64 (32-bit Windows programs run in the 64-bit wine64 process:
                 i386-windows + x86_64-windows PE DLLs, x86_64-unix only, no i386 Linux libraries) and it ships
                 winewayland.drv/winewayland.so. Its unix side links glibc >= 2.38 only, so it runs on the disk's Debian 13
                 (trixie) userland; the runtime libraries it needs come from trixie (libunwind8, libxkbregistry0,
                 libz-mingw-w64, libxxf86vm1, dash for the /bin/sh launcher scripts).
  xwayland       Debian trixie xwayland 2:24.1.6-1 + xkbcomp (x11-xkb-utils), xserver-common, dash (/bin/sh for xkbcomp),
                 the NexusOS rootful session launcher /usr/bin/nexus-x11-session (+ Aurora entry /nexus/apps/x11.args) and
                 the NexusOS X event logger /usr/bin/nexus-xtest (dependency-free: the session's client without x11-tools).
  x11-tools      xterm, x11-apps (xeyes, xclock, ...), xdotool (and nexus-xtest).
  wayland-tools  wayland-utils (wayland-info).
  addons         Wine Mono 9.4.0 (.NET Framework) and Wine Gecko 2.47.4 x86 + x86_64 (MSHTML) from dl.winehq.org, the
                 versions this Wine build asks for, pinned in wine-addons-lock.json by the SHA-256 that the build's own
                 appwiz.cpl checks (taken from the signed libwine package). They go to /usr/share/wine/{mono,gecko}/,
                 where wineboot finds them: every new prefix gets .NET and MSHTML without a download prompt.
                 --addons-cache DIR keeps the downloads (default: <cache>/addons).

Verification: wine-lock.json pins every .deb (URL + size + SHA-256) together with the InRelease of each suite it came from.
`lock` trusts only the Debian archive signing keys whose fingerprints are in ARCHIVE_KEYS: each InRelease must be one
clearsigned block with a good signature by one of them (gpgv status lines; the keyring file is the debian-archive-keyring
.deb the previous lock pins, replaced only by --update-keyring), name its suite, be within Valid-Until and not older than the
lock's (unless --allow-older). Only gpgv's verified text is parsed; Packages.xz must match its one signed SHA-256 entry before
it is decompressed (bounded). It then resolves the dependency closure (packages of chrome-lock.json are on every desktop disk
and are not repeated; packages that only enable optional Wine features are listed in SKIP with the reason) and checks that
every relation is met. Installing only uses the lock: every .deb is checked against its pinned size and SHA-256. Downloads:
deb.debian.org, then the pinned snapshot.debian.org URL (the snapshot run that first had the file), then a fresh lookup of
that run through snapshot's API, once a version has left the live archive (sid moves quickly).

Space: the wine group adds about 1.2 GB (libwine holds ~1500 PE DLLs); use --grow MIB when the FAT volume is short.
OpenGL through winewayland needs libEGL (arch/x86_64/gpu_userland.py, virgl); GDI applications do not.

What the maintainer scripts would do is emulated from tables: update-alternatives (/usr/bin/wine -> wine-stable, wine64,
msiexec, regedit, ... and /usr/share/wine/collection), libwine's trigger that copies libz-mingw-w64's zlib1.dll into the
i386-windows and x86_64-windows directories with Wine's builtin marker, and merged-usr links (/bin/<n> -> /usr/bin/<n>).
Package symlinks become NexusOS FAT symlinks. Files go through arch/x86_64/apps_install.py's FAT32 engine: read-only
analysis first, then a sparse working copy that replaces the image only after the reserved prefix was verified unchanged;
existing files are never overwritten (identical ones are skipped, different ones reported as conflicts). /nexus/wine-installed.json
records the groups, packages, links, and every library Wine's unix modules link or dlopen that is not on the disk, merged over
every run (installing groups one at a time keeps the earlier ones). After writing, every entry point of the selected groups
must resolve, or the image is left untouched.
"""
from pathlib import Path, PurePosixPath
import argparse
import array
import email.utils
import functools
import hashlib
import importlib.util
import json
import lzma
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
LOCK = HERE / 'wine-lock.json'
DEFAULT_CACHE = nexus_paths.cache('wayland')
VERSION = 1

_spec = importlib.util.spec_from_file_location('wine_apps_install', HERE / 'apps_install.py')
ai = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(ai)
InstallError = ai.InstallError
fold = ai.fold

MIRRORS = {'trixie': 'https://deb.debian.org/debian/', 'trixie-updates': 'https://deb.debian.org/debian/',
           'trixie-security': 'https://deb.debian.org/debian-security/', 'sid': 'https://deb.debian.org/debian/'}
TRIXIE = ('trixie', 'trixie-updates', 'trixie-security')
GROUPS = {
    'wine': {'sid': ['wine', 'wine64', 'libwine', 'wine-common', 'fonts-wine', 'wine64-preloader'],
             # apploader scripts: dash + coreutils (basename, cut); libc-bin only for its C.UTF-8 locale (apps_install.ONLY_PATHS):
             # LANG=C.UTF-8 gives Wine a UTF-8 unix code page, so non-ASCII Windows file names map to FAT long names
             'trixie': ['libunwind8', 'libxkbregistry0', 'libz-mingw-w64', 'libxxf86vm1', 'dash', 'coreutils', 'libc-bin']},
    'xwayland': {'trixie': ['xwayland', 'xserver-common', 'x11-xkb-utils', 'dash']},
    'x11-tools': {'trixie': ['xterm', 'x11-apps', 'xdotool']},
    'wayland-tools': {'trixie': ['wayland-utils']},
    'addons': {},                     # Wine Mono + Gecko (wine-addons-lock.json), not Debian packages
}
DEFAULT_GROUPS = 'wine,xwayland,addons'
ADDONS_LOCK = HERE / 'wine-addons-lock.json'
ADDON_ORIGINS = ('https://dl.winehq.org/',)


def fetch_addons(cache):
    """[(guest path, bytes)] of the pinned Wine Mono/Gecko installers, downloaded once into cache and verified."""
    lock = json.loads(ADDONS_LOCK.read_bytes()); out = []
    cache.mkdir(parents=True, exist_ok=True)
    for a in lock['addons']:
        name = PurePosixPath(urllib.parse.urlsplit(a['url']).path).name
        if not re.fullmatch(r'wine-(mono|gecko)-[0-9.]+-x86(_64)?\.msi', name) or a['subdir'] not in ('mono', 'gecko'):
            raise InstallError('bad add-on entry in %s: %r' % (ADDONS_LOCK.name, a))
        path = cache / name
        if not (path.exists() and path.stat().st_size == a['bytes'] and ai.sha256_file(path) == a['sha256']):
            data = fetch_url(a['url'], a['bytes'], ADDON_ORIGINS)
            if len(data) != a['bytes'] or sha256(data) != a['sha256']: raise InstallError('verification failed for ' + a['url'])
            part = path.with_name(name + '.part'); part.write_bytes(data); part.replace(path)
        data = path.read_bytes()
        if sha256(data) != a['sha256']: raise InstallError('cache verification failed for ' + name)
        out.append(('usr/share/wine/%s/%s' % (a['subdir'], name), data))
    return out
# Dependencies deliberately not installed (name -> reason). Wine loads these only for the named optional feature.
SKIP = {
    'libavcodec62': 'winedmo.so (Media Foundation demuxing): only in sid with FFmpeg 8, not ABI-compatible with the trixie userland',
    'libavformat62': 'winedmo.so (Media Foundation demuxing), sid FFmpeg 8 only',
    'libavutil60': 'winedmo.so (Media Foundation demuxing), sid FFmpeg 8 only',
    'libcapi20-3t64': 'capi2032.so (ISDN CAPI)',
    'libgphoto2-6t64': 'gphoto2.so (digital cameras via TWAIN)',
    'libgphoto2-port12t64': 'gphoto2.so (digital cameras via TWAIN)',
    'libgstreamer1.0-0': 'winegstreamer.so (DirectShow/Media Foundation playback)',
    'libgstreamer-plugins-base1.0-0': 'winegstreamer.so (DirectShow/Media Foundation playback)',
    'libpcap0.8t64': 'wpcap.so (packet capture)',
    'libpcsclite1': 'winscard.so (smart cards)',
    'libpulse0': 'winepulse.so (PulseAudio; NexusOS has no sound server, winealsa.drv is used)',
    'libusb-1.0-0': 'wineusb.so (raw USB devices)',
    'ocl-icd-libopencl1': 'opencl.so (OpenCL)',
    'libopencl1': 'opencl.so (OpenCL)',
    'libopencl-1.2-1': 'opencl.so (OpenCL)',
    'libglx-mesa0': 'Mesa GLX vendor library: OpenGL rendering comes from gpu_userland.py (virgl); libGL.so.1 loads without it',
    'debianutils': 'run-parts/which/tempfile helpers for maintainer scripts; /bin/sh itself does not need them',
    'man-db': 'manual pages',
    'lsb-base': 'init-script helpers',
    'xorg-sgml-doctools': 'documentation',
}
# update-alternatives emulation (package that must be selected, link, relative target)
ALTERNATIVES = [('wine', 'usr/bin/' + name, name + '-stable') for name in
                ('wine', 'msiexec', 'regedit', 'regsvr32', 'wineboot', 'winecfg', 'wineconsole', 'winedbg', 'winefile', 'winepath')]
ALTERNATIVES += [('wine64', 'usr/bin/wine64', 'wine64-stable'), ('libwine', 'usr/share/wine/collection', 'collection-stable')]
# libwine's dpkg trigger: zlib1.dll from libz-mingw-w64 marked as a Wine builtin DLL (source, destination)
ZLIB_COPIES = [('usr/i686-w64-mingw32/lib/zlib1.dll', 'usr/lib/x86_64-linux-gnu/wine/i386-windows/zlib1.dll'),
               ('usr/x86_64-w64-mingw32/lib/zlib1.dll', 'usr/lib/x86_64-linux-gnu/wine/x86_64-windows/zlib1.dll')]
WINE_UNIX = 'usr/lib/x86_64-linux-gnu/wine/x86_64-unix/'
# Libraries Wine 10.0's unix modules dlopen (SONAME_LIB* strings in this exact build, see the lock's notes)
DLOPEN = {
    'win32u.so': ['libfontconfig.so.1', 'libfreetype.so.6', 'libvulkan.so.1'],
    'winewayland.so': ['libEGL.so.1'],
    'winex11.so': ['libGL.so.1', 'libXcomposite.so.1', 'libXcursor.so.1', 'libXfixes.so.3', 'libXi.so.6', 'libXinerama.so.1',
                   'libXrandr.so.2', 'libXrender.so.1', 'libXxf86vm.so.1', 'libvulkan.so.1'],
    'bcrypt.so': ['libgnutls.so.30'], 'crypt32.so': ['libgnutls.so.30'], 'secur32.so': ['libgnutls.so.30'],
    'dwrite.so': ['libfreetype.so.6'], 'kerberos.so': ['libgssapi_krb5.so.2', 'libkrb5.so.3'],
    'localspl.so': ['libcups.so.2'], 'winspool.so': ['libcups.so.2'], 'mountmgr.so': ['libdbus-1.so.3'], 'winebth.so': ['libdbus-1.so.3'],
    'odbc32.so': ['libodbc.so.2'], 'qcap.so': ['libv4l2.so.0'], 'winebus.so': ['libSDL2-2.0.so.0'],
}
# Files whose NEEDED libraries must all be present (others are optional modules: missing libraries are only reported).
REQUIRED_ELF = {'usr/lib/x86_64-linux-gnu/wine/wine', 'usr/lib/x86_64-linux-gnu/wine/wineserver', WINE_UNIX + 'ntdll.so',
                WINE_UNIX + 'win32u.so', WINE_UNIX + 'winewayland.so', WINE_UNIX + 'winex11.so', WINE_UNIX + 'ws2_32.so',
                WINE_UNIX + 'winealsa.so', WINE_UNIX + 'opengl32.so', WINE_UNIX + 'winevulkan.so', 'usr/bin/Xwayland',
                'usr/bin/xkbcomp', 'usr/bin/dash', 'usr/bin/xterm', 'usr/bin/xeyes', 'usr/bin/xdotool', 'usr/bin/wayland-info',
                'usr/bin/basename', 'usr/bin/cut'}
SESSION = HERE / 'out' / 'x11_session_user.elf'
XTEST = HERE / 'out' / 'x11_test_user.elf'
X11_ARGS = b'/usr/bin/nexus-x11-session\n'
# Aurora launchers (desktop.c actions 45/46): winecfg, and Wine's explorer on the Start menu folder of installed programs
WINE_LAUNCHERS = {'nexus/apps/winecfg.args': b'/usr/bin/winecfg\n',
                  'nexus/apps/wine-programs.args': b'/usr/bin/wine\nexplorer\nC:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\n'}


def log(*args): print('[wine]', *args, flush=True)
def sha256(data): return hashlib.sha256(data).hexdigest()


# ------------------------------------------------------------------------------------------------
# lock: signed indices -> dependency closure
# ------------------------------------------------------------------------------------------------
DEBIAN = ('https://deb.debian.org/',)
SNAPSHOT = 'https://snapshot.debian.org/'
DEB_NAME = re.compile(r'[A-Za-z0-9+_~-][A-Za-z0-9+._~-]*\.deb')            # a .deb basename: no separator, no '..'
POOL_PATH = re.compile(r'pool/(?:[A-Za-z0-9+_~-][A-Za-z0-9+._~-]*/)+' + DEB_NAME.pattern)
# The trust anchor of `lock`: primary-key fingerprints of Debian's archive signing keys (ftp-master.debian.org/keys.html,
# the same keys debian-archive-keyring 2025.1 ships). The keyring file gpgv gets only supplies public keys: an InRelease
# counts only with a good signature whose primary key is one of these.
ARCHIVE_KEYS = {
    '1F89983E0081FDE018F3CC9673A4F27B8DD47936': 'Debian Archive Automatic Signing Key (11/bullseye)',
    'AC530D520F2F3269F5E98313A48449044AAD5C5D': 'Debian Security Archive Automatic Signing Key (11/bullseye)',
    'B8B80B5B623EAB6AD8775C45B7C5D7D6350947F8': 'Debian Archive Automatic Signing Key (12/bookworm)',
    '05AB90340C0C5E797F44A8C8254CF3B5AEC0A8F0': 'Debian Security Archive Automatic Signing Key (12/bookworm)',
    '04B54C3CDCA79751B16BC6B5225629DF75B188BD': 'Debian Archive Automatic Signing Key (13/trixie)',
    '5E04A1E3223A19A20706E20F9904613D4CCE68C6': 'Debian Security Archive Automatic Signing Key (13/trixie)',
    'A4285295FC7B1A81600062A9605C66F00D6C9793': 'Debian Stable Release Key (11/bullseye)',
    '4D64FEC119C2029067D6E791F8D2585B8783D481': 'Debian Stable Release Key (12/bookworm)',
    '41587F7DB8C774BCCF131416762F67A0B2C39DE4': 'Debian Stable Release Key (13/trixie)',
}
PACKAGES_LIMIT = 1 << 28            # decompressed Packages bound (sid amd64 is far below)


def open_url(url, origins):
    """urlopen that never leaves `origins`, redirects included."""
    class Redirects(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            target = urllib.parse.urljoin(req.full_url, newurl)
            if not target.startswith(origins): raise InstallError('redirected off Debian: ' + target)
            return super().redirect_request(req, fp, code, msg, headers, newurl)
    if not url.startswith(origins): raise InstallError('download origin not allowed: ' + url)
    response = urllib.request.build_opener(Redirects()).open(url, timeout=300)
    if not response.geturl().startswith(origins): response.close(); raise InstallError('redirected off Debian: ' + response.geturl())
    return response


def fetch_url(url, limit=1 << 31, origins=DEBIAN):
    with open_url(url, origins) as response:
        data = response.read(limit + 1)
    if len(data) > limit: raise InstallError('oversized download ' + url)
    return data


def release_time(value):
    parsed = email.utils.parsedate_tz(value or '')
    if not parsed or parsed[9] is None: raise InstallError('bad Release date %r' % value)
    return email.utils.mktime_tz(parsed)


def snapshot_url(suite, date, filename):
    """Fallback while snapshot.debian.org has not imported a file yet: half a day after the InRelease Date, so the URL
    names a snapshot run after the dinstall that published it (superseded files stay in the pool longer than that)."""
    stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime(release_time(date) + 12 * 3600))
    archive = 'debian-security' if suite == 'trixie-security' else 'debian'
    return SNAPSHOT + 'archive/%s/%s/%s' % (archive, stamp, filename)


def snapshot_lookup(name, version, filename, size):
    """URL of the .deb in the snapshot run where snapshot.debian.org first saw it (its machine-readable API), or None."""
    try:
        info = json.loads(fetch_url(SNAPSHOT + 'mr/binary/%s/%s/binfiles?fileinfo=1' % (urllib.parse.quote(name, safe=''),
                                    urllib.parse.quote(version, safe='')), 1 << 22, (SNAPSHOT,)))
        for files in info.get('fileinfo', {}).values():
            for f in files:
                if (f.get('name') == filename and f.get('size') == size and f.get('archive_name') in ('debian', 'debian-security')
                        and re.fullmatch(r'\d{8}T\d{6}Z', f.get('first_seen', '')) and POOL_PATH.fullmatch(f.get('path', '')[1:] + '/' + filename)):
                    return SNAPSHOT + 'archive/%s/%s%s/%s' % (f['archive_name'], f['first_seen'], f['path'], filename)
    except (OSError, ValueError, TypeError, AttributeError, InstallError):
        pass
    return None


def fetch_package(entry, cache):
    """Cached .deb verified against the lock's size and SHA-256: deb.debian.org, then the pinned snapshot URL, then the
    snapshot run that first had the file (looked up by package and version; sid moves on quickly)."""
    parts = PurePosixPath(entry['filename']).parts
    if len(parts) != 2 or parts[0] != 'debs' or not DEB_NAME.fullmatch(parts[1]): raise InstallError('bad file name in the lock: %r' % entry['filename'])
    debs = (cache / 'debs').resolve(); path = debs / parts[1]
    if path.resolve().parent != debs: raise InstallError('cache path escapes %s: %r' % (debs, entry['filename']))
    if path.exists() and path.stat().st_size == entry['bytes'] and ai.sha256_file(path) == entry['sha256']: return path
    debs.mkdir(parents=True, exist_ok=True)
    def urls():
        yield entry['url']; yield entry.get('snapshot')
        yield snapshot_lookup(entry['name'], entry['version'], parts[1], entry['bytes'])
    errors, tried = [], set()
    for url in urls():
        if not url or url in tried: continue
        tried.add(url)
        part = path.with_name(path.name + '.part'); digest, received = hashlib.sha256(), 0
        try:
            with open_url(url, DEBIAN + (SNAPSHOT,)) as response, open(part, 'wb') as out:
                while block := response.read(1 << 20):
                    received += len(block)
                    if received > entry['bytes']: raise InstallError('oversized download: ' + entry['name'])
                    digest.update(block); out.write(block)
        except (OSError, ValueError, InstallError) as error:
            errors.append('%s: %s' % (url, error)); part.unlink(missing_ok=True); continue
        if received != entry['bytes'] or digest.hexdigest() != entry['sha256']:
            part.unlink(); raise InstallError('verification failed for ' + entry['name'] + ' from ' + url)
        part.replace(path); return path
    raise InstallError('could not download %s: %s' % (entry['name'], '; '.join(errors)))


def gpgv():
    """gpgv at a fixed place (Git for Windows' build, or $NEXUS_GPGV), else from absolute PATH entries; never the current folder."""
    if os.environ.get('NEXUS_GPGV'): candidates = [Path(os.environ['NEXUS_GPGV'])]
    else:
        candidates = [Path(os.environ[v]) / 'Git/usr/bin/gpgv.exe' for v in ('ProgramW6432', 'ProgramFiles') if os.environ.get(v)]
        here = Path.cwd().resolve()
        for d in os.environ.get('PATH', '').split(os.pathsep):
            if d and os.path.isabs(d) and Path(d).resolve() != here: candidates += [Path(d) / 'gpgv.exe', Path(d) / 'gpgv']
    for found in candidates:
        if found.is_absolute() and found.is_file(): return str(found)
    raise InstallError('gpgv (Git for Windows) is needed to verify Debian InRelease signatures')


def native(path):
    return Path(path).resolve().as_posix()          # C:/... : MSYS (Git for Windows) and native GnuPG both accept it


def parse_packages(text):
    for block in text.split('\n\n'):
        fields, key = {}, None
        for line in block.split('\n'):
            if not line: continue
            if line[0] in ' \t':
                if key: fields[key] += '\n' + line
                continue
            key, _, value = line.partition(':'); fields[key] = value.strip()
        if 'Package' in fields: yield fields


def relations(value):
    """'a (>= 1) | b, c' -> [[(a, '>=', '1'), (b, None, None)], [(c, None, None)]] (amd64/any arch qualifiers only)."""
    groups = []
    for group in (value or '').replace('\n', ' ').split(','):
        alternatives = []
        for alternative in group.split('|'):
            match = re.match(r'^\s*([a-z0-9][a-z0-9+.\-]*)(?::([a-z0-9-]+))?\s*(?:\(\s*(<<|<=|=|>=|>>)\s*([^)\s]+)\s*\))?\s*(?:\[[^\]]*\])?\s*$', alternative)
            if not match:
                if alternative.strip(): raise InstallError('bad relation %r' % alternative)
                continue
            name, arch, op, version = match.groups()
            if arch not in (None, 'any', 'amd64', 'native'): continue
            alternatives.append((name, op, version))
        if alternatives: groups.append(alternatives)
    return groups


def _order(c):
    return -1 if c == '~' else 0 if c.isdigit() or not c else ord(c) if c.isalpha() else ord(c) + 256


def _compare_part(a, b):
    i = j = 0
    while i < len(a) or j < len(b):
        while (i < len(a) and not a[i].isdigit()) or (j < len(b) and not b[j].isdigit()):
            ac = _order(a[i]) if i < len(a) else 0; bc = _order(b[j]) if j < len(b) else 0
            if ac != bc: return ac - bc
            i += 1; j += 1
        na = nb = ''
        while i < len(a) and a[i].isdigit(): na += a[i]; i += 1
        while j < len(b) and b[j].isdigit(): nb += b[j]; j += 1
        if int(na or 0) != int(nb or 0): return int(na or 0) - int(nb or 0)
    return 0


def version_compare(a, b):
    def split(v):
        epoch, _, rest = v.partition(':') if ':' in v else ('0', '', v)
        upstream, _, revision = rest.rpartition('-') if '-' in rest else (rest, '', '')
        return int(epoch), upstream, revision
    ea, ua, ra = split(a); eb, ub, rb = split(b)
    if ea != eb: return ea - eb
    return _compare_part(ua, ub) or _compare_part(ra, rb)


def satisfies(version, op, wanted):
    if op is None: return True
    c = version_compare(version, wanted)
    return {'<<': c < 0, '<=': c <= 0, '=': c == 0, '>=': c >= 0, '>>': c > 0}[op]


def release_fields(text):
    """Fields of a Release file (first stanza of gpgv's verified output); a repeated field is an error."""
    fields, key = {}, None
    for line in text.split('\n'):
        if not line.strip():
            if fields: break
            continue
        if line[0] in ' \t':
            if key: fields[key] += '\n' + line
            continue
        key, _, value = line.partition(':')
        if key in fields: raise InstallError('Release field %s repeated' % key)
        fields[key] = value.strip()
    return fields


def verify_release(suite, raw, keyring, work, pinned_date=None, allow_older=False):
    """(fields, signers) of an InRelease, parsed only from the text gpgv verified. It must be exactly one clearsigned block
    (nothing outside it: apt's CVE-2013-1051 rule), carry a good signature by one of ARCHIVE_KEYS (gpgv's status lines,
    not its exit code alone), name this suite (Codename or Suite), not be past Valid-Until, and not be older than the
    Date the previous lock pinned unless allow_older."""
    if (not raw.startswith(b'-----BEGIN PGP SIGNED MESSAGE-----\n') or raw.count(b'-----BEGIN PGP SIGNED MESSAGE-----') != 1
            or raw.count(b'-----BEGIN PGP SIGNATURE-----') != 1 or not raw.rstrip().endswith(b'\n-----END PGP SIGNATURE-----')):
        raise InstallError(suite + ': InRelease is not exactly one clearsigned block')
    source, signed = work / (suite + '.InRelease'), work / (suite + '.signed')
    source.write_bytes(raw); signed.unlink(missing_ok=True)
    result = subprocess.run([gpgv(), '--status-fd', '1', '--keyring', native(keyring), '--output', native(signed), native(source)],
                            capture_output=True, text=True, errors='replace')
    status = [line.split()[1:] for line in result.stdout.splitlines() if line.startswith('[GNUPG:] ') and len(line.split()) > 1]
    good = {s[1]: ' '.join(s[2:]) for s in status if s[0] == 'GOODSIG' and len(s) > 2}
    signers = sorted({good[s[1][-16:]] for s in status if s[0] == 'VALIDSIG' and len(s) >= 11 and s[1][-16:] in good and s[-1] in ARCHIVE_KEYS})
    bad = [' '.join(s) for s in status if s[0] in ('BADSIG', 'FAILURE') or (s[0] == 'ERRSIG' and s[6:7] != ['9'])]
    if bad or not signers or not signed.is_file() or (result.returncode and not any(s[0] == 'NO_PUBKEY' for s in status)):
        raise InstallError('%s: InRelease signature verification failed (no good signature by a Debian archive key)\n%s%s' % (suite, result.stderr, '\n'.join(bad)))
    fields = release_fields(signed.read_bytes().decode('utf-8'))
    if suite not in (fields.get('Codename'), fields.get('Suite')):
        raise InstallError('%s: the InRelease served is for %s/%s' % (suite, fields.get('Codename'), fields.get('Suite')))
    date = release_time(fields.get('Date'))
    if fields.get('Valid-Until') and release_time(fields['Valid-Until']) < time.time():
        raise InstallError('%s: InRelease expired (Valid-Until %s)' % (suite, fields['Valid-Until']))
    if pinned_date and date < release_time(pinned_date) and not allow_older:
        raise InstallError('%s: InRelease of %s is older than the lock\'s (%s); `lock --allow-older` accepts it' % (suite, fields['Date'], pinned_date))
    return fields, signers


def keyring_file(entry, work):
    """debian-archive-keyring.pgp out of the keyring .deb the lock pins (size + SHA-256)."""
    deb = fetch_package(dict(entry, filename='debs/' + PurePosixPath(entry['url']).name), work)
    with ai.open_payload({'kind': 'deb'}, deb) as archive:
        for member in archive:
            if ai.safe_member(member.name) == 'usr/share/keyrings/debian-archive-keyring.pgp':
                keyring = work / ('keyring-%s.pgp' % entry['sha256'][:16]); keyring.write_bytes(archive.extractfile(member).read())
                return keyring
    raise InstallError('keyring file missing from debian-archive-keyring ' + entry['version'])


def unxz(data, limit=PACKAGES_LIMIT):
    d = lzma.LZMADecompressor(lzma.FORMAT_XZ)
    out = d.decompress(data, max_length=limit + 1)
    if len(out) > limit or not d.eof or d.unused_data: raise InstallError('Packages.xz is malformed or expands beyond %d MiB' % (limit >> 20))
    return out


def verified_indices(keyring_entry, pinned=None, allow_older=False, update_keyring=False):
    """InRelease + Packages.xz of every suite; returns ({suite: {name: [stanza]}}, meta, keyring entry). Each InRelease is
    verified (verify_release) with the keyring the previous lock pins before anything in it is used; Packages.xz must
    then match the one SHA-256 entry of the signed text and is decompressed with a size bound. The keyring entry changes
    only with update_keyring: to the newest debian-archive-keyring of the verified trixie index, which must verify too."""
    work = Path(tempfile.mkdtemp(prefix='wine-lock-'))
    try:
        keyring = keyring_file(keyring_entry, work)
        packages, meta, raws = {}, {}, {}
        for suite in MIRRORS:
            raws[suite] = raw = fetch_url(MIRRORS[suite] + f'dists/{suite}/InRelease', 1 << 24)
            fields, signers = verify_release(suite, raw, keyring, work, (pinned or {}).get(suite, {}).get('date'), allow_older)
            listing = [line.split() for line in fields.get('SHA256', '').split('\n')]
            listing = [x[:2] for x in listing if len(x) == 3 and x[2] == 'main/binary-amd64/Packages.xz']
            if len(listing) != 1: raise InstallError(suite + ': InRelease does not list main/binary-amd64/Packages.xz exactly once')
            index = fetch_url(MIRRORS[suite] + f'dists/{suite}/main/binary-amd64/Packages.xz', 1 << 27)
            if [sha256(index), str(len(index))] != listing[0]: raise InstallError(suite + ': Packages.xz does not match InRelease')
            meta[suite] = {'inrelease_sha256': sha256(raw), 'packages_xz_sha256': sha256(index), 'date': fields['Date'], 'signed_by': signers}
            table = {}
            for stanza in parse_packages(unxz(index).decode('utf-8')):
                if stanza.get('Architecture') in ('amd64', 'all'): table.setdefault(stanza['Package'], []).append(stanza)
            packages[suite] = table
        newest = max(packages['trixie'].get('debian-archive-keyring', []), default=None,
                     key=functools.cmp_to_key(lambda a, b: version_compare(a['Version'], b['Version'])))
        if newest and newest['SHA256'] != keyring_entry['sha256']:
            if not update_keyring:
                log('note: trixie has debian-archive-keyring %s; the lock keeps %s (`lock --update-keyring` adopts it)' % (newest['Version'], keyring_entry['version']))
            else:
                if not POOL_PATH.fullmatch(newest['Filename']): raise InstallError('bad Filename %r in the trixie index' % newest['Filename'])
                keyring_entry = {'name': 'debian-archive-keyring', 'version': newest['Version'], 'sha256': newest['SHA256'],
                                 'url': MIRRORS['trixie'] + newest['Filename'], 'bytes': int(newest['Size'])}
                keyring = keyring_file(keyring_entry, work)
                for suite, raw in raws.items(): verify_release(suite, raw, keyring, work)
                log('debian-archive-keyring is now', newest['Version'])
        return packages, meta, keyring_entry
    finally:
        shutil.rmtree(work, ignore_errors=True)


def resolve(packages, present):
    """Dependency closure of every group: (selected, skipped, satisfied). Every group gets its own full closure (needed_by
    lists all groups); chrome-lock packages are on the disk, SKIP ones are left out. A versioned relation that no
    alternative meets constrains every choice of its target while the package that asked is selected at the version that
    asked, or is not chosen yet (only such constraints give way when no candidate meets them all). A pass that meets a
    package chosen at a version a relation rules out, or no candidate, is repeated with the constraints of the packages it
    selected, so a package that a later pass replaces or leaves out constrains nothing; when those constraints were tried
    before, the pass would only repeat itself and its conflicts are the error. Finally every relation of every package is
    checked in each group."""
    wants, tried, selected, conflicts = {}, [], {}, {}  # wants: name -> {(op, version, package that asked, its version): None}
    def rules(name, live_only=False):                   # the constraints on name in force (see above)
        return [r for r in wants.get(name, ()) if (selected[r[2]]['version'] == r[3] if r[2] in selected else not live_only)]
    def newest(name, choices, constraints):             # newest candidate meeting them of the first suite list that has one
        for suites in choices:
            top = None
            for suite in suites:
                for s in packages[suite].get(name, []):
                    if all(satisfies(s['Version'], op, v) for op, v, _, _ in constraints) and \
                            (top is None or version_compare(s['Version'], top[1]['Version']) > 0): top = (suite, s)
            if top: return top
        return None
    def best(name, choices):
        return newest(name, choices, rules(name)) or newest(name, choices, rules(name, True))
    def missing(name, choices):
        return 'no version meets every constraint' if newest(name, choices, []) else 'not in the selected suites'
    def unmet(name, what):                              # a conflict of this pass, naming every constraint on name in force
        conflicts[what + ''.join(f'; {by} {at} needs {name} ({op} {v})' for op, v, by, at in rules(name, True))] = None
    for _ in range(100):
        tried.append({(name, r) for name, rs in wants.items() for r in rs})
        selected, skipped, satisfied, stanzas, conflicts = {}, {}, {}, {}, {}
        for group, roots in GROUPS.items():
            queue, visited = [(name, suite, None) for suite, names in roots.items() for name in names], set()
            while queue:
                name, suite, parent = queue.pop(0)
                if name in visited: continue
                visited.add(name)
                if name in present: satisfied.setdefault(name, present[name]); continue
                if name in SKIP:
                    entry = skipped.setdefault(name, {'reason': SKIP[name], 'required_by': []})
                    if parent not in entry['required_by']: entry['required_by'].append(parent)
                    continue
                if name not in selected:
                    choices = [['sid'] if suite == 'sid' else TRIXIE] + ([['sid']] if parent or rules(name) else [])
                    found = best(name, choices)
                    if not found: unmet(name, f'{name} (needed by {parent}): ' + missing(name, choices)); continue
                    where, stanza = found
                    if not POOL_PATH.fullmatch(stanza['Filename']): raise InstallError(f'{name}: bad Filename {stanza["Filename"]!r} in the {where} index')
                    stanzas[name] = stanza
                    selected[name] = {'name': name, 'version': stanza['Version'], 'url': MIRRORS[where] + stanza['Filename'],
                                      'sha256': stanza['SHA256'], 'bytes': int(stanza['Size']), 'kind': 'deb', 'app': group,
                                      'suite': where, 'needed_by': [], 'filename': 'debs/' + PurePosixPath(stanza['Filename']).name}
                selected[name]['needed_by'] = sorted(set(selected[name]['needed_by']) | {group})
                for relation in relations(stanzas[name].get('Pre-Depends')) + relations(stanzas[name].get('Depends')):
                    choice = None
                    for alt, op, wanted in relation:      # the disk (chrome-lock) or an already selected package satisfies it?
                        have = present.get(alt) or (selected[alt]['version'] if alt in selected else None)
                        if have and satisfies(have, op, wanted): choice = (alt, None); break
                    if choice is None:
                        alt, op, wanted = next(((a, o, w) for a, o, w in relation if a not in SKIP), relation[0])
                        pick = None
                        if alt not in SKIP:
                            version = selected[name]['version']; asked = f'{name} {version} needs {alt}' + (f' ({op} {wanted})' if op else '')
                            if op: wants.setdefault(alt, {})[(op, wanted, name, version)] = None
                            if alt in present or alt in selected:   # chosen at a version this rules out: the next pass knows
                                unmet(alt, asked + (', the desktop disk has ' + present[alt] if alt in present else
                                                    ', the pass chose ' + selected[alt]['version'])); continue
                            pick = best(alt, [TRIXIE, ['sid']])
                            if not pick: unmet(alt, asked + ': ' + missing(alt, [TRIXIE, ['sid']])); continue
                        choice = (alt, pick[0] if pick else None)
                    queue.append((choice[0], choice[1], name))
        if not conflicts: break
        wants = {name: {r: None for r in rs if r[2] in selected and selected[r[2]]['version'] == r[3]} for name, rs in wants.items()}
        if {(name, r) for name, rs in wants.items() for r in rs} in tried:
            raise InstallError('dependencies cannot be met:\n  ' + '\n  '.join(conflicts))
    else:
        raise InstallError('dependency resolution does not settle:\n  ' + '\n  '.join(conflicts))
    for name, entry in selected.items():
        for relation in relations(stanzas[name].get('Pre-Depends')) + relations(stanzas[name].get('Depends')):
            for group in entry['needed_by']:
                if not any(alt in SKIP or (alt in present and satisfies(present[alt], op, wanted)) or
                           (alt in selected and group in selected[alt]['needed_by'] and satisfies(selected[alt]['version'], op, wanted))
                           for alt, op, wanted in relation):
                    raise InstallError('%s %s (group %s): %s is not met by the lock' % (name, entry['version'], group, ' | '.join(
                        alt + (' (%s %s)' % (op, wanted) if op else '') for alt, op, wanted in relation)))
    return selected, skipped, satisfied


def make_lock(update_keyring=False, allow_older=False):
    previous = json.loads(LOCK.read_bytes()) if LOCK.exists() else {}
    if not previous.get('keyring'): raise InstallError('%s pins no debian-archive-keyring (restore it from version control)' % LOCK)
    packages, meta, keyring = verified_indices(previous['keyring'], previous.get('suites'), allow_older, update_keyring)
    present = {p['name']: p['version'] for p in json.loads((HERE / 'chrome-lock.json').read_text())}
    selected, skipped, satisfied = resolve(packages, present)
    for p in selected.values():   # snapshot.debian.org keeps every .deb after it left the live archive (sid moves on quickly)
        p['snapshot'] = snapshot_lookup(p['name'], p['version'], PurePosixPath(p['filename']).name, p['bytes'])
        if not p['snapshot']:
            log('  snapshot.debian.org has no %s %s yet: pinned the run half a day after the InRelease Date' % (p['name'], p['version']))
            p['snapshot'] = snapshot_url(p['suite'], meta[p['suite']]['date'], p['url'][len(MIRRORS[p['suite']]):])
    lock = {'format': VERSION, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'suites': meta, 'keyring': keyring,
            'present_from_chrome_lock': dict(sorted(satisfied.items())), 'skipped': dict(sorted(skipped.items())),
            'packages': sorted(selected.values(), key=lambda p: p['name'])}
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    log('wrote', LOCK, len(selected), 'packages,', len(skipped), 'skipped')
    for p in lock['packages']: log('  %-34s %-28s %-15s %s' % (p['name'], p['version'], p['suite'], ','.join(p['needed_by'])))


# ------------------------------------------------------------------------------------------------
# install
# ------------------------------------------------------------------------------------------------
class WineInstaller(ai.Installer):
    def __init__(self, vol, artifacts, paths, groups):
        super().__init__(vol, artifacts, paths, groups)
        self.extras = []

    def existing(self, path, kind, art, bucket, mtime):
        # C headers are not needed at run time (xbitmaps' X11/bitmaps also holds names that differ only in case).
        if path == 'usr/include' or path.startswith('usr/include/'):
            self.skip('development headers', '/' + path, bucket); return None
        return super().existing(path, kind, art, bucket, mtime)

    def put_bytes(self, path, data, bucket, now):
        r = self.existing(path, 'file', {'name': 'wine_install', 'app': bucket}, bucket, now)
        if r is None: return
        d, leaf, ent = r
        if ent is not None:
            if not ent.is_dir and not ent.is_link and ent.size == len(data) and ent.first >= 2 and b''.join(self.vol.read_file(ent.first, ent.size)) == data:
                self.stat(bucket)['files_identical'] += 1; return
            self.conflict(path, 'existing file differs (kept)', {'name': 'wine_install'}, bucket); return
        self.vol.write_file(d, leaf, len(data), [data], now); self.changes += 1; self.created.append('/' + path)

    def file_bytes(self, path):
        ent = self.resolve('/' + path)
        if ent is None or ent.is_dir: return None
        if ent.first < 0: return None                      # analysis mode: content not written yet
        return b''.join(self.vol.read_file(ent.first, ent.size)) if ent.size else b''

    def finish(self, versions):
        now = time.time(); names = set(self.by_name)
        for package, link, target in ALTERNATIVES:                      # update-alternatives emulation
            if package in names:
                self.managed_link(link, target, self.by_name[package]['app'], now)
                if link.startswith('usr/bin/'): self.usr_names.setdefault(('bin', link[8:]), self.by_name[package]['app'])
        if 'libwine' in names and 'libz-mingw-w64' in names:          # libwine trigger: builtin zlib1.dll copies
            for source, destination in ZLIB_COPIES:
                data = self.file_bytes(source)
                if data is None:
                    if self.vol.writable: raise InstallError('missing ' + source)
                    data = bytes(self.resolve('/' + source).size)   # analysis: same size placeholder
                marked = data[:64] + b'Wine builtin DLL' + bytes(16) + data[96:]
                self.put_bytes(destination, marked, 'wine', now)
        for (top, n), app in sorted(self.usr_names.items()):           # merged-usr emulation
            target = '/usr/%s/%s' % (top, n)
            d = self.walk(top, False)
            ent = d.names.get(fold(n)) if d else None
            if ent is not None and not (ent.is_link and ent.name == n and self.read_link(ent) == target):
                self.skip('merged-usr link: existing entry kept', '/%s/%s' % (top, n)); continue
            self.managed_link('%s/%s' % (top, n), target, app, now)
        if 'wine' in self.groups:
            for guest, data in WINE_LAUNCHERS.items(): self.put_bytes(guest, data, 'wine', now)
        if 'addons' in self.groups:    # Wine Mono / Gecko installers where wineboot looks (no download prompt)
            for guest, data in getattr(self, 'addon_files', ()): self.put_bytes(guest, data, 'addons', now)
        if 'xwayland' in self.groups:
            self.walk('var/lib/xkb', True, now, 'xwayland')
            self.put_built(SESSION, 'usr/bin/nexus-x11-session', 'xwayland', now)
            self.put_bytes('nexus/apps/x11.args', X11_ARGS, 'xwayland', now)
        if self.groups & {'xwayland', 'x11-tools'}:    # dependency-free X client: the session's default when xterm/xeyes are absent
            self.put_built(XTEST, 'usr/bin/nexus-xtest', 'xwayland' if 'xwayland' in self.groups else 'x11-tools', now)
        for pkg, a in self.by_name.items():
            if pkg not in self.licenses: self.licenses[pkg] = 'none'
        self.write_manifest(versions, now)

    def put_built(self, source, path, bucket, now):
        if source.exists(): self.put_bytes(path, source.read_bytes(), bucket, now)
        else: self.fatal.append('missing %s: build it with python -I arch/x86_64/desktop.py' % source.name)

    def write_manifest(self, versions, now):
        """/nexus/wine-installed.json describes everything this installer put on the disk: this run merged into it."""
        d = self.walk('nexus', True, now, 'wine')
        ent = d.names.get(fold('wine-installed.json'))
        old = {}
        if ent is not None:
            if ent.is_dir or ent.is_link or ent.is_special: raise InstallError('/nexus/wine-installed.json is not a regular file')
            if ent.first >= 2 and ent.size:
                try: old = json.loads(b''.join(self.vol.read_file(ent.first, ent.size)))
                except ValueError: old = {}
        def part(key, kind): return old.get(key) if isinstance(old, dict) and isinstance(old.get(key), kind) else kind()
        def names(key): return {x for x in part(key, list) if isinstance(x, str)}
        packages = {p['name']: p for p in part('packages', list) if isinstance(p, dict) and isinstance(p.get('name'), str)}
        for a in self.artifacts: packages[a['name']] = {'name': a['name'], 'version': a['version'], 'suite': a['suite'], 'group': a['app']}
        new_lib, old_lib = getattr(self, 'library_report', {}), part('libraries', dict)
        checked = set(new_lib.get('checked', ()))             # files this run examined replace their old entries
        libraries = {k: dict({f: v for f, v in (old_lib.get(k) if isinstance(old_lib.get(k), dict) else {}).items() if f not in checked},
                             **new_lib.get(k, {})) for k in ('needed_missing_required', 'needed_missing_optional', 'dlopen')}
        manifest = {'format': VERSION, 'installer': 'arch/x86_64/wine_install.py', 'lock_sha256': self.lock_sha256,
                    'groups': sorted(names('groups') | set(self.groups)), 'versions': dict(part('versions', dict), **versions),
                    'packages': [packages[k] for k in sorted(packages)],
                    'symlinks_created': sorted(names('symlinks_created') | set(self.managed_links)), 'libraries': libraries,
                    'post_install_not_run': ['fc-cache (fontconfig works without its cache)', 'update-desktop-database', 'gtk-update-icon-cache']}
        self.manifest = manifest
        data = (json.dumps(manifest, indent=1) + '\n').encode()
        if ent is None: self.vol.write_file(d, 'wine-installed.json', len(data), [data], now); self.changes += 1; return
        if ent.first >= 2 and b''.join(self.vol.read_file(ent.first, ent.size)) == data: return
        if self.vol.writable and ent.first >= 2:          # replace the installer-owned manifest
            old_chain = self.vol.chain(ent.first)
            for c in old_chain: self.vol.fat_set(c, 0)
            self.vol.free += len(old_chain)
        clusters = self.vol.alloc((len(data) + self.vol.cl - 1) // self.vol.cl)
        if self.vol.writable:
            off = 0
            for c, k in self.vol.runs(clusters):
                self.vol.wr(self.vol.cl_off(c), data[off:off + k * self.vol.cl].ljust(k * self.vol.cl, b'\0')); off += k * self.vol.cl
        self.vol.set_entry_data(d, ent, clusters[0] if clusters else 0, len(data), now); self.changes += 1


def library_report(inst, artifacts, paths):
    """NEEDED of every ELF this install adds (required ones must resolve) and the dlopen'd libraries Wine may use."""
    readelf = ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'
    available = set()
    for top in ('usr/lib/x86_64-linux-gnu', 'lib/x86_64-linux-gnu', 'usr/lib', 'lib', 'lib64'):
        d = inst.walk(top, False)
        if d: available |= {e.name for e in d.names.values()}
    needed = {}
    with tempfile.TemporaryDirectory(prefix='wine-elf-') as work:
        target = Path(work) / 'elf'
        for art in artifacts:
            with ai.open_payload(art, paths[art['name']]) as tar:
                for m in tar:   # PE DLLs (*-windows/) and data files are skipped; the stream is read once
                    name = ai.safe_member(m.name)
                    if m.issym(): available.add(PurePosixPath(name).name); continue
                    if not m.isreg() or m.size < 64 or m.size > (64 << 20) or name.startswith('usr/share/') or '-windows/' in name: continue
                    data = tar.extractfile(m).read()
                    if data[:4] != b'\x7fELF': continue
                    target.write_bytes(data)
                    text = subprocess.check_output([str(readelf), '-d', str(target)], text=True, errors='replace')
                    needed[name] = re.findall(r'Shared library: \[(.*?)\]', text)
                    available.add(PurePosixPath(name).name)
    missing_required, missing_optional = {}, {}
    for name, libs in sorted(needed.items()):
        lack = [lib for lib in libs if lib not in available]
        if lack: (missing_required if name in REQUIRED_ELF else missing_optional)[name] = lack
    dlopen = {module: {lib: lib in available for lib in libs} for module, libs in DLOPEN.items() if WINE_UNIX + module in needed}
    return {'needed_missing_required': missing_required, 'needed_missing_optional': missing_optional, 'dlopen': dlopen,
            'checked': sorted(needed)}


def main(argv=None):
    if argv is None: argv = sys.argv[1:]
    if argv[:1] == ['lock']:
        lp = argparse.ArgumentParser(prog='wine_install.py lock', description='re-resolve wine-lock.json from signed Debian indices')
        lp.add_argument('--update-keyring', action='store_true', help='adopt the newest debian-archive-keyring of the verified trixie index')
        lp.add_argument('--allow-older', action='store_true', help='accept an InRelease older than the one the lock pins')
        a = lp.parse_args(argv[1:]); make_lock(a.update_keyring, a.allow_older); return 0
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('disk'); ap.add_argument('--grow', type=int, metavar='MIB')
    ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--only', default=DEFAULT_GROUPS)
    ap.add_argument('--report', default=None)
    ap.add_argument('--addons-cache', default=None, help='where the Wine Mono/Gecko downloads are kept (default: <cache>/addons)')
    args = ap.parse_args(argv)
    t0 = time.time()
    disk = Path(args.disk).resolve()
    if not disk.is_file(): raise SystemExit('no such disk image: %s' % disk)
    cache = Path(args.cache).resolve(); cache.mkdir(parents=True, exist_ok=True)
    report_path = Path(args.report).resolve() if args.report else cache / 'wine-install-report.json'
    groups = {x.strip() for x in args.only.split(',') if x.strip()}
    if not groups or groups - set(GROUPS): raise SystemExit('--only takes a comma list of ' + ','.join(GROUPS))
    unbuilt = sorted({p.name for g, p in (('xwayland', SESSION), ('xwayland', XTEST), ('x11-tools', XTEST)) if g in groups and not p.exists()})
    if unbuilt: raise SystemExit('build %s first (python -I arch/x86_64/desktop.py)' % ', '.join('out/' + n for n in unbuilt))
    lock_bytes = LOCK.read_bytes(); lock = json.loads(lock_bytes)
    artifacts = [dict(p) for p in lock['packages'] if set(p['needed_by']) & groups]
    for a in artifacts: a['app'] = sorted(set(a['needed_by']) & groups)[0]
    log('selected %d packages for %s' % (len(artifacts), ','.join(sorted(groups))))
    paths = {}
    for a in artifacts: paths[a['name']] = fetch_package(a, cache)
    log('packages verified (%.0f MiB) in %.1fs' % (sum(a['bytes'] for a in artifacts) / 2**20, time.time() - t0))
    addon_files = fetch_addons(Path(args.addons_cache).resolve() if args.addons_cache else cache / 'addons') if 'addons' in groups else []
    if addon_files: log('Wine Mono/Gecko installers verified: ' + ', '.join(PurePosixPath(p).name for p, _ in addon_files))
    versions = {g: sorted('%s %s' % (a['name'], a['version']) for a in artifacts if g in a['needed_by'] and a['name'] in
                         ('wine', 'libwine', 'xwayland', 'xterm', 'x11-apps', 'xdotool', 'wayland-utils')) for g in sorted(groups)}
    report = {'disk': str(disk), 'groups': sorted(groups), 'lock_sha256': sha256(lock_bytes)}
    # 1. read-only analysis
    st0 = disk.stat(); head0 = ai.prefix_digest(disk, 0, ai.BASE)
    vol = ai.Volume(disk, writable=False)
    try:
        ana = WineInstaller(vol, artifacts, paths, groups); ana.lock_sha256 = report['lock_sha256']; ana.addon_files = addon_files
        for art in artifacts: ana.install_artifact(art)
        ana.library_report = library_report(ana, artifacts, paths)
        ana.finish(versions)
        needed, free_clusters, cl, spc, ncl = vol.allocated, vol.free_start, vol.cl, vol.spc, vol.nclusters
    finally:
        vol.close()
    libraries = ana.library_report
    log('analysis: %d changes, %.1f MiB to allocate, %.1f MiB free, %d conflicts, %d fatal' % (
        ana.changes, needed * cl / 2**20, free_clusters * cl / 2**20, len(ana.conflicts), len(ana.fatal)))
    if libraries['needed_missing_required']:
        ana.fatal.append('required libraries missing: %r' % libraries['needed_missing_required'])
    if ana.fatal:
        report.update(result='aborted', fatal=ana.fatal, **ai.details(ana)); ai.write_report(report_path, report)
        for x in ana.fatal[:40]: log('FATAL', x)
        raise SystemExit('installation aborted before writing (see %s)' % report_path)
    do_grow, grow_clusters = False, 0
    if args.grow:
        new_size = args.grow * 1024 * 1024
        if new_size < st0.st_size: raise SystemExit('--grow is smaller than the disk')
        do_grow = new_size > st0.st_size
        if do_grow: grow_clusters = ai.format_geometry((new_size - ai.BASE) // ai.SECTOR, spc)[3] - ncl
    if needed > free_clusters + grow_clusters:
        report.update(result='aborted: not enough space', needed_bytes=needed * cl, free_bytes=(free_clusters + grow_clusters) * cl)
        ai.write_report(report_path, report)
        raise SystemExit('not enough space: need %.1f MiB, have %.1f MiB%s' % (needed * cl / 2**20, (free_clusters + grow_clusters) * cl / 2**20,
                                                                             '' if do_grow else ' (use --grow MIB)'))
    if ana.changes == 0 and not do_grow:
        report.update(result='no changes', manifest=ana.manifest, **ai.details(ana)); ai.write_report(report_path, report)
        log('nothing to do; image untouched'); return 0
    # 2. sparse working copy (grown when asked), apply, verify, replace
    temp = disk.with_name('.wine-install-%d.img' % os.getpid())
    try:
        if do_grow: ai.grow(disk, temp, args.grow)
        else: ai.sparse_copy(disk, temp)
        vol = ai.Volume(temp, writable=True)
        try:
            inst = WineInstaller(vol, artifacts, paths, groups); inst.lock_sha256 = report['lock_sha256']; inst.addon_files = addon_files
            for art in artifacts: inst.install_artifact(art)
            inst.library_report = libraries
            inst.finish(versions)
            if inst.fatal: raise InstallError('fatal problems while writing: %r' % inst.fatal[:5])
            vol.flush()
            entries = array.array('I'); entries.frombytes(bytes(vol.fat[:(vol.nclusters + 2) * 4]))
            if sum(1 for x in entries[2:] if not x & 0x0FFFFFFF) != vol.free: raise InstallError('FAT free count mismatch after install')
            entry_points = {'wine': ['/usr/bin/wine', '/usr/bin/wine64', '/usr/bin/wineboot', '/bin/sh', '/usr/bin/basename', '/usr/bin/cut', '/usr/lib/locale/C.utf8/LC_CTYPE', '/usr/lib/x86_64-linux-gnu/wine/wine',
                                     '/usr/lib/x86_64-linux-gnu/wine/i386-windows/zlib1.dll', '/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/zlib1.dll',
                                     '/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winewayland.so'],
                            'xwayland': ['/usr/bin/Xwayland', '/usr/bin/xkbcomp', '/bin/sh', '/usr/bin/nexus-x11-session', '/usr/bin/nexus-xtest'],
                            'x11-tools': ['/usr/bin/xterm', '/usr/bin/xeyes', '/usr/bin/xdotool', '/usr/bin/nexus-xtest'],
                            'wayland-tools': ['/usr/bin/wayland-info'],
                            'addons': ['/' + p for p, _ in addon_files]}
            links = ai.check_links(inst, sorted({p for g in groups for p in entry_points[g]}))
            broken = [p for p, state in links.items() if state == 'missing or dangling']
            if broken: raise InstallError('entry points do not resolve after the install: ' + ', '.join(broken))
            inst.dangling = ai.dangling_links(inst)
        finally:
            vol.close()
        skip = (446 + 12, 446 + 16) if do_grow else None
        if ai.prefix_digest(temp, ai.NPFS, ai.BASE) != ai.prefix_digest(disk, ai.NPFS, ai.BASE): raise InstallError('NPFS region changed')
        if ai.prefix_digest(temp, 0, ai.BASE, skip) != ai.prefix_digest(disk, 0, ai.BASE, skip): raise InstallError('reserved prefix changed')
        st1 = disk.stat()
        if (st1.st_size, st1.st_mtime_ns) != (st0.st_size, st0.st_mtime_ns) or ai.prefix_digest(disk, 0, ai.BASE) != head0:
            raise InstallError('disk changed during installation; stop QEMU and retry')
        os.replace(temp, disk)
    finally:
        if temp.exists(): temp.unlink()
    report.update(result='installed', changes=inst.changes, seconds=round(time.time() - t0, 1), link_checks=links,
                  manifest=inst.manifest, **ai.details(inst))
    ai.write_report(report_path, report)
    tot = report['totals']
    log('installed: %d files (%.1f MiB), %d identical, %d symlinks, %d conflicts; report %s' % (
        tot['files_added'], tot['bytes_added'] / 2**20, tot['files_identical'], tot['symlinks_added'], len(inst.conflicts), report_path))
    for name, state in links.items(): log('  %s: %s' % (name, state))
    missing = sorted({lib for m in libraries['dlopen'].values() for lib, ok in m.items() if not ok})
    if missing: log('  libraries Wine may dlopen that are not on the disk:', ', '.join(missing))
    return 0


if __name__ == '__main__':
    try: sys.exit(main())
    except InstallError as e: raise SystemExit('wine_install: ' + str(e))

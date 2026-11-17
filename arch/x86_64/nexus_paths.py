#!/usr/bin/env python3
"""Where NexusOS host scripts and tests keep their data: ONE scheme for the whole tree.

Every location is a setting with a default inside this tree's arch/x86_64/out/ (build output, never committed), so a
fresh checkout needs no setup and nothing points at someone's workspace. A setting comes from, in this order:
  1. the environment variable of that name,
  2. this machine's settings file: NEXUS_PATHS_FILE, default ~/.nexusos/paths.json (%USERPROFILE%\\.nexusos\\paths.json),
     a JSON object {"NEXUS_...": "path"} kept by `python -I arch/x86_64/nexus_paths.py set|unset|show`,
  3. the default below.

  NEXUS_DATA          root of the heavy data below                         out/data
  NEXUS_RUNS          work folders of tests run without one ($RUNS/<name>)  $NEXUS_DATA/runs
  NEXUS_CACHE         download caches, one folder per kind ($CACHE/<kind>)  $NEXUS_DATA/cache
  NEXUS_<KIND>_CACHE  one kind's cache (kind upper-case, '-' -> '_'): NEXUS_BASE_CACHE (Debian base, debian_base.py),
                      NEXUS_DESK_CACHE, NEXUS_APPS_CACHE (apps_install.py), NEXUS_WINE_CACHE, NEXUS_WAYLAND_CACHE
                      (wine_install.py / Xwayland), NEXUS_WINE_APPS_CACHE, NEXUS_GPU_CACHE, NEXUS_APT_PROXY_CACHE,
                      NEXUS_EXT4_DEBS_CACHE; older names still honoured: NEXUS_NET_PKGS, NEXUS_STORAGE_PKGS,
                      NEXUS_MEM_PKGS, NEXUS_POWER_PKGS, NEXUS_HW_PKGS, NEXUS_DEB_CACHE
  NEXUS_TEST_SOURCE   a FAT32 desktop disk tests COPY from (only read)      $NEXUS_DATA/disks/desktop64-source.img
  NEXUS_TEST_DISK     a writable desktop-disk COPY Aurora tests stage into  $NEXUS_DATA/disks/test/desktop64.img
  NEXUS_CHROME_DEB    google-chrome-stable_current_amd64.deb (optional)    $NEXUS_CACHE/chrome/<that name>
  NEXUS_WINE_SOURCE   wine-10.0.tar.xz (optional, winewayland_build.py)     $NEXUS_CACHE/wine-src/wine-10.0.tar.xz
  NEXUS_GPU_HARNESS   the Chrome GPU perf harness folder (test_x64_gpu_chrome.py)  $NEXUS_DATA/gpu-harness

The launcher's installed disk, arch/x86_64/out/desktop64.img, holds the user's own data (browser profile, logins):
no default is that file, and source_disk()/test_disk()/refuse_user_disk() refuse it - and every
<tree>/arch/x86_64/out/desktop64.img - even when a setting or a command-line argument names it.
"""
from pathlib import Path
import json
import os
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
LEGACY_ENV = {'net': 'NEXUS_NET_PKGS', 'storage': 'NEXUS_STORAGE_PKGS', 'swap-tools': 'NEXUS_MEM_PKGS', 'power': 'NEXUS_POWER_PKGS',
              'alsa-utils': 'NEXUS_HW_PKGS', 'ext4-debs': 'NEXUS_DEB_CACHE'}


def settings_file():
    value = os.environ.get('NEXUS_PATHS_FILE')
    return Path(value) if value else Path.home() / '.nexusos' / 'paths.json'


def _file_settings():
    try: data = json.loads(settings_file().read_text(encoding='utf-8'))
    except (OSError, ValueError): return {}
    return {k: v for k, v in data.items() if isinstance(k, str) and isinstance(v, str) and k.startswith('NEXUS_')} if isinstance(data, dict) else {}


def setting(name, default=None):
    """The value of one setting (environment, then this machine's settings file), else default."""
    value = os.environ.get(name)
    if value: return value
    return _file_settings().get(name, default)


def data():
    return Path(setting('NEXUS_DATA') or OUT / 'data')


def runs(name):
    """Default work folder of a test run without one: $NEXUS_RUNS/<name>."""
    return Path(setting('NEXUS_RUNS') or data() / 'runs') / name


def cache(kind, env=None):
    """Download cache of one kind (folder; created by the script that fills it)."""
    for name in (env, 'NEXUS_%s_CACHE' % kind.upper().replace('-', '_'), LEGACY_ENV.get(kind)):
        value = setting(name) if name else None
        if value: return Path(value)
    return Path(setting('NEXUS_CACHE') or data() / 'cache') / kind


def file(name, default):
    """A single-file or folder setting (NEXUS_CHROME_DEB, NEXUS_WINE_SOURCE, NEXUS_GPU_HARNESS, ...)."""
    return Path(setting(name) or default)


def is_user_disk(path):
    """True for the launcher's installed desktop disk of any NexusOS tree (<tree>/arch/x86_64/out/desktop64.img)."""
    try: p = Path(path).resolve()
    except (OSError, ValueError): p = Path(path).absolute()
    parts = [x.lower() for x in p.parts]
    return parts[-4:] == ['arch', 'x86_64', 'out', 'desktop64.img']


def refuse_user_disk(path, what='disk'):
    if path is not None and is_user_disk(path):
        raise SystemExit('refusing %s %s: that is the launcher\'s installed desktop disk with the user\'s data; '
                         'tests work on copies (NEXUS_TEST_SOURCE / NEXUS_TEST_DISK, see arch/x86_64/nexus_paths.py)' % (what, path))
    return path


def source_disk():
    """A FAT32 desktop disk that tests and installers only read (and copy)."""
    return refuse_user_disk(Path(setting('NEXUS_TEST_SOURCE') or data() / 'disks' / 'desktop64-source.img'), 'NEXUS_TEST_SOURCE')


def test_disk():
    """A writable copy of a desktop disk that Aurora tests stage the tree's build into (never the user's disk)."""
    return refuse_user_disk(Path(setting('NEXUS_TEST_DISK') or data() / 'disks' / 'test' / 'desktop64.img'), 'NEXUS_TEST_DISK')


def unthrottle(pid):
    """Opt a QEMU process out of Windows 11 power throttling (build.py unthrottle); False when not possible."""
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location('nexus_paths_build', HERE / 'build.py')
        build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
        return build.unthrottle(pid)
    except Exception:
        return False


KNOWN = ('NEXUS_DATA', 'NEXUS_RUNS', 'NEXUS_CACHE', 'NEXUS_TEST_SOURCE', 'NEXUS_TEST_DISK', 'NEXUS_BASE_CACHE', 'NEXUS_DESK_CACHE',
         'NEXUS_APPS_CACHE', 'NEXUS_WINE_CACHE', 'NEXUS_WAYLAND_CACHE', 'NEXUS_WINE_APPS_CACHE', 'NEXUS_GPU_CACHE',
         'NEXUS_APT_PROXY_CACHE', 'NEXUS_EXT4_DEBS_CACHE', 'NEXUS_NET_PKGS', 'NEXUS_STORAGE_PKGS', 'NEXUS_MEM_PKGS',
         'NEXUS_POWER_PKGS', 'NEXUS_HW_PKGS', 'NEXUS_CHROME_DEB', 'NEXUS_WINE_SOURCE', 'NEXUS_GPU_HARNESS',
         'NEXUS_WINE_ADDONS_CACHE', 'NEXUS_WINE_DOTNET_CACHE')


def main(argv):
    if not argv or argv[0] == 'show':
        stored = _file_settings()
        print('settings file:', settings_file(), '(present)' if settings_file().is_file() else '(none)')
        for name in KNOWN:
            where = 'env' if os.environ.get(name) else 'file' if name in stored else 'default'
            print('%-24s %-7s %s' % (name, where, setting(name) or '-'))
        return 0
    if argv[0] in ('set', 'unset') and len(argv) == (3 if argv[0] == 'set' else 2) and argv[1].startswith('NEXUS_'):
        stored = _file_settings()
        if argv[0] == 'set':
            if argv[1] in ('NEXUS_TEST_SOURCE', 'NEXUS_TEST_DISK'): refuse_user_disk(argv[2], argv[1])
            stored[argv[1]] = str(Path(argv[2]))
        else: stored.pop(argv[1], None)
        path = settings_file(); path.parent.mkdir(parents=True, exist_ok=True)
        part = path.with_name(path.name + '.part'); part.write_text(json.dumps(stored, indent=1, sort_keys=True) + '\n', encoding='utf-8')
        part.replace(path); print('%s %s in %s' % ('set' if argv[0] == 'set' else 'removed', argv[1], path)); return 0
    print('usage: nexus_paths.py [show] | set NEXUS_NAME PATH | unset NEXUS_NAME', file=sys.stderr); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

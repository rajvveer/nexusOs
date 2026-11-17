"""Pinned Windows applications for the Wine campaign (tests/wine/apps-lock.json).

    python -I tests/wine_apps.py fetch [--cache DIR]      download (once) and verify every pinned file
    python -I tests/wine_apps.py pin NAME [--cache DIR]   record the SHA-256 of an entry whose publisher has no checksum

Downloads come only from the publishers' sites or their GitHub releases (the hosts below); a cached file is used only
when its size and SHA-256 match the lock.
"""
from pathlib import Path
import argparse
import hashlib
import json
import sys
import urllib.request
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / 'tests/wine/apps-lock.json'
DEFAULT_CACHE = nexus_paths.cache('wine-apps')
ALLOWED = ('https://github.com/ip7z/', 'https://github.com/notepad-plus-plus/', 'https://github.com/jrsoftware/', 'https://the.earth.li/~sgtatham/putty/',
           'https://www.sumatrapdfreader.org/dl/', 'https://files.sumatrapdfreader.org/software/sumatrapdf/', 'https://objects.githubusercontent.com/', 'https://release-assets.githubusercontent.com/')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        while block := f.read(1 << 20): digest.update(block)
    return digest.hexdigest()


def load():
    return json.loads(LOCK.read_text())


def download(entry, target):
    if not entry['url'].startswith(ALLOWED): raise SystemExit('download origin not allowed: ' + entry['url'])
    part = target.with_name(target.name + '.part')
    request = urllib.request.Request(entry['url'], headers={'User-Agent': 'NexusOS-wine-test/1'})
    with urllib.request.urlopen(request, timeout=300) as response, open(part, 'wb') as out:
        if not response.geturl().startswith(ALLOWED): raise SystemExit('redirected off the publisher: ' + response.geturl())
        while block := response.read(1 << 20): out.write(block)
    part.replace(target)


def fetch(cache, names=None):
    """{name: path} of verified files (downloads what is missing)."""
    cache.mkdir(parents=True, exist_ok=True); result = {}
    for entry in load()['apps']:
        if names and entry['name'] not in names: continue
        path = cache / entry['name']
        ok = path.exists() and entry['sha256'] and sha256(path) == entry['sha256'] and (entry['bytes'] is None or path.stat().st_size == entry['bytes'])
        if not ok:
            download(entry, path)
            got = sha256(path)
            if not entry['sha256']: raise SystemExit('%s has no pinned SHA-256 yet: run "wine_apps.py pin %s" (got %s)' % (entry['name'], entry['name'], got))
            if got != entry['sha256'] or (entry['bytes'] is not None and path.stat().st_size != entry['bytes']):
                path.unlink(); raise SystemExit('verification failed for %s: %s' % (entry['name'], got))
        result[entry['name']] = path
    return result


def pin(cache, name):
    lock = load(); entry = next(e for e in lock['apps'] if e['name'] == name)
    path = cache / name; cache.mkdir(parents=True, exist_ok=True)
    if not path.exists(): download(entry, path)
    entry['sha256'] = sha256(path); entry['bytes'] = path.stat().st_size
    LOCK.write_bytes((json.dumps(lock, indent=1) + '\n').encode())
    print('pinned', name, entry['sha256'], entry['bytes'])


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('action', choices=('fetch', 'pin')); ap.add_argument('name', nargs='?')
    ap.add_argument('--cache', type=Path, default=DEFAULT_CACHE)
    args = ap.parse_args()
    if args.action == 'pin': pin(args.cache, args.name)
    else:
        for name, path in fetch(args.cache).items(): print('ok', name, path.stat().st_size)

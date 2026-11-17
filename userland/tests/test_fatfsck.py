"""Host checks for userland/fatfsck.py: planted damage (tests/fatcorrupt.py) is reported by -n without writing, -a repairs
it to exactly the expected state, a second -n is clean, --recover keeps lost chains as files, and the planted image's
counts match. Usage: python -I userland/tests/test_fatfsck.py [base FAT image (default arch/x86_64/out/apps64.img)] [work dir]"""
import hashlib, importlib.util, shutil, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path); module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module
fatfsck = load('t_fatfsck', ROOT / 'userland/fatfsck.py')
fatcorrupt = load('t_fatcorrupt', ROOT / 'tests/fatcorrupt.py')
base = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'arch/x86_64/out/apps64.img'
work = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(tempfile.mkdtemp(prefix='fatfsck-'))
work.mkdir(parents=True, exist_ok=True)
digest = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
try:
    clean = work / 'clean.img'; shutil.copyfile(base, clean)
    status, r = fatfsck.check(clean)
    assert status == 0, ('base image not clean', r.problems)
    img = work / 'damaged.img'; shutil.copyfile(base, img)
    expect = fatcorrupt.plant(img)
    before = digest(img)
    status, r = fatfsck.check(img)
    assert status == 4 and digest(img) == before, (status, 'check-only wrote the image')
    c = r.counts
    for key, want in (('lost_clusters', expect['lost']), ('cross_links', 1), ('pending_entries', 1), ('duplicates', 1),
                      ('fat_copies_differ', 1), ('fat_invalid', 1), ('dirty', 1)):
        assert c[key] == want, (key, c[key], want, r.problems)
    assert c['size_mismatch'] >= 3 and c['orphan_lfn'] >= 1 and c['dir_fixes'] >= 2 and c['hidden_entries'] == 1, (c, r.problems)
    recover = work / 'recover.img'; shutil.copyfile(img, recover)
    status, r = fatfsck.check(img, repair=True)
    assert status == 1, (status, r.problems)
    problems = fatcorrupt.verify_repaired(img, expect)
    assert not problems, problems
    status, r2 = fatfsck.check(img)
    assert status == 0, ('not clean after repair', r2.problems)
    assert r2.counts['free'] == expect['free_before'] + expect['lost'], (r2.counts['free'], expect['free_before'], expect['lost'])
    status, r3 = fatfsck.check(recover, repair=True, recover=True)
    assert status == 1 and any('FSCK0000.REC' in a for a in r3.actions), r3.actions
    status, r4 = fatfsck.check(recover)
    assert status == 0 and r4.counts['free'] == expect['free_before'], (status, r4.problems, r4.counts['free'])
    print('PASS fatfsck: %d problems found read-only (image unchanged), repaired to the expected state, clean afterwards; '
          '--recover kept %d lost chains as files' % (len(r.problems), len(r3.actions) - 1))
finally:
    if len(sys.argv) <= 2: shutil.rmtree(work, ignore_errors=True)

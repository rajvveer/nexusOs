"""Run one desk-group Aurora test on qcow2 overlays only (no writable copy of the 8 GB desktop disk).

    python -I tests/desk_overlay_run.py WORK TEST.py [ARGS...]     (base: $NEXUS_DESK_BASE, default the integ desk disk)

The test gets --disk = the read-only base; aurora_vm.stage() and the test's own fatput edits go into a copy-on-write
view of the base (tests/x64_overlay.py), and every DeskVm boot runs on a fresh qcow2 over a snapshot of that view (a test
that keeps its overlay across boots keeps it, as before). Nothing is ever written to the base or to its folder.
w2_suite spec: label=desk_overlay_run.py:{work} test_aurora_wm.py"""
import atexit, os, runpy, subprocess, sys
from pathlib import Path

TREE = Path(__file__).resolve().parents[1]
BASE = Path(os.environ.get('NEXUS_DESK_BASE', r'D:\nexustest\prod\integ_desk_disk\desktop64.img')).resolve()
work, test, rest = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3:]
work.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(TREE / 'tests'))
import aurora_vm, desk_vm, x64_overlay as xo  # noqa: E402

stage_dir = work / '_overlay_stage'; stage_dir.mkdir(exist_ok=True)
view = xo.CowFile(BASE, stage_dir / 'fsync.tmp'); view.close = lambda: None
opener = xo._view_open(view, BASE)
made = []


def guard_base(path):
    if Path(path).resolve() != BASE: raise RuntimeError(f'unexpected disk {path}')


original_module = aurora_vm.module
def module(name, path):
    m = original_module(name, path)
    if Path(path).name == 'fatput.py': m.open = opener   # writes to the base go to the view
    return m
aurora_vm.module = module


def stage(disk, files):
    guard_base(disk); fat = module('overlay_stage_fatput', TREE / 'userland/fatput.py'); args = ['fatput', str(BASE)]
    for i, (guest, host) in enumerate(files.items()):
        if isinstance(host, (bytes, str)) and not isinstance(host, Path):
            data = host.encode() if isinstance(host, str) else host
            host = stage_dir / f'put{len(made)}_{i}'; host.write_bytes(data); made.append(host)
        args += ['put', str(host), guest.strip('/')]
    if fat.edit(args): raise RuntimeError('fatput failed')
aurora_vm.stage = stage


original_init, original_close = desk_vm.DeskVm.__init__, desk_vm.DeskVm.close
def init(self, work_dir, disk, *a, keep_overlay=False, overlay=None, raw_disk=False, **k):
    guard_base(disk)
    if raw_disk: raise RuntimeError('raw_disk boots would write the base')
    ov = Path(overlay) if overlay else Path(work_dir) / 'overlay.qcow2'
    if not keep_overlay or not ov.exists():
        staged = stage_dir / f'staged{len(made)}.qcow2'; xo.snapshot(view, BASE, staged); made.append(staged)
        ov.parent.mkdir(parents=True, exist_ok=True); ov.unlink(missing_ok=True)
        subprocess.run([desk_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(staged), '-F', 'qcow2', str(ov)], check=True)
    original_init(self, work_dir, disk, *a, keep_overlay=True, overlay=ov, raw_disk=False, **k)
    self.overlay_temporary = None if keep_overlay else ov
def close(self):
    original_close(self)
    if getattr(self, 'overlay_temporary', None): self.overlay_temporary.unlink(missing_ok=True)
desk_vm.DeskVm.__init__, desk_vm.DeskVm.close = init, close


@atexit.register
def cleanup():
    for p in sorted(stage_dir.rglob('*'), reverse=True):
        try: p.unlink() if p.is_file() else p.rmdir()
        except OSError: pass
    try: stage_dir.rmdir()
    except OSError: pass


sys.argv = [str(TREE / 'tests' / test), str(work), '--disk', str(BASE), *rest]
runpy.run_path(str(TREE / 'tests' / test), run_name='__main__')

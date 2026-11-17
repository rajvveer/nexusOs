"""Chrome WebGL on the virgl GPU path, end to end through the Aurora desktop (harness-based check).

python -I tests/test_x64_gpu_chrome.py --disk DESKTOP_IMG [--harness DIR] [--port 8840] [--qmp-port 4540] [--min-fps 30] [--gpu-page]

DESKTOP_IMG must carry the GPU userland and GPU Chrome arguments:
    python -I arch/x86_64/gpu_userland.py install DESKTOP_IMG --chrome-args ARGS
(ARGS = arch/x86_64/etc/chrome-desktop-gpu.args), plus the kernel and /sbin/init (desktop.elf) from this tree.
The harness (--harness, default NEXUS_GPU_HARNESS: a perf harness whose perf_run.py has --device/--display/--url) boots
the disk with snapshot=on on -device virtio-vga-gl -display sdl,gl=on (hidden window), opens
tests/fixtures/gpu/webgl.html in Chrome and asserts: the WebGL renderer is Mesa virgl through ANGLE, all
three scenes (textured cubes, 20k CPU-updated particles, 300k GPU particles) render (pixel readback) at
>= --min-fps, Aurora received dma-buf buffers, and no GPU process crash. With --gpu-page it also captures
chrome://gpu screenshots into the run folder (PNG) and deletes the raw PPM dumps.
"""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
CPU = 'qemu64,+rdrand,+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm,+smep,+smap'
ap = argparse.ArgumentParser()
ap.add_argument('--disk', type=Path, required=True)
ap.add_argument('--harness', type=Path, default=nexus_paths.file('NEXUS_GPU_HARNESS', nexus_paths.data() / 'gpu-harness'))
ap.add_argument('--port', type=int, default=8840); ap.add_argument('--qmp-port', type=int, default=4540)
ap.add_argument('--min-fps', type=float, default=30)
ap.add_argument('--name', default='gpu_chrome_check')
ap.add_argument('--gpu-page', action='store_true')
args = ap.parse_args()
for page in ('webgl.html', 'gpu.html'): shutil.copyfile(ROOT / 'tests/fixtures/gpu' / page, args.harness / 'www' / page)
common = [sys.executable, '-I', str(args.harness / 'perf_run.py'), '--boot', str(OUT / 'nexus64.img'), '--kbin', str(OUT / 'kernel.bin'),
          '--kelf', str(OUT / 'kernel.elf'), '--disk', str(args.disk), '--port', str(args.port), '--qmp-port', str(args.qmp_port),
          '--smp', '2', '--cpu', CPU, '--device', 'virtio-vga-gl', '--display', 'sdl,gl=on']
subprocess.run([*common[:3], args.name, *common[3:], '--page', 'webgl', '--timeout', '400'], check=True, cwd=args.harness)
run = args.harness / 'runs' / args.name
summary = json.loads((run / 'summary.json').read_text())
log = (run / 'serial.log').read_bytes().decode('latin1')
results = [json.loads(line) for line in (run / 'results.jsonl').read_text().splitlines() if line.strip()]
done = next(r['data'] for r in results if r.get('kind') == 'done')
for ppm in run.glob('*.ppm'): ppm.unlink()
assert summary['done'] and summary['disk_unchanged'], summary
assert 'virgl' in (done.get('renderer') or '') and 'ANGLE' in done['renderer'], done.get('renderer')
for scene in ('cubes', 'particles_cpu_20k', 'particles_gpu_300k'):
    r = done['tests'][scene]
    assert r['error'] == 0 and r['lit_samples'] >= 1 and r['fps'] >= args.min_fps, (scene, r)
assert '[64] AURORA DMABUF zwp_linux_dmabuf_v1' in log and '[64] AURORA DMABUF ' in log.split('zwp_linux_dmabuf_v1', 1)[1], 'no dma-buf buffers'
assert 'GPU process exited unexpectedly' not in log and 'KERNEL_FAULT' not in log and 'VIRTIO-GPU error' not in log, 'GPU failure in log'
print('PASS Chrome WebGL on virgl:', done['renderer'], {k: round(v['fps'], 1) for k, v in done['tests'].items()}, flush=True)
if args.gpu_page:
    subprocess.run([*common[:3], args.name + '_gpu_page', *common[3:], '--url', 'chrome://gpu', '--settle', '20', '--pages', '3'], check=True, cwd=args.harness)
    from PIL import Image
    page = args.harness / 'runs' / (args.name + '_gpu_page')
    for ppm in sorted(page.glob('*.ppm')):
        if ppm.name.startswith('url_'): Image.open(ppm).save(ppm.with_suffix('.png'))
        ppm.unlink()
    print('chrome://gpu captured in', page, flush=True)

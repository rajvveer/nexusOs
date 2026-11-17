"""Drive NexusOS (Aurora UI) in QEMU: screenshots of splash, login, desktop, overlays.

usage: python -I harness.py <project_dir> <work_dir> <port> <tag> <vga: virtio|std> <script>
script is a ';'-separated list of steps:
  sleep:N  shot:NAME  type:TEXT  key:NAME  ret  mouse:DX,DY  click  rclick  serial
  run:FUNC   (Phase 55) call nettests.FUNC(ctx) on the host while the VM is up;
             ctx = {'ports': {guest_port: host_port}, 'work', 'tag', 'log'}.
env HOSTFWD="hp:gp,hp:gp,..." adds -netdev hostfwd=tcp:127.0.0.1:hp-:gp entries.
env USBDEV=kbd|storage adds a UHCI controller + device.
"""
import os, sys, time, socket, shutil, subprocess, traceback
from PIL import Image
proj, work, port, tag, vga, script = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], sys.argv[6]
QEMU = r"C:\Program Files\qemu\qemu-system-i386.exe"
os.makedirs(work, exist_ok=True)
shots = os.path.join(work, "shots_" + tag); os.makedirs(shots, exist_ok=True)
serial = os.path.join(work, f"serial_{tag}.log")
for f in ("nexus.img", "disk.img"):
    shutil.copyfile(os.path.join(proj, f), os.path.join(work, f"{tag}_{f}"))
try: os.remove(serial)
except OSError: pass
args = [QEMU, "-m", "2048", "-drive", f"file={os.path.join(work, tag + '_nexus.img')},format=raw,if=floppy", "-boot", "a",
        "-drive", f"file={os.path.join(work, tag + '_disk.img')},format=raw,if=ide"]
args += (["-vga", "none", "-device", "virtio-vga"] if vga == "virtio" else ["-vga", "std"])
if os.environ.get("USBDEV") == "kbd": args += ["-device", "piix3-usb-uhci", "-device", "usb-kbd"]
elif os.environ.get("USBDEV") == "storage":
    _d = os.path.join(work, "stick.img")
    if not os.path.exists(_d): open(_d, "wb").write(b"NEXUSOS-USBSTICK" + bytes(8 * 1024 * 1024 - 16))
    args += ["-device", "piix3-usb-uhci", "-drive", "if=none,id=ustick,format=raw,file=" + _d, "-device", "usb-storage,drive=ustick"]
# Phase 55: host port forwards (host:guest pairs)
ports = {}
netdev = "user,id=n0"
for pair in filter(None, os.environ.get("HOSTFWD", "").split(',')):
    hp, gp = (int(v) for v in pair.split(':'))
    ports[gp] = hp
    netdev += f",hostfwd=tcp:127.0.0.1:{hp}-:{gp}"
args += ["-serial", f"file:{serial}", "-monitor", f"tcp:127.0.0.1:{port},server,nowait",
         "-netdev", netdev, "-device", "rtl8139,netdev=n0",
         "-audiodev", "none,id=snd0", "-device", "AC97,audiodev=snd0", "-display", "none"]
proc = subprocess.Popen(args)
s = None
for _ in range(40):
    time.sleep(0.25)
    try: s = socket.create_connection(('127.0.0.1', port)); break
    except OSError: pass
s.settimeout(0.1)
def drain():
    try:
        while s.recv(4096): pass
    except Exception: pass
def raw(c, w=0.1): s.sendall((c + '\n').encode()); time.sleep(w); drain()
KM = {' ': 'spc', '.': 'dot', '-': 'minus', '/': 'slash', ';': 'semicolon', ',': 'comma', '=': 'equal',
      '_': 'shift-minus', ':': 'shift-semicolon', '>': 'shift-dot', '<': 'shift-comma', '?': 'shift-slash',
      '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4', '%': 'shift-5', '^': 'shift-6',
      '&': 'shift-7', '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', '"': 'shift-apostrophe', "'": 'apostrophe',
      '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent', '~': 'shift-grave_accent',
      '+': 'shift-equal', '{': 'shift-bracket_left', '}': 'shift-bracket_right', '|': 'shift-backslash'}
def typ(t):
    for ch in t:
        if ch.isupper(): raw('sendkey shift-' + ch.lower())
        else: raw('sendkey ' + KM.get(ch, ch))
def shot(n):
    p = os.path.join(work, f"_{tag}_{n}.ppm"); raw('screendump ' + p, 0.5)
    for _ in range(10):
        if os.path.exists(p) and os.path.getsize(p) > 0: break
        time.sleep(0.2)
    try: Image.open(p).convert('RGB').save(os.path.join(shots, n + '.png')); os.remove(p)
    except Exception as e: print("shot fail", n, e)
def ser():
    try: return open(serial, 'rb').read().decode('latin1', 'replace')
    except OSError: return ''
t0 = time.time()
logf = open(os.path.join(work, f"host_{tag}.log"), "a")
def log(*a):
    line = f"[{time.time()-t0:5.1f}s] " + " ".join(str(x) for x in a)
    print(line, flush=True); logf.write(line + "\n"); logf.flush()
ctx = {'ports': ports, 'work': work, 'tag': tag, 'log': log, 'shots': shots}
for step in script.split(';'):
    step = step.strip()
    if not step: continue
    cmd, _, arg = step.partition(':')
    if cmd == 'sleep': time.sleep(float(arg))
    elif cmd == 'shot': shot(arg)
    elif cmd == 'type': typ(arg)
    elif cmd == 'key': raw('sendkey ' + arg, 0.15)
    elif cmd == 'ret': raw('sendkey ret', 0.15)
    elif cmd == 'mouse':
        dx, dy = (int(v) for v in arg.split(','))
        while dx or dy:
            sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
            raw(f'mouse_move {sx} {sy}', 0.05); dx -= sx; dy -= sy
    elif cmd == 'click': raw('mouse_button 1', 0.12); raw('mouse_button 0', 0.3)
    elif cmd == 'rclick': raw('mouse_button 2', 0.12); raw('mouse_button 0', 0.3)
    elif cmd == 'serial':
        d = ser(); i = d.find('!EX:')
        print(f"[{time.time()-t0:5.1f}s] serial:", ('CRASH ' + d[i:i+40]) if i >= 0 else 'ok')
    elif cmd == 'run':
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import nettests
        log(f"== run {arg} ==")
        try: nettests.__dict__[arg](ctx)
        except Exception:
            log("EXCEPTION in", arg); log(traceback.format_exc())
        log(f"== end {arg} ==")
d = ser(); i = d.find('!EX:')
print("FINAL:", ('CRASH ' + d[i:i+40].replace('\n', ' ')) if i >= 0 else 'no faults', f"({time.time()-t0:.0f}s)")
raw('quit')
try: proc.wait(timeout=5)
except Exception: proc.kill()

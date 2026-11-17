"""Nexus Browser test driver (Phase 57) = harness.py + an embedded test web server + absolute mouse moves.

usage: python -I bharness.py <project_dir> <work_dir> <monitor_port> <tag> <site_dir> <http_port> <script>
script steps (';'-separated): sleep:N  shot:NAME  type:TEXT  key:NAME  ret  mouse:DX,DY  mouseabs:X,Y  click  rclick  serial
The web server serves <site_dir> on 127.0.0.1:<http_port> (the guest reaches it as http://10.0.2.2:<http_port>/),
answers /redirect with a 302 to /index.html and logs every request to <work_dir>/http_<tag>.log.
"""
import os, sys, time, socket, shutil, subprocess, threading, http.server, functools
from PIL import Image
proj, work, port, tag, site, hport, script = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5], int(sys.argv[6]), sys.argv[7]
QEMU = r"C:\Program Files\qemu\qemu-system-i386.exe"
os.makedirs(work, exist_ok=True)
shots = os.path.join(work, "shots_" + tag); os.makedirs(shots, exist_ok=True)
serial = os.path.join(work, f"serial_{tag}.log")
httplog = open(os.path.join(work, f"http_{tag}.log"), "a")

class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, fmt, *a):
        httplog.write(f"[{time.strftime('%H:%M:%S')}] {self.address_string()} {fmt % a}\n"); httplog.flush()
    def do_GET(self):
        if self.path == "/truncated":
            self.send_response(200); self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "100"); self.end_headers()
            self.wfile.write(b"partial"); self.close_connection = True; return
        if self.path == "/redirect":
            self.send_response(302); self.send_header("Location", "/index.html"); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path == "/redirect2":
            self.send_response(301); self.send_header("Location", "http://10.0.2.2:%d/redirect" % hport); self.send_header("Content-Length", "0"); self.end_headers(); return
        if self.path == "/chunked":
            body = b"<html><head><title>Chunked</title></head><body><h1>Chunked transfer</h1>" + b"".join(b"<p>chunk %d of a chunked body</p>" % i for i in range(40)) + b"</body></html>"
            self.protocol_version = "HTTP/1.1"
            self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close"); self.end_headers()
            for i in range(0, len(body), 700):
                c = body[i:i+700]; self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
            self.wfile.write(b"0\r\n\r\n"); return
        return super().do_GET()
    def guess_type(self, path):
        if path.endswith(".bin"): return "application/octet-stream"
        return super().guess_type(path)

srv = http.server.ThreadingHTTPServer(("127.0.0.1", hport), functools.partial(H, directory=site))
threading.Thread(target=srv.serve_forever, daemon=True).start()

for f in ("nexus.img", "disk.img"):
    shutil.copyfile(os.path.join(proj, f), os.path.join(work, f"{tag}_{f}"))
try: os.remove(serial)
except OSError: pass
args = [QEMU, "-cpu", "max", "-m", "2048", "-drive", f"file={os.path.join(work, tag + '_nexus.img')},format=raw,if=floppy", "-boot", "a",
        "-drive", f"file={os.path.join(work, tag + '_disk.img')},format=raw,if=ide",
        "-vga", "none", "-device", "virtio-vga",
        "-serial", f"file:{serial}", "-monitor", f"tcp:127.0.0.1:{port},server,nowait",
        "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0",
        "-object", f"filter-dump,id=fd0,netdev=n0,file={os.path.join(work, 'net_' + tag + '.pcap')}",
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
def mouse_rel(dx, dy):
    while dx or dy:
        sx = max(-100, min(100, dx)); sy = max(-100, min(100, dy))
        raw(f'mouse_move {sx} {sy}', 0.04); dx -= sx; dy -= sy
t0 = time.time()
logf = open(os.path.join(work, f"host_{tag}.log"), "a")
def log(*a):
    line = f"[{time.time()-t0:5.1f}s] " + " ".join(str(x) for x in a)
    print(line, flush=True); logf.write(line + "\n"); logf.flush()
for step in script.split(';'):
    step = step.strip()
    if not step: continue
    cmd, _, arg = step.partition(':')
    if cmd == 'sleep': time.sleep(float(arg))
    elif cmd == 'shot': shot(arg); log("shot", arg)
    elif cmd == 'type': typ(arg)
    elif cmd == 'key': raw('sendkey ' + arg, 0.15)
    elif cmd == 'ret': raw('sendkey ret', 0.15)
    elif cmd == 'mouse':
        dx, dy = (int(v) for v in arg.split(',')); mouse_rel(dx, dy)
    elif cmd == 'mouseabs':
        x, y = (int(v) for v in arg.split(','))
        mouse_rel(-2200, -1300); time.sleep(0.2); mouse_rel(x, y); time.sleep(0.3)
    elif cmd == 'click': raw('mouse_button 1', 0.12); raw('mouse_button 0', 0.3)
    elif cmd == 'rclick': raw('mouse_button 2', 0.12); raw('mouse_button 0', 0.3)
    elif cmd == 'serial':
        d = ser(); i = d.find('!EX:'); k = d.find('!KR:')
        log("serial:", ('CRASH ' + d[i:i+40]) if i >= 0 else ('KR ' + d[k:k+60]) if k >= 0 else 'ok')
d = ser(); i = d.find('!EX:'); k = d.find('!KR:')
print("FINAL:", ('CRASH ' + d[i:i+40].replace('\n', ' ')) if i >= 0 else ('KGUARD-RECOVERED ' + d[k:k+60].replace('\n', ' ')) if k >= 0 else 'no faults', f"({time.time()-t0:.0f}s)")
raw('quit')
try: proc.wait(timeout=5)
except Exception: proc.kill()
srv.shutdown()

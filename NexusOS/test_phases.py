#!/usr/bin/env python3
"""
test_phases.py — headless NexusOS phase-verification driver.

Boots nexus.img in QEMU with a monitor socket + serial log, logs in, drops to
the text shell (Esc), then runs a scripted batch of shell commands, screenshotting
at checkpoints. Detects CPU-exception crashes via the '!EX:' marker the kernel
writes to COM1 (serial.log).

Usage:
    python test_phases.py <batch> [monitor_port]

<batch> is one of the keys in BATCHES below (e.g. era1, era2, ...). Screenshots
land in test_out/<batch>_<label>.png. A summary is printed and written to
test_out/<batch>_report.txt.

Requires PIL. Assumes a QEMU instance is ALREADY running on the given monitor
port (launch it with run_qemu.py first), so multiple batches can reuse one boot,
OR pass --launch to start+drive+quit a private instance.
"""
import socket, time, sys, os, subprocess, signal

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "test_out")
os.makedirs(OUT, exist_ok=True)

# QEMU monitor `screendump` splits its filename argument on spaces, so the dump
# target MUST be a path with no spaces (our project path has "New folder").
# Dump to a space-free scratch dir, then convert into OUT.
TMP = os.environ.get("NEXUS_SHOT_TMP", r"C:\nexustest")
os.makedirs(TMP, exist_ok=True)

QEMU = r"C:\Program Files\qemu\qemu-system-i386.exe"

KEYMAP = {'.': 'dot', '/': 'slash', '-': 'minus', ' ': 'spc', '\\': 'backslash',
          ',': 'comma', ';': 'semicolon', '=': 'equal', "'": 'apostrophe',
          '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent'}

class Mon:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port))
        self.s.settimeout(2.0)
        self.drain()
    def drain(self):
        try:
            while True:
                d = self.s.recv(4096)
                if not d: break
        except Exception:
            pass
    def raw(self, c):
        self.s.sendall((c + '\n').encode()); time.sleep(0.22); self.drain()
    def typ(self, text):
        for ch in text:
            if ch.isalpha(): self.raw('sendkey ' + ch.lower())
            elif ch.isdigit(): self.raw('sendkey ' + ch)
            elif ch in KEYMAP: self.raw('sendkey ' + KEYMAP[ch])
            time.sleep(0.02)
    def ret(self): self.raw('sendkey ret')
    def key(self, k): self.raw('sendkey ' + k)
    def line(self, text):
        """type a command and press enter"""
        self.typ(text); self.ret()
    def shot(self, name):
        # dump to a NO-SPACE scratch path (monitor splits filename on spaces)
        raw = os.path.join(TMP, name + '.ppm')
        self.raw('screendump ' + raw)
        time.sleep(0.5)
        try:
            from PIL import Image
            im = Image.open(raw).convert('RGB')
            im.save(os.path.join(OUT, name + '.png'))
            os.remove(raw)
        except Exception as e:
            print('  [shot warn]', name, e)
    def quit(self):
        try: self.raw('quit')
        except Exception: pass
        try: self.s.close()
        except Exception: pass


def login_and_shell(m, boot_wait=14):
    """Wait for boot, log in (root/root), drop to text shell via Esc."""
    time.sleep(boot_wait)
    m.line('root')      # username
    m.line('root')      # password
    time.sleep(3)
    m.key('esc')        # desktop -> text shell
    time.sleep(1.5)


# Each batch = list of (command, screenshot_label_or_None, post_sleep)
BATCHES = {
  # Era 0-2: core shell, fs, processes, editor
  # NOTE: sysinfo/hexview/contacts/colors/todo/pong/tetris/calc/files/etc all
  # route to cmd_gui() and would drop us into the GUI desktop, breaking the rest
  # of the text-shell campaign. Keep this batch to pure text commands only.
  "core": [
    ("help", "help", 1.0), ("about", None, 0.5), ("uname", "uname", 0.5),
    ("whoami", None, 0.5), ("meminfo", "meminfo", 0.8), ("heapinfo", "heapinfo", 0.8),
    ("uptime", None, 0.5), ("date", None, 0.5), ("ps", "ps", 0.8),
    ("echo hello-nexus", "echo", 0.5), ("history", None, 0.5),
    ("ls", "ls", 0.8), ("env", "env", 0.6),
  ],
  "fs": [
    ("touch testfile.txt", None, 0.5), ("write testfile.txt hello world", None, 0.5),
    ("cat testfile.txt", "cat", 0.8), ("ls", "ls_after", 0.8),
    ("head testfile.txt", None, 0.5), ("wc testfile.txt", None, 0.5),
    ("grep hello testfile.txt", "grep", 0.8), ("rm testfile.txt", None, 0.5),
    ("trash", None, 0.5),
  ],
  # Era 3: networking
  "net": [
    ("ifconfig", "ifconfig", 0.8), ("net", "net", 0.8), ("netstat", "netstat", 0.8),
    ("arp", None, 0.8), ("dhcp", "dhcp", 2.0), ("dns nexusos.org", "dns", 3.0),
    ("ping 10.0.2.2", "ping", 3.0), ("ntp", None, 2.0),
  ],
  # Era 4: compatibility layers
  "compat": [
    ("xinfo", "xinfo", 0.8), ("win32info", "win32info", 0.8),
    ("machoinfo", "machoinfo", 0.8), ("regedit", None, 0.8),
    ("ldconfig", None, 0.8), ("npkg list", "npkg", 1.0),
    ("script", None, 0.8),
  ],
  # Era 5: multimedia
  "media": [
    ("sndinfo", "sndinfo", 0.8), ("tone 440 200", None, 1.0),
    ("imginfo", "imginfo", 1.5), ("vidinfo", "vidinfo", 1.0),
    ("gpuinfo", "gpuinfo", 0.8), ("gpubench", "gpubench", 3.0),
  ],
  # Era 6: polish (accessibility, security, cloud, ai, mobile, perf)
  "polish": [
    ("accinfo", "accinfo", 0.8), ("id", None, 0.5), ("users", "users", 0.8),
    ("firewall list", None, 0.8), ("ai", "ai", 0.8),
    ("ask date", "ask", 1.5), ("mobileinfo", "mobileinfo", 0.8),
    ("perf", "perf", 1.0), ("smp", None, 0.5), ("preempt", None, 0.5),
  ],
  # Era 7: world domination (appstore, finale, npfs)
  "domination": [
    ("store", "store", 1.0), ("finale", "finale", 1.0),
    ("npfs", "npfs_usage", 0.8), ("npfs format", "npfs_format", 1.5),
    ("npfs write hello.txt NexusOS-rocks", None, 1.0),
    ("npfs cat hello.txt", "npfs_cat", 1.0),
    ("npfs crashtest", "npfs_crashtest", 2.0),
  ],
}


def check_serial():
    path = os.path.join(HERE, "serial.log")
    if not os.path.exists(path): return "(no serial.log)"
    try:
        data = open(path, 'rb').read().decode('latin1', 'replace')
    except Exception as e:
        return f"(serial read err {e})"
    if "!EX:" in data:
        idx = data.find("!EX:")
        return "CRASH: " + data[idx:idx+40].replace('\n',' ')
    return "no faults"


def run_batch(port, batch, do_login):
    m = Mon(port)
    if do_login:
        login_and_shell(m)
    cmds = BATCHES[batch]
    log = [f"=== batch {batch} ==="]
    for (c, label, slp) in cmds:
        m.line(c)
        time.sleep(slp)
        if label: m.shot(f"{batch}_{label}")
        log.append(f"ran: {c}" + (f"  [shot {label}]" if label else ""))
    m.shot(f"{batch}_final")
    log.append("serial: " + check_serial())
    report = "\n".join(log)
    open(os.path.join(OUT, f"{batch}_report.txt"), 'w').write(report)
    print(report)
    return m


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("batches:", ", ".join(BATCHES)); sys.exit(1)
    batch = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 55600
    do_login = "--nologin" not in sys.argv
    m = run_batch(port, batch, do_login)
    if "--keep" not in sys.argv:
        pass  # leave instance running for next batch

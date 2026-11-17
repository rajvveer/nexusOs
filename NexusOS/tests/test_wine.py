"""Wine on NexusOS: real Windows programs (64-bit and 32-bit) on the Aurora desktop.

    python -I tests/test_wine.py WORK [--source DESKTOP64.img | --disk PREPARED.img] [--keep-disk]
           [--cache DIR] [--apps-cache DIR] [--accel auto|whpx|tcg] [--qmp-port 4621] [--http-port 8920] [--tcp-port 8921]
           [--phases console,apps,gui,desktop]

Disk: a sparse copy of --source with Wine 10.0 (new WoW64 + Wayland driver) installed by arch/x86_64/wine_install.py
(every package pinned in wine-lock.json, cached in --cache). --disk uses a disk that already has Wine: the test files
are staged into a sparse copy of it in WORK (booted through a qcow2 overlay), so the given image is never written. The worktree's Aurora (out/desktop.elf as /sbin/init,
out/aurora_app.elf as /bin/aurora-app, out/nexus-installer-ui.elf as /bin/xdg-open) and the script runner
(out/wine_runner_user.elf) are staged. Test programs (tests/wine/*.c, *.cpp) are built here with the pinned zig for
x86_64-windows-gnu and x86-windows-gnu; the applications are the pinned official downloads of tests/wine/apps-lock.json
(cached in --apps-cache). A disk copy made here is deleted at the end unless --keep-disk.

One boot runs a generated runner script; at each "gate" the script waits while this test drives the desktop with QMP
input and screendumps, then continues:
  console  wineboot --init creates the prefix (timed), cmd ver, reg query, hostname, ipconfig (netlink), 32-bit cmd (WoW64),
           winetest64/32 (environment, time, memory, threads, SEH, files, registry, processes and pipes, 32<->64-bit,
           winsock incl. loopback and a TCP server on the host), C++ exceptions in both bitnesses
  apps     MSI install/uninstall into a custom folder (64- and 32-bit 7-Zip packages), 7-Zip x64/x86 installers and archive
           round trips with 7z.exe (contents compared), PuTTY MSI, Notepad++ NSIS installer, Inno Setup x64/x86 (its
           ISCC compiles a script into an installer that installs and uninstalls silently), portable zips
  gui      wtgui64 (painting, click, typing, button, check box, list, menu, second window, Alt+F4, modal dialog, message
           box, maximize/restore, resize, move, minimize/restore, close button), wtgui32, notepad (type + Save As),
           Notepad++ x64 portable and x86 installed (edit + save), winecfg, regedit, PuTTY x64/x86, SumatraPDF x64/x86 with a
           PDF, 7-Zip File Manager x64/x86
  desktop  Files opens an .exe with Wine (double-click), xdg-open (how Chrome opens a finished download) runs a downloaded
           .exe with Wine, Spotlight lists and starts installed Windows programs (winemenubuilder entries + icons), Wine
           Configuration and Windows Programs, the Terminal runs "wine cmd", and no Wine process is left once the programs
           are closed
Result: WORK/result.json (each check with its evidence), screenshots WORK/*.png, guest log WORK/serial.log.
"""
from collections import Counter
from pathlib import Path
import argparse
import json
import random
import re
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import aurora_vm  # noqa: E402
import wine_apps  # noqa: E402
import wine_host  # noqa: E402
import wine_pdf  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--source', type=Path, default=nexus_paths.source_disk())
ap.add_argument('--disk', type=Path, help='a desktop disk that already has Wine installed (booted through an overlay)')
ap.add_argument('--keep-disk', action='store_true')
ap.add_argument('--stage-in-place', action='store_true', help='with --disk: stage into that scratch disk (no copy; never the real desktop disk)')
ap.add_argument('--cache', type=Path, default=nexus_paths.cache('wine'))
ap.add_argument('--apps-cache', type=Path, default=wine_apps.DEFAULT_CACHE)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--qmp-port', type=int, default=4621); ap.add_argument('--http-port', type=int, default=8920)
ap.add_argument('--tcp-port', type=int, default=8921)
ap.add_argument('--phases', default='console,apps,gui,desktop')
ap.add_argument('--machine', default='pc', choices=('pc', 'q35'), help='pc: IDE root + RTL8139; q35: NVMe root + e1000e (the launcher default)')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
PHASES = set(args.phases.split(','))
OUT = aurora_vm.OUT
ZIG = ROOT / 'tools/zig-x86_64-windows-0.15.2/zig.exe'
WINE = '/usr/bin/wine'
started = time.time()
checks = []
result = {'test': 'wine', 'passed': False, 'phases': sorted(PHASES), 'checks': checks}


def check(phase, name, ok, detail=''):
    checks.append({'phase': phase, 'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %-8s %s%s' % ('PASS' if ok else 'FAIL', phase, name, '' if ok else '  (%s)' % str(detail)[:300]), flush=True)
    return bool(ok)


# ------------------------------------------------------------------ test programs and data
def build_programs():
    out = WORK / 'bin'; out.mkdir(exist_ok=True); built = {}
    for arch, bits in (('x86_64', 64), ('x86', 32)):
        for name, command in (('winetest', ['cc', str(ROOT / 'tests/wine/winetest.c'), '-lws2_32', '-ladvapi32']),
                              ('wtgui', ['cc', '-municode', '-Wl,--subsystem,windows', str(ROOT / 'tests/wine/wtgui.c'), '-lcomdlg32', '-lgdi32', '-luser32']),
                              ('cxx', ['c++', str(ROOT / 'tests/wine/cxxtest.cpp')])):
            exe = out / ('%s%d.exe' % (name, bits))
            subprocess.run([str(ZIG), command[0], '-target', arch + '-windows-gnu', '-O1', '-Wall', '-Wextra', '-Werror', *command[1:], '-o', str(exe)], check=True)
            built[exe.name] = exe
    return built


DATA_FILES = ['small.txt', 'big.bin', 'zeros.dat', 'sub/nested.txt']


def data_files(directory):
    (directory / 'sub').mkdir(parents=True, exist_ok=True)
    rng = random.Random(42)
    (directory / 'small.txt').write_bytes(b'NexusOS 7-Zip test file\n' * 50)
    (directory / 'big.bin').write_bytes(rng.randbytes(3 * 1024 * 1024))
    (directory / 'zeros.dat').write_bytes(bytes(1024 * 1024))
    (directory / 'sub/nested.txt').write_bytes(b'nested file\n')
    (directory / 'test.pdf').write_bytes(wine_pdf.make_pdf())
    (directory / 'npp.txt').write_bytes(b'hello64')     # different seeds: each saved file shows which Notepad++ wrote it
    (directory / 'npp32.txt').write_bytes(b'hello32')
    (directory / 'nexus.iss').write_bytes(INNO_SCRIPT.replace('\n', '\r\n').encode())
    return directory


# An Inno Setup script: compiled by ISCC under Wine into an installer that is then installed and uninstalled silently.
INNO_SCRIPT = r'''; NexusOS Wine test (tests/test_wine.py)
[Setup]
AppName=NexusInnoTest
AppVersion=1.0
AppPublisher=NexusOS tests
DefaultDirName=C:\NexusInnoTest
DisableProgramGroupPage=yes
OutputDir=Z:\home\nexus\innoout
OutputBaseFilename=nexus-inno-setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin

[Files]
Source: "Z:\home\nexus\wtdata\small.txt"; DestDir: "{app}"
Source: "Z:\home\nexus\wtdata\big.bin"; DestDir: "{app}"
'''


# ------------------------------------------------------------------ guest script (nexus-wine-runner syntax)
def q(argument):
    """Quote one argument for the runner: backslash escapes, double quotes around blanks."""
    text = str(argument).replace('\\', '\\\\').replace('"', '\\"')
    return '"%s"' % text if not text or any(ch in text for ch in ' \t\'#') else text


def z(path):
    """The Windows path of a Unix path (Wine's Z: drive)."""
    return 'Z:' + path.replace('/', '\\')


class Script:
    def __init__(self): self.lines = []
    def cmd(self, name, *words): self.lines.append(' '.join([name, *map(q, words)]))
    def run(self, tag, seconds, *argv): self.cmd('run', tag, str(seconds), *argv)
    def bg(self, tag, *argv): self.cmd('bg', tag, *argv)
    def text(self): return '\n'.join(self.lines) + '\n'


SEVENZIP64, SEVENZIP32 = r'C:\Program Files\7-Zip\7z.exe', r'C:\Program Files (x86)\7-Zip\7z.exe'
NPP32 = r'C:\Program Files (x86)\Notepad++\notepad++.exe'
GUI_GATES = [('gui64', [WINE, '/home/nexus/wt/wtgui64.exe']), ('gui32', [WINE, '/home/nexus/wt/wtgui32.exe']),
             ('notepad', [WINE, 'notepad']),
             ('npp64', [WINE, r'C:\npp64\notepad++.exe', z('/home/nexus/wtdata/npp.txt')]),
             ('npp32', [WINE, NPP32, z('/home/nexus/wtdata/npp32.txt')]),
             ('winecfg', ['/usr/bin/winecfg']), ('regedit', ['/usr/bin/regedit']),
             ('putty64', [WINE, '/home/nexus/apps/putty-w64.exe']), ('putty32', [WINE, '/home/nexus/apps/putty-w32.exe']),
             ('sumatra64', [WINE, r'C:\sumatra64\SumatraPDF-3.6.1-64.exe', z('/home/nexus/wtdata/test.pdf')]),
             ('sumatra32', [WINE, r'C:\sumatra32\SumatraPDF-3.6.1-32.exe', z('/home/nexus/wtdata/test.pdf')]),
             ('fm64', [WINE, r'C:\Program Files\7-Zip\7zFM.exe']), ('fm32', [WINE, r'C:\Program Files (x86)\7-Zip\7zFM.exe'])]


def guest_script():
    s = Script()
    for name, value in (('HOME', '/home/nexus'), ('USER', 'nexus'), ('LOGNAME', 'nexus'), ('PATH', '/usr/bin:/bin'), ('LANG', 'C.UTF-8'),
                        ('WINEDEBUG', 'err+all,fixme-all')):
        s.cmd('env', '%s=%s' % (name, value))
    s.cmd('host', str(args.http_port)); s.cmd('cd', '/home/nexus')
    s.cmd('mark', 'PHASE prefix')
    s.run('boot', 1800, '/usr/bin/wineboot', '--init')
    s.cmd('du', '/home/nexus/.wine')
    s.run('nocrashdlg', 300, WINE, 'reg', 'add', r'HKCU\Software\Wine\WineDbg', '/v', 'ShowCrashDialog', '/t', 'REG_DWORD', '/d', '0', '/f')
    if 'console' in PHASES:
        s.cmd('mark', 'PHASE console')
        s.run('ver', 300, WINE, 'cmd', '/c', 'ver')
        s.run('regq', 300, WINE, 'reg', 'query', r'HKLM\Software\Microsoft\Windows NT\CurrentVersion', '/v', 'ProductName')
        s.run('hostname', 300, WINE, 'hostname')
        s.run('ipconfig', 300, WINE, 'ipconfig', '/all')
        s.run('cmd32', 300, WINE, r'C:\windows\syswow64\cmd.exe', '/c', 'echo wow64 %PROCESSOR_ARCHITECTURE%')
        for bits, peer in ((64, 32), (32, 64)):
            s.run('wt%d' % bits, 900, WINE, '/home/nexus/wt/winetest%d.exe' % bits, 'all', '--peer', z('/home/nexus/wt/winetest%d.exe' % peer),
                  '--host', '10.0.2.2:%d' % args.tcp_port)
        s.run('cxx64', 300, WINE, '/home/nexus/wt/cxx64.exe'); s.run('cxx32', 300, WINE, '/home/nexus/wt/cxx32.exe')
    if PHASES & {'apps', 'gui', 'desktop'}:
        s.cmd('mark', 'PHASE apps')
        # MSI first: 7-Zip's packages take INSTALLDIR from an existing 7-Zip installation (AppSearch on Software\7-Zip Path)
        for tag, package, folder in (('msi64', '7z2604-x64.msi', 'SevenZipMsi64'), ('msi32', '7z2604.msi', 'SevenZipMsi32')):
            s.run(tag, 900, WINE, 'msiexec', '/i', z('/home/nexus/apps/' + package), '/qn', 'INSTALLDIR=C:\\' + folder)
            s.cmd('ls', '/home/nexus/.wine/drive_c/' + folder)
            s.run(tag + 'run', 300, WINE, 'C:\\%s\\7z.exe' % folder, 't', z('/home/nexus/apps/SumatraPDF-3.6.1-64.zip'))
            s.run(tag + 'x', 900, WINE, 'msiexec', '/x', z('/home/nexus/apps/' + package), '/qn')
            s.cmd('exists', '/home/nexus/.wine/drive_c/%s/7z.exe' % folder)
        s.run('7zinst64', 600, WINE, '/home/nexus/apps/7z2604-x64.exe', '/S')
        s.run('7zinst32', 600, WINE, '/home/nexus/apps/7z2604.exe', '/S')
        s.cmd('ls', '/home/nexus/.wine/drive_c/Program Files/7-Zip'); s.cmd('ls', '/home/nexus/.wine/drive_c/Program Files (x86)/7-Zip')
        s.run('7za64', 600, WINE, SEVENZIP64, 'a', '-r', z('/home/nexus/t64.7z'), z('/home/nexus/wtdata/') + '*')
        s.run('7zt64', 600, WINE, SEVENZIP64, 't', z('/home/nexus/t64.7z'))
        s.run('7zx64', 600, WINE, SEVENZIP64, 'x', '-o' + z('/home/nexus/out64'), z('/home/nexus/t64.7z'))
        s.run('7za32', 600, WINE, SEVENZIP32, 'a', '-tzip', z('/home/nexus/t32.zip'), z('/home/nexus/wtdata/') + '*')
        s.run('7zx32', 600, WINE, SEVENZIP32, 'x', '-o' + z('/home/nexus/out32'), z('/home/nexus/t32.zip'))
        for name in DATA_FILES:
            for out in ('out64', 'out32'): s.cmd('cmp', '/home/nexus/wtdata/' + name, '/home/nexus/%s/%s' % (out, name))
        s.run('msiputty', 900, WINE, 'msiexec', '/i', z('/home/nexus/apps/putty-64bit-0.85-installer.msi'), '/qn')
        s.cmd('ls', '/home/nexus/.wine/drive_c/Program Files/PuTTY')
        s.run('nppinst', 900, WINE, '/home/nexus/apps/npp.8.9.8.1.Installer.exe', '/S')
        s.cmd('ls', '/home/nexus/.wine/drive_c/Program Files (x86)/Notepad++')
        # Inno Setup: both editions install silently; their compilers build an installer that installs and uninstalls
        silent = ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-']
        s.run('inno64', 900, WINE, '/home/nexus/apps/innosetup-7.1.0-x64.exe', *silent, '/ALLUSERS', r'/DIR=C:\Inno64')
        s.cmd('exists', '/home/nexus/.wine/drive_c/Inno64/ISCC.exe')
        s.run('iscc64', 600, WINE, r'C:\Inno64\ISCC.exe', z('/home/nexus/wtdata/nexus.iss'))
        s.run('innosetup', 600, WINE, z('/home/nexus/innoout/nexus-inno-setup.exe'), *silent)
        for name in ('small.txt', 'big.bin'): s.cmd('cmp', '/home/nexus/wtdata/' + name, '/home/nexus/.wine/drive_c/NexusInnoTest/' + name)
        s.run('innounins', 600, WINE, r'C:\NexusInnoTest\unins000.exe', *silent[:3])
        s.cmd('waitgone', '/home/nexus/.wine/drive_c/NexusInnoTest/big.bin', '120')
        s.run('inno32', 900, WINE, '/home/nexus/apps/innosetup-7.1.0-x86.exe', *silent, '/ALLUSERS', r'/DIR=C:\Inno32')
        s.run('iscc32', 600, WINE, r'C:\Inno32\ISCC.exe', '/Fnexus-inno-setup32', z('/home/nexus/wtdata/nexus.iss'))
        s.cmd('ls', '/home/nexus/innoout')
        s.run('unzipnpp', 600, WINE, SEVENZIP64, 'x', r'-oC:\npp64', z('/home/nexus/apps/npp.8.9.8.1.portable.x64.zip'))
        s.run('unzipsum64', 600, WINE, SEVENZIP64, 'x', r'-oC:\sumatra64', z('/home/nexus/apps/SumatraPDF-3.6.1-64.zip'))
        s.run('unzipsum32', 600, WINE, SEVENZIP32, 'x', r'-oC:\sumatra32', z('/home/nexus/apps/SumatraPDF-3.6.1.zip'))
        s.cmd('sleep', '3000')
        s.cmd('tree', '/home/nexus/.local/share/applications/wine', '6')
        s.cmd('cat', '/home/nexus/.local/share/applications/wine/Programs/PuTTY (64-bit)/PuTTY.desktop')
        s.cmd('tree', '/home/nexus/.local/share/icons/hicolor', '3')
    if 'gui' in PHASES:
        s.cmd('mark', 'PHASE gui')
        for tag, argv in GUI_GATES:
            s.bg(tag, *argv); s.cmd('gate', tag, '900'); s.cmd('wait', tag, '120')
        s.cmd('cat', '/home/nexus/.wine/drive_c/users/nexus/notepad-saved.txt')
        s.cmd('cat', '/home/nexus/wtdata/npp.txt'); s.cmd('cat', '/home/nexus/wtdata/npp32.txt')
    if 'desktop' in PHASES:
        s.cmd('mark', 'PHASE desktop')
        s.cmd('exists', '/home/nexus/Downloads')
        s.cmd('env', 'WTGUI_LOG=' + z('/home/nexus/files-gui.log'))
        s.bg('files', '/bin/aurora-app', 'files', '/home/nexus/wtfiles'); s.cmd('gate', 'files', '600'); s.cmd('wait', 'files', '60')
        s.cmd('unset', 'WTGUI_LOG'); s.cmd('cat', '/home/nexus/files-gui.log')
        # xdg-open execs "wine start /unix FILE", which returns once the program is started: the job's status is start.exe's.
        # The program itself is followed by its processes: listed while its window is open (after gate xdgopen), they must
        # exit 0 once the test closed the window (gate xdgclose)
        s.bg('xdgopen', '/bin/xdg-open', '/home/nexus/Downloads/putty-download.exe'); s.cmd('gate', 'xdgopen', '600')
        s.cmd('procs', 'putty-download.exe'); s.cmd('gate', 'xdgclose', '600'); s.cmd('procwait', 'putty-download.exe', '60')
        s.cmd('wait', 'xdgopen', '60')
        s.cmd('gate', 'spotlight', '900')
        s.cmd('gate', 'terminal', '600'); s.cmd('cat', '/home/nexus/term.txt')
    s.cmd('mark', 'PHASE cleanup')
    s.cmd('procwait', '.exe', '120'); s.cmd('procwait', 'wineserver', '60'); s.cmd('procs', 'wine'); s.cmd('sync')
    return s.text()


# ------------------------------------------------------------------ VM and conductor
class WineVm(aurora_vm.Vm):
    def __init__(self, work, disk, qmp_port, accel='auto', smp=2, memory='3072', machine='pc', width=1920, height=1080):
        """aurora_vm.Vm's boot (qcow2 overlay over disk, serial log file, QMP) with a choice of machine."""
        self.work = Path(work); self.serial, self.overlay = self.work / 'serial.log', self.work / 'overlay.qcow2'
        for old in self.work.iterdir():
            if old.is_file() and old.suffix in ('.log', '.qcow2', '.png', '.ppm'): old.unlink()
        subprocess.run([aurora_vm.QEMU_IMG, 'create', '-q', '-f', 'qcow2', '-b', str(Path(disk).resolve()), '-F', 'raw', str(self.overlay)], check=True)
        self.accel = ('whpx' if aurora_vm.whpx_usable() else 'tcg') if accel == 'auto' else accel
        acceleration = ['-accel', 'tcg', '-cpu', 'max'] if self.accel == 'tcg' else ['-accel', 'whpx,kernel-irqchip=off', '-cpu', aurora_vm.CPU]
        boot = ['-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-boot', 'a']
        if machine == 'q35':
            devices = ['-machine', 'q35', *boot, '-drive', f'if=none,id=root,file={self.overlay},format=qcow2', '-device', 'nvme,serial=nexus-root,drive=root',
                       '-netdev', 'user,id=network', '-device', 'e1000e,netdev=network']
        else:
            devices = [*boot, '-drive', f'file={self.overlay},format=qcow2,if=ide', '-netdev', 'user,id=network', '-device', 'rtl8139,netdev=network']
        command = [aurora_vm.QEMU, *acceleration, '-smp', str(smp), '-m', memory, *devices, '-vga', 'none', '-device', 'virtio-vga', '-display', 'none',
                   '-device', 'isa-debug-exit,iobase=0xf4,iosize=4', '-serial', f'file:{self.serial}',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off', '-no-reboot']
        (self.work / 'cmd.json').write_text(json.dumps(command, indent=1))
        self.proc = subprocess.Popen(command, stdout=(self.work / 'qemu.log').open('wb'), stderr=subprocess.STDOUT)
        print('VM', self.proc.pid, self.accel, machine, flush=True)
        try: nexus_paths.unthrottle(self.proc.pid)
        except Exception: pass
        self.q = aurora_vm.Qmp(qmp_port); self.pos = [width // 2, height // 2]

    KEYS = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal', ';': 'semicolon', "'": 'apostrophe',
            ',': 'comma', '\\': 'backslash', '\t': 'tab'}
    SHIFTED = {':': 'semicolon', '_': 'minus', '>': 'dot', '"': 'apostrophe', '|': 'backslash', '!': '1', '&': '7', '%': '5', '*': '8',
               '(': '9', ')': '0', '?': 'slash', '<': 'comma', '+': 'equal'}

    def type(self, text):
        for ch in text:
            if ch in self.SHIFTED: self.key('shift', True); self.tap(self.SHIFTED[ch]); self.key('shift', False)
            elif ch.isupper(): self.key('shift', True); self.tap(ch.lower()); self.key('shift', False)
            else: self.tap(self.KEYS.get(ch, ch))

    def home(self):
        """Re-synchronise the tracked pointer: push it into the top-left corner (Aurora clamps it at 0,0)."""
        self.move(-2600, -1600, step=80, delay=0.01); self.pos = [0, 0]; time.sleep(0.3)

    def drag(self, x, y, dx, dy, hold=1.5):
        """Press at x,y, hold (Wine starts xdg_toplevel.move/resize while the button is down), move, release."""
        self.move_to(x, y); time.sleep(0.4); self.button('left', True); time.sleep(hold)
        self.move(dx, dy, step=5, delay=0.08); self.pos = [x + dx, y + dy]; time.sleep(1.0); self.button('left', False)

    def double_click(self):
        for _ in range(2):
            self.q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': True}}]}); time.sleep(0.05)
            self.q.cmd('input-send-event', {'events': [{'type': 'btn', 'data': {'button': 'left', 'down': False}}]}); time.sleep(0.12)

    def alt(self, key): self.key('alt', True); self.tap(key); self.key('alt', False)
    def ctrl(self, key): self.key('ctrl', True); self.tap(key); self.key('ctrl', False)


class Conductor:
    def __init__(self, vm): self.vm = vm; self.mark = 0

    def log(self): return self.vm.text()
    def here(self): self.mark = len(self.log()); return self.mark

    def wait(self, pattern, seconds=60, after=None):
        """First match of pattern after `after` (default: the conductor's mark), or None."""
        start = self.mark if after is None else after; end = time.time() + seconds
        while True:
            m = re.search(pattern, self.log()[start:])
            if m or time.time() > end or self.vm.proc.poll() is not None: return m
            time.sleep(0.25)

    def all(self, pattern, after=None): return re.findall(pattern, self.log()[self.mark if after is None else after:])

    def window(self, title, seconds=120, after=None):
        """(app, title, x, y) of a toplevel whose title (when mapped or changed later) matches the regex `title`."""
        m = self.wait(r'AURORA (?:WINDOW|TITLE) app=(\S*) title=(%s) x=(-?\d+) y=(-?\d+)\r?\n' % title, seconds, after)
        return (m.group(1), m.group(2), int(m.group(3)), int(m.group(4))) if m else None

    def shot(self, name):
        try: return self.vm.shot(name)
        except Exception as error: print('screenshot failed', name, error, flush=True); return None

    def close(self, win, settle=0.8):
        """Focus a window through its title bar, then Alt+F4 (Aurora sends xdg_toplevel.close)."""
        self.vm.move_to(win[2] + 140, win[3] + 12); time.sleep(0.3); self.vm.click(); time.sleep(settle); self.vm.alt('f4')


def colours_in(picture, x, y, w, h, step=6):
    found = set()
    for yy in range(max(0, y), min(picture.height, y + h), step):
        for xx in range(max(0, x), min(picture.width, x + w), step): found.add(picture.getpixel((xx, yy)))
    return found


def client_area(picture, win, top=56, w=392, h=244, step=6):
    """(colours, most common colour, share of samples off it) of a window's client area. Aurora windows are client-side
    decorated: the surface at the window origin starts with the frame and caption Wine draws (and a menu bar below), which
    alone give many colours, so the samples start below them and inside the left frame."""
    x, y = win[2] + 8, win[3] + top
    samples = [picture.getpixel((xx, yy)) for yy in range(max(0, y), min(picture.height, y + h), step)
               for xx in range(max(0, x), min(picture.width, x + w), step)]
    if not samples: return 0, None, 0.0
    background, count = Counter(samples).most_common(1)[0]
    return len(set(samples)), background, 1 - count / len(samples)


def near(colour, wanted, tolerance=40): return all(abs(a - b) <= tolerance for a, b in zip(colour, wanted))


# ------------------------------------------------------------------ gates (the guest script waits while these run)
def gate_wtgui(c, bits):
    phase, vm, tag = 'gui', c.vm, 'wtgui%d' % bits
    ready = c.wait(r'GUI READY bits=%d hwnd=\S+ client=(\d+),(\d+) size=(\d+),(\d+) window=(-?\d+),(-?\d+),(-?\d+),(-?\d+) '
                   r'caption=(\d+),(\d+),(\d+) frame=(\d+),(\d+)' % bits, 240)
    win = c.window('Wine GUI Test', 60)
    if not check(phase, '%s: main window appears as an Aurora toplevel with its title' % tag, ready and win, 'ready=%s window=%s' % (bool(ready), win)): return
    cx, cy, w0, h0 = (int(ready.group(i)) for i in (1, 2, 3, 4))
    ww, wh = int(ready.group(7)) - int(ready.group(5)), int(ready.group(8)) - int(ready.group(6))
    cxsize, cycaption, fx, fy = int(ready.group(9)), int(ready.group(11)), int(ready.group(12)), int(ready.group(13))
    wx, wy = win[2], win[3]
    def client(x, y): return wx + cx + x, wy + cy + y
    def bars_ok(picture):
        expected = [((80, 85), (255, 0, 0)), ((175, 85), (0, 255, 0)), ((270, 85), (0, 0, 255)), ((365, 85), (255, 255, 255)), ((15, 15), (0, 0x80, 0x80))]
        got = [picture.getpixel(client(*point)) for point, _ in expected]
        return all(near(g, colour, 8) for g, (_, colour) in zip(got, expected)), got
    vm.home(); time.sleep(1.5); picture = c.shot('%s-ready' % tag)
    if picture: ok, got = bars_ok(picture); check(phase, '%s: GDI painting reaches the screen (colour bars at their client positions)' % tag, ok, got)
    c.here(); vm.move_to(*client(100, 30)); vm.click()
    m = c.wait(r'GUI CLICK window=1 x=(\d+) y=(\d+)', 20)
    check(phase, '%s: mouse click arrives at the clicked client position' % tag, m and abs(int(m.group(1)) - 100) <= 1 and abs(int(m.group(2)) - 30) <= 1, m and m.group(0))
    c.here(); vm.type('Nexus 42')
    check(phase, '%s: keyboard typing reaches the edit box (shift, digits, space)' % tag, c.wait(r'GUI EDIT text=Nexus 42\r?\n', 20), c.all(r'GUI EDIT text=[^\n]*')[-1:])
    if bits == 32:
        c.here(); vm.move_to(wx + ww - fx - cxsize // 2 - 2, wy + fy + cycaption // 2); vm.click()
        return check(phase, '%s: caption close button ends the program (WM_CLOSE, exit 0)' % tag, c.wait(r'GUI EXIT code=0', 30), '')
    c.here(); vm.move_to(*client(345, 183)); vm.click(); check(phase, 'wtgui64: push button click (BN_CLICKED)', c.wait(r'GUI BUTTON pressed', 15), '')
    c.here(); vm.move_to(*client(25, 216)); vm.click(); check(phase, 'wtgui64: check box toggles', c.wait(r'GUI CHECK state=1', 15), '')
    c.here(); vm.move_to(*client(240, 230)); vm.click(); check(phase, 'wtgui64: list box selection', c.wait(r'GUI LIST selected=1', 15), '')
    vm.move_to(*client(100, 30)); vm.click(); time.sleep(0.5)
    c.here(); vm.alt('f'); time.sleep(1.2); c.shot('wtgui64-menu'); vm.tap('n')
    second = c.window('Wine Second Window', 30)
    check(phase, 'wtgui64: menu (Alt+F, N) opens a second top-level window', c.wait(r'GUI WINDOW n=2 created', 20) and second, second)
    if second:
        time.sleep(1.5); c.shot('wtgui64-two-windows'); c.here(); vm.alt('f4')
        check(phase, 'wtgui64: Alt+F4 closes the focused second window only', c.wait(r'GUI WINDOW n=2 closed', 20) and not c.all(r'GUI EXIT'), '')
    time.sleep(1); vm.move_to(*client(100, 30)); vm.click(); time.sleep(0.8)
    c.here(); vm.tap('f2')
    dialog = c.window('Wine Test Dialog', 30)
    check(phase, 'wtgui64: modal dialog (template, DS_CENTER) appears as its own window', c.wait(r'GUI DIALOG open', 20) and dialog, dialog)
    time.sleep(1.5); c.shot('wtgui64-dialog'); c.here(); vm.tap('ret')
    check(phase, 'wtgui64: Enter activates the default button and ends the dialog (IDOK)', c.wait(r'GUI DIALOG closed result=1', 20), '')
    time.sleep(1); vm.move_to(*client(100, 30)); vm.click(); time.sleep(0.8)
    c.here(); vm.alt('h'); time.sleep(1); vm.tap('a')
    about = c.window('About', 30); time.sleep(1.5); c.shot('wtgui64-messagebox'); vm.tap('ret')
    check(phase, 'wtgui64: MessageBox (Help, About) appears and Enter closes it', about and c.wait(r'GUI ABOUT closed', 20), about)
    time.sleep(1); vm.move_to(*client(100, 30)); vm.click(); time.sleep(0.8)
    # maximize through Wine's caption button (Wine draws the non-client area), restore with SW_RESTORE (F6)
    c.here(); vm.move_to(wx + ww - fx - cxsize - cxsize // 2 - 2, wy + fy + cycaption // 2); vm.click()
    first = c.wait(r'GUI SIZE window=1 type=2 w=(\d+) h=(\d+)', 20); time.sleep(2.5)
    sizes = c.all(r'GUI SIZE window=1 type=2 w=(\d+) h=(\d+)'); c.shot('wtgui64-maximized')
    check(phase, 'wtgui64: maximize button gives SIZE_MAXIMIZED with the size Aurora configured', first and sizes and int(sizes[-1][0]) >= 1800, sizes)
    c.here(); vm.tap('f6')
    check(phase, 'wtgui64: restore (SW_RESTORE) returns to the normal size', c.wait(r'GUI SIZE window=1 type=0 w=%d h=%d' % (w0, h0), 20), c.all(r'GUI SIZE[^\n]*'))
    time.sleep(2); picture = c.shot('wtgui64-restored')
    if picture: ok, got = bars_ok(picture); check(phase, 'wtgui64: restored window is back at its place and repainted', ok, got)
    # interactive resize from the bottom-right corner of Wine's frame (xdg_toplevel.resize)
    c.here(); vm.drag(wx + ww - 2, wy + wh - 2, 60, 40)
    check(phase, 'wtgui64: interactive resize by dragging the frame corner', c.wait(r'GUI SIZE window=1 type=0 w=%d h=%d' % (w0 + 60, h0 + 40), 20),
          c.all(r'GUI SIZE[^\n]*')[-3:])
    ww += 60; wh += 40; time.sleep(1); c.shot('wtgui64-resized')
    # interactive move by dragging the caption (xdg_toplevel.move)
    vm.drag(wx + 100, wy + fy + cycaption // 2, 300, 150); wx += 300; wy += 150
    time.sleep(2); picture = c.shot('wtgui64-moved')
    if picture: ok, got = bars_ok(picture); check(phase, 'wtgui64: window follows a caption drag (interactive move)', ok, got)
    # minimize with Wine's caption button. NexusOS's Wayland driver (arch/x86_64/winewayland_build.py; Wine 10.0's has no
    # xdg minimize: the window collapsed into a 160x31 caption bar) minimizes the toplevel: Aurora hides it and shows its
    # picture in the dock; Alt+Tab gives it the keyboard again and the driver restores it (SC_RESTORE)
    c.here(); vm.move_to(wx + ww - fx - 2 * cxsize - cxsize // 2 - 2, wy + fy + cycaption // 2); vm.click()
    check(phase, 'wtgui64: the minimize button minimizes the window (SIZE_MINIMIZED)', c.wait(r'GUI SIZE window=1 type=1', 20), c.all(r'GUI (?:SIZE|NC|SYS)[^\n]*'))
    minimized = c.wait(r'AURORA MINIMIZE app=\S+', 20)
    check(phase, 'wtgui64: Aurora minimizes the Wayland window (xdg_toplevel.set_minimized, dock picture)', minimized, c.all(r'AURORA (?:MINIMIZE|ACTIVATE)[^\n]*'))
    time.sleep(2); c.shot('wtgui64-minimized')
    c.here()
    for _ in range(4):   # Alt+Tab until the switcher brings back this window (the switcher lists minimized windows too)
        mark = len(c.log()); vm.key('alt', True); vm.tap('tab'); time.sleep(0.8); vm.key('alt', False)
        switched = c.wait(r'AURORA SWITCHED app=(\S+)', 10, mark)
        if switched and switched.group(1) == win[0]: break
    check(phase, 'wtgui64: Alt+Tab restores the minimized window (keyboard focus -> SC_RESTORE)', c.wait(r'GUI SIZE window=1 type=0 w=%d h=%d' % (w0 + 60, h0 + 40), 20),
          c.all(r'(?:GUI (?:SIZE|SYS)|AURORA (?:SWITCH|ACTIVATE))[^\n]*'))
    time.sleep(2); picture = c.shot('wtgui64-unminimized')
    if picture: ok, got = bars_ok(picture); check(phase, 'wtgui64: the restored window is shown again at its place', ok, got)
    # the program minimizes itself and restores itself 2 s later (ShowWindow(SW_RESTORE) from a timer): the driver asks
    # Aurora to show the window again through xdg_activation_v1
    time.sleep(1); vm.move_to(*client(100, 30)); vm.click(); time.sleep(0.8)
    c.here(); vm.tap('f11')   # (F10 is the Windows menu key)
    check(phase, 'wtgui64: a program-requested restore (SW_RESTORE) shows the window again (xdg_activation_v1)',
          c.wait(r'GUI SIZE window=1 type=1', 20) and c.wait(r'AURORA ACTIVATION app=\S+ granted', 20) and c.wait(r'GUI SIZE window=1 type=0', 20),
          c.all(r'(?:GUI SIZE|AURORA (?:MINIMIZE|ACTIVATION|ACTIVATE))[^\n]*'))
    time.sleep(2); picture = c.shot('wtgui64-selfrestored')
    if picture: ok, got = bars_ok(picture); check(phase, 'wtgui64: the self-restored window is visible and repainted', ok, got)
    c.here(); vm.move_to(wx + ww - fx - cxsize // 2 - 2, wy + fy + cycaption // 2); vm.click()
    return check(phase, 'wtgui64: caption close button ends the program (WM_CLOSE, exit 0)', c.wait(r'GUI EXIT code=0', 30), '')


def gate_notepad(c):
    win = c.window(r'Untitled - Notepad', 240)
    if not check('gui', 'notepad: window appears', win, ''): return
    _, _, x, y = win; c.vm.home(); time.sleep(2)
    c.vm.move_to(x + 200, y + 150); c.vm.click(); time.sleep(0.5)
    c.vm.type('Hello from NexusOS notepad'); time.sleep(0.8); c.shot('notepad-typed')
    c.here(); c.vm.ctrl('s')
    save = c.window('Save As', 60)
    if not check('gui', 'notepad: Ctrl+S opens the common Save As dialog', save, ''): return
    time.sleep(2); c.shot('notepad-saveas')
    c.vm.type('C:\\users\\nexus\\notepad-saved.txt\n')
    titled = c.window(r'notepad-saved[^\n]*Notepad', 30)
    check('gui', 'notepad: Save As writes the file and the title shows its name', titled, c.all(r'AURORA TITLE[^\n]*'))
    time.sleep(1); c.close(win)


def gate_npp(c, tag, filename):
    win = c.window(r'[^\n]*%s - Notepad\+\+[^\n]*' % re.escape(filename), 240)
    if not check('gui', '%s: Notepad++ opens the file given on its command line' % tag, win, c.all(r'AURORA (?:WINDOW|TITLE) app=notepad[^\n]*')): return
    _, _, x, y = win; c.vm.home(); time.sleep(3); c.shot(tag + '-open')
    c.vm.move_to(x + 300, y + 220); c.vm.click(); time.sleep(0.5)
    c.vm.ctrl('end'); time.sleep(1.0)   # let the editor settle: a key release that reaches it late would auto-repeat
    c.vm.type(' world from NexusOS'); time.sleep(0.8); c.shot(tag + '-edited')
    c.here(); c.vm.ctrl('s'); time.sleep(3); c.shot(tag + '-saved')
    c.close(win)


def gate_window(c, name, title, verify=None, settle=4):
    """Wait for an application's window, check its client area is drawn, run `verify` on the screenshot, close it with Alt+F4."""
    win = c.window(title, 240)
    if not check('gui', '%s: window appears (%s)' % (name, title), win, c.all(r'AURORA (?:WINDOW|TITLE)[^\n]*')): return
    c.vm.home(); time.sleep(settle); picture = c.shot(name)
    if picture:
        # the program's own drawing on its window/dialog background (light in Wine's default theme): a blank client area
        # gives one colour, a missing or transparent surface the dark wallpaper (measured: 8-43 colours, 5-49% drawn)
        colours, background, drawn = client_area(picture, win)
        check('gui', '%s: the program draws its client area (%d colours, %.0f%% off its background)' % (name, colours, 100 * drawn),
              colours >= 4 and drawn >= 0.02 and background is not None and min(background) >= 192,
              'colours=%d background=%s drawn=%.3f' % (colours, background, drawn))
        if verify: verify(picture, win)
    c.close(win)


def sumatra_verify(name):
    def verify(picture, win):
        colours = colours_in(picture, win[2], win[3], 1920 - win[2], 1000 - win[3], 4)
        check('gui', '%s: the PDF page is rendered (its blue box is on screen)' % name, any(near(colour, (26, 102, 230), 24) for colour in colours), len(colours))
    return verify


def gate_files(c):
    win = c.window('Files', 60)
    if not check('desktop', 'Files: window opens on a folder with a Windows program', win, ''): return
    _, _, x, y = win; c.vm.home(); time.sleep(2); c.shot('files')
    c.here(); c.vm.move_to(x + 1 + 168 + 120, y + 28 + 1 + 82 + 13); c.vm.double_click()   # first row: content y 82..108, right of the 168 px drives sidebar (aurora_extra.c, aurora_places.c)
    started_wine = c.wait(r'AURORA FILES WINE pid=\d+ path=/home/nexus/wtfiles/HelloWindows\.exe', 30)
    check('desktop', 'Files: double-click on an .exe starts it with Wine (wine start /unix)', started_wine, '')
    gui = c.window('Wine GUI Test', 180)
    check('desktop', 'Files: the Windows program shows its window', gui, '')
    if gui:
        time.sleep(2); c.shot('files-launched'); c.close(gui)
        time.sleep(4)
    c.close(win)


def gate_xdgopen(c):
    begin = c.log().rfind('WR START tag=xdgopen')   # the window can map before the gate line
    opened = c.wait(r'NEXUS OPEN WINDOWS path=/home/nexus/Downloads/putty-download\.exe', 30, after=begin)
    win = c.window('PuTTY Configuration', 180, after=begin)
    check('desktop', 'xdg-open (Chrome opening a finished download) runs a downloaded .exe with Wine', opened and win, opened and opened.group(0))
    if win: c.vm.home(); time.sleep(2); c.shot('xdgopen-putty')
    c.xdg_window = win   # closed at the next gate, once the guest listed the program's processes


def gate_xdgclose(c):
    win = getattr(c, 'xdg_window', None)
    if win: c.close(win)


def gate_spotlight(c):
    c.vm.home(); c.here(); c.vm.ctrl('spc'); time.sleep(1.2)
    names = c.wait(r'AURORA WINEAPPS count=([1-9]\d*) names=([^\n]*)', 15)
    c.vm.type('putty'); time.sleep(1.2); c.shot('spotlight-putty')
    check('desktop', 'Spotlight lists the installed Windows programs (winemenubuilder .desktop entries)', names and 'PuTTY' in names.group(2).split('|'),
          names and names.group(0))
    c.here(); c.vm.tap('ret')
    launched = c.wait(r'AURORA LAUNCH app=wine:PuTTY pid=\d+', 30); win = c.window('PuTTY Configuration', 180)
    check('desktop', 'Spotlight starts PuTTY through its .desktop entry', launched and win, c.all(r'AURORA LAUNCH[^\n]*'))
    icons = c.all(r'AURORA WINEAPPS ICON app=([^\n]*?) size=(\d+)', after=0)
    check('desktop', 'Windows program icons (winemenubuilder PNGs) are drawn in Spotlight/Launchpad', any(name == 'PuTTY' for name, _ in icons), icons[:8])
    if win: time.sleep(2); c.shot('spotlight-putty-window'); c.close(win)
    time.sleep(3)
    # Launchpad: 17 fixed entries plus the Windows programs, 35 per page at 1080p (kernel/spotlight.c), page dots at y=960
    count = int(names.group(1)) if names else 0; items = 17 + count; pages = max(1, -(-items // 35))
    def dots(picture): return [sum(picture.getpixel((960 - (pages - 1) * 9 + p * 18, 960))) // 3 for p in range(pages)]
    c.vm.tap('f4'); time.sleep(1.5); picture = c.shot('launchpad')
    if pages > 1 and picture:
        first = dots(picture); c.vm.move_to(960 - (pages - 1) * 9 + 18, 960); c.vm.click()
        c.vm.move_to(960, 600); time.sleep(1.5)   # the pointer must not cover the dots in the screenshot
        picture = c.shot('launchpad-page2'); second = dots(picture) if picture else []
        check('desktop', 'Launchpad pages %d entries (%d pages) and switches page with its dots' % (items, pages),
              first and second and first[0] > first[1] + 40 and second[1] > second[0] + 40, (first, second))
    c.vm.tap('f4'); time.sleep(1)
    c.here(); c.vm.ctrl('spc'); time.sleep(1.2); c.vm.type('wine conf'); time.sleep(1.2); c.shot('spotlight-winecfg'); c.vm.tap('ret')
    launched = c.wait(r'AURORA LAUNCH app=winecfg pid=\d+', 30); win = c.window('Wine configuration', 180)
    check('desktop', 'Spotlight "Wine Configuration" opens winecfg', launched and win, c.all(r'AURORA LAUNCH[^\n]*'))
    if win: time.sleep(2); c.shot('spotlight-winecfg-window'); c.close(win)
    time.sleep(2)
    c.here(); c.vm.ctrl('spc'); time.sleep(1.2); c.vm.type('windows prog'); time.sleep(1.2); c.vm.tap('ret')
    launched = c.wait(r'AURORA LAUNCH app=wineprograms pid=\d+', 30); win = c.window(r'[^\n]*Programs[^\n]*', 180)
    check('desktop', 'Spotlight "Windows Programs" opens the Start-menu folder in Wine\'s explorer', launched and win, c.all(r'AURORA (?:LAUNCH|WINDOW)[^\n]*'))
    if win: time.sleep(3); c.shot('windows-programs'); c.close(win)
    time.sleep(2)


def gate_terminal(c):
    c.vm.home(); c.here(); c.vm.ctrl('spc'); time.sleep(1.2); c.vm.type('terminal'); time.sleep(1.2); c.vm.tap('ret')
    win = c.window('Terminal', 60)
    if not check('desktop', 'Terminal opens', win, ''): return
    time.sleep(2); c.here()
    c.vm.type('wine cmd /c echo terminal-ok > Z:/home/nexus/term.txt\n')
    done = c.wait(r'AURORA COMMAND DONE status=(\d+)', 240)
    check('desktop', 'the Terminal runs "wine cmd" (exit status 0)', done and done.group(1) == '0', done and done.group(0))
    time.sleep(1); c.shot('terminal'); c.close(win)


# ------------------------------------------------------------------ main
own_disk = args.disk is None
in_place = bool(args.disk and args.stage_in_place)   # a scratch disk owned by the caller: staged into, booted through the overlay
if in_place and 'new folder' in str(args.disk.resolve()).lower(): raise SystemExit('--stage-in-place refuses the real desktop tree')
disk = args.disk.resolve() if in_place else (WORK / 'disk.img').resolve()   # otherwise a copy: --disk may be a desktop disk in use
servers = vm = None
try:
    t0 = time.time()
    programs = build_programs()
    apps = wine_apps.fetch(args.apps_cache)
    data = data_files(WORK / 'data')
    if not in_place: disk.unlink(missing_ok=True)
    if in_place: pass
    elif not own_disk: aurora_vm.sparse_copy(args.disk, disk)
    else:
        (WORK / 'install').mkdir(exist_ok=True)
        aurora_vm.sparse_copy(args.source, disk)
        subprocess.run([sys.executable, '-I', str(ROOT / 'arch/x86_64/wine_install.py'), str(disk), '--only', 'wine', '--grow', '6144',
                        '--cache', str(args.cache), '--report', str(WORK / 'install' / 'report.json')], check=True)
    result['prepare_seconds'] = round(time.time() - t0, 1)
    script = guest_script(); (WORK / 'wine-test.script').write_text(script)
    files = {'sbin/init': OUT / 'desktop.elf', 'bin/aurora-app': OUT / 'aurora_app.elf', 'bin/xdg-open': OUT / 'nexus-installer-ui.elf',
             'usr/bin/nexus-wine-runner': OUT / 'wine_runner_user.elf', 'nexus/wine-test.script': script,
             'nexus/desktop.args': '--aurora\n/usr/bin/nexus-wine-runner\n/nexus/wine-test.script\n',
             'home/nexus/wtfiles/HelloWindows.exe': programs['wtgui32.exe'], 'home/nexus/Downloads/putty-download.exe': apps['putty-w64.exe']}
    for name, path in programs.items(): files['home/nexus/wt/' + name] = path
    for name, path in apps.items(): files['home/nexus/apps/' + name] = path
    for name in DATA_FILES + ['test.pdf', 'npp.txt', 'npp32.txt', 'nexus.iss']: files['home/nexus/wtdata/' + name] = data / name
    aurora_vm.stage(disk, files)
    servers = wine_host.HostServers(args.http_port, args.tcp_port)
    vm = WineVm(WORK, disk, args.qmp_port, args.accel, smp=2, memory='3072', machine=args.machine)
    result['accel'] = vm.accel; result['machine'] = args.machine
    c = Conductor(vm)
    gates = {'gui64': lambda: gate_wtgui(c, 64), 'gui32': lambda: gate_wtgui(c, 32), 'notepad': lambda: gate_notepad(c),
             'npp64': lambda: gate_npp(c, 'npp64', 'npp.txt'), 'npp32': lambda: gate_npp(c, 'npp32', 'npp32.txt'),
             'winecfg': lambda: gate_window(c, 'winecfg', 'Wine configuration'), 'regedit': lambda: gate_window(c, 'regedit', 'Registry Editor'),
             'putty64': lambda: gate_window(c, 'putty64', 'PuTTY Configuration'), 'putty32': lambda: gate_window(c, 'putty32', 'PuTTY Configuration'),
             'sumatra64': lambda: gate_window(c, 'sumatra64', r'[^\n]*test\.pdf[^\n]*', sumatra_verify('sumatra64'), settle=6),
             'sumatra32': lambda: gate_window(c, 'sumatra32', r'[^\n]*test\.pdf[^\n]*', sumatra_verify('sumatra32'), settle=6),
             'fm64': lambda: gate_window(c, '7zfm64', '7-Zip'), 'fm32': lambda: gate_window(c, '7zfm32', '7-Zip'),
             'files': lambda: gate_files(c), 'xdgopen': lambda: gate_xdgopen(c), 'xdgclose': lambda: gate_xdgclose(c), 'spotlight': lambda: gate_spotlight(c),
             'terminal': lambda: gate_terminal(c)}
    handled = set(); deadline = time.time() + 3600   # a full run takes about 8 minutes under WHPX
    vm.wait(r'AURORA READY', seconds=300)
    while time.time() < deadline:
        log = vm.text()
        for m in re.finditer(r'\[64\] WR GATE WAIT (\S+)', log):
            name = m.group(1)
            if name in handled: continue
            handled.add(name); c.mark = m.end(); print('-- gate', name, flush=True)
            try: gates[name]()
            except Exception as error: check('desktop' if name in ('files', 'xdgopen', 'xdgclose', 'spotlight', 'terminal') else 'gui', 'gate %s' % name, False, repr(error))
            servers.open_gate(name)
        if re.search(r'\[64\] WR DONE failures=\d+', log) or vm.proc.poll() is not None: break
        time.sleep(0.5)
    log = vm.text()
    done = re.search(r'\[64\] WR DONE failures=(\d+)', log)
    check('run', 'guest script finished', done, log[-400:])
    check('run', 'every guest command succeeded (runner failure count 0)', done and done.group(1) == '0',
          done and re.findall(r'\[64\] WR END tag=\S+ status=(?!0 )\S+[^\n]*', log)[:10])

    def end(tag):
        m = re.search(r'\[64\] WR END tag=%s status=(\S+) ms=(\d+)' % re.escape(tag), log); return (m.group(1), int(m.group(2))) if m else (None, None)

    def output(tag): return '\n'.join(re.findall(r'\[64\] WR %s\| ([^\n]*)' % re.escape(tag), log))
    status, ms = end('boot'); du = re.search(r'WR du path=/home/nexus/.wine bytes=(\d+) files=(\d+)', log)
    result['wineboot_seconds'] = round((ms or 0) / 1000, 1); result['prefix_bytes'] = int(du.group(1)) if du else None
    result['prefix_files'] = int(du.group(2)) if du else None
    check('prefix', 'wineboot --init creates the prefix (%.0f s, %s files)' % ((ms or 0) / 1000, du and du.group(2)), status == '0' and du and int(du.group(2)) > 1000, status)
    check('prefix', 'crash dialog disabled through wine reg add', end('nocrashdlg')[0] == '0', output('nocrashdlg')[-200:])
    if 'console' in PHASES:
        check('console', 'wine cmd /c ver', end('ver')[0] == '0' and 'Microsoft Windows 10' in output('ver'), output('ver')[-200:])
        check('console', 'wine reg query HKLM ... ProductName', end('regq')[0] == '0' and 'Windows 10' in output('regq'), output('regq')[-200:])
        check('console', 'wine hostname', end('hostname')[0] == '0' and output('hostname').strip(), output('hostname')[-120:])
        check('console', 'wine ipconfig lists the NIC with address and gateway (netlink, SIOCGIF*)', end('ipconfig')[0] == '0' and '10.0.2.15' in output('ipconfig')
              and '10.0.2.2' in output('ipconfig'), output('ipconfig')[-500:])
        check('console', '32-bit cmd.exe under WoW64 reports x86', end('cmd32')[0] == '0' and 'wow64 x86' in output('cmd32'), output('cmd32')[-200:])
        for bits in (64, 32):
            text = output('wt%d' % bits); summary = re.search(r'WT SUMMARY bits=%d pass=(\d+) fail=(\d+)' % bits, text)
            result['winetest%d' % bits] = summary and summary.group(0)
            check('console', 'winetest%d: every Win32 API check passes (%s)' % (bits, summary and summary.group(0)),
                  summary and summary.group(2) == '0' and int(summary.group(1)) >= 100 and end('wt%d' % bits)[0] == '0', re.findall(r'WT FAIL [^\n]*', text)[:6])
            check('console', 'C++ exceptions unwind through destructors (%d-bit)' % bits, end('cxx%d' % bits)[0] == '0'
                  and 'caught deep dtors=11' in output('cxx%d' % bits), output('cxx%d' % bits)[-200:])
        check('console', 'winsock TCP from 64- and 32-bit programs reached the host test server', servers.pings >= 2, 'pings=%d' % servers.pings)
    if PHASES & {'apps', 'gui', 'desktop'}:
        for tag in ('msi64', 'msi64run', 'msi64x', 'msi32', 'msi32run', 'msi32x', '7zinst64', '7zinst32', '7za64', '7zt64', '7zx64', '7za32', '7zx32',
                    'msiputty', 'nppinst', 'inno64', 'iscc64', 'innosetup', 'innounins', 'inno32', 'iscc32', 'unzipnpp', 'unzipsum64', 'unzipsum32'):
            check('apps', '%s exits 0' % tag, end(tag)[0] == '0', end(tag))
        check('apps', 'Inno Setup (x64 edition) installs ISCC.exe into /DIR', 'EXISTS path=/home/nexus/.wine/drive_c/Inno64/ISCC.exe yes' in log, '')
        for name in ('small.txt', 'big.bin'):
            check('apps', 'the installer ISCC built installs %s intact' % name, 'CMP /home/nexus/wtdata/%s /home/nexus/.wine/drive_c/NexusInnoTest/%s same' % (name, name) in log, '')
        check('apps', 'its uninstaller (unins000.exe) removes the installed files', 'FILE waitgone path=/home/nexus/.wine/drive_c/NexusInnoTest/big.bin ok' in log, '')
        check('apps', 'the x64 and x86 ISCC compilers each produced an installer', re.search(r'WR ls\| F +\d+ /home/nexus/innoout/nexus-inno-setup\.exe', log)
              and re.search(r'WR ls\| F +\d+ /home/nexus/innoout/nexus-inno-setup32\.exe', log), re.findall(r'WR ls\| [^\n]*innoout[^\n]*', log))
        for folder in ('SevenZipMsi64', 'SevenZipMsi32'):
            check('apps', 'MSI installs into INSTALLDIR=C:\\%s' % folder, re.search(r'WR ls\| F +\d+ /home/nexus/.wine/drive_c/%s/7z\.exe' % folder, log), '')
            check('apps', 'MSI uninstall removes C:\\%s\\7z.exe' % folder, 'EXISTS path=/home/nexus/.wine/drive_c/%s/7z.exe no' % folder in log, '')
        for name in DATA_FILES:
            for out in ('out64', 'out32'):
                check('apps', '7-Zip round trip %s/%s is identical' % (out, name), 'CMP /home/nexus/wtdata/%s /home/nexus/%s/%s same' % (name, out, name) in log, '')
        check('apps', 'PuTTY MSI installs putty.exe', re.search(r'WR ls\| F +\d+ /home/nexus/.wine/drive_c/Program Files/PuTTY/putty\.exe', log), '')
        check('apps', 'Notepad++ NSIS installer installs notepad++.exe', re.search(r'WR ls\| F +\d+ /home/nexus/.wine/drive_c/Program Files \(x86\)/Notepad\+\+/notepad\+\+\.exe', log), '')
        check('apps', 'winemenubuilder wrote a .desktop entry for PuTTY', re.search(r'WR ls\| F +\d+ /home/nexus/.local/share/applications/wine/[^\n]*/PuTTY\.desktop', log), '')
        check('apps', 'the PuTTY entry starts its shortcut with wine and has an extracted PNG icon', re.search(r'WR cat\| Exec=[^\n]*wine[^\n]*PuTTY', log)
              and re.search(r'WR ls\| F +\d+ /home/nexus/.local/share/icons/hicolor/[^\n]*/apps/[^\n]*putty[^\n]*\.png', log, re.I), re.findall(r'WR cat\| (?:Exec|Icon)=[^\n]*', log)[:4])
    if 'gui' in PHASES:
        check('gui', 'notepad saved the typed text', re.search(r'WR cat\| [^\n]{0,3}Hello from NexusOS notepad', log), '')
        for bits, edition in ((64, 'x64 (portable)'), (32, 'x86 (installed)')):
            check('gui', 'Notepad++ %s edited and saved its file' % edition, re.search(r'WR cat\| hello%d world from NexusOS' % bits, log),
                  re.findall(r'WR cat\| hello[^\n]*', log))
        for tag, _ in GUI_GATES:
            check('gui', '%s: the process exits 0 after its window was closed' % tag, end(tag)[0] == '0', end(tag))
    if 'desktop' in PHASES:
        check('desktop', '~/Downloads exists (where Chrome saves downloads)', 'EXISTS path=/home/nexus/Downloads yes' in log, '')
        check('desktop', 'the program started from Files ran and exited normally', re.search(r'WR cat\| GUI READY bits=32', log) and re.search(r'WR cat\| GUI EXIT code=0', log), '')
        check('desktop', 'Files closes (exit 0)', end('files')[0] == '0', end('files'))
        check('desktop', 'xdg-open hands the .exe to "wine start /unix" (exit 0)', end('xdgopen')[0] == '0', end('xdgopen'))
        # the started program: each process listed while its window was open exits 0 after the close (kernel PROCESS_EXIT trace)
        listing = re.search(r'\[64\] WR GATE xdgopen open[^\n]*\n(.*?)\[64\] WR PROCS text=putty-download\.exe count=', log, re.S)
        pids = [int(pid) for pid in re.findall(r'\[64\] WR procs\| (\d+) ', listing.group(1))] if listing else []
        exits = {pid: re.search(r'PROCESS_EXIT pid=0x%016X tid=0x[0-9A-F]+ wait_status=0x([0-9A-F]+)' % pid, log[listing.end():]) for pid in pids}
        check('desktop', 'the program xdg-open started (PuTTY, %d processes) exits 0 once its window is closed' % len(pids),
              pids and 'WR PROCWAIT text=putty-download.exe remaining=0 ' in log and all(m and int(m.group(1), 16) == 0 for m in exits.values()),
              {pid: m and m.group(0) for pid, m in exits.items()} or (listing and listing.group(1)[-300:]))
        check('desktop', 'the Terminal command wrote its output file', re.search(r'WR cat\| terminal-ok', log), '')
    m = re.search(r'WR PROCWAIT text=\.exe remaining=(\d+) ms=(\d+)', log); n = re.search(r'WR PROCWAIT text=wineserver remaining=(\d+) ms=(\d+)', log)
    check('cleanup', 'no Windows process and no wineserver remain once the programs are closed', m and n and m.group(1) == '0' and n.group(1) == '0',
          re.findall(r'WR procs\| [^\n]*', log)[:10])
    result['user_faults'] = len(re.findall(r'USER_FAULT', log))
    kernel = [line for line in log.splitlines() if 'KERNEL_FAULT' in line]
    check('run', 'no kernel fault', not kernel, kernel[:5])
    result['passed'] = all(item['ok'] for item in checks)
finally:
    if vm: vm.close()
    if servers: servers.close()
    result['seconds'] = round(time.time() - started, 1)
    result['failed'] = [item for item in checks if not item['ok']]
    (WORK / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    if not args.keep_disk and not in_place: disk.unlink(missing_ok=True)
print('PASS Wine on NexusOS (%d checks)' % len(checks) if result['passed'] else 'FAIL Wine on NexusOS: %d of %d checks failed' % (len(result['failed']), len(checks)))
sys.exit(0 if result['passed'] else 1)

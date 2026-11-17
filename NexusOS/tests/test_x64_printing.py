"""Printing on an installed NexusOS (ext4 root, Debian base): CUPS with the Print to PDF queue, a driverless IPP Everywhere
network printer added by its URI, and the print dialogs of Chrome, Firefox and LibreOffice Writer.

    python -I tests/test_x64_printing.py WORK --base BASE [--accel auto|whpx|tcg] [--qmp-port 4675] [--ipp-port 8971]
           [--proxy-cache DIR] [--apps-cache DIR]

BASE is an installed system (a tests/test_x64_services.py base, e.g. one compressed qcow2); nothing below it is written:
WORK/stage.qcow2 over it gets the worktree's services and an Aurora desktop session, the boot runs on an overlay of that.

The network printer is tests/ipp_printer.py on the host (guest ipp://10.0.2.2:<ipp-port>/ipp/print): IPP/2.0, PDF, every
received document kept in WORK/printed and listed to the guest. Guest side (tests/printing_guest.sh, root, run by
test-hook.service): lp of a PDF to the PDF queue (cups-pdf; the text is checked with pdftotext); lpadmin -m everywhere
makes a driverless queue from the printer's own attributes and the default; jobs from root and from the user arrive with
their text; hostile and failure cases (a user cannot add a printer or cancel root's job, a paused queue holds a job, the
printer goes away (the host stops it) and gets the job once it is back); then each application prints through its own
print dialog: Chrome (the desktop's /nexus/chrome-desktop.args), Firefox (Mozilla's release, as NexusOS ships it,
pinned in arch/x86_64/apps-lock.json; Debian's firefox-esr crashes on NexusOS today, see HANDOFF) and LibreOffice Writer
(apt, through the host's caching proxy for deb.debian.org). The host waits for each window, presses Ctrl+P, takes a
screenshot of the dialog and presses Enter; the document must reach the network printer with its text. LibreOffice also
prints with soffice --pt to the PDF queue. Results: WORK/result.json, screenshots, WORK/printed, WORK/uploads.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'tests'
sys.path.insert(0, str(HERE))
import pkg_vm  # noqa: E402
import ipp_printer  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path); ap.add_argument('--base', type=Path, required=True)
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--smp', type=int, default=2)
ap.add_argument('--qmp-port', type=int, default=4675); ap.add_argument('--ipp-port', type=int, default=8971)
ap.add_argument('--proxy-cache', type=Path, default=ROOT / 'arch/x86_64/out/apt-proxy')
ap.add_argument('--apps-cache', type=Path, default=nexus_paths.cache('apps'), help='apps_install.py cache (read only)')
ap.add_argument('--timeout', type=int, default=5400)
ap.add_argument('--slow-cli', type=int, default=0, metavar='SECONDS',
                help='the guest waits this long before the dialog phase (a slow command-line phase: past Aurora\'s 300 s idle lock)')
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
results = {'test': 'printing', 'checks': [], 'passed': False}


def check(name, ok, detail=''):
    results['checks'].append({'check': name, 'ok': bool(ok), 'detail': str(detail)[:800]})
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def make_pdf(text):
    """A one-page PDF with `text` in Helvetica (a standard font: pdftotext reads it back after any filter chain)."""
    stream = b'BT /F1 28 Tf 72 740 Td (' + text.encode() + b') Tj 0 -40 Td /F1 14 Tf (NexusOS printing test document) Tj ET'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
               b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream',
               b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>']
    out, offsets = bytearray(b'%PDF-1.4\n'), []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out)); out += b'%d 0 obj\n' % i + body + b'\nendobj\n'
    xref = len(out)
    out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1) + b''.join(b'%010d 00000 n \n' % o for o in offsets)
    out += b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)
    return bytes(out)


def firefox_tarball():
    lock = json.loads((ROOT / 'arch/x86_64/apps-lock.json').read_text())
    entry = next(e for e in lock if e.get('name') == 'firefox')
    path = args.apps_cache / entry['filename']
    data = path.read_bytes()
    if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']: raise SystemExit('Firefox in the apps cache does not match apps-lock.json')
    return data, entry['version']


HOOK = '''wget -q -O /usr/lib/nexus/tests/printing_guest.sh @HOST@/files/printing_guest.sh
IPP_PORT=%d sh /usr/lib/nexus/tests/printing_guest.sh
''' % args.ipp_port   # the guest's printer URI follows --ipp-port

# The print dialogs. title: the Aurora toplevel title of the application's window (AURORA WINDOW/TITLE lines).
APPS = {'chrome': r'NexusOS Chrome print test[^\n]*', 'firefox': r'NexusOS Firefox print test[^\n]*', 'writer': r'print-lo[^\n]*LibreOffice[^\n]*'}


def window(vm, title, after, seconds):
    end = time.time() + seconds
    while time.time() < end:
        m = None
        for m in re.finditer(r'AURORA (?:WINDOW|TITLE) app=(\S*) title=(%s) x=(-?\d+) y=(-?\d+)' % title, vm.text()[after:]): pass
        if m: return m.group(2), int(m.group(3)), int(m.group(4))
        if vm.proc.poll() is not None: return None
        time.sleep(0.5)
    return None


def ctrl(vm, key): vm.key('ctrl', True); vm.tap(key); vm.key('ctrl', False)


def drive_dialog(vm, server, app):
    """Window up -> click into it -> Ctrl+P -> screenshot of the dialog -> Enter (prints to the default printer)."""
    win = window(vm, APPS[app], 0, 600)   # each application's window title is unique
    check('%s: window appears' % app, win, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', vm.text())[-6:])
    if win:
        time.sleep(12 if app == 'writer' else 10)
        vm.click(min(1800, max(40, win[1] + 300)), min(1000, max(120, win[2] + 300))); time.sleep(1.5)
        vm.shot('%s-window' % app)
        mark = len(vm.text()); ctrl(vm, 'p'); time.sleep(12)
        if app == 'writer':   # the dialog is its own toplevel ("Print")
            dialog = window(vm, r'Print', mark, 60)
            check('writer: the Print dialog opens', dialog, re.findall(r'AURORA (?:WINDOW|TITLE)[^\n]*', vm.text()[mark:])[-4:])
            time.sleep(3)
        vm.shot('%s-dialog' % app)
        vm.tap('ret'); time.sleep(5); vm.shot('%s-after' % app)
    server.open_gate('%s-printed' % app)


def publish_printed(server, printer, stop):
    """Make the printer's documents visible to the guest: /files/printed/index and /files/printed/<name>."""
    while not stop.is_set():
        names = []
        for job in list(printer.jobs):
            if job['path']:
                name = Path(job['path']).name; names.append(name)
                if 'printed/' + name not in server.files: server.files['printed/' + name] = Path(job['path']).read_bytes()
        server.files['printed/index'] = ''.join(n + '\n' for n in names).encode()
        stop.wait(1)


started = time.time()
server = vm = printer = None; stop = threading.Event()
try:
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no base image ' + str(base))
    def session(fs):
        pkg_vm.refresh_services(fs)   # the worktree's services over the installed base
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
        # no idle lock in the test session: the command-line phase takes 20+ minutes without input, and a locked
        # screen would hide the print dialogs (the greeter at boot stays: autologin=0)
        fs.ensure_dir(fs.split('/home/nexus/.aurora'), 0o755, 1000, 1000)   # the user's folder if the base has none yet
        pkg_vm.put(fs, '/home/nexus/.aurora/login.conf', 'autologin=0\nlock_after=0\nlock_on_sleep=1\n', 0o644, 1000, 1000)
    print('stage overlay: %d blocks changed' % pkg_vm.edit_overlay(base, WORK / 'stage.qcow2', session), flush=True)
    firefox, firefox_version = firefox_tarball()
    files = {'printing_guest.sh': (HERE / 'printing_guest.sh').read_bytes().replace(b'\r\n', b'\n'),
             'registrymodifications.xcu': (ROOT / 'arch/x86_64/apps-config/libreoffice-registrymodifications.xcu').read_bytes(),
             'firefox-user.js': (ROOT / 'arch/x86_64/apps-config/firefox-user.js').read_bytes(), 'firefox.tar.xz': firefox,
             'printed/index': b'', 'cli-delay': str(max(0, args.slow_cli)).encode()}
    for marker in ('NEXUS-CLI-PDF', 'NEXUS-CLI-IPP', 'NEXUS-HELD-JOB', 'NEXUS-OFFLINE-JOB', 'NEXUS-USER-JOB'):
        files['doc-%s.pdf' % marker] = make_pdf(marker)
    for old in list((WORK / 'uploads').glob('*')) + list((WORK / 'printed').glob('*')): old.unlink()
    server = pkg_vm.HookServer(HOOK, files, WORK / 'uploads', args.proxy_cache)
    printer = ipp_printer.IppPrinter(args.ipp_port, WORK / 'printed')
    threading.Thread(target=publish_printed, args=(server, printer, stop), daemon=True).start()
    vm = pkg_vm.Vm(WORK, WORK / 'stage.qcow2', args.qmp_port, tag='printing', accel=args.accel, smp=args.smp, memory='3072', hook_port=server.port)
    logged_in, detail = pkg_vm.greeter_login(vm)   # before the print dialogs are driven (the base boots to the greeter)
    check('logged in through the greeter', logged_in, detail)
    handled = set(); deadline = time.time() + args.timeout
    while time.time() < deadline and vm.proc.poll() is None:
        log = vm.text()
        if 'PRN DONE' in log or re.search(r'KERNEL_FAULT', log): break
        for when, name, _ in list(server.events):
            if name in handled: continue
            handled.add(name)
            if name == 'printer-off': printer.go_offline(); print('-- printer off', time.strftime('%H:%M:%S'), flush=True)
            elif name == 'printer-on': printer.go_online(); print('-- printer on', time.strftime('%H:%M:%S'), flush=True)
            elif name.endswith('-launched'):
                app = name[:-len('-launched')]; print('-- dialog', app, flush=True)
                try: drive_dialog(vm, server, app)
                except Exception as error: check('%s: dialog driven' % app, False, repr(error)); server.open_gate('%s-printed' % app)
        time.sleep(1)
    log = vm.text()
    for m in re.finditer(r'^PRN (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check(m.group(1), m.group(2) == 'PASS', m.group(3))
    check('guest checks ran to the end', 'PRN DONE' in log, log[-300:])
    check('the session was never locked while the test ran (idle lock off in its login.conf)', not re.search(r'\[64\] AURORA LOCKED reason=', log),
          re.findall(r'\[64\] AURORA LOCKED[^\n]*', log)[:3])
    if args.slow_cli: check('slow command-line phase: the dialogs came after %d s without input' % args.slow_cli, re.search(r'PRN slow-cli waited %d s' % args.slow_cli, log), args.slow_cli)
    check('guest checks ran', len(re.findall(r'^PRN \S+ (?:PASS|FAIL)', log, re.M)) >= 30)
    check('no kernel faults', not re.search(r'KERNEL_FAULT', log), re.findall(r'.*KERNEL_FAULT.*', log)[:3])
    results['user_faults'] = re.findall(r'.*USER_FAULT.*', log)[:10]
    check('the archive was reached only through deb.debian.org', not server.refused, server.refused[:5])
    results['ipp_requests'] = sorted(set(printer.requests)); results['ipp_errors'] = printer.errors[:10]
    results['ipp_jobs'] = [{k: j[k] for k in ('id', 'name', 'user', 'format', 'bytes')} for j in printer.jobs]
    check('the network printer got its jobs through IPP (Create-Job/Print-Job, Get-Printer-Attributes)',
          'Get-Printer-Attributes' in printer.requests and ('Print-Job' in printer.requests or 'Send-Document' in printer.requests), results['ipp_requests'])
    # what the applications said about D-Bus services (login1, portals, notifications...): evidence, not checks
    results['app_dbus_messages'] = {name: [line for line in (WORK / 'uploads' / name).read_text('utf-8', 'replace').splitlines()
                                           if re.search(r'dbus|org\.freedesktop|portal|login1', line, re.I)][:40]
                                    for name in ('chrome.log', 'firefox.log', 'writer.log') if (WORK / 'uploads' / name).exists()}
    results['firefox'] = firefox_version
    for path in sorted((WORK / 'printed').glob('*.pdf')):
        data = path.read_bytes()
        check('%s (network printer) is a PDF document' % path.name, data.startswith(b'%PDF-') and b'%%EOF' in data[-2048:], data[:16])
    print('proxy: %d fetched, %d from cache' % (server.proxied, server.cached), flush=True)
finally:
    stop.set()
    if vm: vm.close()
    if server: server.close()
    if printer: printer.close()
    results['seconds'] = round(time.time() - started)
    results['passed'] = bool(results['checks']) and all(c['ok'] for c in results['checks'])
    (WORK / 'result.json').write_text(json.dumps(results, indent=1))
    (WORK / 'stage.qcow2').unlink(missing_ok=True)
    for shot in WORK.glob('*.ppm'): shot.unlink()
    print('RESULT', 'PASS' if results['passed'] else 'FAIL', '%d checks' % len(results['checks']), flush=True)
sys.exit(0 if results['passed'] else 1)

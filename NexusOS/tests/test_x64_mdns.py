"""mDNS / DNS-SD on an installed NexusOS (ext4 root, Debian base): avahi-daemon on by default, .local names in programs
(nss-mdns), DNS-SD browsing and publishing, a network printer found and printed to by CUPS through DNS-SD, the
guest's own IPP Everywhere printer (ippeveprinter) announced on the wire. Checked from both ends.

    python -I C:\\nexustest\\wave2\\pkgvm_lock.py run net2 -- python -I tests/test_x64_mdns.py WORK
           [--base INSTALLED.qcow2] [--smp 2] [--qmp-port 4724] [--port 9126]

BASE (default: the production round's installed test base, read only) gets the worktree's services through
WORK/stage.qcow2 (pkg_vm.edit_overlay; qemu-nbd on --port+3). The network is tests/netfix.py (QEMU -netdev dgram):
DHCPv4 10.0.2.15 with the gateway 10.0.2.2 as QEMU's user network would give, an IPv6 prefix, and on 10.0.2.2 a relay
of TCP PORT to the test hook server (pkg_vm.HookServer on a free port; the guest's test hook gets it as
nexus.testhook=PORT through pkg_vm.Vm(hook_port=)) and of TCP 631 to
tests/ipp_printer.py (--port+2). Its mDNS responder answers fixture.local (A, AAAA, reverse PTR) and advertises
"Fixture Printer" (_ipp._tcp: PTR/SRV/TXT). tests/mdns_guest.sh runs as root (test hook) and checks: avahi-daemon
running by default, avahi-publish + ippeveprinter publishing, avahi-resolve (A/AAAA/reverse), getent through
nss-mdns (and an unknown .local name failing), avahi-browse (fixture printer resolved to 10.0.2.2:631, own services),
lpinfo dnssd URIs, lpadmin -m everywhere + lp to the fixture's printer (the job must reach the host printer) and to
the guest's own ippeveprinter, avahi restart. The host checks the wire: IGMP/MLD joins of 224.0.0.251 / ff02::fb,
the guest's announcements (TTL 255, both families) of its printer and service, and its answer to the host's query
for <hostname>.local. No kernel fault. The overlays are deleted at the end."""
from pathlib import Path
import argparse
import re
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
import netfix  # noqa: E402
import pkg_vm  # noqa: E402
import ipp_printer  # noqa: E402

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('work', type=Path)
ap.add_argument('--base', type=Path, default=Path(r'D:\nexustest\prod\ws_desk_runs\pkgbase\installed.qcow2'))
ap.add_argument('--smp', type=int, default=2); ap.add_argument('--qmp-port', type=int, default=4724)
ap.add_argument('--port', type=int, default=9126, help='fixture UDP; +1 QEMU dgram; +2 IPP printer; +3 qemu-nbd')
ap.add_argument('--accel', default='auto', choices=('auto', 'whpx', 'tcg'))
ap.add_argument('--timeout', type=int, default=1500)
args = ap.parse_args()
WORK = args.work.resolve(); WORK.mkdir(parents=True, exist_ok=True)
GUEST4 = '10.0.2.15'
checks = []


def check(name, ok, detail=''):
    checks.append((bool(ok), name))
    print('%s %s%s' % ('PASS' if ok else 'FAIL', name, '' if ok else '  (%s)' % str(detail)[:400]), flush=True)
    return ok


def make_pdf(text):
    stream = b'BT /F1 24 Tf 72 740 Td (' + text.encode() + b') Tj ET'
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
               b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>',
               b'<< /Length %d >>\nstream\n' % len(stream) + stream + b'\nendstream', b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']
    out, offsets = b'%PDF-1.4\n', []
    for i, body in enumerate(objects, 1): offsets.append(len(out)); out += b'%d 0 obj\n' % i + body + b'\nendobj\n'
    xref = len(out); out += b'xref\n0 %d\n0000000000 65535 f \n' % (len(objects) + 1) + b''.join(b'%010d 00000 n \n' % o for o in offsets)
    return out + b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(objects) + 1, xref)


HOOK = 'wget -q -O /tmp/mdns_guest.sh @HOST@/files/mdns_guest.sh && sh /tmp/mdns_guest.sh\n'   # @HOST@: HookServer.text()


def main():
    base = args.base.resolve()
    if not base.is_file(): raise SystemExit('no installed base ' + str(base))
    stage = WORK / 'stage.qcow2'
    def session(fs):
        pkg_vm.refresh_services(fs)                                        # the worktree's units (avahi on by default)
        pkg_vm.put(fs, '/nexus/desktop.args', '--aurora\n/usr/lib/nexus/tests/svctest-wait\n')
    print('stage overlay: %d blocks changed' % pkg_vm.edit_overlay(base, stage, session, nbd_port=args.port + 3), flush=True)
    files = {'mdns_guest.sh': (ROOT / 'tests/mdns_guest.sh').read_bytes().replace(b'\r\n', b'\n'), 'mdns-doc.pdf': make_pdf('NEXUS-MDNS-JOB')}
    server = pkg_vm.HookServer(HOOK, files, WORK / 'uploads')
    printer = ipp_printer.IppPrinter(args.port + 2, WORK / 'printed', name='Fixture Printer')
    fx = netfix.Fixture()
    lan = fx.link('lan', args.port, args.port + 1, guest_mac=bytes.fromhex('525400123456'))
    lan.server4(router='10.0.2.2', lease_ip=GUEST4, dns=('10.0.2.3',), domain='lan.test')
    lan.router6(prefixes=(('fd00:2::', 64, 0xC0, 86400, 14400),))
    lan.onlink6.add(netfix.a6('fd00:2::2'))
    fx.host('10.0.2.2', 'fd00:2::2'); fx.dns_server('10.0.2.3'); lan.onlink4.add(netfix.a4('10.0.2.3'))
    fx.relay(server.port, server.port); fx.relay(631, args.port + 2)
    mdns = netfix.Mdns(fx, lan)
    mdns.host('fixture.local', ['10.0.2.2', 'fd00:2::2'])
    mdns.service('Fixture Printer', '_ipp._tcp.local', 'fixture.local', 631,
                 ['txtvers=1', 'qtotal=1', 'rp=ipp/print', 'ty=NexusOS Fixture Printer', 'product=(Fixture Printer)',
                  'pdl=application/pdf', 'kind=document', 'Color=F', 'Duplex=F', 'URF=none', 'printer-type=0x1046',
                  'UUID=6e657875-732d-6d64-6e73-000000000001'])
    fx.start()
    vm = None; stop = threading.Event()
    try:
        net = (f'dgram,id=network,local.type=inet,local.host=127.0.0.1,local.port={args.port + 1},'
               f'remote.type=inet,remote.host=127.0.0.1,remote.port={args.port}')
        vm = pkg_vm.Vm(WORK, stage, args.qmp_port, tag='mdns', accel=args.accel, smp=args.smp, memory='2048', net=net,
                        hook_port=server.port)
        deadline = time.time() + args.timeout; handled = set(); hostname = None
        while time.time() < deadline and vm.proc.poll() is None:
            log = vm.text()
            if 'MDNS DONE' in log or 'KERNEL_FAULT' in log: break
            if printer.jobs: server.open_gate('printed')
            for _, name, _ in list(server.events):
                if name in handled: continue
                handled.add(name)
                if name == 'ask-host':
                    m = re.search(r'MDNS-INFO hostname (\S+)', log); hostname = (m.group(1) if m else 'nexus') + '.local'
                    t = time.monotonic()
                    for family in (4, 6): mdns.query(hostname, 1 if family == 4 else 28, family)
                    try:
                        fx.wait(lambda: [e for e in fx.find('mdns', t) if e['response'] and any(r[0] == hostname.lower() and r[1] in (1, 28) for r in e['records'])], 10, 'answer')
                    except AssertionError: pass
                    server.open_gate('asked')
            time.sleep(1)
        log = vm.text()
        for m in re.finditer(r'^MDNS (\S+) (PASS|FAIL) ?(.*)$', log, re.M): check('guest: ' + m.group(1), m.group(2) == 'PASS', m.group(3))
        check('guest checks ran to the end', 'MDNS DONE' in log, log[-1500:])
        for line in re.findall(r'^MDNS-(?:INFO|BROWSE|LPINFO|LOG|PROC|CONF|DEBUG) .*$', log, re.M)[:200]: print('   ', line[:200])
        # ---- the wire ----
        joins4 = [e for e in fx.find('igmp') if e['src'] == GUEST4 and e['message'][:1] == b'\x22' and netfix.a4('224.0.0.251') in e['message']]
        check('IGMPv3 join of 224.0.0.251 by the guest', joins4)
        joins6 = [e for e in fx.find('icmp6') if e['type'] == 143 and netfix.a6('ff02::fb') in e['message']]
        check('MLDv2 join of ff02::fb by the guest', joins6)
        fixture_addresses = {'10.0.2.2', 'fe80::1', 'fd00:2::2'}
        mine = [e for e in fx.find('mdns') if e['response'] and e['src'] not in fixture_addresses]
        def announced(family, name):
            return [e for e in mine if e['family'] == family and e['ttl'] == 255 and any(r[1] == 12 and r[4].lower() == name.lower() for r in e['records'])]
        check('the guest announced its IPP printer over IPv4 (PTR _ipp._tcp.local, TTL 255)', announced(4, 'NexusOS Guest Printer._ipp._tcp.local'),
              sorted({str(r[:2]) for e in mine for r in e['records']})[:20])
        check('... and over IPv6 (ff02::fb)', announced(6, 'NexusOS Guest Printer._ipp._tcp.local'))
        check('the guest announced the avahi-publish service', announced(4, 'NexusOS Guest Web._http._tcp.local'))
        srv = [r for e in mine for r in e['records'] if r[1] == 33 and r[0] == 'nexusos guest printer._ipp._tcp.local']
        check('its SRV record names port 8631', srv and srv[0][4][2] == 8631, srv[:2])
        if hostname:
            answers = [r for e in mine for r in e['records'] if r[0] == hostname.lower() and r[1] == 1]
            check(f'the guest (avahi) answered the host\'s query for {hostname} with {GUEST4}', answers and any(r[4] == GUEST4 for r in answers), answers[:3])
        asked = [e for e in fx.find('mdns') if not e['response'] and e['src'] in (GUEST4,) and any(q[0] == 'fixture.local' for q in e['questions'])]
        check('the guest asked the network for fixture.local (mDNS query)', asked)
        jobs = [j for j in printer.jobs if j['path']]
        check('the fixture printer (found by DNS-SD) received the job as PDF',
              jobs and Path(jobs[0]['path']).read_bytes().startswith(b'%PDF-'), [(j['name'], j['format']) for j in printer.jobs])
        check('no kernel fault', 'KERNEL_FAULT' not in log, re.findall(r'.*KERNEL_FAULT.*', log)[:2])
        check('fixture without errors', not fx.errors, fx.errors[:2])
    finally:
        stop.set()
        if vm: vm.close()
        server.close(); printer.close(); fx.stop()
        for leftover in (stage,): leftover.unlink(missing_ok=True)
    failed = [n for ok, n in checks if not ok]
    print('SUMMARY mdns: %d/%d checks passed' % (len(checks) - len(failed), len(checks)), flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())

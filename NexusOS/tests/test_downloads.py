"""QEMU download campaign on private image copies (HTTP server is loopback-only).

Usage: python -I tests/test_downloads.py [work-directory]
Run build.bat first. Each completed file is checked byte-for-byte after the VM exits.
"""
from pathlib import Path
import hashlib
import http.server
import importlib.util
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
WORK = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else nexus_paths.runs('campaign')
WORK.mkdir(parents=True, exist_ok=True)


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


fatput = module('fatput', ROOT / 'userland/fatput.py')
storage = module('storage', ROOT / 'storage.py')
PATTERN = bytes(range(256)) * 257
HTML = b'<html><head><title>Saved large page</title></head><body>' + b'<p>Save the complete page.</p>' * 40000 + b'</body></html>'


class Server(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        with (WORK / 'http.log').open('a') as log:
            log.write((fmt % args) + '\n')

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        n = int(query.get('n', ['2097152'])[0])
        name = query.get('name', ['download.bin'])[0]
        if parsed.path == '/error': self.send_error(404); return
        if parsed.path == '/redirect':
            self.send_response(302); self.send_header('Location', '/file?n=1048577&name=redirect.zip')
            self.send_header('Content-Length', '0'); self.end_headers(); return
        if parsed.path == '/large.html': n = len(HTML)
        if parsed.path == '/empty': n = 0
        self.send_response(200)
        self.send_header('Content-Type', 'text/html' if parsed.path == '/large.html' else 'application/octet-stream')
        self.send_header('Content-Disposition', ('inline' if parsed.path == '/large.html' else 'attachment') + "; filename*=UTF-8''" + urllib.parse.quote('large.html' if parsed.path == '/large.html' else name))
        if parsed.path == '/huge':
            self.send_header('Content-Length', '4294967296'); n = 0
        elif parsed.path == '/chunked': self.send_header('Transfer-Encoding', 'chunked')
        else: self.send_header('Content-Length', str(n))
        self.send_header('Connection', 'close'); self.end_headers()
        amount = min(n, 65536) if parsed.path == '/broken' else n
        try:
            for offset in range(0, amount, 65536):
                size = min(65536, amount - offset)
                data = HTML[offset:offset+size] if parsed.path == '/large.html' else PATTERN[offset % 256:offset % 256 + size]
                if parsed.path == '/chunked': self.wfile.write(('%x;test=1\r\n' % size).encode())
                self.wfile.write(data)
                if parsed.path == '/chunked': self.wfile.write(b'\r\n')
                self.wfile.flush()
                if parsed.path == '/slow': time.sleep(.3)
            if parsed.path == '/chunked': self.wfile.write(b'0\r\nX-Test: done\r\n\r\n')
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        self.close_connection = True


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run(source, tag, steps):
    subprocess.run([sys.executable, '-u', '-I', str(ROOT / 'handoff/ws_tls/harness57.py'),
                    str(source), str(WORK / tag), str(port()), tag, 'virtio', ';'.join(steps)],
                   check=True, env=dict(os.environ, WAITSER_TIMEOUT='300'))
    log = (WORK / tag / ('serial_' + tag + '.log')).read_text()
    assert '!EX:' not in log and '!KR:' not in log, log[-1000:]
    return WORK / tag / (tag + '_disk.img'), log


def verify(disk, path, size, literal=None):
    fs = fatput.Fat32(disk)
    try:
        parent, leaf = path.rsplit('/', 1)
        hit = fs.find_name(fs.lookup_dir(parent.split('/')), leaf)
        assert hit, 'missing ' + path
        entry = hit[2]
        assert struct.unpack_from('<I', entry, 28)[0] == size, path
        first = (struct.unpack_from('<H', entry, 20)[0] << 16) | struct.unpack_from('<H', entry, 26)[0]
        offset = 0
        for cluster in fs.chain(first) if size else ():
            length = min(size - offset, fs.cl_bytes)
            actual = fs.rd(fs.cl_off(cluster), length)
            expected = literal[offset:offset+length] if literal is not None else PATTERN[offset % 256:offset % 256 + length]
            assert actual == expected, (path, offset)
            offset += length
        assert offset == size
        print('PASS saved bytes:', path, size, flush=True)
    finally:
        fs.close()


def verify_protected(before, after, boots):
    if before[:4] == b'SFPN':
        # NPFS deliberately persists its mount counter and superblock CRC on each boot.
        assert before[:52] == after[:52] and before[60:] == after[60:]
        count, checksum = struct.unpack_from('<II', after, 52)
        assert count == (struct.unpack_from('<I', before, 52)[0] + boots) & 0xFFFFFFFF
        assert checksum == zlib.crc32(after[:56])
    else:
        assert before == after


def main():
    source = WORK / 'source'; source.mkdir(exist_ok=True)
    shutil.copyfile(ROOT / 'nexus.img', source / 'nexus.img')
    shutil.copyfile(ROOT / 'disk.img', source / 'disk.img')
    if (source / 'disk.img').stat().st_size < 128 * 1024**2:
        storage.grow(source / 'disk.img', 128)
    fixture_names = ['CLI large archive.zip', 'chunked.bin', 'redirect.zip', 'empty.bin',
                     'broken.bin', 'huge.bin', 'Browser large installer.exe', 'large.html']
    fs = fatput.Fat32(source / 'disk.img')
    try:
        if fs.find_name(fs.rootc, 'Downloads'):
            directory = fs.lookup_dir(['Downloads'])
            for name in fixture_names:
                if fs.find_name(directory, name): fs.rm('Downloads/' + name)
    finally:
        fs.close()
    with open(source / 'disk.img', 'rb') as image:
        image.seek(fatput.LIMIT); before = image.read(storage.BASE-fatput.LIMIT)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Server)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    base = 'http://10.0.2.2:%d' % server.server_address[1]
    login = ['sleep:9', 'type:root', 'ret', 'sleep:1', 'type:root', 'ret', 'sleep:3']
    try:
        steps = login + ['key:f12', 'sleep:2']
        requests = [('/file?n=33554432&name=CLI%20large%20archive.zip', ''),
                    ('/chunked?n=3145745&name=chunked.bin', ''),
                    ('/redirect', ''), ('/empty?name=empty.bin', ''),
                    ('/file?n=1024&name=CLI%20large%20archive.zip', ' "/disk/Downloads/CLI large archive.zip"'),
                    ('/broken?name=broken.bin', ''), ('/huge?name=huge.bin', ''), ('/error', '')]
        for url, destination in requests:
            steps += ['cmd:wget ' + base + url + destination, 'waitser:[NET] wget url=']
        steps += ['shot:cli', 'serial']
        disk, log = run(source, 'cli', steps)
        verify(disk, 'Downloads/CLI large archive.zip', 33554432)
        verify(disk, 'Downloads/chunked.bin', 3145745)
        verify(disk, 'Downloads/redirect.zip', 1048577)
        verify(disk, 'Downloads/empty.bin', 0)
        assert log.count(' ok=0') == 4, log[-2000:]
        fs = fatput.Fat32(disk)
        names = [name for name, *_ in fs.named_entries(fs.lookup_dir(['Downloads']))]
        fs.close()
        assert not any(n.lower().endswith('.tmp') or n in ('broken.bin', 'huge.bin') for n in names), names
        shutil.copyfile(disk, source / 'disk.img')
        steps = login + ['key:ctrl-spc', 'sleep:1', 'type:browser', 'ret', 'sleep:2',
                 'key:ctrl-l', 'type:' + base + '/file?n=8388608&name=Browser%20large%20installer.exe', 'ret',
                 'sleep:50', 'shot:browser_download',
                 'key:ctrl-l', 'type:' + base + '/large.html', 'ret', 'sleep:3', 'key:ctrl-s',
                 'sleep:15', 'shot:save_page', 'key:ctrl-l', 'type:about:downloads', 'ret', 'sleep:2', 'shot:files', 'serial']
        disk, log = run(source, 'browser', steps)
        verify(disk, 'Downloads/Browser large installer.exe', 8388608)
        verify(disk, 'Downloads/large.html', len(HTML), HTML)
        verify(disk, 'Downloads/CLI large archive.zip', 33554432)
        with open(disk, 'rb') as image:
            image.seek(fatput.LIMIT); verify_protected(before, image.read(storage.BASE-fatput.LIMIT), 2)
        print('PASS protected NPFS/persistence region', flush=True)
    finally:
        server.shutdown(); server.server_close(); thread.join()
    print('DOWNLOAD CAMPAIGN PASSED', flush=True)


if __name__ == '__main__':
    main()

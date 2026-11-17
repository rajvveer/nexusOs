"""A driverless IPP Everywhere test printer on the host (tests/test_x64_printing.py): IPP/2.0 over HTTP (RFC 8010/8011,
PWG 5100.14 attributes) that accepts PDF and keeps every document it receives.

    printer = IppPrinter(port, spool)      # guest URI: ipp://10.0.2.2:<port>/ipp/print
    printer.jobs                           # [{'id', 'name', 'user', 'format', 'path', 'bytes'}]
    printer.go_offline() / go_online()     # the printer "disappears" (connections refused) and comes back

Operations: Get-Printer-Attributes, Validate-Job, Print-Job, Create-Job, Send-Document, Get-Job-Attributes, Get-Jobs,
Cancel-Job. Requests may come with chunked transfer encoding and Expect: 100-continue (CUPS' ipp backend uses both).
Malformed requests get client-error-bad-request; nothing in a request is trusted beyond its declared lengths.
"""
from pathlib import Path
import http.server
import socket
import struct
import threading
import time
import uuid

OPS = {0x0002: 'Print-Job', 0x0004: 'Validate-Job', 0x0005: 'Create-Job', 0x0006: 'Send-Document', 0x0008: 'Cancel-Job',
       0x0009: 'Get-Job-Attributes', 0x000A: 'Get-Jobs', 0x000B: 'Get-Printer-Attributes'}
INTEGER, BOOLEAN, ENUM, OCTETS, RESOLUTION, RANGE = 0x21, 0x22, 0x23, 0x30, 0x32, 0x33
TEXT, NAME, KEYWORD, URI, URISCHEME, CHARSET, LANGUAGE, MIME = 0x41, 0x42, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49
BEGIN, END, MEMBER = 0x34, 0x37, 0x4A


def parse(body):
    """(version, operation, request_id, {group: [(name, tag, value)]}, document bytes); ValueError on malformed input."""
    if len(body) < 9: raise ValueError('short request')
    version, operation, request_id = struct.unpack('>HHI', body[:8])
    groups, pos, group, name = {}, 8, None, None
    while True:
        if pos >= len(body): raise ValueError('no end-of-attributes tag')
        tag = body[pos]; pos += 1
        if tag == 0x03: break
        if tag <= 0x0F: group = tag; groups.setdefault(group, []); continue
        if group is None or pos + 2 > len(body): raise ValueError('attribute outside a group')
        n = struct.unpack('>H', body[pos:pos + 2])[0]; pos += 2
        if pos + n + 2 > len(body): raise ValueError('truncated attribute name')
        if n: name = body[pos:pos + n].decode('utf-8', 'replace')
        pos += n
        length = struct.unpack('>H', body[pos:pos + 2])[0]; pos += 2
        if pos + length > len(body): raise ValueError('truncated attribute value')
        groups[group].append((name, tag, body[pos:pos + length])); pos += length
    return version, operation, request_id, groups, body[pos:]


def attr(tag, name, values):
    """Encode one attribute with one or more values (the extra values with an empty name)."""
    out = b''
    for i, v in enumerate(values if isinstance(values, list) else [values]):
        n = name.encode() if i == 0 else b''
        if tag == BEGIN: out += bytes([BEGIN]) + struct.pack('>H', len(n)) + n + b'\0\0' + collection(v) + bytes([END]) + b'\0\0\0\0'; continue
        if tag in (INTEGER, ENUM): data = struct.pack('>i', v)
        elif tag == BOOLEAN: data = b'\x01' if v else b'\x00'
        elif tag == RESOLUTION: data = struct.pack('>iib', v[0], v[1], 3)
        elif tag == RANGE: data = struct.pack('>ii', v[0], v[1])
        else: data = v.encode() if isinstance(v, str) else v
        out += bytes([tag]) + struct.pack('>H', len(n)) + n + struct.pack('>H', len(data)) + data
    return out


def collection(members):
    """members: [(name, tag, value)] -> memberAttrName + value pairs (values with an empty name)."""
    out = b''
    for name, tag, value in members:
        out += bytes([MEMBER]) + b'\0\0' + struct.pack('>H', len(name)) + name.encode()
        if tag == BEGIN: out += bytes([BEGIN]) + b'\0\0\0\0' + collection(value) + bytes([END]) + b'\0\0\0\0'
        else: out += attr(tag, '', value)[0:1] + b'\0\0' + attr(tag, 'x', value)[4:]   # same value encoding, empty name
    return out


SIZES = {'iso_a4_210x297mm': (21000, 29700), 'na_letter_8.5x11in': (21590, 27940)}


def media_col(size, margins=423):
    x, y = SIZES[size]
    return [('media-size', BEGIN, [('x-dimension', INTEGER, x), ('y-dimension', INTEGER, y)]),
            ('media-bottom-margin', INTEGER, margins), ('media-left-margin', INTEGER, margins),
            ('media-right-margin', INTEGER, margins), ('media-top-margin', INTEGER, margins),
            ('media-source', KEYWORD, 'main'), ('media-type', KEYWORD, 'stationery')]


class IppPrinter(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, spool, name='NexusOS Test Printer'):
        self.port, self.spool, self.name = port, Path(spool), name
        self.spool.mkdir(parents=True, exist_ok=True)
        self.jobs, self.requests, self.errors, self.next_id, self.started = [], [], [], 1, time.time()
        self.uuid = 'urn:uuid:' + str(uuid.uuid4()); self.lock = threading.Lock(); self.online = True
        printer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'
            def log_message(self, *a): pass

            def body(self):
                if 'chunked' in self.headers.get('Transfer-Encoding', '').lower():
                    data = b''
                    while True:
                        size = int(self.rfile.readline().split(b';')[0].strip() or b'0', 16)
                        if size > (256 << 20) - len(data): raise ValueError('request too large')
                        if not size: self.rfile.readline(); return data
                        data += self.rfile.read(size); self.rfile.readline()
                length = int(self.headers.get('Content-Length', '0'))
                if length > (256 << 20): raise ValueError('request too large')
                return self.rfile.read(length)

            def do_POST(self):
                try: data = self.body()
                except (ValueError, OSError) as error: printer.errors.append(str(error)); self.send_error(400); return
                reply = printer.handle(data, self.headers.get('Host', '10.0.2.2'))
                self.send_response(200); self.send_header('Content-Type', 'application/ipp')
                self.send_header('Content-Length', str(len(reply))); self.end_headers(); self.wfile.write(reply)

            def do_GET(self):   # the printer's "web page"
                page = ('<html><body><h1>%s</h1><p>%d jobs</p></body></html>' % (printer.name, len(printer.jobs))).encode()
                self.send_response(200); self.send_header('Content-Type', 'text/html'); self.send_header('Content-Length', str(len(page)))
                self.end_headers(); self.wfile.write(page)
        self.handler = Handler
        super().__init__(('127.0.0.1', port), Handler)
        self.thread = threading.Thread(target=self.serve_forever, daemon=True); self.thread.start()

    # ---- the printer goes away and comes back (connections are refused meanwhile)
    def go_offline(self):
        self.shutdown(); self.server_close(); self.online = False

    def go_online(self):
        self.socket = socket.socket(self.address_family, self.socket_type)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # bind directly: HTTPServer.server_bind also resolves the host name (getfqdn), which can take a minute on Windows
        self.socket.bind(self.server_address); self.server_activate(); self.online = True
        self.thread = threading.Thread(target=self.serve_forever, daemon=True); self.thread.start()

    def close(self):
        if self.online: self.shutdown(); self.server_close()

    # ---- IPP
    def printer_attributes(self, host):
        uri = 'ipp://%s/ipp/print' % host
        formats = ['application/pdf', 'application/octet-stream']
        return b''.join([
            attr(URI, 'printer-uri-supported', uri), attr(KEYWORD, 'uri-security-supported', 'none'),
            attr(KEYWORD, 'uri-authentication-supported', 'none'), attr(NAME, 'printer-name', 'NexusOS-Test-Printer'),
            attr(TEXT, 'printer-info', self.name), attr(TEXT, 'printer-location', 'QEMU host network'),
            attr(TEXT, 'printer-make-and-model', 'NexusOS Test Printer'), attr(URI, 'printer-uuid', self.uuid),
            attr(URI, 'printer-more-info', 'http://%s/' % host), attr(TEXT, 'printer-device-id', 'MFG:NexusOS;MDL:Test Printer;CMD:PDF;'),
            attr(ENUM, 'printer-state', 3), attr(KEYWORD, 'printer-state-reasons', 'none'),
            attr(BOOLEAN, 'printer-is-accepting-jobs', True), attr(INTEGER, 'printer-up-time', int(time.time() - self.started) + 1),
            attr(KEYWORD, 'ipp-versions-supported', ['1.1', '2.0']), attr(KEYWORD, 'ipp-features-supported', 'ipp-everywhere'),
            attr(ENUM, 'operations-supported', [0x0002, 0x0004, 0x0005, 0x0006, 0x0008, 0x0009, 0x000A, 0x000B]),
            attr(CHARSET, 'charset-configured', 'utf-8'), attr(CHARSET, 'charset-supported', 'utf-8'),
            attr(LANGUAGE, 'natural-language-configured', 'en'), attr(LANGUAGE, 'generated-natural-language-supported', 'en'),
            attr(MIME, 'document-format-default', 'application/pdf'), attr(MIME, 'document-format-supported', formats),
            attr(KEYWORD, 'pdf-versions-supported', ['adobe-1.3', 'adobe-1.4', 'adobe-1.5', 'adobe-1.6', 'adobe-1.7', 'iso-32000-1_2008']),
            attr(KEYWORD, 'compression-supported', 'none'), attr(BOOLEAN, 'color-supported', True),
            attr(KEYWORD, 'print-color-mode-default', 'auto'), attr(KEYWORD, 'print-color-mode-supported', ['auto', 'color', 'monochrome']),
            attr(ENUM, 'print-quality-default', 4), attr(ENUM, 'print-quality-supported', [3, 4, 5]),
            attr(RESOLUTION, 'printer-resolution-default', (300, 300)), attr(RESOLUTION, 'printer-resolution-supported', [(300, 300), (600, 600)]),
            attr(KEYWORD, 'sides-default', 'one-sided'), attr(KEYWORD, 'sides-supported', ['one-sided']),
            attr(KEYWORD, 'output-bin-default', 'face-down'), attr(KEYWORD, 'output-bin-supported', 'face-down'),
            attr(ENUM, 'orientation-requested-default', 3), attr(ENUM, 'orientation-requested-supported', [3, 4]),
            attr(INTEGER, 'copies-default', 1), attr(RANGE, 'copies-supported', (1, 99)),
            attr(KEYWORD, 'multiple-document-handling-supported', ['separate-documents-uncollated-copies', 'separate-documents-collated-copies']),
            attr(KEYWORD, 'media-default', 'iso_a4_210x297mm'), attr(KEYWORD, 'media-supported', list(SIZES)),
            attr(KEYWORD, 'media-ready', list(SIZES)), attr(KEYWORD, 'media-source-supported', 'main'),
            attr(KEYWORD, 'media-type-supported', 'stationery'),
            attr(BEGIN, 'media-col-default', [media_col('iso_a4_210x297mm')]),
            attr(BEGIN, 'media-col-ready', [media_col(s) for s in SIZES]),
            attr(BEGIN, 'media-col-database', [media_col(s) for s in SIZES]),
            attr(KEYWORD, 'media-col-supported', ['media-bottom-margin', 'media-left-margin', 'media-right-margin', 'media-size', 'media-source', 'media-top-margin', 'media-type']),
            attr(BEGIN, 'media-size-supported', [[('x-dimension', INTEGER, x), ('y-dimension', INTEGER, y)] for x, y in SIZES.values()]),
            attr(INTEGER, 'media-bottom-margin-supported', 423), attr(INTEGER, 'media-left-margin-supported', 423),
            attr(INTEGER, 'media-right-margin-supported', 423), attr(INTEGER, 'media-top-margin-supported', 423),
            attr(KEYWORD, 'job-creation-attributes-supported', ['copies', 'media', 'media-col', 'print-color-mode', 'print-quality', 'sides', 'orientation-requested']),
            attr(KEYWORD, 'which-jobs-supported', ['completed', 'not-completed', 'all']),
            attr(INTEGER, 'queued-job-count', 0), attr(KEYWORD, 'printer-kind', 'document'),
        ])

    def job_attributes(self, job, host):
        return b''.join([attr(INTEGER, 'job-id', job['id']), attr(URI, 'job-uri', 'ipp://%s/ipp/print/%d' % (host, job['id'])),
                         attr(ENUM, 'job-state', job['state']), attr(KEYWORD, 'job-state-reasons', job['reasons']),
                         attr(NAME, 'job-name', job['name'] or 'untitled'), attr(NAME, 'job-originating-user-name', job['user'] or 'anonymous'),
                         attr(INTEGER, 'job-impressions-completed', 1 if job['state'] == 9 else 0)])

    def new_job(self, operation_attrs):
        def value(name):
            for n, tag, v in operation_attrs:
                if n == name: return v.decode('utf-8', 'replace')
            return ''
        with self.lock:
            job = {'id': self.next_id, 'name': value('job-name'), 'user': value('requesting-user-name'), 'format': value('document-format'),
                   'path': None, 'bytes': 0, 'state': 5, 'reasons': 'job-incoming'}
            self.next_id += 1; self.jobs.append(job)
        return job

    def store(self, job, document):
        path = self.spool / ('job%03d.pdf' % job['id']); path.write_bytes(document)
        job.update(path=str(path), bytes=len(document), state=9, reasons='job-completed-successfully')

    def handle(self, data, host):
        try: version, operation, request_id, groups, document = parse(data)
        except ValueError as error:
            self.errors.append(str(error)); return struct.pack('>HHI', 0x0200, 0x0400, 0) + b'\x01' + attr(CHARSET, 'attributes-charset', 'utf-8') + b'\x03'
        self.requests.append(OPS.get(operation, hex(operation)))
        head = struct.pack('>HHI', 0x0200, 0, request_id)
        status, extra = 0x0000, b''
        operation_attrs = groups.get(0x01, [])
        def job_id():
            for n, tag, v in operation_attrs:
                if n == 'job-id' and len(v) == 4: return struct.unpack('>i', v)[0]
                if n == 'job-uri': return int(v.decode('ascii', 'replace').rsplit('/', 1)[-1] or 0)
            return 0
        def find(jid): return next((j for j in self.jobs if j['id'] == jid), None)
        if (version >> 8) not in (1, 2): status = 0x0503   # server-error-version-not-supported
        elif operation == 0x000B: extra = b'\x04' + self.printer_attributes(host)
        elif operation == 0x0004: pass
        elif operation == 0x0002:
            job = self.new_job(operation_attrs); self.store(job, document); extra = b'\x02' + self.job_attributes(job, host)
        elif operation == 0x0005:
            job = self.new_job(operation_attrs); job['reasons'] = 'job-data-insufficient'; extra = b'\x02' + self.job_attributes(job, host)
        elif operation == 0x0006:
            job = find(job_id())
            if not job: status = 0x0406   # client-error-not-found
            else: self.store(job, document); extra = b'\x02' + self.job_attributes(job, host)
        elif operation == 0x0009:
            job = find(job_id())
            if not job: status = 0x0406
            else: extra = b'\x02' + self.job_attributes(job, host)
        elif operation == 0x000A: extra = b''.join(b'\x02' + self.job_attributes(j, host) for j in self.jobs)
        elif operation == 0x0008:
            job = find(job_id())
            if not job: status = 0x0406
            elif job['state'] < 7: job.update(state=7, reasons='job-canceled-by-user')
        else: status = 0x0501   # server-error-operation-not-supported
        head = struct.pack('>HHI', 0x0200, status, request_id)
        return head + b'\x01' + attr(CHARSET, 'attributes-charset', 'utf-8') + attr(LANGUAGE, 'attributes-natural-language', 'en') + extra + b'\x03'


if __name__ == '__main__':   # a manual run: python -I tests/ipp_printer.py PORT SPOOL
    import sys
    p = IppPrinter(int(sys.argv[1]), sys.argv[2]); print('IPP printer on port', p.port, flush=True)
    try:
        while True: time.sleep(5); print('jobs:', [(j['id'], j['bytes']) for j in p.jobs], flush=True)
    except KeyboardInterrupt: p.close()

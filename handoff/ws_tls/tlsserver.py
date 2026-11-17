"""Phase 57 host-side test servers for NexusOS (reachable from the VM as 10.0.2.2).

  18101  plain HTTP  : /1k /100k /2m (deterministic bodies), /chunked, /redirect,
                       /redirect-abs, /redirect-https, /slow (1 KB every 200 ms), /echo
  18102  HTTPS 1.3   : self-signed ECDSA P-256 cert, SAN IP:10.0.2.2 + DNS:localhost
  18103  HTTPS 1.3   : self-signed RSA-2048 cert, SAN IP:10.0.2.2
  18104  HTTPS 1.3   : P-256 cert for the WRONG host (SAN DNS:wronghost.test only)
usage: python -I tlsserver.py <work_dir>      (runs until killed; log in <work_dir>/tlsserver.log)
"""
import sys, os, ssl, time, threading, datetime, zlib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
import ipaddress

work = sys.argv[1]
os.makedirs(work, exist_ok=True)
logf = open(os.path.join(work, "tlsserver.log"), "a", buffering=1)
def log(*a):
    logf.write(time.strftime("%H:%M:%S ") + " ".join(str(x) for x in a) + "\n")

def body(n):
    """Deterministic pseudo-random body of n bytes (same function in tlscheck.py)."""
    out = bytearray(n)
    x = 12345 + n
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out[i] = (x >> 16) & 0xff
    return bytes(out)

BODIES = {"/1k": body(1024), "/100k": body(102400), "/2m": body(2 * 1024 * 1024)}
CHUNK_BODY = body(50000)
for k, v in BODIES.items(): log("body", k, len(v), "crc32=%08x" % zlib.crc32(v))
log("chunked", len(CHUNK_BODY), "crc32=%08x" % zlib.crc32(CHUNK_BODY))
log("slow", 20480, "crc32=%08x" % zlib.crc32(body(20480)))

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "NexusTestServer/1.0"
    def log_message(self, fmt, *a): log(self.client_address, fmt % a)

    def _send(self, code, data, ctype="application/octet-stream", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD": self.wfile.write(data)

    def do_HEAD(self): self.do_GET()

    def do_GET(self):
        p = self.path.split("?")[0]
        log("GET", p, "UA=%s" % self.headers.get("User-Agent"), "Host=%s" % self.headers.get("Host"))
        if p in BODIES:
            self._send(200, BODIES[p]); return
        if p == "/" or p == "/index.html":
            self._send(200, b"<!doctype html><html><body><h1>NexusOS test server</h1></body></html>", "text/html; charset=utf-8"); return
        if p == "/echo":
            txt = "".join("%s: %s\n" % (k, v) for k, v in self.headers.items())
            self._send(200, txt.encode(), "text/plain"); return
        if p == "/redirect":
            self._send(302, b"moved", "text/plain", {"Location": "/1k"}); return
        if p == "/redirect-abs":
            self._send(301, b"moved", "text/plain", {"Location": "http://10.0.2.2:18101/1k"}); return
        if p == "/redirect-https":
            self._send(302, b"moved", "text/plain", {"Location": "https://10.0.2.2:18102/1k"}); return
        if p == "/redirect-loop":
            self._send(302, b"moved", "text/plain", {"Location": "/redirect-loop"}); return
        if p == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Checksum")
            self.send_header("Connection", "close")
            self.end_headers()
            sizes = [1, 2, 3, 100, 1000, 4000, 8191, 16000, 9000, 5]
            off, i = 0, 0
            while off < len(CHUNK_BODY):
                n = min(sizes[i % len(sizes)], len(CHUNK_BODY) - off)
                ext = b";ext=1" if i == 3 else b""
                self.wfile.write(b"%x" % n + ext + b"\r\n" + CHUNK_BODY[off:off + n] + b"\r\n")
                self.wfile.flush()
                off += n; i += 1
                if i % 4 == 0: time.sleep(0.05)
            self.wfile.write(b"0\r\nX-Checksum: %08x\r\n\r\n" % zlib.crc32(CHUNK_BODY))
            return
        if p == "/slow":
            data = body(20480)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            for off in range(0, len(data), 1024):
                self.wfile.write(data[off:off + 1024]); self.wfile.flush()
                time.sleep(0.2)
            return
        self._send(404, b"not found", "text/plain")

def make_cert(path_base, key, cn, sans, days_before=1, days_after=30):
    now = datetime.datetime.now(datetime.timezone.utc)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn), x509.NameAttribute(NameOID.ORGANIZATION_NAME, "NexusOS Test")])
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now - datetime.timedelta(days=days_before))
            .not_valid_after(now + datetime.timedelta(days=days_after))
            .add_extension(x509.SubjectAlternativeName(sans), critical=False)
            .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
            .sign(key, hashes.SHA256()))
    open(path_base + ".crt", "wb").write(cert.public_bytes(serialization.Encoding.PEM))
    open(path_base + ".key", "wb").write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))
    return path_base + ".crt", path_base + ".key"

def serve(port, certfiles=None):
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    srv.daemon_threads = True
    if certfiles:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
        ctx.load_cert_chain(*certfiles)
        ctx.set_alpn_protocols(["http/1.1"])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    log("listening", port, certfiles)
    srv.serve_forever()

ip = ipaddress.IPv4Address("10.0.2.2")
c_ec  = make_cert(os.path.join(work, "ec"),  ec.generate_private_key(ec.SECP256R1()), "nexus-test-ec",
                  [x509.IPAddress(ip), x509.DNSName("localhost")])
c_rsa = make_cert(os.path.join(work, "rsa"), rsa.generate_private_key(65537, 2048), "nexus-test-rsa",
                  [x509.IPAddress(ip)])
c_bad = make_cert(os.path.join(work, "bad"), ec.generate_private_key(ec.SECP256R1()), "wronghost.test",
                  [x509.DNSName("wronghost.test")])
for port, cf in ((18101, None), (18102, c_ec), (18103, c_rsa), (18104, c_bad)):
    threading.Thread(target=serve, args=(port, cf), daemon=True).start()
print("tlsserver up on 18101-18104", flush=True)
open(os.path.join(work, "tlsserver.ready"), "w").write("ok")
while True: time.sleep(3600)

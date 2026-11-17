"""Phase 55 host-side network tests for NexusOS, run by harness.py `run:<func>`
steps while the VM is up. Each check logs PASS/FAIL with the bytes seen.
ctx['ports'] maps guest port -> host port (from HOSTFWD)."""
import socket, struct, time, json, http.client, base64
import pydes

RESULTS = []

def check(ctx, name, ok, detail=""):
    tag = "PASS" if ok else "FAIL"
    RESULTS.append((name, ok))
    ctx['log'](f"{tag}: {name}  {detail}")
    return ok

def recvn(s, n):
    buf = b''
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError("peer closed (have %d/%d)" % (len(buf), n))
        buf += d
    return buf

def rfb_connect(ctx, timeout=10):
    s = socket.create_connection(('127.0.0.1', ctx['ports'][5900]), timeout=timeout)
    s.settimeout(timeout)
    t0 = time.time()
    ver = recvn(s, 12)
    return s, ver, time.time() - t0

# ---------------------------------------------------------------------------
# DES
# ---------------------------------------------------------------------------
def des_vector(ctx):
    key = bytes.fromhex('133457799BBCDFF1'); pt = bytes.fromhex('0123456789ABCDEF')
    ct = pydes.encrypt(key, pt)
    check(ctx, "host DES FIPS-46 vector", ct.hex().upper() == '85E813540F0AB405', "got " + ct.hex().upper())

# ---------------------------------------------------------------------------
# VNC
# ---------------------------------------------------------------------------
def vnc_auth(ctx):
    """Server must be started with a VNC password 'secret1'."""
    log = ctx['log']
    # --- (1) wrong password, RFB 3.3 ---
    s, ver, dt = rfb_connect(ctx)
    log("server version:", ver)
    check(ctx, "vnc: server announces RFB 003.00x", ver.startswith(b"RFB 003.00"), repr(ver))
    s.sendall(b"RFB 003.003\n")
    sec = struct.unpack(">I", recvn(s, 4))[0]
    check(ctx, "vnc: 3.3 security type == 2 (VNC Authentication)", sec == 2, f"type={sec}")
    chal1 = recvn(s, 16)
    log("challenge#1:", chal1.hex())
    s.sendall(pydes.vnc_response("wrongpw", chal1))
    res = struct.unpack(">I", recvn(s, 4))[0]
    check(ctx, "vnc: wrong password -> SecurityResult 1", res == 1, f"result={res}")
    closed = False
    try:
        d = s.recv(64)
        closed = (d == b'')
        log("after failure recv ->", repr(d))
    except (ConnectionResetError, ConnectionAbortedError, OSError) as e:
        closed = True; log("after failure recv -> exception", type(e).__name__)
    check(ctx, "vnc: socket closed after failed auth", closed)
    s.close()
    t_fail = time.time()

    # --- (2) right password, RFB 3.3; the handshake must be stalled by the 3 s lockout ---
    s, ver, dt = rfb_connect(ctx)
    waited = time.time() - t_fail
    log(f"version after lockout arrived {waited:.2f}s after the failure")
    check(ctx, "vnc: 3 s lockout stalls the next handshake (>= 2.5 s)", waited >= 2.5, f"{waited:.2f}s")
    s.sendall(b"RFB 003.003\n")
    sec = struct.unpack(">I", recvn(s, 4))[0]
    chal2 = recvn(s, 16)
    log("challenge#2:", chal2.hex())
    check(ctx, "vnc: challenge differs per connection", chal1 != chal2)
    s.sendall(pydes.vnc_response("secret1", chal2))
    res = struct.unpack(">I", recvn(s, 4))[0]
    check(ctx, "vnc: right password -> SecurityResult 0", res == 0, f"result={res}")
    s.sendall(b"\x01")                      # ClientInit (shared)
    hdr = recvn(s, 24)
    w, h = struct.unpack(">HH", hdr[0:4])
    bpp, depth = hdr[4], hdr[5]
    nlen = struct.unpack(">I", hdr[20:24])[0]
    name = recvn(s, nlen)
    log(f"ServerInit {w}x{h} bpp={bpp} depth={depth} name={name!r}")
    check(ctx, "vnc: ServerInit 1920x1080 name NexusOS", (w, h, name) == (1920, 1080, b"NexusOS"), f"{w}x{h} {name!r}")
    s.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))        # SetEncodings Raw
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))                    # FramebufferUpdateRequest
    s.settimeout(15)
    mt = recvn(s, 1)[0]
    check(ctx, "vnc: FramebufferUpdate message type 0", mt == 0, f"type={mt}")
    recvn(s, 1); nrects = struct.unpack(">H", recvn(s, 2))[0]
    rx, ry, rw, rh, enc = struct.unpack(">HHHHi", recvn(s, 12))
    log(f"rect {rw}x{rh} at ({rx},{ry}) enc={enc} nrects={nrects}")
    want = min(rw * rh * 4, 262144)
    data = recvn(s, want)
    nz = sum(1 for b in data if b)
    check(ctx, "vnc: raw pixel data received, non-zero bytes", nz > 0 and enc == 0 and rw == 1920,
          f"{len(data)} bytes, {nz} non-zero, first16={data[:16].hex()}")
    s.close()
    time.sleep(0.5)

    # --- (3) RFB 3.8 path: type list, SecurityResult + reason on failure ---
    s, ver, dt = rfb_connect(ctx)
    s.sendall(b"RFB 003.008\n")
    n = recvn(s, 1)[0]; types = recvn(s, n)
    check(ctx, "vnc: 3.8 security type list == [2]", list(types) == [2], f"n={n} types={list(types)}")
    s.sendall(b"\x02")
    chal3 = recvn(s, 16)
    s.sendall(pydes.vnc_response("nope", chal3))
    res = struct.unpack(">I", recvn(s, 4))[0]
    rl = struct.unpack(">I", recvn(s, 4))[0]; reason = recvn(s, rl)
    check(ctx, "vnc: 3.8 failure -> result 1 + reason string", res == 1 and b"fail" in reason, f"result={res} reason={reason!r}")
    s.close()
    time.sleep(3.5)                          # let the lockout expire for the next test
    s, ver, dt = rfb_connect(ctx)
    s.sendall(b"RFB 003.008\n")
    n = recvn(s, 1)[0]; recvn(s, n); s.sendall(b"\x02")
    chal4 = recvn(s, 16)
    s.sendall(pydes.vnc_response("secret1", chal4))
    res = struct.unpack(">I", recvn(s, 4))[0]
    check(ctx, "vnc: 3.8 right password -> SecurityResult 0", res == 0, f"result={res}")
    s.sendall(b"\x01"); hdr = recvn(s, 24)
    w, h = struct.unpack(">HH", hdr[0:4]); nlen = struct.unpack(">I", hdr[20:24])[0]; name = recvn(s, nlen)
    check(ctx, "vnc: 3.8 ServerInit", (w, h, name) == (1920, 1080, b"NexusOS"), f"{w}x{h} {name!r}")
    s.close()
    time.sleep(0.5)

def vnc_insecure(ctx):
    s, ver, dt = rfb_connect(ctx)
    s.sendall(b"RFB 003.003\n")
    sec = struct.unpack(">I", recvn(s, 4))[0]
    check(ctx, "vnc --insecure: 3.3 security type == 1 (None)", sec == 1, f"type={sec}")
    s.sendall(b"\x01"); hdr = recvn(s, 24)
    w, h = struct.unpack(">HH", hdr[0:4]); nlen = struct.unpack(">I", hdr[20:24])[0]; name = recvn(s, nlen)
    check(ctx, "vnc --insecure: ServerInit", (w, h, name) == (1920, 1080, b"NexusOS"), f"{w}x{h} {name!r}")
    s.close()
    time.sleep(0.5)

# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
def _http(ctx, method, path, body=None, headers=None, timeout=20):
    """One HTTP exchange. A transport failure becomes status -1 (so the check
    FAILs with the exception text instead of aborting the whole group)."""
    try:
        c = http.client.HTTPConnection('127.0.0.1', ctx['ports'][8080], timeout=timeout)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        data = r.read()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        return r.status, hdrs, data
    except Exception as e:
        return -1, {}, ("EXC %s: %s" % (type(e).__name__, e)).encode()

def _basic(user, pw):
    return {"Authorization": "Basic " + base64.b64encode(f"{user}:{pw}".encode()).decode()}

def _raw(ctx, request_bytes, timeout=10):
    buf = b''
    try:
        s = socket.create_connection(('127.0.0.1', ctx['ports'][8080]), timeout=timeout)
        s.sendall(request_bytes)
        while True:
            d = s.recv(4096)
            if not d: break
            buf += d
        s.close()
    except socket.timeout:
        pass
    except Exception as e:
        buf += ("EXC %s: %s" % (type(e).__name__, e)).encode()
    return buf

def http_tests(ctx):
    log = ctx['log']
    st, h, d = _http(ctx, "GET", "/")
    check(ctx, "http: GET / -> 200 text/html with NexusOS", st == 200 and "text/html" in h.get("content-type", "") and b"NexusOS" in d,
          f"status={st} ctype={h.get('content-type')} len={len(d)} clen={h.get('content-length')} first={d[:40]!r}")
    check(ctx, "http: Content-Length matches body", int(h.get("content-length", -1)) == len(d), f"{h.get('content-length')} vs {len(d)}")

    st, h, d = _http(ctx, "GET", "/status")
    check(ctx, "http: GET /status -> 200 with Uptime", st == 200 and b"Uptime" in d, f"status={st} len={len(d)}")

    st, h, d = _http(ctx, "GET", "/api/status")
    try:
        js = json.loads(d.decode()); ok = st == 200 and "uptime" in js and "mem_free_kb" in js and "ip" in js
    except Exception as e:
        js = str(e); ok = False
    check(ctx, "http: GET /api/status -> 200 JSON parses", ok, f"status={st} ctype={h.get('content-type')} json={js}")

    st, h, d = _http(ctx, "GET", "/style.css")
    check(ctx, "http: GET /style.css -> 200 text/css", st == 200 and h.get("content-type") == "text/css", f"status={st} ctype={h.get('content-type')} len={len(d)}")

    st, h, d = _http(ctx, "GET", "/nope")
    check(ctx, "http: GET /nope -> 404", st == 404, f"status={st} body={d[:60]!r}")

    st, h, d = _http(ctx, "GET", "/fs/home/root/")
    check(ctx, "http: GET /fs/home/root/ no auth -> 401 + WWW-Authenticate", st == 401 and "basic" in h.get("www-authenticate", "").lower(),
          f"status={st} www-authenticate={h.get('www-authenticate')!r}")

    t0 = time.time()
    st, h, d = _http(ctx, "GET", "/fs/home/root/", headers=_basic("root", "wrongpw"))
    check(ctx, "http: wrong password -> 401", st == 401, f"status={st}")
    st, h, d = _http(ctx, "GET", "/fs/home/root/", headers=_basic("root", "root"))
    dt = time.time() - t0
    check(ctx, "http: root:root -> 200 listing with hello.txt", st == 200 and b"hello.txt" in d, f"status={st} len={len(d)}")
    check(ctx, "http: 1 s lockout delayed the next credentialed request (>= 0.8 s)", dt >= 0.8, f"{dt:.2f}s for fail+success")

    st, h, d = _http(ctx, "GET", "/fs/home/root/hello.txt", headers=_basic("root", "root"))
    check(ctx, "http: GET /fs/home/root/hello.txt -> hello-web", st == 200 and d == b"hello-web", f"status={st} body={d!r} ctype={h.get('content-type')}")

    st, h, d = _http(ctx, "PUT", "/fs/home/root/up.txt", body=b"uploaded", headers=_basic("root", "root"))
    check(ctx, "http: PUT /fs/home/root/up.txt -> 201", st == 201, f"status={st} location={h.get('location')} body={d!r}")
    st, h, d = _http(ctx, "GET", "/fs/home/root/up.txt", headers=_basic("root", "root"))
    check(ctx, "http: GET back up.txt == uploaded", st == 200 and d == b"uploaded", f"status={st} body={d!r}")
    st, h, d = _http(ctx, "PUT", "/fs/home/root/up.txt", body=b"uploaded", headers=_basic("root", "root"))
    check(ctx, "http: PUT existing file -> 204", st == 204, f"status={st}")

    big = bytes((i * 7 + 3) & 0xFF if (i * 7 + 3) & 0xFF not in (0,) else 1 for i in range(2048))
    big = b''.join(b"%04d-abcdefghij\n" % i for i in range(128))   # 128 * 16 = 2048 bytes, text
    st, h, d = _http(ctx, "PUT", "/fs/home/root/big.txt", body=big, headers=_basic("root", "root"))
    check(ctx, "http: PUT 2 KB body -> 201", st == 201, f"status={st}")
    st, h, d = _http(ctx, "GET", "/fs/home/root/big.txt", headers=_basic("root", "root"))
    check(ctx, "http: 2 KB file round-trips intact (chunked send)", st == 200 and d == big, f"status={st} len={len(d)} same={d == big}")

    st, h, d = _http(ctx, "HEAD", "/")
    check(ctx, "http: HEAD / -> 200, headers only", st == 200 and int(h.get("content-length", 0)) > 0 and d == b"",
          f"status={st} clen={h.get('content-length')} body={len(d)}")

    st, h, d = _http(ctx, "GET", "/fs/www/", headers=_basic("root", "root"))
    check(ctx, "http: listing of /www shows index.html and style.css", st == 200 and b"index.html" in d and b"style.css" in d, f"status={st} len={len(d)}")

    st, h, d = _http(ctx, "GET", "/fs/etc/vncpasswd", headers=_basic("guest", "guest"))
    check(ctx, "http: guest reading 0600 /etc/vncpasswd -> 403", st == 403, f"status={st}")
    st, h, d = _http(ctx, "PUT", "/fs/home/root/evil.txt", body=b"x", headers=_basic("guest", "guest"))
    check(ctx, "http: guest PUT into /home/root -> 403", st == 403, f"status={st}")
    st, h, d = _http(ctx, "PUT", "/fs/home/root/x.txt", body=b"x")
    check(ctx, "http: PUT without auth -> 401", st == 401, f"status={st}")
    st, h, d = _http(ctx, "PUT", "/up.txt", body=b"x")
    check(ctx, "http: PUT outside /fs -> 405", st == 405, f"status={st} allow={h.get('allow')}")
    st, h, d = _http(ctx, "DELETE", "/")
    check(ctx, "http: DELETE -> 405", st == 405, f"status={st}")
    st, h, d = _http(ctx, "GET", "/fs/does/not/exist", headers=_basic("root", "root"))
    check(ctx, "http: GET /fs/<missing> -> 404", st == 404, f"status={st}")

    r = _raw(ctx, b"GET /../etc HTTP/1.0\r\nHost: x\r\n\r\n")
    check(ctx, "http: GET /../etc rejected (400)", r.startswith(b"HTTP/1.0 400"), repr(r[:60]))
    r = _raw(ctx, b"GET /fs/../etc/vncpasswd HTTP/1.0\r\n" + ("Authorization: %s\r\n" % _basic("root", "root")["Authorization"]).encode() + b"\r\n")
    check(ctx, "http: GET /fs/.. rejected (400)", r.startswith(b"HTTP/1.0 400"), repr(r[:60]))
    r = _raw(ctx, b"GET /%2e%2e/etc HTTP/1.0\r\n\r\n")
    check(ctx, "http: encoded %2e%2e rejected (400)", r.startswith(b"HTTP/1.0 400"), repr(r[:60]))
    r = _raw(ctx, b"GARBAGE\r\n\r\n")
    check(ctx, "http: malformed request line -> 400", r.startswith(b"HTTP/1.0 400"), repr(r[:60]))
    r = _raw(ctx, b"GET /status?x=1&y=2 HTTP/1.1\r\nHost: x\r\n\r\n")
    check(ctx, "http: query string stripped (GET /status?x=1 -> 200)", r.startswith(b"HTTP/1.0 200"), repr(r[:40]))
    r = _raw(ctx, b"GET /fs/home/root/hello%2etxt HTTP/1.0\r\n" + ("Authorization: %s\r\n" % _basic("root", "root")["Authorization"]).encode() + b"\r\n")
    check(ctx, "http: percent-decoding (hello%2etxt)", r.startswith(b"HTTP/1.0 200") and r.endswith(b"hello-web"), repr(r[-30:]))
    big_put = b"PUT /fs/home/root/toobig.txt HTTP/1.0\r\nContent-Length: 5000\r\n" + ("Authorization: %s\r\n" % _basic("root", "root")["Authorization"]).encode() + b"\r\n"
    r = _raw(ctx, big_put)
    check(ctx, "http: PUT Content-Length 5000 -> 413", r.startswith(b"HTTP/1.0 413"), repr(r[:60]))
    # a request split across two TCP segments (headers arrive in pieces)
    s = socket.create_connection(('127.0.0.1', ctx['ports'][8080]), timeout=10)
    s.sendall(b"GET /api/sta"); time.sleep(0.4); s.sendall(b"tus HTTP/1.0\r\nHost: x"); time.sleep(0.4); s.sendall(b"\r\n\r\n")
    buf = b''
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            buf += d
    except socket.timeout: pass
    s.close()
    check(ctx, "http: request split across 3 segments still served", buf.startswith(b"HTTP/1.0 200") and b'"uptime"' in buf, repr(buf[:40]))
    # many sequential connections (TCB slots must be recycled)
    okc = 0
    for i in range(12):
        st, h, d = _http(ctx, "GET", "/api/status")
        okc += (st == 200)
    check(ctx, "http: 12 sequential connections all 200 (TCB recycling)", okc == 12, f"{okc}/12")
    # browser-style parallel connections: open the second before reading the first
    def _recv_all(sock):
        buf = b''
        try:
            while True:
                d = sock.recv(4096)
                if not d: break
                buf += d
        except Exception as e:
            buf += ("EXC %s" % type(e).__name__).encode()
        return buf
    a = socket.create_connection(('127.0.0.1', ctx['ports'][8080]), timeout=15)
    b = socket.create_connection(('127.0.0.1', ctx['ports'][8080]), timeout=15)
    a.sendall(b"GET / HTTP/1.0\r\n\r\n"); b.sendall(b"GET /style.css HTTP/1.0\r\n\r\n")
    ra = _recv_all(a); rb = _recv_all(b); a.close(); b.close()
    check(ctx, "http: 2 parallel connections (HTML + CSS) both served", ra.startswith(b"HTTP/1.0 200") and rb.startswith(b"HTTP/1.0 200") and b"text/css" in rb,
          f"a={ra[:20]!r} b={rb[:20]!r} css_len={len(rb)}")

# ---------------------------------------------------------------------------
# Regression: rshell / sync login handshakes
# ---------------------------------------------------------------------------
def _step(ctx, name, fn):
    """Run one socket conversation; a transport error is a FAIL, not an abort."""
    try:
        fn()
    except Exception as e:
        check(ctx, name, False, "EXC %s: %s" % (type(e).__name__, e))

def rshell_sync(ctx):
    def rs1():
        s = socket.create_connection(('127.0.0.1', ctx['ports'][2323]), timeout=10); s.settimeout(10)
        time.sleep(1.0)
        banner = s.recv(1024)
        s.sendall(b"LOGIN root root\r\n"); time.sleep(0.8)
        resp = s.recv(1024)
        check(ctx, "rshell: LOGIN root root -> 230 Logged in", b"230" in resp, f"banner={banner[:50]!r} resp={resp!r}")
        s.sendall(b"whoami\r\n"); time.sleep(0.6); r2 = s.recv(1024)
        check(ctx, "rshell: whoami == root", b"root" in r2, repr(r2))
        s.sendall(b"exit\r\n"); time.sleep(0.3); s.close()
    _step(ctx, "rshell: first session", rs1)
    def rs2():
        # immediate reconnect right after the server-initiated close (exercises
        # the TCP orphan-adoption path: the SYN lands before rshell re-listens)
        s = socket.create_connection(('127.0.0.1', ctx['ports'][2323]), timeout=10); s.settimeout(10)
        time.sleep(1.0); banner = s.recv(1024)
        s.sendall(b"LOGIN root bad\r\n"); time.sleep(0.8); resp = s.recv(1024)
        check(ctx, "rshell: immediate reconnect gets a banner", b"Remote Shell" in banner, repr(banner[:60]))
        check(ctx, "rshell: LOGIN root bad -> 530", b"530" in resp, repr(resp))
        s.close()
    _step(ctx, "rshell: reconnect + bad login", rs2)
    def sy1():
        s = socket.create_connection(('127.0.0.1', ctx['ports'][7070]), timeout=10); s.settimeout(10)
        s.sendall(b"LOGIN root root\r\n"); time.sleep(0.8); resp = s.recv(1024)
        check(ctx, "sync: LOGIN root root -> OK auth", resp.startswith(b"OK"), repr(resp))
        s.sendall(b"LIST\r\n"); time.sleep(1.0); lst = s.recv(4096)
        check(ctx, "sync: LIST answers FILE ... END", b"END" in lst, repr(lst[:80]))
        s.sendall(b"BYE\r\n"); time.sleep(0.3); s.close()
    _step(ctx, "sync: login + LIST", sy1)
    def sy2():
        s = socket.create_connection(('127.0.0.1', ctx['ports'][7070]), timeout=10); s.settimeout(10)
        s.sendall(b"LIST\r\n"); time.sleep(0.8); resp = s.recv(1024)
        check(ctx, "sync: LIST without login -> ERR auth", b"ERR auth" in resp, repr(resp))
        s.sendall(b"BYE\r\n"); time.sleep(0.3); s.close()
    _step(ctx, "sync: unauthenticated LIST", sy2)

def summary(ctx):
    n_ok = sum(1 for _, ok in RESULTS if ok)
    ctx['log'](f"SUMMARY: {n_ok}/{len(RESULTS)} host checks passed")
    for name, ok in RESULTS:
        if not ok: ctx['log']("  FAILED:", name)

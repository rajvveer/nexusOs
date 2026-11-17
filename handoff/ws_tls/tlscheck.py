"""Evaluate a Phase 57 serial log: parse the "[NET] ..." marker lines written by
netcmd.c and check them against expectations. usage: python -I tlscheck.py <serial.log>"""
import sys, re, zlib

def body(n):
    out = bytearray(n); x = 12345 + n
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        out[i] = (x >> 16) & 0xff
    return bytes(out)
CRC = {n: "%08X" % zlib.crc32(body(n)) for n in (1024, 102400, 2 * 1024 * 1024, 50000, 20480)}

log = open(sys.argv[1], "rb").read().decode("latin1", "replace")
lines = [l.strip() for l in log.splitlines() if l.startswith("[NET]")]
results = []
def check(name, ok, detail=""):
    results.append((name, ok)); print(("PASS " if ok else "FAIL ") + name + ("  " + detail if detail else ""))

def kv(line):
    d = {}
    for m in re.finditer(r"(\w+)=((?:(?!\s\w+=).)*)", line):
        d[m.group(1)] = m.group(2).strip()
    if " info=" in line:
        d["info"] = line.split(" info=", 1)[1]
    return d

curls = [kv(l) for l in lines if l.startswith("[NET] curl ")]
tls   = [kv(l) for l in lines if l.startswith("[NET] tls ")]
fails = [kv(l) for l in lines if l.startswith("[NET] fail ")]
infos = [kv(l) for l in lines if l.startswith("[NET] tlsinfo ")]
wgets = [kv(l) for l in lines if l.startswith("[NET] wget ")]
nets  = [kv(l) for l in lines if l.startswith("[NET] netstat ")]
kats  = [l for l in lines if l.startswith("[NET] kat ")]

def curl_for(url):
    return [c for c in curls if c.get("url") == url and c.get("head") != "1"]

# --- crypto KATs ---
if kats:
    bad = [k for k in kats if " FAIL " in k]
    check("cryptotest: %d known-answer tests, %d failures" % (len(kats), len(bad)), not bad, "; ".join(bad))
else:
    check("cryptotest ran", False)

# --- local HTTP ---
def expect_body(url, n, extra=""):
    cs = curl_for(url)
    ok = bool(cs) and all(c.get("ok") == "1" and c.get("bytes") == str(n) and c.get("crc") == CRC[n] for c in cs)
    check("curl %s -> %d bytes, CRC %s%s (%d run%s)" % (url, n, CRC[n], extra, len(cs), "s" if len(cs) != 1 else ""), ok,
          "" if ok else str(cs[:2]))
    return cs
expect_body("http://10.0.2.2:18101/1k", 1024)
c100 = expect_body("http://10.0.2.2:18101/100k", 102400)
c2m = expect_body("http://10.0.2.2:18101/2m", 2 * 1024 * 1024)
for c in c2m:
    t = int(c.get("ticks", "0")) or 1
    print("   2 MB throughput: %.1f KB/s (%.1f s)" % (2097152 * 18.2 / t / 1024, t / 18.2))
ch = expect_body("http://10.0.2.2:18101/chunked", 50000, " chunked")
check("chunked flag set", bool(ch) and all(c.get("chunked") == "1" for c in ch))
expect_body("http://10.0.2.2:18101/slow", 20480, " (slow drip 200 ms)")
expect_body("http://10.0.2.2:18101/redirect", 1024, " via 302")
expect_body("http://10.0.2.2:18101/redirect-abs", 1024, " via 301 absolute")
expect_body("http://10.0.2.2:18101/redirect-https", 1024, " via 302 http->https")
rh = [t for t in tls if t.get("url") == "https://10.0.2.2:18102/1k"]
check("redirect-https landed on TLS (trusted=0 self-signed)", bool(rh) and all(t.get("trusted") == "0" for t in rh))

# --- local HTTPS ---
rejected = [f for f in fails if f.get("url") == "https://10.0.2.2:18102/1k"]
check("self-signed certificate rejected without -k", bool(rejected) and all("self-signed" in f.get("err", "") for f in rejected))
expect_body("https://10.0.2.2:18102/2m", 2 * 1024 * 1024)
for url, what in (("https://10.0.2.2:18102/100k", "self-signed P-256"), ("https://10.0.2.2:18103/100k", "self-signed RSA-2048")):
    cs = curl_for(url)
    ok = bool(cs) and all(c.get("ok") == "1" and c.get("bytes") == "102400" and c.get("crc") == CRC[102400] for c in cs)
    check("curl %s (%s) -> 100 KB OK" % (url, what), ok, str(cs[:1]) if not ok else "")
    ts = [t for t in tls if t.get("url") == url]
    check("  %s: connected with trusted=0 and self-signed reason" % what,
          bool(ts) and all(t.get("trusted") == "0" and "self-signed" in t.get("info", "") for t in ts), str(ts[:1]))
bad = [f for f in fails if f.get("url") == "https://10.0.2.2:18104/1k"]
check("wrong-hostname cert refused (hostname mismatch)", bool(bad) and all("hostname mismatch" in f.get("err", "") for f in bad), str(bad[:1]))
ins = curl_for("https://10.0.2.2:18104/1k")
check("X-Nexus-Insecure (-k) overrides the hostname failure -> 1 KB fetched",
      bool(ins) and any(c.get("ok") == "1" and c.get("bytes") == "1024" for c in ins), str(ins[:2]))

# --- real sites ---
EXPECT_ROOT = {"example.com": None, "www.google.com": None, "www.cloudflare.com": None, "letsencrypt.org": None}
for host in EXPECT_ROOT:
    ii = [i for i in infos if i.get("host") == host]
    ok = bool(ii) and all(i.get("result") == "ok" and i.get("trusted") == "1" and i.get("root") for i in ii)
    check("tlsinfo %s -> handshake ok, trusted=1, root=%s" % (host, ii[0].get("root") if ii else "?"), ok,
          "" if ok else str(ii[:1]))
    for i in ii: print("    handshake %s ms, rx %s B, tx %s B, certs %s, alpn %s" % (i.get("ms"), i.get("rx"), i.get("tx"), i.get("certs"), i.get("alpn")))
ex = curl_for("https://example.com")
check("curl https://example.com -> 200 with body", bool(ex) and all(c.get("ok") == "1" and c.get("status") == "200" and int(c.get("bytes", "0")) > 200 for c in ex), str(ex[:1]))
wk = curl_for("https://www.wikipedia.org")
check("curl https://www.wikipedia.org -> 200, body counted", bool(wk) and all(c.get("ok") == "1" and c.get("status") == "200" and int(c.get("bytes", "0")) > 10000 for c in wk), str(wk[:1]))
for c in wk: print("    wikipedia: %s bytes, chunked=%s, %.1f KB/s" % (c.get("bytes"), c.get("chunked"), int(c["bytes"]) * 18.2 / max(1, int(c.get("ticks", "1"))) / 1024))
wg = [w for w in wgets if w.get("url") == "https://example.com"]
check("wget https://example.com -> saved file (ok=1, written>0)", bool(wg) and all(w.get("ok") == "1" and int(w.get("written", "0")) > 0 for w in wg), str(wg[:1]))
for t in tls:
    if t.get("url", "").startswith("https://example.com") or "google" in t.get("url", "") or "wikipedia" in t.get("url", ""):
        check("  TLS line %s trusted=1" % t.get("url"), t.get("trusted") == "1", t.get("info", ""))

# --- robustness ---
check("20 sequential curl 100k all OK", len(c100) >= 21 and all(c.get("ok") == "1" for c in c100), "%d runs" % len(c100))
if nets:
    last = nets[-1]
    check("netstat after the campaign: no stuck TCBs (tcbs=%s), no streams leaked (streams=%s)" % (last.get("tcbs"), last.get("streams")),
          last.get("tcbs") == "0" and last.get("streams") == "0", str(last))
    print("    TCP counters:", last)
else:
    check("netstat marker present", False)
check("no !EX: in serial", "!EX:" not in log)
check("no !KR: in serial", "!KR:" not in log, log[log.find("!KR:"):log.find("!KR:") + 60] if "!KR:" in log else "")
n_ok = sum(1 for _, ok in results if ok)
print("SUMMARY: %d/%d checks passed" % (n_ok, len(results)))

sys.exit(0 if n_ok == len(results) else 1)

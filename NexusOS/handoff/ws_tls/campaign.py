"""Phase 57 full VM test campaign: builds the harness57 step script and runs it.
usage: python -I campaign.py <work_dir> <monitor_port> <tag>
Requires tlsserver.py running (ports 18101-18104). Evaluate with tlscheck.py afterwards."""
import os, sys, subprocess
ws = os.path.dirname(os.path.abspath(__file__))
work, port, tag = sys.argv[1], sys.argv[2], sys.argv[3]
proj = sys.argv[4] if len(sys.argv) > 4 else os.path.join(ws, "NexusOS")
os.makedirs(work, exist_ok=True)

steps = ["sleep:9", "type:root", "ret", "sleep:1", "type:root", "ret", "sleep:3", "key:f12", "sleep:2"]
def cmd(c, marker, shot=None):
    steps.append("cmd:" + c); steps.append("waitser:" + marker)
    if shot: steps.append("shot:" + shot)

cmd("cryptotest", "[NET] cryptotest done", "kat")
# --- local plain HTTP ---
cmd("curl -q http://10.0.2.2:18101/1k", "[NET] curl url=", "c1k")
cmd("curl -q http://10.0.2.2:18101/100k", "[NET] curl url=", "c100k")
cmd("curl -q http://10.0.2.2:18101/chunked", "[NET] curl url=", "cchunked")
cmd("curl -q http://10.0.2.2:18101/slow", "[NET] curl url=", "cslow")
cmd("curl -q http://10.0.2.2:18101/redirect", "[NET] curl url=", "credir")
cmd("curl -q http://10.0.2.2:18101/redirect-abs", "[NET] curl url=")
cmd("curl -I http://10.0.2.2:18101/100k", "[NET] curl url=", "chead")
cmd("curl -q http://10.0.2.2:18101/2m", "[NET] curl url=", "c2m")
# --- local HTTPS (test certificates require explicit -k) ---
cmd("curl -q https://10.0.2.2:18102/1k", "[NET] curl url=", "selfsigned_rejected")
cmd("tlsinfo 10.0.2.2:18102 -k", "[NET] tlsinfo host=", "tls_local_ec")
cmd("curl -q -k https://10.0.2.2:18102/100k", "[NET] curl url=", "c100k_tls")
cmd("tlsinfo 10.0.2.2:18103 -k", "[NET] tlsinfo host=", "tls_local_rsa")
cmd("curl -q -k https://10.0.2.2:18103/100k", "[NET] curl url=")
cmd("curl -q https://10.0.2.2:18104/1k", "[NET] ", "c_wronghost")
cmd("curl -q -k https://10.0.2.2:18104/1k", "[NET] curl url=", "c_wronghost_k")
cmd("curl -q -k http://10.0.2.2:18101/redirect-https", "[NET] curl url=", "credir_https")
cmd("curl -q -k https://10.0.2.2:18102/2m", "[NET] curl url=", "c2m_tls")
# --- real internet ---
for host in ("example.com", "www.google.com", "www.cloudflare.com", "letsencrypt.org"):
    cmd("tlsinfo " + host, "[NET] tlsinfo host=", "tls_" + host.replace(".", "_"))
cmd("curl https://example.com", "[NET] curl url=", "curl_example")
cmd("curl -q https://www.wikipedia.org", "[NET] curl url=", "curl_wikipedia")
cmd("mkdir /tmp", "")
steps.pop()                              # mkdir has no marker; just a short wait
steps.append("sleep:1")
cmd("wget https://example.com /disk/ex.html", "[NET] wget url=", "wget_example")
steps += ["cmd:cat /disk/ex.html", "sleep:2", "shot:wget_cat"]
# --- robustness: 20 sequential downloads, then netstat ---
for i in range(20):
    cmd("curl -q http://10.0.2.2:18101/100k", "[NET] curl url=")
steps += ["cmd:netstat", "waitser:[NET] netstat", "shot:netstat"]
steps += ["cmd:ping 10.0.2.2", "sleep:7", "shot:ping", "cmd:dns example.com", "sleep:3", "shot:dns", "serial"]

script = ";".join(steps)
open(os.path.join(work, f"campaign_{tag}_script.txt"), "w").write(script)
env = dict(os.environ, WAITSER_TIMEOUT=os.environ.get("WAITSER_TIMEOUT", "240"))
r = subprocess.run([sys.executable, "-I", os.path.join(ws, "harness57.py"), proj, work, port, tag, "virtio", script], env=env)
print("campaign exit", r.returncode, flush=True)
checked = subprocess.run([sys.executable, "-I", os.path.join(ws, "tlscheck.py"), os.path.join(work, f"serial_{tag}.log")])
sys.exit(r.returncode or checked.returncode)

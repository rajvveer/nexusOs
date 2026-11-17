"""Host side of the Wine campaign (tests/test_wine.py): servers the guest reaches at 10.0.2.2 (QEMU user network).

  HostServers(http_port, tcp_port, files_dir, upload_dir)
    HTTP  GET /gate/NAME   200 once the test opened the gate (open_gate), 404 before
          GET /file/NAME   a file from files_dir
          POST /upload/NAME  stored into upload_dir
    TCP   "PING <text>\\n" -> "PONG <text>\\n" (winetest.exe's winsock group)
"""
from pathlib import Path
import http.server
import socket
import socketserver
import threading


class HostServers:
    def __init__(self, http_port, tcp_port, files_dir=None, upload_dir=None):
        self.gates = set(); self.files = Path(files_dir) if files_dir else None
        self.uploads = Path(upload_dir) if upload_dir else None; self.pings = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass

            def reply(self, code, body=b''):
                self.send_response(code); self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)

            def do_GET(self):
                if self.path.startswith('/gate/'):
                    self.reply(200 if self.path[6:] in owner.gates else 404, b'gate'); return
                if self.path.startswith('/file/') and owner.files:
                    name = self.path[6:]
                    target = (owner.files / name).resolve()
                    if '/' not in name and '\\' not in name and target.is_file() and target.parent == owner.files.resolve():
                        self.reply(200, target.read_bytes()); return
                self.reply(404)

            def do_POST(self):
                size = int(self.headers.get('Content-Length', '0')); data = self.rfile.read(size)
                name = self.path[8:] if self.path.startswith('/upload/') else ''
                if owner.uploads and name and '/' not in name and '\\' not in name:
                    owner.uploads.mkdir(parents=True, exist_ok=True); (owner.uploads / name).write_bytes(data); self.reply(200); return
                self.reply(404)

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True; allow_reuse_address = True

        self.http = Server(('127.0.0.1', http_port), Handler)
        self.tcp = socket.socket(); self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(('127.0.0.1', tcp_port)); self.tcp.listen(16)
        threading.Thread(target=self.http.serve_forever, daemon=True).start()
        threading.Thread(target=self.accept_loop, daemon=True).start()

    def accept_loop(self):
        while True:
            try: client, _ = self.tcp.accept()
            except OSError: return
            threading.Thread(target=self.serve_ping, args=(client,), daemon=True).start()

    def serve_ping(self, client):
        try:
            client.settimeout(30); data = b''
            while b'\n' not in data:
                chunk = client.recv(256)
                if not chunk: break
                data += chunk
            if data.startswith(b'PING '): client.sendall(b'PONG ' + data[5:]); self.pings += 1
        finally:
            client.close()

    def open_gate(self, name): self.gates.add(name)

    def close(self):
        self.http.shutdown(); self.http.server_close(); self.tcp.close()

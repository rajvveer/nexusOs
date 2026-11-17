#!/usr/bin/env python3
"""Tiny client for the NexusOS cloud-sync protocol (port 7070).
Verifies the sync SERVER from the host side over QEMU hostfwd.

Phase 54: the server now requires authentication against the OS user DB. The
client sends "LOGIN <user> <pass>" first and expects "OK auth"; pass creds with
--user/--pass (default root/root).

Usage:
  python sync_client.py [--user U] [--pass P] <host> <port> list
  python sync_client.py [--user U] [--pass P] <host> <port> get <name>
  python sync_client.py [--user U] [--pass P] <host> <port> put <name> <text>
"""
import socket, sys

def main():
    argv = sys.argv[1:]
    user, passwd = "root", "root"
    out = []
    i = 0
    while i < len(argv):
        if argv[i] == '--user':   user = argv[i+1]; i += 2
        elif argv[i] == '--pass': passwd = argv[i+1]; i += 2
        else: out.append(argv[i]); i += 1
    host, port, op = out[0], int(out[1]), out[2]
    sys.argv = [sys.argv[0]] + out      # so positional indexing below still works

    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(8)
    f = s.makefile('rwb')

    def line():
        return f.readline().decode(errors='replace').rstrip('\r\n')

    # --- authenticate ---
    f.write(("LOGIN %s %s\r\n" % (user, passwd)).encode()); f.flush()
    resp = line()
    if not resp.startswith('OK'):
        print("auth failed:", resp)
        s.close()
        sys.exit(1)

    if op == 'list':
        f.write(b"LIST\r\n"); f.flush()
        while True:
            l = line()
            if l == '' or l.startswith('END'):
                break
            print(l)
        f.write(b"BYE\r\n"); f.flush()
    elif op == 'get':
        name = sys.argv[4]
        f.write(("GET %s\r\n" % name).encode()); f.flush()
        l = line()
        print("hdr:", l)
        if l.startswith('FILE '):
            size = int(l.split()[2])
            body = f.read(size)
            print("body (%d bytes):" % size)
            print(body.decode(errors='replace'))
        f.write(b"BYE\r\n"); f.flush()
    elif op == 'put':
        name, text = sys.argv[4], sys.argv[5].encode()
        f.write(("PUT %s %d\r\n" % (name, len(text))).encode())
        f.write(text); f.flush()
        print("resp:", line())
        f.write(b"BYE\r\n"); f.flush()
    s.close()

if __name__ == "__main__":
    main()

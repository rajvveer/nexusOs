#!/usr/bin/env python3
"""Tiny client for the NexusOS Phase 45 cloud-sync protocol (port 7070).
Verifies the sync SERVER from the host side over QEMU hostfwd.

Usage:
  python sync_client.py <host> <port> list
  python sync_client.py <host> <port> get <name>
  python sync_client.py <host> <port> put <name> <text>
"""
import socket, sys

def main():
    host, port, op = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(8)
    f = s.makefile('rwb')

    def line():
        return f.readline().decode(errors='replace').rstrip('\r\n')

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

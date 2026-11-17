#!/usr/bin/env python3
"""Minimal RFB 3.3 client — connects to the NexusOS VNC server, performs the
handshake, requests one full FramebufferUpdate, decodes the Raw rectangle(s),
and saves a PNG. Pure stdlib + PIL. Verifies the Phase 45 VNC server.

Usage: python vnc_capture.py <host> <port> <out.png> [--key K] [--clip TEXT]
"""
import socket, struct, sys, time
from PIL import Image

def recvn(s, n):
    buf = b''
    while len(buf) < n:
        d = s.recv(n - len(buf))
        if not d:
            raise EOFError("peer closed (have %d/%d)" % (len(buf), n))
        buf += d
    return buf

def main():
    host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5900
    out  = sys.argv[3] if len(sys.argv) > 3 else 'vnc_out.png'
    send_key  = None
    send_clip = None
    a = 4
    while a < len(sys.argv):
        if sys.argv[a] == '--key':  send_key  = sys.argv[a+1]; a += 2
        elif sys.argv[a] == '--clip': send_clip = sys.argv[a+1]; a += 2
        else: a += 1

    s = socket.create_connection((host, port), timeout=8)
    s.settimeout(8)

    # 1. ProtocolVersion — server speaks first
    ver = recvn(s, 12)
    print("server version:", ver.decode(errors='replace').strip())
    s.sendall(b"RFB 003.003\n")

    # 2. Security (3.3: server sends a U32 security type)
    sec = struct.unpack(">I", recvn(s, 4))[0]
    print("security type:", sec)
    if sec == 0:
        rlen = struct.unpack(">I", recvn(s, 4))[0]
        reason = recvn(s, rlen)
        raise SystemExit("server refused: " + reason.decode(errors='replace'))
    # type 1 (None): no SecurityResult in 3.3

    # 3. ClientInit (shared flag = 1)
    s.sendall(b"\x01")

    # 4. ServerInit
    hdr = recvn(s, 24)
    w, h = struct.unpack(">HH", hdr[0:4])
    bpp, depth, big_endian, true_colour = struct.unpack(">BBBB", hdr[4:8])
    rmax, gmax, bmax = struct.unpack(">HHH", hdr[8:14])
    rsh, gsh, bsh = struct.unpack(">BBB", hdr[14:17])
    nlen = struct.unpack(">I", hdr[20:24])[0]
    name = recvn(s, nlen).decode(errors='replace')
    print("ServerInit: %dx%d bpp=%d depth=%d be=%d tc=%d name=%r"
          % (w, h, bpp, depth, big_endian, true_colour, name))
    print("  pixfmt rgbmax=(%d,%d,%d) shift=(%d,%d,%d)"
          % (rmax, gmax, bmax, rsh, gsh, bsh))

    # Optional: send a clipboard (ClientCutText, type 6) and/or a KeyEvent
    if send_clip is not None:
        t = send_clip.encode()
        s.sendall(struct.pack(">BBBBI", 6, 0, 0, 0, len(t)) + t)
        print("sent ClientCutText:", send_clip)
    if send_key is not None:
        ks = ord(send_key[0])
        # KeyEvent down then up
        s.sendall(struct.pack(">BBHI", 4, 1, 0, ks))
        s.sendall(struct.pack(">BBHI", 4, 0, 0, ks))
        print("sent KeyEvent:", send_key)

    # 5. SetEncodings: Raw only (type 0)
    s.sendall(struct.pack(">BBH", 2, 0, 1) + struct.pack(">i", 0))

    # 6. FramebufferUpdateRequest (full, non-incremental)
    s.sendall(struct.pack(">BBHHHH", 3, 0, 0, 0, w, h))

    # 7. Read FramebufferUpdate(s) until the whole screen is covered.
    img = Image.new("RGB", (w, h), (0, 0, 0))
    covered = 0
    deadline = time.time() + 12
    requested_again = False
    while covered < w * h and time.time() < deadline:
        msg_type = recvn(s, 1)[0]
        if msg_type == 0:  # FramebufferUpdate
            _pad = recvn(s, 1)
            (nrects,) = struct.unpack(">H", recvn(s, 2))
            for _ in range(nrects):
                rx, ry, rw, rh, enc = struct.unpack(">HHHHi", recvn(s, 12))
                if enc != 0:
                    raise SystemExit("unexpected encoding %d" % enc)
                data = recvn(s, rw * rh * 4)
                # wire bytes are little-endian 0x00RRGGBB => B,G,R,X in memory
                rect = Image.frombytes("RGBA", (rw, rh), data, "raw", "BGRA")
                img.paste(rect.convert("RGB"), (rx, ry))
                covered += rw * rh
                print("  rect %dx%d at (%d,%d) -> covered %d/%d"
                      % (rw, rh, rx, ry, covered, w * h))
            if covered < w * h and not requested_again:
                # band cap may split the screen; ask for the rest (incremental)
                s.sendall(struct.pack(">BBHHHH", 3, 1, 0, 0, w, h))
        elif msg_type == 3:  # ServerCutText
            _pad = recvn(s, 3)
            (clen,) = struct.unpack(">I", recvn(s, 4))
            ctext = recvn(s, clen)
            print("ServerCutText:", ctext.decode(errors='replace'))
        else:
            print("unknown server msg type", msg_type)
            break

    img.save(out)
    print("saved", out, "covered=%d/%d" % (covered, w * h))
    s.close()

if __name__ == "__main__":
    main()

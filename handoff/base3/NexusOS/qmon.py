import socket, sys, time

s = socket.create_connection(("127.0.0.1", 55555))
s.settimeout(2.0)

def cmd(c):
    s.sendall((c + "\n").encode())
    time.sleep(0.6)
    out = b""
    try:
        while True:
            d = s.recv(8192)
            if not d:
                break
            out += d
    except Exception:
        pass
    return out.decode(errors="replace")

cmd("")
for c in sys.argv[1:]:
    print("=== " + c)
    print(cmd(c))

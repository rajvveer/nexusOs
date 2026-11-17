import socket, time, sys
from PIL import Image

s = socket.create_connection(('127.0.0.1', 55555))
s.settimeout(1.5)

def drain():
    try:
        while True:
            d = s.recv(4096)
            if not d:
                break
    except Exception:
        pass

def cmd(c):
    s.sendall((c + '\n').encode())
    time.sleep(0.25)
    drain()

KEYMAP = {'.': 'dot', '/': 'slash', '-': 'minus', ' ': 'spc', '\\': 'backslash'}

def typ(text):
    for ch in text:
        if ch.isalpha():
            cmd('sendkey ' + ch.lower())
        elif ch.isdigit():
            cmd('sendkey ' + ch)
        elif ch in KEYMAP:
            cmd('sendkey ' + KEYMAP[ch])
        time.sleep(0.03)

def shot(name):
    ppm = name + '.ppm'
    cmd('screendump ' + ppm)
    time.sleep(0.4)
    Image.open(ppm).save(name + '.png')
    print('saved', name + '.png')

drain()
actions = sys.argv[1:]
for a in actions:
    if a.startswith('type:'):
        typ(a[5:])
    elif a == 'ret':
        cmd('sendkey ret')
    elif a.startswith('shot:'):
        shot(a[5:])
    elif a.startswith('sleep:'):
        time.sleep(float(a[6:]))
    elif a.startswith('key:'):
        cmd('sendkey ' + a[4:])
s.close()
print('done')

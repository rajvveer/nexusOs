import socket, time, os, sys
from PIL import Image
HERE=os.path.dirname(os.path.abspath(__file__))
OUT=os.path.join(HERE,"test_out"); os.makedirs(OUT,exist_ok=True)
PORT=int(sys.argv[1]) if len(sys.argv)>1 else 55800

s=socket.create_connection(('127.0.0.1',PORT)); s.settimeout(2.0)
def drain():
    try:
        while True:
            d=s.recv(4096)
            if not d: break
    except: pass
def raw(c):
    s.sendall((c+'\n').encode()); time.sleep(0.25); drain()
def shot(name):
    p=os.path.join(OUT,name+'.ppm')
    raw('screendump '+p); time.sleep(0.6)
    if os.path.exists(p):
        Image.open(p).save(os.path.join(OUT,name+'.png')); os.remove(p)
        print('OK shot',name,os.path.getsize(os.path.join(OUT,name+'.png')),'bytes')
    else:
        print('FAIL no ppm for',name)

drain()
print('waiting for boot...'); time.sleep(16)
shot('smoke_login')                 # should show login screen
# login
for ch in 'root': raw('sendkey '+ch)
raw('sendkey ret')
for ch in 'root': raw('sendkey '+ch)
raw('sendkey ret')
time.sleep(4)
shot('smoke_desktop')               # desktop
raw('sendkey esc'); time.sleep(1.5)
shot('smoke_shell')                 # text shell
for ch in 'help': raw('sendkey '+ch)
raw('sendkey ret'); time.sleep(1)
shot('smoke_help')
raw('quit'); s.close()
print('smoke done')

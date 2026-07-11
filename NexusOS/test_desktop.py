#!/usr/bin/env python3
"""Dedicated desktop-render diagnostic. Boot fresh, login, stay on the DESKTOP,
capture at several settle times to distinguish a transient/timing artifact from a
real rendering bug. Also nudges the mouse and types in the terminal window."""
import socket, time, sys, os, subprocess
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from test_phases import Mon, OUT, QEMU
PORT=int(sys.argv[1]) if len(sys.argv)>1 else 55730
SERIAL=os.path.join(HERE,"serial_desk.log")

def launch():
    try: os.remove(SERIAL)
    except OSError: pass
    return subprocess.Popen([QEMU,"-m","2048",
        "-drive",f"file={os.path.join(HERE,'nexus.img')},format=raw,if=floppy",
        "-drive",f"file={os.path.join(HERE,'disk.img')},format=raw,if=ide","-snapshot",
        "-boot","a","-vga","none","-device","virtio-vga","-serial",f"file:{SERIAL}",
        "-monitor",f"tcp:127.0.0.1:{PORT},server,nowait",
        "-netdev","user,id=net0","-device","rtl8139,netdev=net0","-display","none"])

def main():
    proc=launch(); m=None
    for _ in range(40):
        time.sleep(1)
        try: m=Mon(PORT); break
        except Exception: pass
    if not m: print("[FAIL] no monitor"); proc.terminate(); return
    time.sleep(16); m.line('root'); m.line('root')
    # explicit cumulative settle captures
    time.sleep(2);  m.shot("desk_t2")
    time.sleep(2);  m.shot("desk_t4")
    time.sleep(3);  m.shot("desk_t7")
    time.sleep(4);  m.shot("desk_t11")
    # nudge mouse to force a redraw
    m.raw('mouse_move 200 200'); time.sleep(0.5)
    m.raw('mouse_move -100 100'); time.sleep(1)
    m.shot("desk_after_mouse")
    # type 'help' (terminal window is focused on the desktop)
    for ch in 'help': m.raw('sendkey '+ch)
    m.raw('sendkey ret'); time.sleep(1.5)
    m.shot("desk_terminal_help")
    print("done; serial:", open(SERIAL,'rb').read().decode('latin1','replace')[:80] if os.path.exists(SERIAL) else "(none)")
    m.quit(); time.sleep(1.5)
    try: proc.wait(timeout=8)
    except Exception: proc.terminate()

if __name__=="__main__": main()

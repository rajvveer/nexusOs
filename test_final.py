#!/usr/bin/env python3
"""Definitive test: interactive games + desktop GUI, with ROBUST key delivery.

Keystroke-drop fix: before each command, send several backspaces to clear any
stray partial line, type the command char-by-char with verification-friendly
delays, then Enter. Screenshot is taken BEFORE any exit key. Exits a fullscreen
app with a single key then waits well past it. -snapshot so no disk writes."""
import socket, time, sys, os, subprocess
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from test_phases import Mon, OUT, QEMU
PORT=int(sys.argv[1]) if len(sys.argv)>1 else 55740
SERIAL=os.path.join(HERE,"serial_final.log")

def launch():
    try: os.remove(SERIAL)
    except OSError: pass
    return subprocess.Popen([QEMU,"-m","2048",
        "-drive",f"file={os.path.join(HERE,'nexus.img')},format=raw,if=floppy",
        "-drive",f"file={os.path.join(HERE,'disk.img')},format=raw,if=ide","-snapshot",
        "-boot","a","-vga","none","-device","virtio-vga","-serial",f"file:{SERIAL}",
        "-monitor",f"tcp:127.0.0.1:{PORT},server,nowait",
        "-netdev","user,id=net0","-device","rtl8139,netdev=net0","-display","none"])

def faults():
    if not os.path.exists(SERIAL): return "(none)"
    d=open(SERIAL,'rb').read().decode('latin1','replace')
    if "!EX:" in d:
        i=d.find("!EX:"); return "CRASH "+d[i:i+48].replace('\n',' ')
    return "no faults"

class R(Mon):
    def clear_line(self):
        # clear any stray partial input
        for _ in range(12): self.raw('sendkey backspace')
        time.sleep(0.2)
    def cmd_line(self, text):
        """robust: clear, type slowly, enter"""
        self.clear_line()
        for ch in text:
            if ch.isalpha(): self.raw('sendkey '+ch.lower())
            elif ch.isdigit(): self.raw('sendkey '+ch)
            elif ch==' ': self.raw('sendkey spc')
            time.sleep(0.06)
        time.sleep(0.2)
        self.raw('sendkey ret')

def main():
    proc=launch(); m=None
    for _ in range(40):
        time.sleep(1)
        try: m=R(PORT); break
        except Exception: pass
    if not m: print("[FAIL] no monitor"); proc.terminate(); return
    time.sleep(16); m.line('root'); m.line('root'); time.sleep(4)

    log=[]
    # ===== DESKTOP (Phases 4-18): stay on desktop, settle long, capture =====
    m.shot("fin_desktop_t5")             # right after login (no esc)
    time.sleep(5); m.shot("fin_desktop_t10")
    time.sleep(5); m.shot("fin_desktop_t15")
    log.append("desktop captured @5/10/15s: "+faults())
    # drop to text shell for games
    m.key('esc'); time.sleep(2)
    m.cmd_line('clear'); time.sleep(1); m.shot("fin_shell")

    # ===== GAMES with robust delivery, screenshot BEFORE exit =====
    # snake
    m.cmd_line('snake'); time.sleep(3); m.shot("fin_snake")
    m.key('q'); time.sleep(2)
    log.append("snake: "+faults())
    # breakout
    m.cmd_line('breakout'); time.sleep(3); m.shot("fin_breakout_title")
    m.key('spc'); time.sleep(2); m.shot("fin_breakout_play")
    m.key('esc'); time.sleep(2)
    log.append("breakout: "+faults())
    # doom
    m.cmd_line('doom'); time.sleep(3); m.shot("fin_doom_title")
    m.key('ret'); time.sleep(2.5); m.shot("fin_doom_play")
    m.key('esc'); time.sleep(2)
    log.append("doom: "+faults())
    # gamepad tester (Phase 42)
    m.cmd_line('gamepad'); time.sleep(2.5); m.shot("fin_gamepad")
    m.key('esc'); time.sleep(2)
    log.append("gamepad: "+faults())

    m.shot("fin_zz_end")
    log.append("FINAL: "+faults())
    open(os.path.join(OUT,"FINAL_SUMMARY.txt"),'w').write("\n".join(log))
    print("\n".join(log))
    m.quit(); time.sleep(1.5)
    try: proc.wait(timeout=8)
    except Exception: proc.terminate()

if __name__=="__main__": main()

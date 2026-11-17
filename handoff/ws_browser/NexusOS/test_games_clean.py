#!/usr/bin/env python3
"""Clean isolated test of the real fullscreen games (doom/breakout/snake/sprites/
gpubench/gamepad) — boot fresh, drop to shell, run ONE game, screenshot, exit,
screenshot, then next. No desktop thrashing. Confirms Phase 41/42 in isolation."""
import socket, time, sys, os, subprocess
HERE=os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0,HERE)
from test_phases import Mon, OUT, QEMU
PORT=int(sys.argv[1]) if len(sys.argv)>1 else 55720
SERIAL=os.path.join(HERE,"serial_games.log")

def launch():
    try: os.remove(SERIAL)
    except OSError: pass
    return subprocess.Popen([QEMU,"-m","2048",
        "-drive",f"file={os.path.join(HERE,'nexus.img')},format=raw,if=floppy",
        "-drive",f"file={os.path.join(HERE,'disk.img')},format=raw,if=ide",
        "-snapshot",
        "-boot","a","-vga","none","-device","virtio-vga","-serial",f"file:{SERIAL}",
        "-monitor",f"tcp:127.0.0.1:{PORT},server,nowait",
        "-netdev","user,id=net0","-device","rtl8139,netdev=net0",
        "-audiodev","none,id=snd0","-device","AC97,audiodev=snd0","-display","none"])

def faults():
    if not os.path.exists(SERIAL): return "(none)"
    d=open(SERIAL,'rb').read().decode('latin1','replace')
    if "!EX:" in d:
        i=d.find("!EX:"); return "CRASH "+d[i:i+48].replace('\n',' ')
    return "no faults"

def main():
    proc=launch(); m=None
    for _ in range(40):
        time.sleep(1)
        try: m=Mon(PORT); break
        except Exception: pass
    if not m: print("[FAIL] no monitor"); proc.terminate(); return
    time.sleep(16); m.line('root'); m.line('root'); time.sleep(4)
    m.key('esc'); time.sleep(1.5)
    m.shot("clean_00_shell")
    log=[]

    # gpubench (Phase 41) — text output, returns to shell
    m.line('gpubench'); time.sleep(4); m.shot("clean_gpubench")
    log.append("gpubench: "+faults())

    # sprites (Phase 41) — fullscreen demo
    m.line('sprites'); time.sleep(4); m.shot("clean_sprites")
    time.sleep(2)  # sprites runs for a few seconds then returns
    log.append("sprites: "+faults())

    # snake (Phase 3) — fullscreen, q to quit
    m.line('snake'); time.sleep(2.5); m.shot("clean_snake")
    m.key('q'); time.sleep(1.5)
    log.append("snake: "+faults())

    # breakout (Phase 42) — fullscreen
    m.line('breakout'); time.sleep(2.5); m.shot("clean_breakout_title")
    m.key('spc'); time.sleep(1.5); m.shot("clean_breakout_play")
    m.key('esc'); time.sleep(1); m.key('q'); time.sleep(1.5)
    log.append("breakout: "+faults())

    # doom (Phase 42) — fullscreen raycaster
    m.line('doom'); time.sleep(3); m.shot("clean_doom_title")
    m.key('ret'); time.sleep(2); m.shot("clean_doom_play1")
    m.key('w 700'); time.sleep(1.5); m.shot("clean_doom_play2")
    m.key('esc'); time.sleep(1.5); m.key('esc'); time.sleep(1.5)
    log.append("doom: "+faults())

    m.shot("clean_zz_end")
    log.append("FINAL: "+faults())
    open(os.path.join(OUT,"GAMES_SUMMARY.txt"),'w').write("\n".join(log))
    print("\n".join(log))
    m.quit(); time.sleep(1.5)
    try: proc.wait(timeout=8)
    except Exception: proc.terminate()

if __name__=="__main__": main()

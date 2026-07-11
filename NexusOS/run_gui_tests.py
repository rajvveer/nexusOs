#!/usr/bin/env python3
"""
run_gui_tests.py — boot NexusOS once, exercise the GRAPHICAL desktop, GUI apps,
and games (Phases 4-18 desktop/apps + Phase 42 games), screenshotting each.

Many of these are launched from the text shell by name (they open a fullscreen
or windowed UI), driven a few keys, screenshotted, then exited (Esc/q). Games
get a couple of held-key inputs to show motion. Quits cleanly; reports faults.

Usage:  python run_gui_tests.py [monitor_port]
"""
import socket, time, sys, os, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_phases import Mon, login_and_shell, OUT, QEMU

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 55700
SERIAL = os.path.join(HERE, "serial_gui.log")

def launch():
    try: os.remove(SERIAL)
    except OSError: pass
    args = [QEMU, "-m", "2048",
        "-drive", f"file={os.path.join(HERE,'nexus.img')},format=raw,if=floppy",
        "-drive", f"file={os.path.join(HERE,'disk.img')},format=raw,if=ide",
        "-boot", "a", "-vga", "none", "-device", "virtio-vga",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"tcp:127.0.0.1:{PORT},server,nowait",
        "-netdev", "user,id=net0", "-device", "rtl8139,netdev=net0",
        "-audiodev", "none,id=snd0", "-device", "AC97,audiodev=snd0",
        "-display", "none"]
    return subprocess.Popen(args)

def faults():
    if not os.path.exists(SERIAL): return "(none)"
    d = open(SERIAL,'rb').read().decode('latin1','replace')
    if "!EX:" in d:
        i=d.find("!EX:"); return "CRASH "+d[i:i+48].replace('\n',' ')
    return "no faults"

def main():
    proc = launch()
    m=None
    for _ in range(40):
        time.sleep(1)
        try: m=Mon(PORT); break
        except Exception: pass
    if not m:
        print("[FAIL] no monitor"); proc.terminate(); return
    print("[ok] connected, boot+login")
    # First screenshot the DESKTOP (before dropping to shell)
    time.sleep(16)
    m.line('root'); m.line('root')
    time.sleep(4)
    m.shot("gui_00_desktop")          # Phases 4-18: desktop, taskbar, icons, wallpaper
    m.key('esc'); time.sleep(1.5)     # -> text shell to launch apps

    log=[]
    def app(cmd, label, settle=2.0, exit_keys=('esc',), pre_keys=()):
        m.line(cmd); time.sleep(settle)
        for k in pre_keys:
            m.key(k); time.sleep(0.4)
        m.shot(label)
        for k in exit_keys:
            m.key(k); time.sleep(0.6)
        time.sleep(0.8)
        log.append(f"{cmd} -> {label}")

    # --- GUI apps (windowed; launched, screenshot, q/esc to close) ---
    app("calc", "gui_calc", 2.0, exit_keys=('esc','esc'))
    app("notepad", "gui_notepad", 2.0, exit_keys=('esc','esc'))
    app("files", "gui_filemgr", 2.0, exit_keys=('esc','esc'))
    app("paint", "gui_paint", 2.0, exit_keys=('esc','esc'))
    app("calendar", "gui_calendar", 2.0, exit_keys=('esc','esc'))
    app("settings", "gui_settings", 2.0, exit_keys=('esc','esc'))
    app("taskmgr", "gui_taskmgr", 2.0, exit_keys=('esc','esc'))
    app("sysmon", "gui_sysmon", 2.0, exit_keys=('esc','esc'))
    app("contacts", "gui_contacts", 2.0, exit_keys=('esc','esc'))
    app("colors", "gui_colors", 2.0, exit_keys=('esc',))
    app("clock", "gui_clock", 2.0, exit_keys=('esc','esc'))

    # --- Games ---
    # snake: move a couple of directions
    m.line('snake'); time.sleep(2)
    m.key('d 600'); time.sleep(1); m.key('s 600'); time.sleep(1)
    m.shot("game_snake"); m.key('esc'); time.sleep(0.8); m.key('q'); time.sleep(1)
    log.append("snake")

    # tetris
    m.line('tetris'); time.sleep(2)
    m.key('left'); time.sleep(0.4); m.key('down'); time.sleep(0.4)
    m.shot("game_tetris"); m.key('esc'); time.sleep(0.8); m.key('q'); time.sleep(1)
    log.append("tetris")

    # pong
    m.line('pong'); time.sleep(2)
    m.shot("game_pong"); m.key('esc'); time.sleep(0.8); m.key('q'); time.sleep(1)
    log.append("pong")

    # minesweeper
    m.line('minesweeper'); time.sleep(2)
    m.shot("game_minesweeper"); m.key('esc'); time.sleep(0.8); m.key('q'); time.sleep(1)
    log.append("minesweeper")

    # breakout
    m.line('breakout'); time.sleep(2)
    m.key('spc'); time.sleep(0.5); m.key('d 600'); time.sleep(0.8)
    m.shot("game_breakout"); m.key('esc'); time.sleep(0.8); m.key('q'); time.sleep(1)
    log.append("breakout")

    # doom: title, start, look around, shoot
    m.line('doom'); time.sleep(2.5)
    m.shot("game_doom_title")
    m.key('ret'); time.sleep(1.5)        # start
    m.key('w 800'); time.sleep(1.2)
    m.key('ctrl'); time.sleep(0.5)       # shoot
    m.shot("game_doom_play")
    m.key('esc'); time.sleep(1); m.key('esc'); time.sleep(1)
    log.append("doom")

    # sprites demo (Phase 41)
    m.line('sprites'); time.sleep(3)
    m.shot("gui_sprites"); time.sleep(1)
    log.append("sprites")

    m.shot("gui_zz_end")
    f = faults()
    log.append("serial: " + f)
    open(os.path.join(OUT,"GUI_SUMMARY.txt"),'w').write("\n".join(log))
    print("\n".join(log))
    m.quit(); time.sleep(1.5)
    try: proc.wait(timeout=8)
    except Exception: proc.terminate()
    print("FINAL:", faults())

if __name__=="__main__":
    main()

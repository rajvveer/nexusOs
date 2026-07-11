#!/usr/bin/env python3
"""
run_all_tests.py — boot NexusOS ONCE, run every era batch, screenshot, report.

Launches its own private QEMU on a non-default monitor port (so it won't collide
with a user window), waits for boot, logs in (root/root), drops to the text shell,
then runs all BATCHES from test_phases.py back-to-back, screenshotting at
checkpoints. Quits cleanly so serial.log finalizes, then prints a crash report.

Usage:  python run_all_tests.py [monitor_port]
Output: test_out/*.png  +  test_out/<batch>_report.txt  +  test_out/SUMMARY.txt
"""
import socket, time, sys, os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_phases import Mon, BATCHES, login_and_shell, check_serial, OUT, QEMU

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 55600
SERIAL = os.path.join(HERE, f"serial_test.log")

def launch():
    # remove stale serial log so check_serial only sees this run
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
    print("[launch]", " ".join(args[:8]), "...")
    return subprocess.Popen(args)

def serial_faults():
    if not os.path.exists(SERIAL): return "(no serial_test.log)"
    data = open(SERIAL,'rb').read().decode('latin1','replace')
    if "!EX:" in data:
        i = data.find("!EX:")
        return "CRASH " + data[i:i+48].replace('\n',' ')
    return "no faults"

def main():
    proc = launch()
    # wait for monitor socket
    m = None
    for _ in range(40):
        time.sleep(1)
        try:
            m = Mon(PORT); break
        except Exception:
            continue
    if not m:
        print("[FAIL] could not connect to QEMU monitor"); proc.terminate(); return
    print("[ok] monitor connected, waiting for boot+login")
    login_and_shell(m, boot_wait=16)
    m.shot("00_shell_ready")

    summary = []
    for batch in BATCHES:
        print(f"\n=== running batch: {batch} ===")
        cmds = BATCHES[batch]
        for (c, label, slp) in cmds:
            m.line(c); time.sleep(slp)
            if label: m.shot(f"{batch}_{label}")
        m.shot(f"{batch}_final")
        fault = serial_faults()
        line = f"{batch}: {len(cmds)} cmds ran | serial: {fault}"
        print(line); summary.append(line)
        if "CRASH" in fault:
            print("  [!] crash detected during", batch)

    m.shot("zz_end")
    m.quit()
    time.sleep(1.5)
    try: proc.wait(timeout=8)
    except Exception:
        proc.terminate()
    final = serial_faults()
    summary.append("FINAL serial check: " + final)
    open(os.path.join(OUT, "SUMMARY.txt"), 'w').write("\n".join(summary))
    print("\n========== SUMMARY ==========")
    print("\n".join(summary))

if __name__ == "__main__":
    main()

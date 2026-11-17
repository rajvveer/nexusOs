import subprocess, time, os, shutil, sys
proj, work, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
Q = r"C:\Program Files\qemu\qemu-system-i386.exe"
img = os.path.join(work, "loop_nexus.img"); shutil.copyfile(os.path.join(proj, "nexus.img"), img)
crashes = 0
for i in range(n):
    ser = os.path.join(work, f"loop_{i}.log")
    p = subprocess.Popen([Q, "-m", "2048", "-drive", f"file={img},format=raw,if=floppy,readonly=on", "-boot", "a",
                          "-vga", "none", "-device", "virtio-vga", "-serial", f"file:{ser}", "-display", "none",
                          "-netdev", "user,id=n0", "-device", "rtl8139,netdev=n0"])
    time.sleep(14); p.kill(); p.wait()
    d = open(ser, 'rb').read().decode('latin1', 'replace')
    j = d.find("!EX:")
    r = d[j:j+30].replace('\n', ' ') if j >= 0 else "ok"
    crashes += j >= 0
    print(f"boot {i}: {r}", flush=True)
print(f"crashes {crashes}/{n}")

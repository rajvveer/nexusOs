"""Link the kernel as an ELF (same object list as build.bat) and print section sizes
and the biggest .bss symbols. usage: python -I sizes.py <NexusOS dir>"""
import re, subprocess, sys, os
d = sys.argv[1]
txt = open(os.path.join(d, "build.bat")).read()
objs = list(dict.fromkeys(m.replace("\\", "/") for m in re.findall(r"kernel\\[a-z_0-9]+\.o", txt)))
print(len(objs), "objects")
tools = os.path.join(d, "tools", "i686-elf", "bin")
elf = os.path.join(d, "k57.elf")
r = subprocess.run([os.path.join(tools, "i686-elf-ld.exe"), "-T", os.path.join(d, "linker.ld"), "-nostdlib", "-o", elf] + [os.path.join(d, o) for o in objs],
                   capture_output=True, text=True)
print(r.stderr[:500])
print(subprocess.run([os.path.join(tools, "i686-elf-size.exe"), "-A", elf], capture_output=True, text=True).stdout)
nm = subprocess.run([os.path.join(tools, "i686-elf-nm.exe"), "--size-sort", "-S", elf], capture_output=True, text=True).stdout.splitlines()
print("biggest .bss symbols:")
print("\n".join(l for l in nm if " b " in l or " B " in l)[-900:])
os.remove(elf)

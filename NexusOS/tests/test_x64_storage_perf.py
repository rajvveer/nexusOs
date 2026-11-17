"""Compare durable streaming writes on identical FAT geometry, then reboot/read back."""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-storage')
WORK.mkdir(parents=True,exist_ok=True)
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
fatput=module('storage_fatput',ROOT/'userland/fatput.py');storage=module('storage_grow',ROOT/'storage.py')
app=WORK/'storage.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/storage_user.c'),'-o',str(app)],check=True)
images=[('current',OUT/'nexus64.img')]
if len(sys.argv)>2:images.insert(0,('baseline',Path(sys.argv[2])))
template=WORK/'template.img'
owned_disks=[template,*[WORK/(name+'.img') for name,_ in images]]
for disk in owned_disks:
    if disk.exists(): raise FileExistsError('Refusing to overwrite an existing test disk: '+str(disk))
try:
    shutil.copyfile(OUT/'apps64.img',template);storage.grow(template,2048)
    fatput.main(['fatput',str(template),'put',str(app),'sbin/init'])
    results={}
    for name,boot in images:
        disk=WORK/(name+'.img');shutil.copyfile(template,disk)
        for attempt in range(2):
            serial=WORK/f'{name}-{attempt}.log';serial.unlink(missing_ok=True);start=time.monotonic()
            process=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','128',
                '-drive',f'file={boot},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
                '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}',
                '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=240)
            log=serial.read_text();assert process.returncode==33 and '[64] INIT SESSION RETURNED' in log,(process.stderr,log)
            assert '[64] STORAGE DATA VERIFIED' in log and 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log,log
            if not attempt:
                milliseconds=int(re.search(r'STORAGE WRITE bytes=16777216 ms=(\d+)',log).group(1))
                results[name]={'guest_write_ms':milliseconds,'host_boot_and_check_seconds':time.monotonic()-start,'boot_sha256':hashlib.sha256(boot.read_bytes()).hexdigest()}
                print(name,results[name],flush=True)
            else:assert 'STORAGE WRITE' not in log,'reboot overwrote the persisted file'
        fs=fatput.Fat32(disk)
        try:
            entry=fs.find_name(fs.lookup_dir(['home','nexus']),'storage-bench.bin')[2]
            first=int.from_bytes(entry[20:22],'little')<<16|int.from_bytes(entry[26:28],'little');size=int.from_bytes(entry[28:32],'little')
            block=bytes((i*37+11)&255 for i in range(65536));expected=hashlib.sha256(block*256).digest()
            assert size==16*1024*1024 and hashlib.sha256(fs.read_chain(first,size)).digest()==expected
        finally:fs.close()
        disk.unlink()
    if 'baseline' in results:results['speedup']=results['baseline']['guest_write_ms']/max(1,results['current']['guest_write_ms'])
    (WORK/'results.json').write_text(json.dumps(results,indent=2)+'\n');print('PASS durable 16 MiB streaming writes, reboot and independent disk hash',results,flush=True)
finally:
    for disk in owned_disks: disk.unlink(missing_ok=True)

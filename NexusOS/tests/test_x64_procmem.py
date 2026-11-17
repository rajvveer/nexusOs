"""/proc/PID/{mem,maps,environ}: access is checked for the opener at open and the file is bound to the image of that
moment (arch/x86_64/procmem_user.c booted as /sbin/init, on one CPU and on two).
Usage: test_x64_procmem.py [work dir] [out dir with nexus64.img/apps64.img, default arch/x86_64/out]"""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]
OUT=Path(sys.argv[2]) if len(sys.argv)>2 else ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-procmem')
WORK.mkdir(parents=True,exist_ok=True);disk=WORK/'disk.img'
if disk.exists():raise FileExistsError(disk)
app=WORK/'procmem.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/procmem_user.c'),'-o',str(app)],check=True)
spec=importlib.util.spec_from_file_location('procmem_fatput',ROOT/'userland/fatput.py');fatput=importlib.util.module_from_spec(spec);spec.loader.exec_module(fatput)
try:
    for tag,extra in (('up',[]),('smp2',['-smp','2'])):
        shutil.copyfile(OUT/'apps64.img',disk);fatput.main(['fatput',str(disk),'put',str(app),'sbin/init'])
        serial=WORK/(tag+'.log');serial.unlink(missing_ok=True)
        result=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','256',*extra,
            '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
            '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-no-reboot',
            '-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=120)
        log=serial.read_text(errors='replace')
        assert result.returncode==33 and 'PROCMEM ALL PASSED' in log and 'PROCMEM FAIL' not in log,(result.returncode,result.stderr,log[-4000:])
        assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and 'uncontained' not in log,log[-4000:]
        disk.unlink()
        print('PASS /proc/PID/mem, maps, environ bound at open:',tag,flush=True)
finally:disk.unlink(missing_ok=True)

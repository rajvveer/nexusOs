"""CPU metadata must reflect each actual virtual CPU model, not a canned profile."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-cpuinfo')
WORK.mkdir(exist_ok=True);disk=WORK/'disk.img'
if disk.exists():raise FileExistsError(disk)
app=WORK/'cpuinfo.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/cpuinfo_user.c'),'-o',str(app)],check=True)
spec=importlib.util.spec_from_file_location('cpuinfo_fatput',ROOT/'userland/fatput.py');fatput=importlib.util.module_from_spec(spec);spec.loader.exec_module(fatput)
try:
    shutil.copyfile(OUT/'apps64.img',disk);fatput.main(['fatput',str(disk),'put',str(app),'sbin/init'])
    for cpu in ('max','qemu64,+rdrand','max,-aes'):
        serial=WORK/(cpu.replace(',','_')+'.log');serial.unlink(missing_ok=True)
        result=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu',cpu,'-m','128',
            '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
            '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-no-reboot',
            '-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=30)
        log=serial.read_text();assert result.returncode==33 and 'CPUINFO ALL PASSED' in log and 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log,(result.stderr,log)
        print('PASS native CPU metadata:',cpu,flush=True)
finally:disk.unlink(missing_ok=True)

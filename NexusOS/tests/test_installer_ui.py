"""Native package window: cancel, close during install, reboot/run and orphan lifetime."""
from pathlib import Path
import importlib.util
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-installer-window')
WORK.mkdir(exist_ok=True);disk=WORK/'disk.img'
if disk.exists():raise FileExistsError(disk)
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
installer=module('window_installer',ROOT/'arch/x86_64/installer.py');installer.build();installer.build_ui()
parent=WORK/'parent.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/installer_parent_user.c'),'-o',str(parent)],check=True)
fatput=module('window_fatput',ROOT/'userland/fatput.py');qmp=module('window_qmp',ROOT/'tests/qmp_local.py')
package=json.loads((ROOT/'arch/x86_64/hello-download-lock.json').read_text());destination=['home','nexus','Applications',package['sha256']]
def read(fs,parts):
    found=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1]);assert found,parts;e=found[2]
    first=int.from_bytes(e[20:22],'little')<<16|int.from_bytes(e[26:28],'little');size=int.from_bytes(e[28:32],'little')
    return fs.read_chain(first,size)
try:
    shutil.copyfile(nexus_paths.source_disk(),disk)
    fs=fatput.Fat32(disk)
    try:
        assert hashlib.sha256(read(fs,['home','nexus','Downloads',package['filename']])).hexdigest()==package['sha256']
        assert fs.find_name(fs.lookup_dir(destination[:-1]),destination[-1]) is None
    finally:fs.close()
    args=WORK/'desktop.args';args.write_text('/bin/xdg-open\n/home/nexus/Downloads/'+package['filename']+'\n')
    fatput.main(['fatput',str(disk),'put',str(OUT/'desktop.elf'),'sbin/init','put',str(args),'nexus/desktop.args',
        'put',str(OUT/'nexus-install.elf'),'bin/nexus-install','put',str(OUT/'nexus-installer-ui.elf'),'bin/xdg-open','put',str(parent),'bin/installer-parent-test'])
    for mode in ('cancel','background','reopen','orphan'):
        if mode=='orphan':
            args.write_text('/bin/installer-parent-test\nfile:///home/nexus/Downloads/'+package['filename']+'\n')
            fatput.main(['fatput',str(disk),'put',str(args),'nexus/desktop.args'])
        work=WORK/mode;work.mkdir(exist_ok=True);serial=work/'serial.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(work)
        process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048','-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on',
            '-drive',f'file={disk},format=raw,if=ide','-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,'-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        print('INSTALLER UI VM',mode,process.pid,flush=True)
        def wait(pattern,after=0):
            deadline=time.monotonic()+120
            while time.monotonic()<deadline:
                if serial.exists():
                    match=re.search(pattern,serial.read_text()[after:])
                    if match:return match
                if process.poll() is not None:raise AssertionError(serial.read_text()[-5000:])
                time.sleep(.05)
            raise TimeoutError(pattern)
        try:
            state=3 if mode in ('reopen','orphan') else 1
            layout=wait(r'INSTALLER UI phase='+str(state)+r' primary=(\d+),(\d+) close=(\d+),(\d+)');wait(r'DESKTOP BUFFER surface=\d+ 1920x1080')
            with control.open() as monitor:
                command=monitor.command
                def key(name,down):command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.1)
                def shot(name):
                    time.sleep(.3);command('screendump',{'filename':str(work/'frame.ppm')})
                    with Image.open(work/'frame.ppm') as image:image.save(work/(name+'.png'))
                shot('ready')
                if mode=='background':
                    x,y=960,540;tx,ty=map(int,layout.groups()[:2])
                    while (x,y)!=(tx,ty):
                        dx=max(-90,min(90,tx-x));dy=max(-90,min(90,ty-y));events=[]
                        if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                        if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                        x+=dx;y+=dy;command('input-send-event',{'events':events});time.sleep(.12)
                    for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.1)
                    wait('INSTALLER UI phase=2');assert 'INSTALLER JOB DONE 1 ' not in serial.read_text();shot('installing-before-close')
                if mode=='reopen':
                    before=len(serial.read_text());key('ret',True);key('ret',False)
                    wait(r'INSTALLER JOB DONE 2 status=0 output_bytes=14',before);wait('INSTALLER UI phase=5',before);shot('hello-running')
                command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':'esc'}}} for down in (True,False)]})
                if mode=='background':
                    deadline=time.monotonic()+5
                    while True:
                        shot('closed-while-working')
                        with Image.open(work/'closed-while-working.png') as picture:gone=picture.getpixel((600,200))==(16,21,34)
                        if gone:break
                        assert time.monotonic()<deadline,'Installer window did not close'
                    assert 'INSTALLER JOB DONE 1 ' not in serial.read_text(),'Installer finished before the close-during-work check'
            output=process.communicate(timeout=60)[0];log=serial.read_text()
            assert process.returncode==33 and 'INSTALLER UI CLOSED' in log and 'DESKTOP EXIT status=0' in log,(output,log[-5000:])
            assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log
            if mode=='background':
                assert 'INSTALLER JOB DONE 1 status=0' in log and 'INSTALLER JOB 2 ' not in log
            elif mode=='reopen':assert 'INSTALLER JOB 1 ' not in log and 'INSTALLER JOB DONE 2 status=0 output_bytes=14' in log
            else:assert 'INSTALLER JOB 1 ' not in log and 'INSTALLER JOB 2 ' not in log
            if mode=='orphan':assert log.index('INSTALLER LAUNCHER EXIT')<log.index('NEXUS OPEN PACKAGE')<log.index('DESKTOP EXIT status=0')
        finally:
            if process.poll() is None:process.kill();process.wait()
            control.cleanup(process)
        fs=fatput.Fat32(disk)
        try:
            entry=fs.find_name(fs.lookup_dir(destination[:-1]),destination[-1])
            if mode=='cancel':assert entry is None
            else:
                assert entry and read(fs,destination+['.nexus-package-sha256'])==package['sha256'].encode()+b'\n'
                assert hashlib.sha256(read(fs,destination+['usr','bin','hello'])).hexdigest()=='bfda3fef063414535d116a04689caf300568e0691318bcf255f932930508311b'
        finally:fs.close()
        print('PASS installer window',mode,flush=True)
    print('PASS native installer GUI: cancel, real install click then close while busy, completed background installation, reboot/descriptor launch/output and orphan lifetime',flush=True)
finally:disk.unlink(missing_ok=True)

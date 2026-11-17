"""Real Chrome HTTPS download -> file click -> native Install/Run, without DevTools."""
from pathlib import Path
import hashlib
import html
import importlib.util
import json
import re
import shutil
import subprocess
import sys
import time
from urllib.parse import quote
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
APP=sys.argv[2] if len(sys.argv)>2 else 'hello'
if APP not in ('hello','chrome'):raise ValueError('Choose hello or chrome')
RESUME=len(sys.argv)>3 and sys.argv[3]=='--resume-download'
if len(sys.argv)>3 and not RESUME:raise ValueError('Unknown test argument')
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-chrome-installer-flow')
WORK.mkdir(parents=True,exist_ok=True);disk=WORK/'disk.img'
if RESUME:
    assert not nexus_paths.is_user_disk(disk) and disk.is_file()
    previous=json.loads((WORK/'resume-download.json').read_text());assert Path(previous['disk'])==disk
    commands=subprocess.check_output(['powershell','-NoProfile','-Command',"Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu%'\" | Select-Object -ExpandProperty CommandLine"],text=True)
    assert str(disk).lower() not in commands.lower(),'Test disk is still attached to QEMU'
elif disk.exists():raise FileExistsError(disk)
if (WORK/'result.json').exists():(WORK/'result.json').rename(WORK/('previous-result-'+str(time.time_ns())+'.json'))
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
fatput=module('chrome_installer_fatput',ROOT/'userland/fatput.py');qmp=module('chrome_installer_qmp',ROOT/'tests/qmp_local.py')
builder=module('chrome_installer_build',ROOT/'arch/x86_64/installer.py');builder.build_ui()
package=json.loads((ROOT/'arch/x86_64'/('download-lock.json' if APP=='chrome' else 'hello-download-lock.json')).read_text())
if RESUME:assert previous['package']==package
installed=json.loads((OUT/'desktop64.json').read_text())
destination=['home','nexus','Applications',package['sha256']]
profile=previous.get('profile','/home/nexus/chrome-installer-flow-profile') if RESUME else '/home/nexus/chrome-installer-flow-profile'
assert re.fullmatch(r'/home/nexus/[a-zA-Z0-9_-]+',profile)
page='<title>NexusOS application download</title><style>body{background:#20344d;color:white;font:32px sans-serif;margin:40px}a{color:#8dc7ff}</style><h1>Install a downloaded application</h1><a href="'+html.escape(package['url'],quote=True)+'">Download '+html.escape(package['name'])+' over HTTPS</a>'
arguments=WORK/'desktop.args';arguments.write_text('--trace\n'+installed['chrome']['entry']+'\n--ozone-platform=wayland\n--disable-gpu\n--no-first-run\n--ipc-connection-timeout=120\n--user-data-dir='+profile+'\n'+'data:text/html,'+quote(page,safe='')+'\n')
serial=WORK/'serial.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(WORK);process=None
def read(fs,parts):
    found=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1]);assert found,parts;e=found[2]
    first=int.from_bytes(e[20:22],'little')<<16|int.from_bytes(e[26:28],'little');size=int.from_bytes(e[28:32],'little')
    return fs.read_chain(first,size)
def example_document_visible(picture):
    return picture.getpixel((100,700))==(238,238,238) and sum(max(pixel)<120 for pixel in picture.crop((200,160,1720,800)).get_flattened_data())>1000
assert not example_document_visible(Image.new('RGB',(1920,1080),(238,238,238))),'A title/background-only frame is not a rendered document'
try:
    if not RESUME:shutil.copyfile(nexus_paths.source_disk(),disk)
    if APP=='chrome' and disk.stat().st_size<3072*1024*1024:module('chrome_installer_storage',ROOT/'storage.py').grow(disk,3072)
    fs=fatput.Fat32(disk)
    try:
        assert fs.find_name(fs.lookup_dir(destination[:-1]),destination[-1]) is None
        assert hashlib.sha256(read(fs,installed['chrome']['entry'].strip('/').split('/'))).hexdigest()==installed['chrome']['entry_sha256']
        if RESUME:
            archive=read(fs,['home','nexus','Downloads',package['filename']])
            assert len(archive)==package['bytes'] and hashlib.sha256(archive).hexdigest()==package['sha256']
        else:
            fs.rm('home/nexus/Downloads/'+package['filename'])
            assert fs.find_name(fs.lookup_dir(['home','nexus','Downloads']),package['filename']) is None
    finally:fs.close()
    # This entire private image is disposable; avoid a second 3 GiB staging copy.
    assert fatput.edit(['fatput',str(disk),'put',str(OUT/'desktop.elf'),'sbin/init','put',str(arguments),'nexus/desktop.args',
        'put',str(OUT/'nexus-install.elf'),'bin/nexus-install','put',str(OUT/'nexus-installer-ui.elf'),'bin/xdg-open'])==0
    process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,
        '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4','-netdev','user,id=network','-device','rtl8139,netdev=network'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (WORK/'qmp_path').write_text(str(control.path));print('CHROME INSTALLER FLOW VM',process.pid,flush=True)
    def wait(pattern,after=0,seconds=120):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if serial.exists():
                log=serial.read_text();match=re.search(pattern,log[after:])
                if any(marker in log for marker in ('INSTALLER ERROR','USER_FAULT','KERNEL_FAULT')):raise AssertionError(log[-5000:])
                if match:return match
            if process.poll() is not None:raise AssertionError(serial.read_text()[-5000:])
            time.sleep(.05)
        raise TimeoutError(pattern)
    wait(r'DESKTOP BUFFER surface=\d+ 1920x1080',seconds=300)
    with control.open() as monitor:
        command=monitor.command;position=[960,540]
        def key(name,down):command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.1)
        def chord(modifier,name):
            for k,down in [(modifier,True),(name,True),(name,False),(modifier,False)]:key(k,down)
        def move(x,y):
            while position!=[x,y]:
                dx=max(-90,min(90,x-position[0]));dy=max(-90,min(90,y-position[1]));events=[]
                if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                position[0]+=dx;position[1]+=dy;command('input-send-event',{'events':events});time.sleep(.1)
        def click():
            for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.1)
        def frame(name,predicate,seconds=120):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                command('screendump',{'filename':str(WORK/'frame.ppm')})
                with Image.open(WORK/'frame.ppm') as image:
                    if predicate(image):image.save(WORK/(name+'.png'));return
                if process.poll() is not None:raise AssertionError('VM exited before '+name)
                time.sleep(.3)
            raise AssertionError('Framebuffer condition not met: '+name)
        frame('download-page',lambda p:p.getpixel((100,700))==(32,52,77))
        if not RESUME:move(270,264);click()
        chord('ctrl','j')
        def download_link(p):
            return sum(b>180 and b>r*1.4 and b>g*1.3 for r,g,b in p.crop((724,227,864,242)).get_flattened_data())>50
        frame('fresh-download',download_link,seconds=3600 if APP=='chrome' else 600)
        before=len(serial.read_text());move(790,235);click()
        wait('NEXUS OPEN PACKAGE path=/home/nexus/Downloads/'+re.escape(package['filename']),before,seconds=600)
        layout=wait(r'INSTALLER UI phase=1 primary=(\d+),(\d+) close=(\d+),(\d+)',before,seconds=300)
        frame('installer-opened-by-chrome',lambda p:p.getpixel((100,700))==(16,24,39) and p.getpixel((1334,900))==(40,102,213))
        move(*map(int,layout.groups()[:2]));click()
        programs=int(wait(r'INSTALLER READY TO RUN programs=(\d+)',seconds=1800 if APP=='chrome' else 120).group(1));assert programs>=1
        if APP=='hello':assert programs==1
        wait('INSTALLER UI phase=3')
        frame('installed',lambda p:p.getpixel((600,380))==(220,232,253))
        before=len(serial.read_text());key('ret',True);key('ret',False)
        if APP=='chrome':
            wait(r'set_title\("New Tab - Google Chrome"\)',before,seconds=300)
            frame('newly-installed-chrome',lambda p:p.getpixel((100,700))==(255,255,255))
            chord('ctrl','l')
            for character in 'https://example.com/':
                code={':':'semicolon','/':'slash','.':'dot'}.get(character,character)
                if character==':':key('shift',True)
                key(code,True);key(code,False)
                if character==':':key('shift',False)
            key('ret',True);key('ret',False)
            wait(r'set_title\("Example Domain - Google Chrome"\)',before,seconds=180)
            frame('newly-installed-chrome-https',example_document_visible)
            chord('alt','f4');wait(r'INSTALLER JOB DONE 2 status=0 output_bytes=\d+',before,seconds=120)
        else:
            wait('INSTALLER JOB DONE 2 status=0 output_bytes=14',before)
        wait('INSTALLER UI phase=5',before)
        if APP=='hello':frame('hello-output',lambda p:sum(pixel!=(233,237,244) for pixel in p.crop((500,735,585,750)).get_flattened_data())>80)
        key('esc',True);key('esc',False)
        frame('back-to-chrome',download_link)
        # A second click reuses the receipt; it must neither overwrite nor auto-run.
        before=len(serial.read_text());move(790,235);click();wait('INSTALLER UI phase=3',before)
        frame('repeat-open',lambda p:p.getpixel((600,380))==(220,232,253))
        repeated=serial.read_text()[before:];assert 'INSTALLER JOB 1 ' not in repeated and 'INSTALLER JOB 2 ' not in repeated
        key('esc',True);key('esc',False);frame('browser-restored',download_link);chord('alt','f4')
    output=process.communicate(timeout=60)[0];log=serial.read_text()
    assert process.returncode==33 and 'DESKTOP EXIT status=0' in log and 'INIT SESSION RETURNED' in log,(output,log[-5000:])
    assert not any(marker in log for marker in ('USER_FAULT','KERNEL_FAULT','DESKTOP RESOURCE LEAK','INSTALLER ERROR'))
    fs=fatput.Fat32(disk)
    try:
        archive=read(fs,['home','nexus','Downloads',package['filename']])
        assert len(archive)==package['bytes'] and hashlib.sha256(archive).hexdigest()==package['sha256']
        assert read(fs,destination+['.nexus-package-sha256'])==package['sha256'].encode()+b'\n'
        executable=read(fs,destination+(['opt','google','chrome','chrome'] if APP=='chrome' else ['usr','bin','hello']))
        assert hashlib.sha256(executable).hexdigest()==installed[APP]['entry_sha256']
        assert hashlib.sha256(read(fs,installed['chrome']['entry'].strip('/').split('/'))).hexdigest()==installed['chrome']['entry_sha256']
    finally:fs.close()
    (WORK/'result.json').write_text(json.dumps({'package':package,'chrome_entry':installed['chrome']['entry'],
        'source':('Preserved verified guest Chrome HTTPS download from the recorded prior boot; ' if RESUME else 'Fresh guest Chrome HTTPS download; ')+'real PS/2 file click, Install and Run; no DevTools or host-installed package',
        'resumed_download':RESUME,
        'application':APP,'new_chrome_public_https_verified':APP=='chrome',
        'https_document_pixels_verified':APP=='chrome',
        'boot_sha256':hashlib.sha256((OUT/'nexus64.img').read_bytes()).hexdigest(),
        'desktop_sha256':hashlib.sha256((OUT/'desktop.elf').read_bytes()).hexdigest(),
        'installer_sha256':hashlib.sha256((OUT/'nexus-install.elf').read_bytes()).hexdigest(),
        'installer_ui_sha256':hashlib.sha256((OUT/'nexus-installer-ui.elf').read_bytes()).hexdigest(),
        'entry_sha256':hashlib.sha256(executable).hexdigest(),'receipt_verified':True,'repeat_open_verified':True,
        'normal_shutdown':True,'private_disk_removed_on_exit':True},indent=2)+'\n')
    print('PASS actual Chrome HTTPS download -> native installer -> real '+APP+' execution; repeat-open, persisted hashes and clean shutdown',flush=True)
finally:
    if process is not None:
        if process.poll() is None:process.kill();process.wait()
        control.cleanup(process)
    disk.unlink(missing_ok=True)

"""Real installed Chrome menus, using guest PS/2 input and actual framebuffer changes."""
from pathlib import Path
import hashlib
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
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-popup-chrome')
WORK.mkdir(exist_ok=True);disk=WORK/'disk.img'
if disk.exists():raise FileExistsError('Refusing to overwrite '+str(disk))
if (WORK/'result.json').exists():(WORK/'result.json').rename(WORK/('previous-result-'+str(time.time_ns())+'.json'))
artifacts={name:hashlib.sha256((OUT/name).read_bytes()).hexdigest() for name in ('nexus64.img','desktop.elf')}
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
fatput=module('menu_fatput',ROOT/'userland/fatput.py');qmp=module('menu_qmp',ROOT/'tests/qmp_local.py')
entry=json.loads((OUT/'desktop64.json').read_text())['chrome']['entry']
page='<title>NexusOS menu test</title><style>body{background:#20344d;color:white;font:32px sans-serif;margin:40px}</style><h1>Installed Chrome on NexusOS</h1><a href="https://example.com/">Menu and context-menu test</a>'
arguments=WORK/'desktop.args';arguments.write_text('--trace\n'+entry+'\n--ozone-platform=wayland\n--disable-gpu\n--no-first-run\n--ipc-connection-timeout=120\n--user-data-dir=/home/nexus/chrome-popup-test-profile\n'+'data:text/html,'+quote(page,safe='')+'\n')
serial=WORK/'serial.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(WORK);process=None
try:
    shutil.copyfile(nexus_paths.source_disk(),disk)
    # The entire private image is disposable; avoid another multi-GiB staging copy.
    assert fatput.edit(['fatput',str(disk),'put',str(OUT/'desktop.elf'),'sbin/init','put',str(arguments),'nexus/desktop.args'])==0
    process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,
        '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4','-netdev','user,id=network','-device','rtl8139,netdev=network'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (WORK/'qmp_path').write_text(str(control.path));print('MENU TEST VM',process.pid,flush=True)
    def wait(pattern,after=0,seconds=120):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if serial.exists():
                match=re.search(pattern,serial.read_text()[after:])
                if match:return match
            if process.poll() is not None:raise AssertionError(serial.read_text()[-4000:])
            time.sleep(.05)
        raise TimeoutError(pattern)
    match=wait(r'DESKTOP BUFFER surface=\d+ (\d+)x(\d+)',seconds=300);width,height=map(int,match.groups())
    assert (width,height)==(1920,1080),(width,height)
    with control.open() as monitor:
        command=monitor.command;position=[width//2,height//2]
        def key(name,down):
            command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.08)
        def chord(modifier,name):
            for k,down in [(modifier,True),(name,True),(name,False),(modifier,False)]:key(k,down)
        def move(x,y):
            while position!=[x,y]:
                dx=max(-100,min(100,x-position[0]));dy=max(-100,min(100,y-position[1]));events=[]
                if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                position[0]+=dx;position[1]+=dy;command('input-send-event',{'events':events});time.sleep(.1)
        def click(button='left'):
            for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':down}}]});time.sleep(.1)
        def frame(name,predicate):
            until=time.monotonic()+120
            while time.monotonic()<until:
                command('screendump',{'filename':str(WORK/'frame.ppm')})
                with Image.open(WORK/'frame.ppm') as picture:
                    if predicate(picture):picture.save(WORK/(name+'.png'));return
                if process.poll() is not None:raise AssertionError('browser exited while waiting for pixels')
                time.sleep(.25)
            raise AssertionError('framebuffer condition not met: '+name)
        def blue(p):return p.getpixel((100,700))==(32,52,77)
        frame('before-menu',blue)
        def popup(after):
            result=wait(r'DESKTOP POPUP surface=(\d+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)',after)
            surface,x,y,w,h=map(int,result.groups());wait(r'DESKTOP BUFFER surface='+str(surface)+r' ',after);return x,y,w,h
        before=len(serial.read_text());chord('alt','f');x,y,w,h=popup(before)
        frame('app-menu',lambda p:all(v>220 for v in p.getpixel((x+8,y+40))))
        before=len(serial.read_text());move(x+130,y+203);sx,sy,sw,sh=popup(before)
        assert sx<0 and x+sx>=0,(x,sx,sw)
        frame('history-submenu',lambda p:all(v>220 for v in p.getpixel((x+sx+8,y+sy+40))))
        key('esc',True);key('esc',False);key('esc',True);key('esc',False)
        frame('dismissed',lambda p:blue(p) and p.getpixel((x+8,y+40))==(32,52,77))
        before=len(serial.read_text());chord('alt','f');x,y,w,h=popup(before)
        frame('reopened',lambda p:all(v>220 for v in p.getpixel((x+8,y+40))))
        move(x+80,y+25);click()
        frame('new-tab-action',lambda p:all(v>220 for v in p.getpixel((100,700))))
        chord('ctrl','w');frame('returned-to-document',blue)
        before=len(serial.read_text());move(200,264);click('right');x,y,w,h=popup(before)
        frame('context-menu',lambda p:all(v>220 for v in p.getpixel((x+8,y+40))))
        key('esc',True);key('esc',False);frame('context-dismissed',lambda p:blue(p) and p.getpixel((x+8,y+40))==(32,52,77))
        before=len(serial.read_text());move(width-22,62);click();x,y,w,h=popup(before)
        frame('mouse-menu',lambda p:all(v>220 for v in p.getpixel((x+8,y+40))))
        move(800,600);click();frame('outside-dismissed',lambda p:blue(p) and p.getpixel((x+8,y+40))==(32,52,77))
        chord('alt','f4')
    output=process.communicate(timeout=60)[0];log=serial.read_text()
    assert process.returncode==33 and 'DESKTOP EXIT status=0' in log and 'INIT SESSION RETURNED' in log,(output,log[-4000:])
    assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log
    assert all(hashlib.sha256((OUT/name).read_bytes()).hexdigest()==value for name,value in artifacts.items())
    (WORK/'result.json').write_text(json.dumps({'chrome_entry':entry,'artifacts':artifacts,
        'keyboard_mouse_menu':True,'constrained_history_submenu':True,'new_tab_action':True,
        'context_menu':True,'escape_outside_dismissal':True,'normal_shutdown':True,
        'verification':'PS/2 input, actual framebuffer and native serial; no DevTools'},indent=2)+'\n')
    print('PASS installed Chrome: keyboard/mouse app menu, constrained History submenu, Escape/outside click, New Tab menu action, return to original page, link context menu and clean shutdown',flush=True)
finally:
    if process is not None:
        if process.poll() is None:process.kill();process.wait()
        control.cleanup(process)
    disk.unlink(missing_ok=True)

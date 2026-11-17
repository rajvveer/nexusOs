"""Legacy boot/login smoke on QEMU's disposable overlay; never build or copy a disk.

Saved screenshots require visual review. Persisted account credentials are never
read or guessed. Only an explicitly unrestored factory guest may be logged in.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import subprocess
import time
from PIL import Image
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]
QEMU=Path(r'C:\Program Files\qemu\qemu-system-i386.exe')

def digest(path):
    result=hashlib.sha256()
    with path.open('rb') as source:
        while block:=source.read(1024*1024):result.update(block)
    return result.hexdigest()

def factory_login(log,source):
    if 'PERSIST: no valid snapshot' not in log or 'PERSIST: restored' in log:return None
    match=re.search(r'make_user\(\s*&users\[1\]\s*,\s*1000\s*,\s*1000\s*,\s*"([A-Za-z0-9]+)"\s*,\s*"([A-Za-z0-9]*)"\s*\)',source)
    return match.groups() if match else None

def login_pixels(picture):
    if picture.size!=(1920,1080):return False
    bright=lambda box:sum(min(pixel)>210 for pixel in picture.crop(box).convert('RGB').get_flattened_data())
    return all(high-low>40 for low,high in picture.convert('RGB').getextrema()) and bright((700,95,1220,240))>100 and bright((846,724,912,746))>15

def command(boot,disk,serial,qmp):
    return [str(QEMU),'-cpu','max','-m','2048',
        '-drive',f'file={boot},format=raw,if=floppy,readonly=on',
        '-drive',f'file={disk},format=raw,if=ide,snapshot=on',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga',
        '-serial',f'file:{serial}','-qmp',qmp,'-no-reboot',
        '-netdev','user,id=network','-device','rtl8139,netdev=network',
        '-audiodev','none,id=snd0','-device','AC97,audiodev=snd0']

def selftest():
    factory='make_user(&users[1],1000,1000,"fixture","fixture");'
    assert factory_login('PERSIST: no valid snapshot',factory)==('fixture','fixture')
    assert factory_login('PERSIST: restored gen 1',factory) is None
    assert factory_login('PERSIST: no valid snapshot\nPERSIST: restored gen 1',factory) is None
    assert factory_login('PERSIST: no valid snapshot',factory.replace('1000','0')) is None
    assert not login_pixels(Image.new('RGB',(1920,1080),'black'))
    assert not login_pixels(Image.new('RGB',(1920,1080),'white'))
    args=command(Path('boot.img'),Path('disk.img'),Path('serial.log'),'unix:fixture,server=on,wait=off')
    assert any('if=floppy,readonly=on' in arg for arg in args)
    assert any('if=ide,snapshot=on' in arg for arg in args) and '-monitor' not in args
    print('PASS legacy smoke guards: original boot read-only, disk snapshot, local QMP and no persisted credential guesses')

def main(work):
    boot=(ROOT/'nexus.img').resolve();disk=(ROOT/'disk.img').resolve()
    assert boot.is_file() and disk.is_file(),'Build artifacts must already exist'
    active=subprocess.check_output(['powershell','-NoProfile','-Command',
        "Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu%'\" | Select-Object -ExpandProperty CommandLine"],text=True)
    assert str(disk).replace('\\','/').lower() not in active.replace('\\','/').lower(),'Legacy disk already attached to QEMU'
    work=work.resolve();work.mkdir(parents=True,exist_ok=True)
    assert not (work/'result.json').exists(),'Choose a fresh evidence directory'
    before={str(path):digest(path) for path in (boot,disk)}
    spec=importlib.util.spec_from_file_location('legacy_snapshot_qmp',ROOT/'tests/qmp_local.py')
    qmp=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp)
    control=qmp.QmpMonitor(work);serial=work/'serial.log';serial.unlink(missing_ok=True)
    process=None;authenticated=False;failure=None;after={}
    def read_log():
        text=serial.read_text(encoding='latin1') if serial.exists() else ''
        assert '!EX:' not in text and '!KR:' not in text,'Legacy exception/kguard marker; inspect serial.log'
        return text
    try:
        with (work/'qemu.log').open('wb') as output:
            process=subprocess.Popen(command(boot,disk,serial,control.argument),stdout=output,stderr=subprocess.STDOUT)
        (work/'vm.json').write_text(json.dumps({'pid':process.pid,'snapshot':True,'disk':str(disk),'qmp':str(control.path)},indent=2)+'\n')
        deadline=time.monotonic()+120
        while 'PERSIST:' not in read_log():
            assert process.poll() is None,'Legacy VM exited before persistence/login stage'
            if time.monotonic()>=deadline:raise TimeoutError('Legacy persistence/login stage')
            time.sleep(.2)
        with control.open() as monitor:
            def capture(name):
                monitor.command('screendump',{'filename':str(work/'frame.ppm')})
                with Image.open(work/'frame.ppm') as frame:
                    picture=frame.convert('RGB');picture.save(work/(name+'.png'));return picture
            while not login_pixels(capture('login')):
                read_log();assert process.poll() is None,'Legacy VM exited before login UI'
                if time.monotonic()>=deadline:raise TimeoutError('Legacy login framebuffer')
                time.sleep(.3)
            credentials=factory_login(read_log(),(ROOT/'kernel/users.c').read_text(encoding='utf-8'))
            if credentials:
                def key(name,down):
                    monitor.command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]})
                    time.sleep(.2)
                for text in credentials:
                    for character in text:
                        if character.isupper():key('shift',True)
                        key(character.lower(),True);key(character.lower(),False)
                        if character.isupper():key('shift',False)
                    key('ret',True);key('ret',False);time.sleep(1)
                authenticated=True
                deadline=time.monotonic()+30
                while login_pixels(capture('desktop-candidate')):
                    read_log()
                    if time.monotonic()>=deadline:raise TimeoutError('Factory login did not leave login UI')
                    time.sleep(.3)
            read_log();monitor.command('quit')
        process.wait(timeout=10);assert process.returncode==0,'QEMU did not close normally'
    except BaseException as error:
        failure=type(error).__name__+': '+str(error)
        raise
    finally:
        if process is not None:
            if process.poll() is None:process.kill();process.wait()
            control.cleanup(process)
        after={str(path):digest(path) for path in (boot,disk)}
        result={'boot':str(boot),'disk':str(disk),'snapshot_overlay_discarded':process is not None and process.poll() is not None,
            'original_hashes':before,'after_hashes':after,'originals_unchanged':before==after,
            'factory_login_attempted':authenticated,'persisted_credentials_read':False,
            'visual_review_required':True,'failure':failure}
        (work/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        assert before==after,'Original legacy boot/disk bytes changed'
    print('PASS legacy boot/login framebuffer capture, no !EX:/!KR:, originals unchanged; review login.png'+(' and desktop-candidate.png' if authenticated else ''))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('work',nargs='?',type=Path,default=nexus_paths.runs('legacy-snapshot-smoke'))
    parser.add_argument('--self-test',action='store_true')
    args=parser.parse_args()
    if args.self_test:selftest()
    else:main(args.work)

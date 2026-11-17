"""Normal persistent desktop: public HTTPS -> download -> native Install/Run.

Uses the published desktop without building, staging, cloning or deleting it.
Real user interaction adds a Hello download/bundle and browser history. On failure
a live VM is left available through the recorded QMP socket for safe diagnosis.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
import time
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
DISK=OUT/'desktop64.img'
# this flow WRITES the user's installed disk (a download, browser history): only on explicit confirmation
if '--published-disk' not in sys.argv:raise SystemExit('test_installed_desktop_flow.py changes the user\'s installed disk (out/desktop64.img); pass --published-disk to confirm')
sys.argv=[a for a in sys.argv if a!='--published-disk']
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-installed-desktop-flow')
URL_TEXT_BOX=(170,54,731,73)
FOCUS_TEXT_BOX=(169,52,325,74)
with Image.open(ROOT/'tests/fixtures/chrome-hello-url.png') as fixture:
    url_text=fixture.convert('RGB');url_text_value=fixture.info['url']
with Image.open(ROOT/'tests/fixtures/chrome-selected-example-url.png') as fixture:
    focus_text=fixture.convert('RGB');focus_text_value=fixture.info['url']

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def idle():
    commands=subprocess.check_output(['powershell','-NoProfile','-Command',"Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu%'\" | Select-Object -ExpandProperty CommandLine"],text=True)
    assert str(DISK.resolve()).replace('\\','/').lower() not in commands.replace('\\','/').lower(),'Permanent desktop image is already attached to QEMU'

def read(fs,path):
    parts=path.strip('/').split('/');found=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1]);assert found,path
    entry=found[2];first=int.from_bytes(entry[20:22],'little')<<16|int.from_bytes(entry[26:28],'little')
    return fs.read_chain(first,int.from_bytes(entry[28:32],'little'))

def sha(data):return hashlib.sha256(data).hexdigest()

def example_document_visible(picture):
    return picture.size==(1920,1080) and picture.getpixel((100,700))==(238,238,238) and sum(max(pixel)<120 for pixel in picture.crop((200,160,1720,800)).get_flattened_data())>1000

def character_key(character):
    punctuation={':':('semicolon',True),'_':('minus',True),'-':('minus',False),'.':('dot',False),'/':('slash',False)}
    if character in punctuation:return punctuation[character]
    if character.isascii() and character.isalnum():return character.lower(),character.isupper()
    raise ValueError('Unsupported URL keyboard character: '+repr(character))

def omnibox_url_visible(picture):
    # shortcut: exact pixels for the pinned Chrome/fonts at 1080p; renew the reviewed fixture when they change.
    return (picture.size==(1920,1080) and picture.crop(URL_TEXT_BOX).convert('RGB').tobytes()==url_text.tobytes()
        and picture.crop((732,54,1800,73)).convert('RGB').getextrema()==((255,255),)*3)

def omnibox_selected(picture):
    return picture.size==(1920,1080) and picture.crop(FOCUS_TEXT_BOX).convert('RGB').tobytes()==focus_text.tobytes()

assert character_key('_')==('minus',True) and character_key(':')==('semicolon',True)
assert character_key('-')==('minus',False)
assert not example_document_visible(Image.new('RGB',(1920,1080),(238,238,238)))
sample=Image.new('RGB',(1920,1080),'white')
assert not omnibox_url_visible(sample)
sample.paste(url_text,URL_TEXT_BOX);assert omnibox_url_visible(sample)
sample.paste('white',URL_TEXT_BOX);sample.paste(url_text.crop((8,0,url_text.width,url_text.height)),(170,54))
assert not omnibox_url_visible(sample),'Missing initial h must not be accepted'
sample.paste(url_text,URL_TEXT_BOX);sample.paste('white',(724,54,731,73))
assert not omnibox_url_visible(sample),'Missing final b must not be accepted'
sample.paste(url_text,URL_TEXT_BOX);sample.paste(url_text.crop((0,0,8,url_text.height)),(732,54))
assert not omnibox_url_visible(sample),'A matching URL prefix must not accept extra text'
assert not omnibox_selected(sample),'Typed but unselected URL must not satisfy the focus gate'
sample.paste('white',(0,0,*sample.size));assert not omnibox_selected(sample)
sample.paste(focus_text,FOCUS_TEXT_BOX);assert omnibox_selected(sample)
sample.paste('white',(170,54,178,73));assert not omnibox_selected(sample),'Partial selection must not satisfy the focus gate'
del sample
if sys.argv[1:]==['--self-test']:
    print('PASS normal desktop frame gates: complete selection, reviewed URL, missing h/b, extra text and blank input')
    raise SystemExit(0)

installed=json.loads((OUT/'desktop64.json').read_text())
assert re.fullmatch('[0-9a-f]{64}',installed.get('installer_ui_sha256','')),'Publish the verified installer desktop first'
assert Path(installed['disk']).resolve()==DISK.resolve() and DISK.is_file()
idle()
package=json.loads((ROOT/'arch/x86_64/hello-download-lock.json').read_text())
assert package['url']==url_text_value,'Review a new omnibox fixture when the package URL changes'
assert package['url'].startswith('https://') and package['filename'].endswith('.deb')
for character in package['url']:character_key(character)
bundle='/home/nexus/Applications/'+package['sha256']
runtime={'sbin/init':'desktop_sha256','bin/nexus-install':'installer_sha256','bin/xdg-open':'installer_ui_sha256'}
boot_hash=sha((OUT/'nexus64.img').read_bytes());assert boot_hash==installed['boot_sha256']
fatput=module('normal_desktop_fatput',ROOT/'userland/fatput.py')
qmp=module('normal_desktop_qmp',ROOT/'tests/qmp_local.py')
fs=fatput.Fat32(DISK)
try:
    arguments=read(fs,'nexus/desktop.args')
    assert installed['chrome']['entry'].encode() in arguments and focus_text_value.encode() in arguments
    assert b'--trace' not in arguments and b'--remote-debugging' not in arguments and b'--no-sandbox' not in arguments
    for guest,field in runtime.items():assert sha(read(fs,guest))==installed[field],guest
    original_entries={installed[app]['entry']:installed[app]['entry_sha256'] for app in ('chrome','hello')}
    for guest,expected in original_entries.items():assert sha(read(fs,guest))==expected,guest
    downloads=fs.lookup_dir(['home','nexus','Downloads'])
    old_downloads={name for name,_,_,entry,_ in fs.named_entries(downloads) if entry[0] not in (0,0xe5) and entry[11]!=0xf and not entry[11]&0x18}
    had_bundle=fs.find_name(fs.lookup_dir(['home','nexus','Applications']),package['sha256']) is not None
    if had_bundle:
        assert read(fs,bundle+'/.nexus-package-sha256')==package['sha256'].encode()+b'\n'
        assert sha(read(fs,bundle+'/usr/bin/hello'))==installed['hello']['entry_sha256']
finally:fs.close()

WORK.mkdir(parents=True,exist_ok=True)
if (WORK/'result.json').exists():(WORK/'result.json').rename(WORK/('previous-result-'+str(time.time_ns())+'.json'))
serial=WORK/'serial.log';serial.unlink(missing_ok=True)
control=qmp.QmpMonitor(WORK);process=None
faults=('USER_FAULT','KERNEL_FAULT','DESKTOP RESOURCE LEAK','INSTALLER ERROR')
try:
    idle()
    with (WORK/'qemu.log').open('wb') as output:
        process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048',
            '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={DISK},format=raw,if=ide',
            '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,
            '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4','-netdev','user,id=network','-device','rtl8139,netdev=network'],stdout=output,stderr=subprocess.STDOUT)
    (WORK/'qmp_path').write_text(str(control.path))
    (WORK/'vm.json').write_text(json.dumps({'pid':process.pid,'disk':str(DISK),'qmp':str(control.path)},indent=2)+'\n')
    print('NORMAL INSTALLED DESKTOP VM',process.pid,flush=True)
    def log():
        text=serial.read_text() if serial.exists() else ''
        assert not any(marker in text for marker in faults),text[-5000:]
        return text
    def wait(pattern,after=0,seconds=120):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            text=log();match=re.search(pattern,text[after:])
            if match:return match
            assert process.poll() is None,text[-5000:]
            time.sleep(.1)
        raise TimeoutError(pattern)
    wait(r'DESKTOP BUFFER surface=\d+ 1920x1080',seconds=300)
    with control.open() as monitor:
        command=monitor.command;position=[960,540]
        def key(name,down):
            command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.1)
        def chord(modifier,name):
            for k,down in ((modifier,True),(name,True),(name,False),(modifier,False)):key(k,down)
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
                log();command('screendump',{'filename':str(WORK/'frame.ppm')})
                with Image.open(WORK/'frame.ppm') as picture:
                    if predicate(picture):picture.save(WORK/(name+'.png'));return
                assert process.poll() is None,'VM exited before '+name
                time.sleep(.3)
            raise AssertionError('Framebuffer condition not met: '+name)
        frame('normal-public-https',example_document_visible,seconds=300)
        chord('ctrl','l')
        frame('omnibox-selected',omnibox_selected)
        for character in package['url']:
            code,shift=character_key(character)
            if shift:key('shift',True)
            key(code,True);key(code,False)
            if shift:key('shift',False)
        frame('typed-hello-url',omnibox_url_visible)
        key('ret',True);key('ret',False);chord('ctrl','j')
        def download_link(picture):
            return sum(b>180 and b>r*1.4 and b>g*1.3 for r,g,b in picture.crop((724,227,864,242)).get_flattened_data())>50
        frame('hello-downloaded',download_link,seconds=600)
        before=len(log());move(790,235);click()
        opened=wait(r'NEXUS OPEN PACKAGE path=([^\r\n]+)',before,seconds=300).group(1)
        filename=opened.removeprefix('/home/nexus/Downloads/')
        assert opened=='/home/nexus/Downloads/'+filename
        assert re.fullmatch(re.escape(package['filename'][:-4])+r'(?: \([1-9][0-9]*\))?\.deb',filename),opened
        assert filename not in old_downloads,'The click opened an older download instead of this fresh transfer'
        layout=wait(r'INSTALLER UI phase=([13]) primary=(\d+),(\d+) close=(\d+),(\d+)',before,seconds=300)
        phase=int(layout.group(1));assert (phase==3)==had_bundle
        frame('installer-from-normal-chrome',lambda p:p.getpixel((100,700))==(16,24,39) and p.getpixel((1334,900))==(40,102,213))
        if phase==1:
            move(int(layout.group(2)),int(layout.group(3)));click()
        wait(r'INSTALLER READY TO RUN programs=1\b',before)
        wait('INSTALLER UI phase=3',before)
        frame('hello-installed',lambda p:p.getpixel((600,380))==(220,232,253))
        if had_bundle:assert 'INSTALLER JOB 1 ' not in log()[before:],'An existing bundle was reinstalled'
        before=len(log());key('ret',True);key('ret',False)
        wait('INSTALLER JOB DONE 2 status=0 output_bytes=14',before)
        wait('INSTALLER UI phase=5',before)
        frame('hello-output',lambda p:sum(pixel!=(233,237,244) for pixel in p.crop((500,735,585,750)).get_flattened_data())>80)
        key('esc',True);key('esc',False);frame('back-to-normal-chrome',download_link)
        chord('alt','f4')
    process.wait(timeout=120);text=log()
    assert process.returncode==33 and 'DESKTOP EXIT status=0' in text and 'INIT SESSION RETURNED' in text,text[-5000:]
    idle();fs=fatput.Fat32(DISK)
    try:
        archive=read(fs,opened);assert len(archive)==package['bytes'] and sha(archive)==package['sha256']
        assert read(fs,bundle+'/.nexus-package-sha256')==package['sha256'].encode()+b'\n'
        assert sha(read(fs,bundle+'/usr/bin/hello'))==installed['hello']['entry_sha256']
        assert read(fs,'nexus/desktop.args')==arguments
        for guest,expected in original_entries.items():assert sha(read(fs,guest))==expected,guest
        for guest,field in runtime.items():assert sha(read(fs,guest))==installed[field],guest
    finally:fs.close()
    assert sha((OUT/'nexus64.img').read_bytes())==boot_hash
    (WORK/'result.json').write_text(json.dumps({'disk':str(DISK),'permanent_image_preserved':True,
        'normal_desktop_args_sha256':sha(arguments),'no_staging_or_devtools':True,
        'https_document_pixels_verified':True,'package':package,'downloaded_path':opened,
        'receipt_verified':True,'entry_sha256':installed['hello']['entry_sha256'],'reused_bundle':had_bundle,
        'hello_output_bytes':14,'normal_shutdown':True,'original_entries_preserved':original_entries,
        **{field:installed[field] for field in ('boot_sha256','desktop_sha256','installer_sha256','installer_ui_sha256')}},indent=2)+'\n')
    print('PASS normal persistent Chrome: public document, fresh HTTPS download, native Install/Run, Hello output and clean shutdown',flush=True)
except BaseException as error:
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'pid':process.pid if process else None,
        'vm_still_running':process is not None and process.poll() is None,'qmp':str(control.path),
        'disk_preserved':str(DISK)},indent=2)+'\n')
    raise
finally:
    if process is not None:
        if process.poll() is None:print('VM left running for safe diagnosis; permanent disk retained; QMP '+str(control.path),flush=True)
        else:control.cleanup(process)

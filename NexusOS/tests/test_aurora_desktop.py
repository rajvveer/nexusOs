"""Two native Aurora boots: Calculator/Notes, window controls, Chrome and Hello.

Never builds or publishes. One private image is reused across both boots; failures
retain it and any live VM for diagnosis. --reuse-image requires reviewed SHA-256
provenance (disk, disk_sha256, source, source_sha256, reviewed=true).
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import re
import shutil
import statistics
import subprocess
import time
from PIL import Image
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out';SOURCE=None   # nexus_paths.source_disk() (never the user's disk)
# Initial pointer travel stays right of this area; closed application windows overlap it.
DESKTOP_PROOF_BOX=(400,450,850,850)
CHROME_STATE_FILE='home/nexus/.config/google-chrome/crash reports/settings.dat'
FAT_CASE=str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ','abcdefghijklmnopqrstuvwxyz')
def fat_path_key(path):return path.strip('/').translate(FAT_CASE)

def manifest_index(records):
    indexed={}
    for path,record in records.items():
        key=fat_path_key(path)
        assert key not in indexed,'Ambiguous FAT paths: '+path
        indexed[key]=record
    return indexed

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

def character_key(character):
    punctuation={':':('semicolon',True),'_':('minus',True),'-':('minus',False),'.':('dot',False),'/':('slash',False),
        '+':('equal',True),' ':('spc',False),'\n':('ret',False)}
    if character in punctuation:return punctuation[character]
    if character.isascii() and character.isalnum():return character.lower(),character.isupper()
    raise ValueError('Unsupported keyboard character '+repr(character))

def crop(picture,origin,box):
    x,y=origin;return picture.crop((x+box[0],y+box[1],x+box[2],y+box[3])).convert('RGB')

def surface_commits(text):
    return len(re.findall(r'(?m)^\[[^\r\n]+\]\s+wl_surface#\d+\.commit\(\)\r?$',text))

def document_visible(picture):
    return picture.getpixel((100,732))==(238,238,238) and sum(max(p)<120 for p in picture.crop((200,192,1720,832)).get_flattened_data())>1000

with Image.open(ROOT/'tests/fixtures/chrome-hello-url.png') as fixture:
    url_fixture=fixture.convert('RGB');fixture_url=fixture.info['url']
with Image.open(ROOT/'tests/fixtures/chrome-selected-example-url.png') as fixture:focus_fixture=fixture.convert('RGB')

def selected_url(picture):return crop(picture,(0,32),(169,52,325,74)).tobytes()==focus_fixture.tobytes()
def complete_url(picture):
    return crop(picture,(0,32),(170,54,731,73)).tobytes()==url_fixture.tobytes() and crop(picture,(0,32),(732,54,1800,73)).getextrema()==((255,255),)*3

assert character_key('_')==('minus',True) and character_key('+')==('equal',True)
assert surface_commits('[1.0] wl_surface#3.commit()\n[2.0] wl_surface#9.commit()\r\n')==2
assert surface_commits('[1.0] xdg_surface#3.ack_configure(8)\n[2.0]  -> wl_buffer#13.release()\n')==0
assert surface_commits('[1.0]  -> wl_surface#3.commit()\n')==0,'Count received client commits only'
sample=Image.new('RGB',(1920,1080),'white');assert not complete_url(sample) and not selected_url(sample) and not document_visible(sample)
assert not document_visible(Image.new('RGB',(1920,1080),(238,238,238))),'Background alone is not a document'
sample.paste(url_fixture,(170,86));assert complete_url(sample)
sample.paste('white',(170,86,178,105));assert not complete_url(sample),'Lost first URL character accepted'
sample.paste(url_fixture,(170,86));sample.paste(url_fixture.crop((0,0,8,url_fixture.height)),(732,86));assert not complete_url(sample),'Extra URL suffix accepted'
sample.paste('white',(0,0,1920,1080));sample.paste(focus_fixture,(169,84));assert selected_url(sample)
sample.paste('white',(170,86,178,105));assert not selected_url(sample),'Incomplete selection accepted'
sample.paste('white',(0,0,1920,1080));wallpaper_sample=sample.crop(DESKTOP_PROOF_BOX).tobytes()
sample.paste('black',(1228,808,1246,834));assert sample.crop(DESKTOP_PROOF_BOX).tobytes()==wallpaper_sample
sample.paste('black',(450,500,500,550));assert sample.crop(DESKTOP_PROOF_BOX).tobytes()!=wallpaper_sample,'An application covering the proof region must be detected'
del sample
sample_files={'HOME/NEXUS/NOTES.TXT':{'bytes':32,'sha256':'existing note'},'HOME/NEXUS/KEEP.TXT':{'bytes':7,'sha256':'original'},
    'HOME/NEXUS/Applications/abc/.NEXUS-PACKAGE-SHA256':{'bytes':65},'NEXUS/CHROME-DESKTOP.ARGS':{'bytes':216}}
sample_index=manifest_index(sample_files)
assert sample_index[fat_path_key('home/nexus/notes.txt')]['bytes']==32,'Existing uppercase Notes file must be read'
assert fat_path_key('home/nexus/Applications/abc/.nexus-package-sha256') in sample_index
assert fat_path_key('nexus/chrome-desktop.args') in sample_index
assert fat_path_key('HOME/NEXUS/DOWNLOADS/HELLO.DEB')==fat_path_key('/home/nexus/Downloads/hello.deb')
assert fat_path_key('Stra\u00dfe')!=fat_path_key('STRASSE'),'Native FAT folding is ASCII only'
assert fat_path_key('HOME/NEXUS/.config/google-chrome/Crash Reports/SETTINGS.DAT')==CHROME_STATE_FILE
assert fat_path_key('home/nexus/.config/google-chrome/Preferences')!=CHROME_STATE_FILE
assert fat_path_key('home/nexus/.config/google-chrome/Crash Reports/SETTINGS.DAT.backup')!=CHROME_STATE_FILE
changed_index=manifest_index({'home/nexus/keep.txt':{'bytes':7,'sha256':'changed'}})
assert changed_index[fat_path_key('HOME/NEXUS/KEEP.TXT')]!=sample_index[fat_path_key('home/nexus/keep.txt')],'Canonical lookup must still compare the full preservation record'
try:manifest_index({'notes.txt':{},'NOTES.TXT':{}})
except AssertionError:pass
else:raise AssertionError('Ambiguous case aliases must not silently overwrite preservation records')
del sample_files,sample_index,changed_index

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('work',type=Path,nargs='?',default=nexus_paths.runs('native64-aurora-desktop'))
parser.add_argument('--reuse-image',type=Path);parser.add_argument('--reuse-provenance',type=Path)
parser.add_argument('--backend-test',type=Path,default=OUT/'aurora_extra_fixed_test.elf')
parser.add_argument('--accel',choices=('tcg','whpx'),default='tcg')
parser.add_argument('--self-test',action='store_true');args=parser.parse_args()
acceleration={'tcg':['-cpu','max'],'whpx':['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand']}[args.accel]
cpu_model='max' if args.accel=='tcg' else 'qemu64,+rdrand'
if args.self_test:print('PASS Aurora frame/commit gates and native FAT case-insensitive manifest checks');raise SystemExit(0)
WORK=args.work.resolve();WORK.mkdir(parents=True,exist_ok=True);SOURCE=nexus_paths.source_disk()
aurora=module('aurora_lifecycle_host',ROOT/'arch/x86_64/aurora.py');fat=aurora.fat
qmp=module('aurora_lifecycle_qmp',ROOT/'tests/qmp_local.py')
disk=(args.reuse_image or WORK/'disk.img').resolve()
assert not nexus_paths.is_user_disk(disk) and disk!=SOURCE.resolve()
if disk.exists():assert not disk.samefile(SOURCE),'A private path aliases the permanent image'
aurora.require_idle([disk,SOURCE])
source_hash=aurora.digest(SOURCE)
if args.reuse_image:
    assert args.reuse_provenance and disk.is_file(),'Reuse requires existing image and reviewed provenance'
    provenance=json.loads(args.reuse_provenance.read_text())
    assert provenance.get('reviewed') is True and Path(provenance['disk']).resolve()==disk
    assert Path(provenance['source']).resolve()==SOURCE.resolve() and str(provenance['source_sha256']).lower()==source_hash
    assert str(provenance['disk_sha256']).lower()==aurora.digest(disk),'Reused image changed since ownership review'
else:
    assert not args.reuse_provenance and not disk.exists(),'Refusing to overwrite an existing test image'
if (WORK/'result.json').exists():(WORK/'result.json').rename(WORK/('previous-result-'+str(time.time_ns())+'.json'))
package=json.loads((ROOT/'arch/x86_64/hello-download-lock.json').read_text());assert package['url']==fixture_url
installed=json.loads((OUT/'desktop64.json').read_text());bundle='home/nexus/Applications/'+package['sha256']
artifacts={'sbin/init':OUT/'desktop.elf','bin/aurora-app':OUT/'aurora_app.elf',
    'bin/aurora-backend-test':args.backend_test,'bin/nexus-install':OUT/'nexus-install.elf','bin/xdg-open':OUT/'nexus-installer-ui.elf'}
hashes={path:aurora.digest(host) for path,host in artifacts.items()};boot_hash=aurora.digest(OUT/'nexus64.img')
notes_path='home/nexus/notes.txt';note_prefix=b'aurora native notes\nsaved twice\n';session=None;copy_complete=bool(args.reuse_image)

class Session:
    def __init__(self,number):
        self.work=WORK/('boot-'+str(number));self.work.mkdir(exist_ok=True);self.serial=self.work/'serial.log';self.serial.unlink(missing_ok=True)
        self.control=qmp.QmpMonitor(self.work);self.monitor=None;self.position=[960,540]
        aurora.require_idle([disk])
        with (self.work/'qemu.log').open('wb') as output:
            self.process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe',*acceleration,'-m','2048',
                '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
                '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{self.serial}','-qmp',self.control.argument,
                '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4','-netdev','user,id=network','-device','rtl8139,netdev=network'],stdout=output,stderr=subprocess.STDOUT)
        (self.work/'vm.json').write_text(json.dumps({'pid':self.process.pid,'disk':str(disk),'qmp':str(self.control.path),
            'accelerator':args.accel,'cpu':cpu_model,'acceleration_arguments':acceleration},indent=2)+'\n')
        (self.work/'qmp_path').write_text(str(self.control.path));print('AURORA LIFECYCLE VM',number,self.process.pid,flush=True)
    def log(self):
        text=self.serial.read_text() if self.serial.exists() else ''
        bad=('USER_FAULT','KERNEL_FAULT','DESKTOP RESOURCE LEAK','INSTALLER ERROR','[64] FAIL:','AURORA SAVE FAILED')
        assert not any(marker in text for marker in bad) and not re.search(r'AURORA APP CLOSED app=\w+ status=[1-9]',text),text[-6000:]
        return text
    def wait(self,pattern,after=0,seconds=120):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            text=self.log();match=re.search(pattern,text[after:])
            if match:return match
            assert self.process.poll() is None,text[-6000:]
            time.sleep(.1)
        raise TimeoutError(pattern)
    def ready(self):
        self.wait('AURORA READY persistent desktop',seconds=300)
        self.wait('AURORA BACKEND TEST PASSED:',seconds=120)
        self.wait(r'AURORA CHILD EXIT pid=\d+ status=0',seconds=120)
        self.monitor=self.control.open();self.move(1900,1040)
        return self.frame('desktop',lambda p:p.size==(1920,1080) and len(p.crop(DESKTOP_PROOF_BOX).getcolors(1000000))>100)
    def command(self,name,arguments=None):return self.monitor.command(name,arguments)
    def key(self,name,down):
        self.command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.15)
    def tap(self,name):self.key(name,True);self.key(name,False)
    def chord(self,modifier,name):
        for key,down in ((modifier,True),(name,True),(name,False),(modifier,False)):self.key(key,down)
    def type(self,text):
        for character in text:
            name,shift=character_key(character)
            if shift:self.key('shift',True)
            self.tap(name)
            if shift:self.key('shift',False)
    def move(self,x,y):
        while self.position!=[x,y]:
            dx=max(-90,min(90,x-self.position[0]));dy=max(-90,min(90,y-self.position[1]));events=[]
            if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
            if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
            self.position[0]+=dx;self.position[1]+=dy;self.command('input-send-event',{'events':events});time.sleep(.1)
    def button(self,down):self.command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.15)
    def click(self,x,y):self.move(x,y);self.button(True);self.button(False)
    def frame(self,name,predicate,seconds=120):
        until=time.monotonic()+seconds
        while time.monotonic()<until:
            self.log();self.command('screendump',{'filename':str(self.work/'frame.ppm')})
            with Image.open(self.work/'frame.ppm') as source:
                picture=source.convert('RGB')
                if predicate(picture):picture.save(self.work/(name+'.png'));return picture
            assert self.process.poll() is None,'VM exited before '+name
            time.sleep(.3)
        raise AssertionError('Framebuffer condition not met: '+name)
    def window(self,app,after=0,title=None):
        pattern=r'AURORA WINDOW app='+re.escape(app)+r' title='+ (re.escape(title) if title else r'[^\r\n]*?')+r' x=(-?\d+) y=(-?\d+)'
        return tuple(map(int,self.wait(pattern,after,seconds=300).groups()))
    def open_app(self,name,x):
        before=len(self.log());self.click(x,1040);self.wait('AURORA APP OPEN app='+name+' ',before)
        origin=self.window('nexus.'+name,before)
        self.frame(name+'-opened',lambda p:p.getpixel((origin[0]+15,origin[1]+14))==(255,95,87))
        return origin
    def close_app(self,name):
        before=len(self.log());self.chord('alt','f4');self.wait('AURORA APP CLOSED app='+name+' status=0',before)
    def settled_commits(self,after,seconds=30):
        until=time.monotonic()+seconds;previous=-1;stable=time.monotonic()
        while time.monotonic()<until:
            text=self.log()[after:];count=surface_commits(text)
            if count!=previous:previous=count;stable=time.monotonic()
            config=list(re.finditer(r' -> xdg_surface#\d+\.configure\((\d+)\)',text))
            ack=list(re.finditer(r'(?m)^\[[^\r\n]+\]\s+xdg_surface#\d+\.ack_configure\((\d+)\)',text))
            commits=list(re.finditer(r'(?m)^\[[^\r\n]+\]\s+wl_surface#\d+\.commit\(\)',text))
            releases=list(re.finditer(r' -> wl_buffer#\d+\.release\(\)',text))
            if config and ack and commits and releases and config[-1].group(1)==ack[-1].group(1) and \
                    releases[-1].start()>commits[-1].start()>ack[-1].start() and time.monotonic()-stable>=.3:return count
            assert self.process.poll() is None,'VM exited while waiting for settled client buffers'
            time.sleep(.05)
        raise AssertionError('Client configure/commit/release sequence did not settle')
    def static_notes_redraw(self):
        # Notes is the only Wayland client here, so resource IDs cannot alias another app.
        start=len(self.log());origin=self.open_app('notes',992);x,y=origin
        self.move(x+220,y+80);baseline=self.settled_commits(start)
        idle_started=time.monotonic()
        while time.monotonic()-idle_started<2:
            assert surface_commits(self.log()[start:])==baseline,'Idle Notes repainted without changed content'
            assert self.process.poll() is None,'VM exited during idle Notes observation'
            time.sleep(.05)
        idle_seconds=time.monotonic()-idle_started
        lights=(9,8,22,21)
        plain=crop(self.frame('notes-idle-no-repaint',lambda p:p.getpixel((x+15,y+14))==(255,95,87)),origin,lights).tobytes()
        for px,py in ((300,100),(200,14),(320,14),(220,80)):
            before=len(self.log());self.move(x+px,y+py)
            self.wait(r'wl_pointer#\d+\.motion\(\d+, '+str(px)+r'\.0+, '+str(py)+r'\.0+\)',before,seconds=30)
        assert self.settled_commits(start)==baseline,'Motion outside the traffic lights repainted Notes'
        before=len(self.log());self.move(x+61,y+21)
        self.wait(r'wl_pointer#\d+\.motion\(\d+, 61\.0+, 21\.0+\)',before,seconds=30)
        self.frame('notes-hover-glyphs',lambda p:crop(p,origin,lights).tobytes()!=plain,seconds=30)
        hovered=self.settled_commits(start)
        assert hovered==baseline+1,'Entering the traffic lights must repaint exactly once'
        before=len(self.log());self.move(x+160,y+14)
        self.wait(r'wl_pointer#\d+\.motion\(\d+, 160\.0+, 14\.0+\)',before,seconds=30)
        self.frame('notes-hover-cleared',lambda p:crop(p,origin,lights).tobytes()==plain,seconds=30)
        left=self.settled_commits(start)
        assert left==hovered+1,'Leaving the traffic lights must repaint exactly once'
        self.close_app('notes')
        return {'idle_seconds':idle_seconds,'idle_extra_commits':0,'ordinary_motion_extra_commits':0,
            'hover_enter_commits':hovered-baseline,'hover_leave_commits':left-hovered,'hover_pixels_verified':True}
    def window_drag_latency(self,origin):
        x,y=origin;self.move(x+140,y+14);before=len(self.log());self.button(True)
        samples=[];offset=0;red=(255,95,87)
        try:
            self.wait(r'xdg_toplevel#\d+\.move\(wl_seat#\d+, \d+\)',before,seconds=30)
            for step in (40,-40,40,-40,40,-40):
                previous=(x+offset+15,y+14);offset+=step;current=(x+offset+15,y+14)
                started=time.perf_counter();self.command('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':step}}]})
                self.position[0]+=step
                while time.perf_counter()-started<30:
                    self.command('screendump',{'filename':str(self.work/'frame.ppm')})
                    with Image.open(self.work/'frame.ppm') as source:
                        picture=source.convert('RGB')
                        if picture.getpixel(current)==red and picture.getpixel(previous)!=red:
                            samples.append((time.perf_counter()-started)*1000);break
                    self.log();assert self.process.poll() is None,'VM exited during window drag observation'
                else:raise AssertionError('Dragged Calculator did not reach its expected framebuffer location')
            picture.save(self.work/'calculator-drag-latency.png')
        finally:
            if self.process.poll() is None:self.button(False)
        assert offset==0 and self.position==[x+140,y+14]
        result={'samples_ms':samples,'median_ms':statistics.median(samples),'max_ms':max(samples),
            'relative_steps_px':[40,-40,40,-40,40,-40],'net_window_movement_px':0,
            'measurement':'QMP physical move to observed new title button and erased old button; includes QMP, screendump and image decode overhead; not an FPS measurement.'}
        (self.work/'window-drag-latency.json').write_text(json.dumps(result,indent=2)+'\n')
        return result
    def shutdown(self):
        self.click(16,14)
        for _ in range(3):self.tap('down')
        self.command('input-send-event',{'events':[{'type':'key','data':{'down':value,'key':{'type':'qcode','data':'ret'}}} for value in (True,False)]})
        self.process.wait(timeout=120);text=self.log()
        assert self.process.returncode==33 and 'DESKTOP EXIT status=0' in text and 'INIT SESSION RETURNED' in text,text[-6000:]
        self.monitor.close();self.control.cleanup(self.process)

def disk_check(expected_notes,downloaded=None):
    aurora.require_idle([disk]);fs=fat.Fat32(disk)
    try:
        assert aurora.read(fs,notes_path)==expected_notes
        for app in ('chrome','hello'):assert hashlib.sha256(aurora.read(fs,installed[app]['entry'])).hexdigest()==installed[app]['entry_sha256']
        for path,expected in hashes.items():assert hashlib.sha256(aurora.read(fs,path)).hexdigest()==expected
        if downloaded:
            data=aurora.read(fs,downloaded);assert len(data)==package['bytes'] and hashlib.sha256(data).hexdigest()==package['sha256']
            assert aurora.read(fs,bundle+'/.nexus-package-sha256')==package['sha256'].encode()+b'\n'
            assert hashlib.sha256(aurora.read(fs,bundle+'/usr/bin/hello')).hexdigest()==installed['hello']['entry_sha256']
    finally:fs.f.close()

try:
    if not args.reuse_image:shutil.copyfile(SOURCE,disk);copy_complete=True
    fs=fat.Fat32(disk)
    try:
        before_private,_=aurora.inventory(fs)
        before_index=manifest_index(before_private)
        original_notes=aurora.read(fs,notes_path) if fat_path_key(notes_path) in before_index else b''
        assert b'\0' not in original_notes,'Existing note is binary; do not replace it as a test fixture'
        chrome_arguments=aurora.read(fs,'nexus/chrome-desktop.args' if fat_path_key('nexus/chrome-desktop.args') in before_index else 'nexus/desktop.args')
        chrome_lines=[line for line in chrome_arguments.splitlines() if line and line!=b'--trace']
        assert chrome_lines and chrome_lines[0]==installed['chrome']['entry'].encode()
        assert b'--remote-debugging' not in chrome_arguments and b'chrome-inspect' not in chrome_arguments and b'--no-sandbox' not in chrome_arguments
        old_downloads={fat_path_key(Path(path).name) for path in before_private if fat_path_key(path).startswith('home/nexus/downloads/')}
        had_bundle=fat_path_key(bundle+'/.nexus-package-sha256') in before_index
        if had_bundle:assert aurora.read(fs,bundle+'/.nexus-package-sha256')==package['sha256'].encode()+b'\n'
    finally:fs.f.close()
    protected=aurora.reserved(disk)
    (WORK/'before-files.json').write_text(json.dumps(before_private,indent=2)+'\n')
    (WORK/'desktop.args').write_text('--trace\n--aurora\n/bin/aurora-backend-test\n')
    (WORK/'chrome-desktop.args').write_bytes(chrome_arguments.rstrip()+b'\n--start-maximized\n')
    stage={**artifacts,'nexus/desktop.args':WORK/'desktop.args','nexus/chrome-desktop.args':WORK/'chrome-desktop.args'}
    assert fat.edit(['fatput',str(disk),*[item for path,host in stage.items() for item in ('put',str(host),path)]])==0
    session=Session(0);desktop=session.ready();static_redraw=session.static_notes_redraw()
    calc=session.open_app('calculator',1120);drag_latency=session.window_drag_latency(calc);x,y=calc
    session.move(x+140,y+14);session.button(True);session.move(x+260,y+104);session.button(False);calc=(x+120,y+90);x,y=calc
    session.frame('calculator-moved',lambda p:p.getpixel((x+15,y+14))==(255,95,87))
    session.move(x+237,y+381);session.button(True);session.move(x+317,y+445);session.button(False)
    session.wait(r'DESKTOP BUFFER surface=\d+ 320x448')
    display_box=(1,29,300,107)
    zero=crop(session.frame('calculator-zero',lambda p:p.getpixel((x+15,y+14))==(255,95,87) and p.getpixel((x+260,y+119))==(255,159,10)),calc,display_box).tobytes()
    # Hold a modifier across a mouse-opened shell menu, then verify plain digit input after release.
    session.key('ctrl',True);session.click(16,14);session.key('ctrl',False);session.tap('esc');session.tap('5')
    five=crop(session.frame('calculator-reference-five',lambda p:crop(p,calc,display_box).tobytes()!=zero),calc,display_box).tobytes()
    session.tap('backspace');session.frame('calculator-cleared',lambda p:crop(p,calc,display_box).tobytes()==zero)
    session.type('2+3\n');session.frame('calculator-result-five',lambda p:crop(p,calc,display_box).tobytes()==five)
    before=len(session.log());session.click(x+36,y+14);session.wait('AURORA MINIMIZE app=nexus.calculator',before)
    session.move(1900,1040);session.frame('calculator-minimized',lambda p:p.getpixel((x+15,y+14))!=(255,95,87))
    before=len(session.log());session.click(1120,1040);session.wait('AURORA RESTORE app=nexus.calculator',before)
    session.frame('calculator-restored',lambda p:crop(p,calc,display_box).tobytes()==five)
    notes=session.open_app('notes',992)
    note_length=len(original_notes)
    for text in ('aurora native notes\n','saved twice\n'):
        note_length+=len(text.encode())
        before=len(session.log());session.type(text);session.chord('ctrl','s');session.wait(r'AURORA NOTES SAVED bytes='+str(note_length)+r' path=/home/nexus/notes\.txt',before)
    note_box=(19,99,19+8*19,135)
    note_pixels=crop(session.frame('notes-saved-twice',lambda p:p.getpixel((notes[0]+15,notes[1]+14))==(255,95,87)),notes,note_box).tobytes()
    before=len(session.log());session.click(864,1040);chrome_pid=session.wait(r'AURORA LAUNCH app=chrome pid=(\d+)',before).group(1)
    session.window('google-chrome',before);session.wait(r'DESKTOP CONFIGURE surface=\d+ 1920x960',before)
    session.frame('chrome-public-https-in-aurora',document_visible,seconds=300)
    before=len(session.log());session.chord('alt','tab');session.chord('ctrl','s')
    session.wait(r'AURORA NOTES SAVED bytes='+str(len(original_notes)+len(note_prefix))+r' path=/home/nexus/notes\.txt',before)
    session.frame('notes-above-chrome',lambda p:crop(p,notes,note_box).tobytes()==note_pixels)
    session.chord('alt','tab');session.frame('chrome-restored-after-alt-tab',document_visible)
    session.chord('ctrl','l');session.frame('chrome-omnibox-focused',selected_url)
    session.type(package['url']);session.frame('chrome-complete-download-url',complete_url)
    session.tap('ret');session.chord('ctrl','j')
    def download_visible(p):return sum(b>180 and b>r*1.4 and b>g*1.3 for r,g,b in p.crop((724,259,864,274)).get_flattened_data())>50
    session.frame('hello-downloaded',download_visible,seconds=600)
    before=len(session.log());session.click(790,267)
    downloaded=session.wait(r'NEXUS OPEN PACKAGE path=([^\r\n]+)',before,seconds=300).group(1)
    filename=downloaded.rsplit('/',1)[-1]
    assert fat_path_key(downloaded)==fat_path_key('/home/nexus/Downloads/'+filename) and fat_path_key(filename) not in old_downloads
    assert re.fullmatch(re.escape(fat_path_key(package['filename'][:-4]))+r'(?: \([1-9][0-9]*\))?\.deb',fat_path_key(filename))
    installer=session.window('',before,'NexusOS Application Installer')
    layout=session.wait(r'INSTALLER UI phase=([13]) primary=(\d+),(\d+) close=(\d+),(\d+)',before,seconds=300)
    phase,px,py,cx,cy=map(int,layout.groups());assert (phase==3)==had_bundle
    session.frame('installer-from-aurora-chrome',lambda p:p.getpixel((installer[0]+px,installer[1]+py))==(40,102,213))
    if phase==1:session.click(installer[0]+px,installer[1]+py)
    session.wait(r'INSTALLER READY TO RUN programs=1\b',before);session.wait('INSTALLER UI phase=3',before)
    before=len(session.log());session.tap('ret');session.wait('INSTALLER JOB DONE 2 status=0 output_bytes=14',before)
    session.wait('INSTALLER UI phase=5',before)
    session.frame('hello-output-in-aurora',lambda p:sum(pixel!=(233,237,244) for pixel in crop(p,installer,(cx-62,py-165,cx+33,py-147)).get_flattened_data())>50)
    session.tap('esc');session.frame('chrome-after-installer',download_visible)
    before=len(session.log());session.chord('alt','f4');session.wait('AURORA CHILD EXIT pid='+chrome_pid+' status=0',before,seconds=180)
    session.click(1120,1040);session.frame('calculator-after-chrome',lambda p:crop(p,calc,display_box).tobytes()==five)
    session.click(x+45,y+136);session.frame('calculator-final-clear',lambda p:crop(p,calc,display_box).tobytes()==zero)
    session.type('2+3\n');session.frame('calculator-after-chrome-input',lambda p:crop(p,calc,display_box).tobytes()==five)
    session.close_app('calculator');session.click(992,1040);session.close_app('notes')
    session.move(1900,1040);wallpaper=desktop.crop(DESKTOP_PROOF_BOX).tobytes()
    session.frame('desktop-survives-all-apps',lambda p:p.crop(DESKTOP_PROOF_BOX).tobytes()==wallpaper)
    session.shutdown();expected_notes=note_prefix+original_notes;disk_check(expected_notes,downloaded)
    session=Session(1);session.ready();notes=session.open_app('notes',992)
    session.frame('notes-reloaded-after-cold-boot',lambda p:crop(p,notes,note_box).tobytes()==note_pixels)
    session.close_app('notes');session.shutdown();disk_check(expected_notes,downloaded)
    after_index=manifest_index(aurora.manifest(disk));mutable=('home/nexus/chrome-', 'tmp/', 'run/user/1000/')
    staged_paths={fat_path_key(path) for path in stage}
    for path,record in before_private.items():
        key=fat_path_key(path)
        if key in staged_paths or key==fat_path_key(notes_path) or key.startswith(mutable):continue
        if key==CHROME_STATE_FILE:
            state=after_index.get(key)
            assert state and not state['directory'] and state['native_kind']==record['native_kind']==0 and state['bytes']==record['bytes']==40,'Chrome state file disappeared or changed type/size'
            continue
        assert after_index.get(key)==record,'Unrelated original file changed: '+path
    assert aurora.reserved(disk)==protected and aurora.digest(SOURCE)==source_hash
    assert aurora.digest(OUT/'nexus64.img')==boot_hash
    for path,host in artifacts.items():assert aurora.digest(host)==hashes[path]
    aurora.require_idle([disk]);disk.unlink()
    result={'aurora_desktop_verified':True,'chrome_verified':True,'legacy_apps_verified':True,'normal_shutdown':True,
        'accelerator':args.accel,'cpu':cpu_model,'acceleration_arguments':acceleration,
        'verified_apps':['original Calculator','native Notes'],'scope':'Two applications, shell/window lifecycle, real Chrome and downloaded Hello; other legacy apps not covered by this campaign.',
        'boot_sha256':boot_hash,'desktop_sha256':hashes['sbin/init'],'aurora_app_sha256':hashes['bin/aurora-app'],
        'backend_test_sha256':hashes['bin/aurora-backend-test'],'installer_sha256':hashes['bin/nexus-install'],'installer_ui_sha256':hashes['bin/xdg-open'],
        'source_sha256':source_hash,'source_preserved':True,'private_image_removed':True,'cold_boot_notes_verified':True,
        'static_app_redraw_verified':True,'static_app_redraw':static_redraw,
        'window_drag_latency':drag_latency,
        'physical_modifier_release_verified':True,'physical_alt_tab_verified':True,'hello_download':downloaded,'hello_package':package,
        'hello_reused_bundle':had_bundle,'hello_output_bytes':14,'test_chrome_start_maximized':True,'no_devtools':True}
    (WORK/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('PASS Aurora two-boot lifecycle, Calculator/Notes, Chrome HTTPS and Hello; private disk removed',flush=True)
except BaseException as error:
    if not copy_complete and disk.exists():disk.unlink()
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'disk':str(disk),'retained':disk.exists(),
        'pid':session.process.pid if session else None,'live':session is not None and session.process.poll() is None,
        'qmp':str(session.control.path) if session else None},indent=2)+'\n')
    raise
finally:
    if session:
        if session.monitor:session.monitor.close()
        if session.process.poll() is not None:session.control.cleanup(session.process)
        else:print('Live private VM retained for diagnosis:',session.process.pid,str(disk),flush=True)

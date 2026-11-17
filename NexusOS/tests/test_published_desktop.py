"""Smoke the actual published desktop/chrome launcher without building or cloning.

By default only display, serial destination and a host-local QMP socket differ
from its SDL command; failures leave the persistent VM alive. --interact implies
a disposable QEMU snapshot, discarded after shutdown or failure. Guest arguments
remain untouched; the optional page fixture runs ordinary browser JavaScript.
"""
from pathlib import Path
from unittest.mock import patch
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import importlib.util
import json
import re
import runpy
import secrets
import subprocess
import threading
import time
from PIL import Image,ImageChops

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out';DISK=OUT/'desktop64.img'
PROOF=(400,450,850,850)
FIXTURE_MARKER=(13,173,191)

def pointer_press(command,button):
    sent=time.perf_counter()
    command('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':True}}]})
    time.sleep(.04)
    command('input-send-event',{'events':[{'type':'btn','data':{'button':button,'down':False}}]})
    return sent,(time.perf_counter()-sent)*1000

def fixture_origin(picture):
    delta=ImageChops.difference(picture.convert('RGB'),Image.new('RGB',picture.size,FIXTURE_MARKER)).split()
    mask=ImageChops.lighter(ImageChops.lighter(delta[0],delta[1]),delta[2]).point(lambda value:255 if value==0 else 0)
    box=mask.getbbox()
    if not box or box[2]-box[0]<800 or box[3]-box[1]!=12:return None
    if mask.crop(box).getextrema()!=(255,255):return None
    return box[:2]

class InteractionFixture:
    def __init__(self):
        self.path='/nexus-input-'+secrets.token_hex(8);self.events=[];self.requests=0;self.lock=threading.Lock()
        cards=''.join(f'<article style="background:{color}"><h2>Card {i+1:02d}</h2><p>Scroll the real browser page.</p></article>'
            for i,color in enumerate(('#bd6028','#6547ad','#227b8a','#8d496b')*4))
        self.html=('''<!doctype html><meta charset="utf-8"><title>Nexus input test</title>
<style>html{color-scheme:light}body{margin:0;font:24px monospace;color:white}header{position:sticky;top:0;height:180px;background:#20334c;z-index:1}
#marker{width:1000px;max-width:100%;height:12px;background:#0dadbf}h1{font:28px monospace;margin:16px 32px}input,button,#status{position:absolute;top:88px;height:48px;box-sizing:border-box;font:22px monospace;border:0;border-radius:5px}
input{left:32px;width:240px;padding:8px;background:white;color:#182234}button{left:300px;width:160px;background:#327dcb;color:white}
#status{left:500px;width:240px;padding:10px;background:#9c2f47}article{height:320px;box-sizing:border-box;padding:32px 60px;border-bottom:24px solid #182234}h2{font:64px monospace;margin:10px 0}p{font:24px monospace}</style>
<header><div id="marker"></div><h1>NexusOS browser interaction test</h1><input id="entry" aria-label="Type nexus" autocomplete="off"><button id="confirm">Confirm</button><div id="status">Type nexus</div></header>'''+cards+'''
<script>
const entry=document.getElementById('entry'),status=document.getElementById('status');
function report(event,value=0){fetch(location.pathname+'/event',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({event,value})}).catch(()=>{});}
entry.addEventListener('input',()=>{if(entry.value==='nexus'){status.style.background='#2ca36c';status.textContent='Typed nexus';report('typed');}});
document.getElementById('confirm').addEventListener('click',()=>{if(entry.value==='nexus'){status.style.background='#6751c8';status.textContent='Confirmed nexus';report('clicked');}});
let scrolling;addEventListener('scroll',()=>{clearTimeout(scrolling);scrolling=setTimeout(()=>report('scroll',Math.round(scrollY)),30);});
report('ready',devicePixelRatio);
</script>''').encode()
        fixture=self
        class Handler(BaseHTTPRequestHandler):
            def log_message(self,*values):pass
            def do_GET(self):
                if self.path!=fixture.path:self.send_error(404);return
                with fixture.lock:fixture.requests+=1
                self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8');self.send_header('Content-Length',str(len(fixture.html)));self.send_header('Cache-Control','no-store');self.end_headers();self.wfile.write(fixture.html)
            def do_POST(self):
                if self.path!=fixture.path+'/event':self.send_error(404);return
                try:
                    count=int(self.headers.get('Content-Length','0'))
                    if not 0<count<=128:raise ValueError()
                    data=json.loads(self.rfile.read(count));event=data['event'];value=data['value']
                    if set(data)!= {'event','value'} or event not in ('ready','typed','clicked','scroll') or type(value) not in (int,float) or not 0<=value<=10000:raise ValueError()
                except (ValueError,KeyError,TypeError):self.send_error(400);return
                with fixture.lock:fixture.events.append({'event':event,'value':value,'received':time.perf_counter()})
                self.send_response(204);self.end_headers()
        self.server=ThreadingHTTPServer(('127.0.0.1',0),Handler);self.server.daemon_threads=True
        self.thread=threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.url=f'http://10.0.2.2:{self.server.server_port}'+self.path
    def received(self,event):
        with self.lock:return [row.copy() for row in self.events if row['event']==event]
    def stop(self):self.server.shutdown();self.server.server_close();self.thread.join(timeout=5);assert not self.thread.is_alive()

def document_visible(picture):
    # Count document ink surrounded by page background, independent of window placement.
    picture=picture.convert('RGB');channels=picture.split()
    dark=ImageChops.lighter(ImageChops.lighter(channels[0],channels[1]),channels[2]).point(lambda v:255 if v<120 else 0)
    delta=ImageChops.difference(picture,Image.new('RGB',picture.size,(238,238,238))).split()
    gray=ImageChops.lighter(ImageChops.lighter(delta[0],delta[1]),delta[2]).point(lambda v:255 if v==0 else 0)
    for dx,dy in ((24,24),(-24,24),(24,-24),(-24,-24)):dark=ImageChops.multiply(dark,ImageChops.offset(gray,dx,dy))
    return dark.crop((32,140,picture.width-32,picture.height-100)).histogram()[255]>1000

assert not document_visible(Image.new('RGB',(1920,1080),(238,238,238)))
sample=Image.new('RGB',(1920,1080),(238,238,238));sample.paste('black',(20,20,500,80));assert not document_visible(sample)
for y in range(260,390,26):
    for x in range(300,650,14):sample.paste('black',(x,y,x+5,y+10))
assert document_visible(sample),'The page-ink gate must accept a floating document region'
assert fixture_origin(sample) is None
sample.paste(FIXTURE_MARKER,(100,200,1100,212));assert fixture_origin(sample)==(100,200)
sample.paste('white',(1600,180,1920,420));assert fixture_origin(sample)==(100,200),'A right-side browser bubble must not obscure the bounded fixture marker'
sample.putpixel((120,202),(0,0,0));assert fixture_origin(sample) is None
del sample

parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('mode',choices=('desktop','chrome'))
parser.add_argument('work',type=Path);parser.add_argument('--snapshot',action='store_true')
parser.add_argument('--interact',action='store_true',help='Use a disposable snapshot to test real browser typing, a button and scrolling against a local fixture')
parser.add_argument('--self-test',action='store_true')
parser.add_argument('--published-disk',action='store_true',help='confirm: this boots the launcher on the user\'s installed disk (out/desktop64.img)')
args=parser.parse_args();args.snapshot=args.snapshot or args.interact
if not args.self_test and not args.published_disk:parser.error('this check runs the real launcher on the user\'s installed disk (out/desktop64.img); pass --published-disk to confirm')
if args.self_test:
    from urllib.error import HTTPError
    from urllib.request import Request,urlopen
    for button_name in ('left','wheel-down'):
        sent_events=[]
        def capture_input(name,arguments):sent_events.append((name,arguments))
        with patch('time.sleep',side_effect=lambda seconds:sent_events.append(('hold',seconds))),patch('time.perf_counter',side_effect=(1,1.042)):
            started,held=pointer_press(capture_input,button_name)
        assert started==1 and abs(held-42)<.001 and len(sent_events)==3 and sent_events[1]==('hold',.04)
        assert sent_events[0]==('input-send-event',{'events':[{'type':'btn','data':{'button':button_name,'down':True}}]})
        assert sent_events[2]==('input-send-event',{'events':[{'type':'btn','data':{'button':button_name,'down':False}}]})
    fixture=InteractionFixture()
    try:
        url=f'http://127.0.0.1:{fixture.server.server_port}'+fixture.path
        with urlopen(url,timeout=5) as response:assert response.status==200 and response.read()==fixture.html
        with urlopen(Request(url+'/event',data=b'{"event":"scroll","value":123}',headers={'Content-Type':'application/json'}),timeout=5) as response:assert response.status==204
        assert fixture.requests==1 and fixture.received('scroll')[0]['value']==123
        for payload in (b'{"event":"typed","value":"nexus"}',b' '*129):
            try:urlopen(Request(url+'/event',data=payload),timeout=5)
            except HTTPError as error:assert error.code==400;error.close()
            else:raise AssertionError('Local fixture accepted an invalid event')
        assert not fixture.received('typed')
    finally:fixture.stop()
    print('PASS document/fixture pixel gates and bounded local HTTP fixture; no VM');raise SystemExit(0)
WORK=args.work.resolve();WORK.mkdir(parents=True,exist_ok=True)
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
host=module('published_aurora',ROOT/'arch/x86_64/aurora.py');qmp=module('published_qmp',ROOT/'tests/qmp_local.py')
host.require_idle([DISK,OUT/'nexus64.img']);metadata=json.loads((OUT/'desktop64.json').read_text())
assert metadata.get('aurora',{}).get('verified') is True,'Verified Aurora publication required'
boot_hash=host.digest(OUT/'nexus64.img');assert boot_hash==metadata['boot_sha256']
base_hash=host.digest(DISK) if args.snapshot else None
protected={'sbin/init':metadata['desktop_sha256'],'bin/aurora-app':metadata['aurora_app_sha256'],
    **{metadata[name]['entry']:metadata[name]['entry_sha256'] for name in ('chrome','hello')}}
fs=host.fat.Fat32(DISK)
try:
    saved_args={name:host.read(fs,name) for name in ('nexus/desktop.args','nexus/chrome-desktop.args')}
    assert saved_args['nexus/desktop.args'].strip()==b'--aurora'
    lines=saved_args['nexus/chrome-desktop.args'].splitlines()
    assert lines[0]==metadata['chrome']['entry'].encode() and b'https://example.com/' in lines
    assert not any(b'--trace' in data or b'--remote-debugging' in data or b'--no-sandbox' in data for data in saved_args.values())
    for path,expected in protected.items():assert hashlib.sha256(host.read(fs,path)).hexdigest()==expected
finally:fs.f.close()
if (WORK/'result.json').exists():(WORK/'result.json').rename(WORK/('previous-result-'+str(time.time_ns())+'.json'))
serial=WORK/'serial.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(WORK);process=None;actual_command=None;fixture=None;interaction=None;failed=False

def log():
    value=serial.read_text() if serial.exists() else ''
    assert not any(marker in value for marker in ('USER_FAULT','KERNEL_FAULT','DESKTOP RESOURCE LEAK','[64] FAIL:')),value[-5000:]
    return value

def wait(pattern,after=0,seconds=180):
    until=time.monotonic()+seconds
    while time.monotonic()<until:
        value=log();match=re.search(pattern,value[after:])
        if match:return match
        assert process.poll() is None,value[-5000:]
        time.sleep(.1)
    raise TimeoutError(pattern)

def launch(command,**kwargs):
    global process,actual_command,fixture,interaction
    assert process is None and Path(command[0]).name=='qemu-system-x86_64.exe','Launcher attempted an unexpected build/helper'
    actual_command=[str(value) for value in command]
    assert 'name=opt/nexus/session,string=chrome' in actual_command if args.mode=='chrome' else '-fw_cfg' not in actual_command
    test_command=actual_command.copy();assert test_command[test_command.index('-display')+1]=='sdl'
    test_command[test_command.index('-display')+1]='none';test_command[test_command.index('-serial')+1]='file:'+str(serial)
    if args.snapshot:
        drives=[i for i,value in enumerate(test_command) if value.startswith('file='+str(DISK)+',') and 'if=ide' in value]
        assert len(drives)==1 and 'snapshot=' not in test_command[drives[0]]
        test_command[drives[0]]+=',snapshot=on'
    test_command+=['-qmp',control.argument]
    (WORK/'launch-command.json').write_text(json.dumps({'actual_launcher':actual_command,'observed_command':test_command},indent=2)+'\n')
    with (WORK/'qemu.log').open('wb') as output:process=subprocess.Popen(test_command,cwd=kwargs.get('cwd'),stdout=output,stderr=subprocess.STDOUT)
    (WORK/'vm.json').write_text(json.dumps({'pid':process.pid,'disk':str(DISK),'qmp':str(control.path),'snapshot':args.snapshot},indent=2)+'\n')
    (WORK/'qmp_path').write_text(str(control.path));print('PUBLISHED',args.mode,'VM',process.pid,flush=True)
    wait('INIT SESSION '+('chrome override' if args.mode=='chrome' else 'configured desktop'));wait('DESKTOP READY')
    with control.open() as monitor:
        command=monitor.command;position=[960,540]
        def key(name,down):
            command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]});time.sleep(.15)
        def tap(name):key(name,True);key(name,False)
        def close_window():command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}} for name,down in (('alt',True),('f4',True),('f4',False),('alt',False))]})
        def move_to(x,y):
            while position!=[x,y]:
                dx=max(-90,min(90,x-position[0]));dy=max(-90,min(90,y-position[1]));events=[]
                if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                position[0]+=dx;position[1]+=dy;command('input-send-event',{'events':events});time.sleep(.1)
        def click(x,y):
            move_to(x,y)
            for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.15)
        def chord(modifier,name):
            for code,down in ((modifier,True),(name,True),(name,False),(modifier,False)):key(code,down)
        def type_text(text):
            for character in text:
                code={':':'semicolon','.':'dot','/':'slash','-':'minus'}.get(character,character)
                assert character in 'abcdefghijklmnopqrstuvwxyz0123456789:./-'
                if character==':':key('shift',True)
                tap(code)
                if character==':':key('shift',False)
        def frame(name,predicate,seconds=300):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                log();command('screendump',{'filename':str(WORK/'frame.ppm')})
                with Image.open(WORK/'frame.ppm') as value:
                    picture=value.convert('RGB')
                    if predicate(picture):picture.save(WORK/(name+'.png'));return picture
                assert process.poll() is None,'VM exited before '+name
                time.sleep(.3)
            raise AssertionError('Framebuffer condition not met: '+name)
        if args.mode=='desktop':
            wait('AURORA READY persistent desktop')
            original=frame('published-aurora',lambda p:p.size==(1920,1080) and len(p.crop(PROOF).getcolors(1000000))>100)
            before=len(log());click(864,1040);pid=wait(r'AURORA LAUNCH app=chrome pid=(\d+)',before).group(1)
            placed=wait(r'AURORA WINDOW app=google-chrome [^\r\n]* x=(-?\d+) y=(-?\d+)',before,seconds=300);chrome_origin=tuple(map(int,placed.groups()))
        else:assert 'AURORA READY' not in log();chrome_origin=(0,0)
        frame('published-'+args.mode+'-https',document_visible)
        if args.interact:
            fixture=InteractionFixture();(WORK/'interaction-fixture.html').write_bytes(fixture.html)
            # Chrome elides the HTTP scheme for numeric hosts even when the loaded address is selected.
            typed_address=fixture.url.removeprefix('http://')
            chord('ctrl','l');type_text(typed_address);chord('ctrl','a')
            url_box=(chrome_origin[0]+159,chrome_origin[1]+52,min(1920,chrome_origin[0]+1000),chrome_origin[1]+76)
            command('screendump',{'filename':str(WORK/'frame.ppm')})
            with Image.open(WORK/'frame.ppm') as image:url_pixels=image.convert('RGB').crop(url_box)
            assert sum(count for count,color in url_pixels.getcolors(100000) if color[2]>color[0]+30)>300,'Typed URL is not visibly selected'
            url_pixels.save(WORK/'interaction-url-typed.png');tap('ret')
            rendered=frame('interaction-fixture-loaded',lambda p:fixture.requests>0 and bool(fixture.received('ready')) and fixture_origin(p) is not None)
            origin=fixture_origin(rendered);assert all(row['value']==1 for row in fixture.received('ready')),'Fixture requires native pixel scale'
            x,y=origin;assert y+650<1080 and x+800<1920,'Fixture controls and scroll proof must be visible'
            chord('ctrl','l')
            frame('interaction-loaded-url',lambda p:p.crop(url_box).tobytes()==url_pixels.tobytes(),seconds=30);tap('esc')
            def response(name,predicate,sent,seconds=10):
                deadline=time.perf_counter()+seconds;captures=0
                while time.perf_counter()<deadline:
                    log();command('screendump',{'filename':str(WORK/'frame.ppm')});captures+=1
                    with Image.open(WORK/'frame.ppm') as image:picture=image.convert('RGB')
                    if predicate(picture):
                        latency=(time.perf_counter()-sent)*1000;picture.save(WORK/(name+'.png'));return picture,{'sent_to_visible_ms':latency,'captures':captures}
                    assert process.poll() is None,'VM exited during browser interaction';time.sleep(.01)
                raise TimeoutError('Physical browser response: '+name)
            def receipt(event,seconds=10):
                deadline=time.monotonic()+seconds
                while time.monotonic()<deadline:
                    rows=fixture.received(event)
                    if rows:return rows
                    log();assert process.poll() is None,'VM exited before fixture event';time.sleep(.02)
                raise TimeoutError('Local browser event '+event)
            status_point=(x+730,y+124)
            assert rendered.getpixel(status_point)==(156,47,71)
            click(x+100,y+112);type_text('nexu');sent=time.perf_counter()
            command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':'s'}}} for down in (True,False)]})
            typed,typing=response('interaction-typed-nexus',lambda p:p.getpixel(status_point)==(44,163,108),sent);receipt('typed')
            move_to(x+380,y+112);sent,held=pointer_press(command,'left')
            confirmed,button=response('interaction-button-confirmed',lambda p:p.getpixel(status_point)==(103,81,200),sent);receipt('clicked')
            button.update(minimum_hold_ms=40,press_to_release_ms=held)
            body_box=(x+80,y+210,min(x+800,1800),y+650);before_body=confirmed.crop(body_box).tobytes()
            move_to(min(x+900,1850),y+280);sent,held=pointer_press(command,'wheel-down')
            scrolled,scroll=response('interaction-wheel-scrolled',lambda p:p.crop(body_box).tobytes()!=before_body,sent)
            scroll.update(minimum_hold_ms=40,press_to_release_ms=held)
            positions=receipt('scroll');assert max(row['value'] for row in positions)>0,'Wheel input did not move the real document'
            interaction={'url':fixture.url,'typed_address':typed_address,'http_fixture_received':True,'loaded_url_pixels_verified':True,'rendered_fixture_verified':True,
                'physical_text':'nexus','typing':typing,'button':button,'wheel':scroll,'scroll_y':max(row['value'] for row in positions),
                'body_pixels_changed_outside_cursor':True,'fixture_sha256':hashlib.sha256(fixture.html).hexdigest(),
                'measurement':'Physical input to observed browser pixels, including QMP capture overhead and the recorded pointer hold before release; normal page JavaScript reports only fixed test events and scrollY to the local server. No inspector or browser flags.'}
            (WORK/'interaction-result.json').write_text(json.dumps(interaction,indent=2)+'\n')
        before=len(log());close_window()
        if args.mode=='desktop':
            wait('AURORA CHILD EXIT pid='+pid+' status=0',before)
            frame('published-return-to-aurora',lambda p:p.crop(PROOF).tobytes()==original.crop(PROOF).tobytes())
            assert process.poll() is None,'Closing Chrome terminated Aurora'
            click(16,14)
            for _ in range(3):tap('down')
            command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':'ret'}}} for down in (True,False)]})
    process.wait(timeout=120);value=log()
    assert process.returncode==33 and 'DESKTOP EXIT status=0' in value and 'INIT SESSION RETURNED' in value,value[-5000:]
    return subprocess.CompletedProcess(actual_command,process.returncode)

try:
    with patch('sys.argv',[str(ROOT/'arch/x86_64/build.py'),args.mode]),patch('subprocess.run',side_effect=launch):
        runpy.run_path(str(ROOT/'arch/x86_64/build.py'),run_name='__main__')
    host.require_idle([DISK,OUT/'nexus64.img']);fs=host.fat.Fat32(DISK)
    try:
        for path,data in saved_args.items():assert host.read(fs,path)==data,'Launcher modified persistent arguments'
        for path,expected in protected.items():assert hashlib.sha256(host.read(fs,path)).hexdigest()==expected
    finally:fs.f.close()
    assert host.digest(OUT/'nexus64.img')==boot_hash
    if args.snapshot:assert host.digest(DISK)==base_hash,'Disposable smoke changed the base disk'
    (WORK/'result.json').write_text(json.dumps({'mode':args.mode,'actual_launcher_verified':True,'disk':str(DISK),'no_clone_build_or_staging':True,
        'normal_arguments_preserved':True,'public_document_pixels_verified':True,'chrome_close_returns_to_aurora':args.mode=='desktop',
        'normal_shutdown':True,'exit_code':33,'boot_sha256':boot_hash,'protected_guest_hashes':protected,
        'snapshot':args.snapshot,'base_sha256':base_hash,'base_image_unchanged':True if args.snapshot else None,'interaction':interaction},indent=2)+'\n')
    print('PASS published',args.mode,'actual launch, document pixels and normal shutdown; persistent arguments/apps preserved',flush=True)
except BaseException as error:
    failed=True
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'disk':str(DISK),'pid':process.pid if process else None,
        'live':process is not None and process.poll() is None,'qmp':str(control.path),'persistent_image_retained':True},indent=2)+'\n');raise
finally:
    if process is not None:
        if args.snapshot and process.poll() is None:
            try:
                with control.open() as monitor:monitor.command('quit')
                process.wait(timeout=5)
            except Exception:process.kill();process.wait()
        if process.poll() is not None:control.cleanup(process)
        else:print('Persistent VM retained for safe diagnosis:',process.pid,str(control.path),flush=True)
    if fixture:fixture.stop()
    if args.snapshot:
        assert host.digest(DISK)==base_hash and host.digest(OUT/'nexus64.img')==boot_hash,'Base image changed during snapshot cleanup'
        if failed:
            failure=json.loads((WORK/'failure.json').read_text())
            failure.update(live_after_cleanup=process is not None and process.poll() is None,base_images_unchanged_after_cleanup=True)
            (WORK/'failure.json').write_text(json.dumps(failure,indent=2)+'\n')

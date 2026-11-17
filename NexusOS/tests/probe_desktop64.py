"""Development probe for graphical Chrome on our native Wayland compositor.

Keeps serial/QMP artifacts; only a verified download retains its private disk.
A screenshot alone is not a PASS.
client/subsurfaces/popups may use --reuse-disk=<NEXUS_RUNS>\\...\\disk.img (a disk under the test runs folder);
that existing private image is retained on success or failure, with test args staged.
"""
from pathlib import Path
import importlib.util
import hashlib
import html
import http.server
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from urllib.parse import quote
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
REUSE_ROOT = nexus_paths.runs('.')

def reuse_disk(path,mode):
    if mode not in ('client','subsurfaces','popups'):raise ValueError('Disk reuse is only allowed for client/subsurfaces/popups')
    requested=Path(path)
    if any(c in str(requested) for c in (',','\r','\n','\0')):raise ValueError('Reused disk path contains a QEMU option separator or control character')
    if not requested.is_absolute():raise ValueError('Reused disk path must be absolute')
    selected=requested.resolve(strict=True)
    if not selected.is_relative_to(REUSE_ROOT.resolve()) or not selected.is_file():raise ValueError('Reused disk must be a file within the test runs folder %s' % REUSE_ROOT.resolve())
    for protected in (OUT/'desktop64.img',OUT/'chrome64.img',OUT/'apps64.img',ROOT/'disk.img'):
        if selected==protected.resolve() or (protected.exists() and selected.samefile(protected)):
            raise ValueError('Refusing to reuse a permanent/source disk or its alias')
    live=subprocess.check_output(['powershell','-NoProfile','-Command',
        "Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu%'\" | Select-Object -ExpandProperty ProcessId"],text=True)
    if live.strip():raise RuntimeError('Stop existing QEMU VMs before reusing a test disk')
    return selected

if sys.argv[1:]==['--self-test-reuse']:
    import tempfile
    from unittest.mock import patch
    with tempfile.TemporaryDirectory(prefix='desktop-reuse-guard-',dir=REUSE_ROOT) as directory:
        work=Path(directory).resolve();assert work.is_relative_to(REUSE_ROOT.resolve()) and not work.is_symlink()
        candidate=work/'fixture.bin';candidate.write_bytes(b'guard fixture')
        comma=work/'comma,name.bin';comma.write_bytes(b'guard fixture')
        with patch('subprocess.check_output',return_value=''):
            for mode in ('client','subsurfaces','popups'):assert reuse_disk(candidate,mode)==candidate
            for path,mode in ((candidate,'download'),(ROOT/'docs'/'DEVELOPMENT_NOTES.md','client'),(work,'client'),(Path('relative.img'),'client'),(work/'missing.img','client'),(comma,'client')):
                try:reuse_disk(path,mode)
                except (ValueError,FileNotFoundError):pass
                else:raise AssertionError('Unsafe reuse accepted: '+str(path))
            with patch.object(Path,'samefile',return_value=True),patch.object(Path,'exists',return_value=True):
                try:reuse_disk(candidate,'client')
                except ValueError:pass
                else:raise AssertionError('Permanent-image alias accepted')
        with patch('subprocess.check_output',return_value='1234\n'):
            try:reuse_disk(candidate,'client')
            except RuntimeError:pass
            else:raise AssertionError('Live QEMU accepted')
    print('PASS reuse guard: allowed modes, containment, existence, source aliases and live-QEMU refusal; no VM/image access')
    raise SystemExit(0)

MODE = sys.argv[2] if len(sys.argv) > 2 else 'gpu'
INSTALLED = MODE in ('installed-network','installed-https','installed-download')
if INSTALLED:MODE=MODE.removeprefix('installed-')
if MODE not in ('gpu','software','trace','client','headless','subsurfaces','popups','inspect','startup','interactive','network','https','https-reject','download'): raise ValueError('Unknown desktop probe mode: '+MODE)
interactive = MODE in ('interactive','network','download')
EXTRA=sys.argv[3:]
reuse_args=[arg for arg in EXTRA if arg=='--reuse-disk' or arg.startswith('--reuse-disk=')]
if len(reuse_args)>1 or (reuse_args and (not reuse_args[0].startswith('--reuse-disk=') or not reuse_args[0].split('=',1)[1])):raise ValueError('Use one --reuse-disk=<absolute path> option')
REUSE=reuse_disk(reuse_args[0].split('=',1)[1],MODE) if reuse_args else None
EXTRA=[arg for arg in EXTRA if arg not in reuse_args]
if any(not arg.startswith('--') or '\n' in arg or '\r' in arg for arg in EXTRA):raise ValueError('Extra Chrome arguments must be individual switches')
if MODE in ('https','https-reject','download') and any(arg!='--ipc-connection-timeout=120' for arg in EXTRA):raise ValueError('HTTPS verification only allows the existing IPC timeout accommodation')
WORK = Path(sys.argv[1]) if len(sys.argv) > 1 else nexus_paths.runs('native64-desktop')
WORK.mkdir(parents=True, exist_ok=True)
disk = REUSE or WORK / 'disk.img'
chrome_entry='/opt/google/chrome/chrome'
if INSTALLED:
    installed=json.loads((WORK/'installation-result.json').read_text())
    assert installed['entry']=='/home/nexus/Applications/chrome/opt/google/chrome/chrome'
    assert installed['package']==json.loads((ROOT/'arch/x86_64/download-lock.json').read_text())
    assert installed.get('bootstrap_executable_removed') is True,'retire the verified bootstrap duplicate before testing the installed Chrome'
    disk=Path(installed.get('disk',str(disk)))
    chrome_entry=installed['entry'];assert disk.is_file()
    WORK=WORK/('installed-'+MODE);WORK.mkdir(exist_ok=True)
elif not REUSE:
    if disk.exists(): raise FileExistsError('Refusing to overwrite an existing test disk: '+str(disk))
keep_disk = INSTALLED or REUSE is not None
process = control = serial = server = server_thread = None
requests = []; tls_errors = []
try:
    if not INSTALLED and not REUSE: shutil.copyfile(OUT / 'chrome64.img', disk)
    page = '<!doctype html><title>NexusOS interactive test</title><style>body{background:#0b1220;color:white;font:28px sans-serif;margin:40px}input{font:28px sans-serif}</style><h1>Chrome on NexusOS</h1><p>Native window and input test</p><input id="text" placeholder="Type here"><button onclick="document.body.style.background=\'#18c784\'">Click test</button>'
    arguments = WORK / 'desktop.args'
    software = MODE in ('software','trace','inspect','startup','interactive','network','https','https-reject','download')
    page_url = 'data:text/html,' + quote(page,safe='')
    package = None
    if MODE=='download':
        package=json.loads((ROOT/'arch/x86_64'/('hello-download-lock.json' if INSTALLED else 'download-lock.json')).read_text())
        page+='<p><a id="download" href="'+html.escape(package['url'],quote=True)+'">Download '+html.escape(package['name'])+'</a></p>'
        (WORK/'download.expect').write_text(f'{package["filename"]}\n{package["bytes"]}\n{package["sha256"]}\n')
    if MODE in ('network','https-reject','download'):
        page = page.replace('Native window and input test','Native TCP / HTTP page and input test')
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.path)
                if self.path != '/nexus.html': self.send_error(404); return
                body = page.encode(); self.send_response(200)
                self.send_header('Content-Type','text/html; charset=utf-8'); self.send_header('Content-Length',str(len(body)))
                self.send_header('Connection','close'); self.end_headers(); self.wfile.write(body)
            def log_message(self,format,*args):
                with (WORK/'http.log').open('a') as log: log.write(format % args + '\n')
        if MODE=='https-reject':
            import datetime,ipaddress,ssl
            from cryptography import x509
            from cryptography.x509.oid import NameOID
            from cryptography.hazmat.primitives import hashes,serialization
            from cryptography.hazmat.primitives.asymmetric import ec
            key=ec.generate_private_key(ec.SECP256R1());now=datetime.datetime.now(datetime.timezone.utc)
            name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'NexusOS local test')])
            certificate=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
                .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(days=1)).not_valid_after(now+datetime.timedelta(days=7))
                .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('10.0.2.2'))]),critical=False)
                .add_extension(x509.BasicConstraints(ca=False,path_length=None),critical=True).sign(key,hashes.SHA256()))
            (WORK/'test-key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
            (WORK/'test-cert.pem').write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
            context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);context.load_cert_chain(WORK/'test-cert.pem',WORK/'test-key.pem');context.set_alpn_protocols(['http/1.1'])
            class TLSHost(http.server.ThreadingHTTPServer):
                def get_request(self):
                    peer,address=super().get_request();peer.settimeout(30)
                    try:return context.wrap_socket(peer,server_side=True),address
                    except ssl.SSLError as error:
                        tls_errors.append(str(error));peer.close();raise
            server=TLSHost(('127.0.0.1',0),Handler)
        else:server = http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
        server_thread=threading.Thread(target=server.serve_forever,daemon=True);server_thread.start()
        page_url = f'{"https" if MODE=="https-reject" else "http"}://10.0.2.2:{server.server_port}/nexus.html'
    if MODE=='https':page_url='https://example.com/'
    arguments.write_text(chrome_entry+'\n--ozone-platform=wayland\n--enable-logging=stderr\n--no-first-run\n--user-data-dir=/home/nexus/'+('chrome-installed-profile' if INSTALLED else 'chrome-desktop-profile')+'\n' + ('--disable-gpu\n' if software else '') + page_url + '\n')
    client = MODE == 'client'
    if client: arguments.write_text('/bin/desktop-test\n')
    subsurfaces = MODE == 'subsurfaces'
    if subsurfaces: arguments.write_text('/bin/subsurface-test\n')
    popups = MODE == 'popups'
    if popups:arguments.write_text('/bin/popup-test\n')
    headless = MODE == 'headless'
    if headless:
        document = '<!doctype html><title>Native child test</title><main id="result">pending</main><script>document.getElementById("result").textContent="NEXUS_CHILD_JS_PASS:"+(123*456)</script>'
        arguments.write_text('/opt/google/chrome/chrome\n--headless\n--dump-dom\n--enable-logging=stderr\n--user-data-dir=/home/nexus/chrome-desktop-profile\n' + 'data:text/html,' + quote(document,safe='') + '\n')
    if EXTRA:arguments.write_text(arguments.read_text()+''.join(arg+'\n' for arg in EXTRA))
    if MODE == 'trace': arguments.write_text('--trace\n' + arguments.read_text())
    if MODE == 'inspect': arguments.write_text('/bin/chrome-inspect\n' + arguments.read_text())
    if MODE == 'startup': arguments.write_text('/bin/chrome-inspect\n--startup\n' + arguments.read_text())
    if MODE == 'https': arguments.write_text('/bin/chrome-inspect\n--https\n' + arguments.read_text())
    if MODE == 'https-reject': arguments.write_text('/bin/chrome-inspect\n--reject-certificate\n' + arguments.read_text())
    if interactive: arguments.write_text('--trace\n/bin/chrome-inspect\n'+('--download' if MODE=='download' else '--interactive')+'\n' + arguments.read_text())
    spec=importlib.util.spec_from_file_location('desktop_fatput',ROOT/'userland/fatput.py');fatput=importlib.util.module_from_spec(spec);spec.loader.exec_module(fatput)
    stage = fatput.main if INSTALLED else fatput.edit
    assert stage(['fatput',str(disk),'put',str(OUT/'desktop.elf'),'sbin/init','put',str(arguments),'nexus/desktop.args',
                *[item for name in ('resolv.conf','nsswitch.conf','hosts') for item in ('put',str(ROOT/'arch/x86_64/etc'/name),'etc/'+name)],
                *(['put',str(WORK/'download.expect'),'nexus/download.expect'] if MODE=='download' else []),
                *(['put',str(OUT/'desktop_user.elf'),'bin/desktop-test'] if client else [])]) == 0
    if subsurfaces: assert stage(['fatput',str(disk),'put',str(OUT/'subsurface_user.elf'),'bin/subsurface-test']) == 0
    if popups:assert stage(['fatput',str(disk),'put',str(OUT/'popup_user.elf'),'bin/popup-test']) == 0
    if MODE in ('inspect','startup','interactive','network','https','https-reject','download'): assert stage(['fatput',str(disk),'put',str(OUT/'inspect_user.elf'),'bin/chrome-inspect']) == 0
    spec=importlib.util.spec_from_file_location('qmp_local',ROOT/'tests/qmp_local.py');qmp_local=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp_local)
    control=qmp_local.QmpMonitor(WORK)
    (WORK/'qmp_path').write_text(str(control.path))
    (WORK/'qmp_port').unlink(missing_ok=True)
    serial=WORK/'serial.log';serial.unlink(missing_ok=True)
    process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}',
        '-qmp',control.argument,'-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4',
        *(['-netdev','user,id=network','-device','rtl8139,netdev=network','-object',f'filter-dump,id=capture,netdev=network,file={WORK/"network.pcap"}'] if MODE in ('network','https','https-reject','download') else [])],
        stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    print('DESKTOP VM',process.pid,'QMP',control.path,flush=True)
    if client or subsurfaces or popups or interactive:
        def wait_marker(marker,after=0):
            until=time.monotonic()+(360 if interactive and marker=='CDP UI LAYOUT' else 120 if popups else 20)
            while time.monotonic()<until:
                if serial.exists() and marker in serial.read_text()[after:]: return
                if process.poll() is not None: raise AssertionError(serial.read_text())
                time.sleep(.03)
            raise AssertionError('Missing '+marker)
        wait_marker('CDP UI LAYOUT' if interactive else '[64] POPUP INPUT READY' if popups else '[64] SUBSURFACE INPUT READY' if subsurfaces else '[64] DESKTOP BUFFER')
        with control.open() as monitor:
            qmp=monitor.command
            def key(name,down):
                qmp('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}}]})
            if interactive:
                layout=json.loads(next(line.split('CDP UI LAYOUT ',1)[1] for line in serial.read_text().splitlines() if line.startswith('[64] CDP UI LAYOUT ')))
                assert layout['dpr']==1 and layout['screen']==[0,0],layout
                screen_w,screen_h=layout['outer'];assert screen_w>0 and screen_h>=layout['viewport'][1],layout
                top=screen_h-layout['viewport'][1];position=[screen_w//2,screen_h//2]
                def received(pattern,after,seconds=15):
                    until=time.monotonic()+seconds
                    while time.monotonic()<until:
                        if re.search(pattern,serial.read_text()[after:]):return
                        if process.poll() is not None:raise AssertionError(serial.read_text()[-3000:])
                        time.sleep(.02)
                    raise AssertionError('Required event not received: '+pattern)
                def move_chrome(target):
                    x,y=target[0],target[1]+top;assert 0<=x<screen_w and 0<=y<screen_h,(x,y,layout)
                    while position!=[x,y]:
                        dx=max(-80,min(80,x-position[0]));dy=max(-80,min(80,y-position[1]));events=[]
                        if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                        if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                        position[0]+=dx;position[1]+=dy;before=len(serial.read_text());qmp('input-send-event',{'events':events})
                        received(r'wl_pointer#\d+\.motion\(\d+, '+re.escape(f'{position[0]:.8f}, {position[1]:.8f})'),before)
                def click_chrome():
                    for down in (True,False):
                        before=len(serial.read_text());qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
                        received(r'wl_pointer#\d+\.button\(\d+, \d+, 272, '+str(int(down))+r'\)',before)
                received(r'DESKTOP BUFFER surface=\d+ '+str(screen_w)+'x'+str(screen_h),0,120)
                move_chrome(layout['input']);click_chrome();wait_marker('CDP UI INPUT FOCUSED')
                for name,code in [('n',49),('e',18),('x',45),('u',22),('s',31)]:
                    for down in (True,False):
                        before=len(serial.read_text());key(name,down)
                        received(r'wl_keyboard#\d+\.key\(\d+, \d+, '+str(code)+', '+str(int(down))+r'\)',before)
                wait_marker('CDP UI TEXT VERIFIED');move_chrome(layout['button']);click_chrome();wait_marker('CDP USER INTERACTION VERIFIED')
                until=time.monotonic()+5
                while time.monotonic()<until:
                    qmp('screendump',{'filename':str(WORK/'chrome-interactive.ppm')})
                    with Image.open(WORK/'chrome-interactive.ppm') as image:
                        if image.getpixel((screen_w-100,screen_h-100))==(24,199,132):image.save(WORK/'chrome-interactive.png');break
                    time.sleep(.1)
                else:raise AssertionError('Chrome click changed DOM but not the actual displayed background')
                if MODE=='download':
                    wait_marker('CDP DOWNLOAD READY')
                    download=json.loads(next(line.split('CDP DOWNLOAD READY ',1)[1] for line in serial.read_text().splitlines() if line.startswith('[64] CDP DOWNLOAD READY ')))
                    assert download['url']==package['url'],download
                    # Use a new tab: the HTTP input fixture is an insecure download initiator.
                    def typed_key(name,code,down):
                        before=len(serial.read_text());key(name,down)
                        received(r'wl_keyboard#\d+\.key\(\d+, \d+, '+str(code)+', '+str(int(down))+r'\)',before)
                    for name,code,down in [('ctrl',29,True),('t',20,True),('t',20,False),('ctrl',29,False)]:typed_key(name,code,down)
                    codes=dict(zip('qwertyuiopasdfghjklzxcvbnm',[16,17,18,19,20,21,22,23,24,25,30,31,32,33,34,35,36,37,38,44,45,46,47,48,49,50]))
                    codes.update(dict(zip('1234567890',range(2,12))));codes.update({'dot':52,'slash':53,'minus':12,'semicolon':39})
                    for character in package['url']:
                        name={'.':'dot','/':'slash','-':'minus',':':'semicolon','_':'minus'}.get(character,character)
                        shifted=character in ':_'
                        if shifted:typed_key('shift',42,True)
                        typed_key(name,codes[name],True);typed_key(name,codes[name],False)
                        if shifted:typed_key('shift',42,False)
                    typed_key('ret',28,True);typed_key('ret',28,False)
                    (WORK/'download-input.json').write_text(json.dumps({'method':'physical PS/2 omnibox typing','url':package['url']},indent=2)+'\n')
            elif popups:
                position=list(map(int,re.search(r'POPUP INPUT READY (\d+) (\d+)',serial.read_text()).groups()))
                def move_popup(x,y,inside=False):
                    while position!=[x,y]:
                        dx=max(-100,min(100,x-position[0]));dy=max(-100,min(100,y-position[1]));events=[]
                        if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                        if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                        position[0]+=dx;position[1]+=dy;before=len(serial.read_text());qmp('input-send-event',{'events':events})
                        offset=105 if inside else 0
                        wait_marker(f'POPUP POINTER {int(inside)} {position[0]-offset} {position[1]-offset}',before)
                def click_popup():
                    for down in (True,False):qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
                move_popup(100,100);click_popup();wait_marker('POPUP GRAB MAPPED')
                move_popup(120,120,True);click_popup();wait_marker('POPUP CLICK PASSED')
                key('a',True);key('a',False);wait_marker('POPUP NESTED MAPPED')
                qmp('screendump',{'filename':str(WORK/'popups.ppm')})
                with Image.open(WORK/'popups.ppm') as picture:picture.save(WORK/'popups.png')
                key('b',True);key('b',False);wait_marker('POPUP NESTED KEY PASSED')
                key('esc',True);key('esc',False);wait_marker('POPUP NESTED DISMISSED')
                move_popup(500,500);click_popup();wait_marker('POPUP OUTSIDE DISMISSED')
                key('c',True);key('c',False)
                wait_marker('POPUP UNGRABBED READY')
                move_popup(220,220);move_popup(120,120,True);click_popup();wait_marker('POPUP UNGRABBED CLICKED')
                key('d',True);key('d',False)
            elif client:
                key('a',True);key('a',False);wait_marker('WAYLAND CLIENT KEY PASSED')
                wait_marker('WAYLAND CLIENT CLIPBOARD PASSED')
                for down in (True,False):qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]})
                wait_marker('WAYLAND CLIENT POINTER PASSED')
                until=time.monotonic()+5
                while time.monotonic()<until:
                    qmp('screendump',{'filename':str(WORK/'interactive.ppm')})
                    with Image.open(WORK/'interactive.ppm') as image:
                        if image.getpixel((100,100))==(24,199,132) and image.getpixel((100,300))==(40,120,221):
                            image.save(WORK/'interactive.png');break
                    time.sleep(.05)
                else:raise AssertionError('Wayland input did not update actual framebuffer pixels')
                for name,down in [('alt',True),('f4',True),('f4',False),('alt',False)]:key(name,down)
            else:
                position=list(map(int,re.search(r'SUBSURFACE INPUT READY (\d+) (\d+)',serial.read_text()).groups()))
                def move(x,y,grab=False,hole=False):
                    while position!=[x,y]:
                        dx=max(-100,min(100,x-position[0]));dy=max(-100,min(100,y-position[1]));events=[]
                        if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
                        if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
                        position[0]+=dx;position[1]+=dy;px,py=position
                        kind=2 if grab or (not hole and 90<=px<122 and 90<=py<122) else 1 if not hole and 80<=px<144 and 80<=py<144 else 0
                        offset=90 if kind==2 else 80 if kind==1 else 0
                        before=len(serial.read_text());qmp('input-send-event',{'events':events})
                        wait_marker(f'SUBSURFACE POINTER {kind} {px-offset} {py-offset}',before)
                move(100,100)
                qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]});wait_marker('SUBSURFACE GRAB READY')
                move(400,300,grab=True);key('a',True);key('a',False);wait_marker('SUBSURFACE KEYBOARD PASSED')
                qmp('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]});wait_marker('SUBSURFACE GRAB PASSED')
                wait_marker('SUBSURFACE REGION READY');move(100,100,hole=True)
    deadline=time.monotonic()+(1200 if MODE=='download' else 600)
    diagnostic_seen={}
    while process.poll() is None and time.monotonic()<deadline:
        if MODE in ('inspect','startup','https','https-reject') and serial.exists():
            log=serial.read_text()
            for marker,name in [('CDP RENDER DIAGNOSTIC READY','renderer-diagnostic'),('CDP NAVIGATION VERIFIED','navigation'),('CDP STARTUP NAVIGATION VERIFIED','startup'),('CDP HTTPS NAVIGATION VERIFIED','https'),('CDP UNTRUSTED CERTIFICATE REJECTED','certificate-rejected')]:
                if marker not in log or diagnostic_seen.get(name) is False:continue
                if name not in diagnostic_seen:diagnostic_seen[name]=time.monotonic()
                if time.monotonic()-diagnostic_seen[name]<3:continue
                with control.open() as monitor:
                    monitor.command('screendump',{'filename':str(WORK/(name+'.ppm'))})
                with Image.open(WORK/(name+'.ppm')) as image:
                    if name=='certificate-rejected':
                        body=image.convert('RGB').crop((0,100,image.width,image.height))
                        red=sum(count for count,(r,g,b) in body.getcolors(body.width*body.height) if r>170 and g<120 and b<120)
                        if red<500:continue
                    image.save(WORK/(name+'.png'))
                diagnostic_seen[name]=False
        time.sleep(.2)
    if process.poll() is None: raise TimeoutError('Desktop probe observation deadline; interactive completion is unproven')
    print('Desktop VM exit:',process.returncode,flush=True)
    print(serial.read_text()[-16000:],flush=True)
    if process.returncode !=33: raise AssertionError('Desktop startup failed')
    if popups:
        log=serial.read_text()
        assert 'POPUP ALL PASSED' in log and 'INIT SESSION RETURNED' in log
        assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log
        print('PASS native popups: actual geometry/pixels, protocol error isolation, nested keyboard/pointer grabs and dismissal',flush=True)
    if client:
        log=serial.read_text()
        assert '[64] WAYLAND CLIENT ALL PASSED:' in log and '[64] INIT SESSION RETURNED' in log,log
        assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log,log
        print('PASS native Wayland configure/SHM/input/close, actual pixels and process cleanup',flush=True)
    if subsurfaces:
        log=serial.read_text()
        assert '[64] SUBSURFACE ALL PASSED:' in log and '[64] INIT SESSION RETURNED' in log,log
        assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log and 'DESKTOP RESOURCE LEAK' not in log,log
        print('PASS native subsurface state, real framebuffer pixels, error isolation and cleanup',flush=True)
    if headless and '<main id="result">NEXUS_CHILD_JS_PASS:56088</main>' not in serial.read_text():
        raise AssertionError('Child Chrome did not execute the page')
    if MODE=='https':
        log=serial.read_text()
        assert 'CDP HTTPS NAVIGATION VERIFIED' in log and 'CDP DIAGNOSTIC DONE responses=complete child_status=0' in log,log
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log,log
        assert diagnostic_seen.get('https') is False and (WORK/'https.png').is_file(),'HTTPS page framebuffer not captured in this run'
        print('PASS graphical Chrome: public HTTPS hostname, original secure document, normal certificate validation and shutdown',flush=True)
    if MODE=='https-reject':
        log=serial.read_text()
        assert 'CDP UNTRUSTED CERTIFICATE REJECTED' in log and 'CDP DIAGNOSTIC DONE responses=complete child_status=0' in log,log[-12000:]
        assert not requests and any('CERTIFICATE' in error or 'UNKNOWN_CA' in error for error in tls_errors),(requests,tls_errors)
        assert diagnostic_seen.get('certificate-rejected') is False and (WORK/'certificate-rejected.png').is_file(),'certificate warning was not painted on the framebuffer'
        assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log,log[-12000:]
        print('PASS Chrome rejected the untrusted certificate: actual TLS alert, visible certificate error and no HTTP request',flush=True)
    if interactive:
        log=serial.read_text()
        assert 'CDP STARTUP NAVIGATION VERIFIED' in log and 'CDP USER INTERACTION VERIFIED' in log and 'CDP DIAGNOSTIC DONE responses=complete child_status=0' in log,log
        assert 'KERNEL_FAULT' not in log and 'DESKTOP RESOURCE LEAK' not in log,log
        if MODE=='network':
            assert '/nexus.html' in requests,requests
            replies=[json.loads(line.split('CDP RECV ',1)[1]) for line in log.splitlines() if line.startswith('[64] CDP RECV ')]
            assert any(reply.get('result',{}).get('result',{}).get('value',{}).get('url')==page_url
                       for reply in replies if isinstance(reply.get('result',{}).get('result',{}).get('value'),dict)), 'HTTP document URL not verified'
            print('PASS Chrome fetched the HTTP document through native AF_INET/TCP/RTL8139',flush=True)
        print('PASS real graphical Chrome: original page, PS/2 typing/click, DOM/pixels and normal shutdown; network/install gates remain',flush=True)
    if MODE=='download':
        log=serial.read_text()
        assert 'CHROME DOWNLOAD VERIFIED' in log,log[-12000:]
        fs=fatput.Fat32(disk)
        try:
            directory=fs.lookup_dir(['home','nexus','Downloads']);entry=fs.find_name(directory,package['filename'])
            assert entry is not None,'download not persisted in FAT'
            record=entry[2];first=int.from_bytes(record[20:22],'little')<<16|int.from_bytes(record[26:28],'little')
            size=int.from_bytes(record[28:32],'little');assert size==package['bytes'],size
            actual=hashlib.sha256(fs.read_chain(first,size)).hexdigest();assert actual==package['sha256'],actual
        finally:fs.close()
        (WORK/'download-result.json').write_text(json.dumps({'package':package,'guest_path':'/home/nexus/Downloads/'+package['filename'],'readback_sha256':actual},indent=2)+'\n')
        keep_disk = True  # The next native installer step needs the downloaded guest file.
        assert 'USER_FAULT' not in log,'Download bytes verified, but a browser subprocess faulted; inspect serial.log'
        print('PASS Chrome downloaded the official HTTPS URL via real PS/2 typing; guest SHA-256/fsync and persisted disk readback match the Google package digest',flush=True)
finally:
    if process is not None and process.poll() is None: process.kill();process.wait()
    # A completed guest download is unique work even if a later browser/readback check fails.
    if process is not None and MODE=='download' and serial.exists() and any(line.startswith('[64] CHROME DOWNLOAD VERIFIED ') for line in serial.read_text().splitlines()):keep_disk=True
    if not keep_disk: disk.unlink(missing_ok=True)
    if control is not None and process is not None: control.cleanup(process)
    if MODE=='https-reject':(WORK/'tls-result.json').write_text(json.dumps({'errors':tls_errors,'requests':requests},indent=2))
    if server:
        if server_thread is not None and server_thread.is_alive(): server.shutdown()
        server.server_close()

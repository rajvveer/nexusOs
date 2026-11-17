"""Physical relative mouse input -> actual cursor pixels, on a disposable QEMU snapshot."""
from pathlib import Path
import argparse
import ctypes
from collections import Counter
import hashlib
import importlib.util
import json
import math
import re
import statistics
import struct
import subprocess
import sys
import time
from PIL import Image,ImageChops
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('work',type=Path)
parser.add_argument('--disk',type=Path,default=None,help='a desktop disk copy (default NEXUS_TEST_SOURCE; never the user\'s disk)')
parser.add_argument('--boot',type=Path,default=OUT/'nexus64.img',help='Read-only boot image, including a frozen baseline or candidate')
parser.add_argument('--accel',choices=('tcg','whpx'),default='tcg',help='Test-only accelerator; WHPX uses the validated conservative CPU model')
parser.add_argument('--chrome',action='store_true',help='Measure over the real public HTTPS page after launching Chrome through the dock')
parser.add_argument('--startup',action='store_true',help='Also time Chrome launch and probe actual cursor response while Chrome loads')
parser.add_argument('--sample-pc',action='store_true',help='Profile only RIP/CPL and verified physical allocator counters while waiting for Chrome startup')
parser.add_argument('--kernel-elf',type=Path,help='Derive allowed allocator symbols from this ELF after matching its flat image to --boot')
parser.add_argument('--self-test',action='store_true');args=parser.parse_args()
if args.startup and not args.chrome:parser.error('--startup requires --chrome')
if args.sample_pc and not args.startup:parser.error('--sample-pc requires --startup')
if args.kernel_elf and not args.sample_pc:parser.error('--kernel-elf requires --sample-pc')
WORK=args.work.resolve();WORK.mkdir(parents=True,exist_ok=True);disk=nexus_paths.refuse_user_disk((args.disk or nexus_paths.source_disk()).resolve());boot=args.boot.resolve()
acceleration_options={'tcg':['-cpu','max'],'whpx':['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand']}
cpu_model='max' if args.accel=='tcg' else 'qemu64,+rdrand'
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value
host=module('latency_host',ROOT/'arch/x86_64/aurora.py');qmp=module('latency_qmp',ROOT/'tests/qmp_local.py')
font=(ROOT/'kernel/uifont.h').read_text(encoding='utf-8')
pixels=[int(value,16) for value in re.findall(r'0x[0-9A-Fa-f]+',re.search(r'uf_cursor\[520\]\s*=\s*\{([^}]+)\}',font).group(1))]
assert len(pixels)==520
opaque=[(i%20-2,i//20-2,((p>>16)&255,(p>>8)&255,p&255)) for i,p in enumerate(pixels) if p>>24==255]
assert len(opaque)>20 and any(color==(255,255,255) for _,_,color in opaque)
def cursor_at(picture,point):return all(picture.getpixel((point[0]+x,point[1]+y))==color for x,y,color in opaque)
def cursor_box(point):return (point[0]-2,point[1]-2,point[0]+18,point[1]+24)

PROFILE_BOOT_SHA='05443cbddcf01061ebef9699d118450cd0a96adee533eb0bdf7dcb97fe6ad6e3'
# Verified OBJECT symbols in the kernel matching PROFILE_BOOT_SHA; no other physical memory is read.
PROFILE_ALLOCATED=0x184aa0;PROFILE_PAGE_COUNT=0x184a88;PROFILE_FIRST=0x184a80;PROFILE_LAST=0x184a78
PROFILE_SIZES={'allocated':8,'page_count':4,'reserved':4,'first_address':8,'last_address':8}
kernel_build=module('latency_kernel_build',ROOT/'arch/x86_64/build.py')
def profile_layout(elf,boot_data,symbol_text):
    flat=kernel_build.flat_kernel(elf)
    if boot_data[2560:2560+len(flat)]!=flat:raise ValueError('Kernel ELF does not match the selected boot payload at byte 2560')
    header=struct.unpack_from('<16sHHIQQQIHHHHHH',elf)
    phoff,phsize,phcount=header[5],header[9],header[10];shoff,shsize,shcount=header[6],header[11],header[12]
    if shsize!=64 or not shcount or shoff>len(elf) or shcount>(len(elf)-shoff)//shsize:raise ValueError('Invalid ELF section table')
    segments=[struct.unpack_from('<II6Q',elf,phoff+i*phsize) for i in range(phcount)]
    addresses={}
    for line in symbol_text.splitlines():
        fields=line.split()
        if len(fields)<8 or fields[7] not in PROFILE_SIZES:continue
        name=fields[7]
        if name in addresses or len(fields)!=8 or fields[3]!='OBJECT' or int(fields[2])!=PROFILE_SIZES[name] or not fields[6].isdigit():
            raise ValueError('Missing, duplicate or wrongly typed allocator symbol: '+name)
        address=int(fields[1],16);size=PROFILE_SIZES[name];section=int(fields[6])
        if not 0<section<shcount:raise ValueError('Allocator symbol has no defined ELF section: '+name)
        _,kind,flags,start,_,length,*_=struct.unpack_from('<II4QII2Q',elf,shoff+section*shsize)
        if kind not in (1,8) or flags&3!=3 or flags&4 or not start<=address<=start+length-size:
            raise ValueError('Allocator symbol is outside allocated writable data/BSS: '+name)
        matching=[s for s in segments if s[0]==1 and s[1]&6==6 and not s[1]&1 and s[3]==s[4] and s[3]<=address<=s[3]+s[6]-size]
        if len(matching)!=1 or address%size:raise ValueError('Allocator symbol is outside one writable identity-mapped segment: '+name)
        if any(address<old+PROFILE_SIZES[other] and address+size>old for other,old in addresses.items()):raise ValueError('Allocator symbol ranges overlap')
        addresses[name]=address
    if set(addresses)!=set(PROFILE_SIZES):raise ValueError('Kernel ELF lacks the unique required allocator OBJECT symbols')
    return addresses

def sample_registers(text):
    rip=re.search(r'\bRIP=([0-9a-fA-F]+)\b',text);cpl=re.search(r'\bCPL=([0-3])\b',text)
    if not rip or not cpl:raise ValueError('QEMU register snapshot lacks RIP/CPL')
    return {'rip':hex(int(rip.group(1),16)),'cpl':int(cpl.group(1))}
def sample_words(text,address,count):
    match=re.fullmatch(r'\s*((?:0x)?[0-9a-fA-F]+):\s*((?:0x[0-9a-fA-F]+\s*)+)\s*',text)
    if not match or int(match.group(1),16)!=address:raise ValueError('Unexpected allocator counter response address')
    values=[int(word,16) for word in re.findall(r'0x[0-9a-fA-F]+',match.group(2))]
    if len(values)!=count:raise ValueError('Unexpected allocator counter response size')
    return values

def process_usage(pid):
    from ctypes import wintypes
    class Memory(ctypes.Structure):
        _fields_=[('cb',wintypes.DWORD),('PageFaultCount',wintypes.DWORD)]+[(name,ctypes.c_size_t) for name in
            ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage',
             'QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage')]
    api=ctypes.WinDLL('kernel32',use_last_error=True)
    api.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];api.OpenProcess.restype=wintypes.HANDLE
    api.CloseHandle.argtypes=[wintypes.HANDLE];api.CloseHandle.restype=wintypes.BOOL
    api.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4;api.GetProcessTimes.restype=wintypes.BOOL
    api.K32GetProcessMemoryInfo.argtypes=[wintypes.HANDLE,ctypes.POINTER(Memory),wintypes.DWORD];api.K32GetProcessMemoryInfo.restype=wintypes.BOOL
    handle=api.OpenProcess(0x410,False,pid)
    if not handle:raise ctypes.WinError(ctypes.get_last_error())
    try:
        created,exited,kernel,user=(wintypes.FILETIME() for _ in range(4));memory=Memory();memory.cb=ctypes.sizeof(memory)
        if not api.GetProcessTimes(handle,ctypes.byref(created),ctypes.byref(exited),ctypes.byref(kernel),ctypes.byref(user)) or \
                not api.K32GetProcessMemoryInfo(handle,ctypes.byref(memory),memory.cb):raise ctypes.WinError(ctypes.get_last_error())
        ticks=lambda value:(value.dwHighDateTime<<32)|value.dwLowDateTime
        return {'host_cpu_seconds':(ticks(kernel)+ticks(user))/10_000_000,'host_rss_bytes':memory.WorkingSetSize,
            'host_peak_rss_bytes':memory.PeakWorkingSetSize}
    finally:api.CloseHandle(handle)

def document_visible(picture):
    # Same page-ink gate as the published desktop smoke: reject blank/title-only frames.
    channels=picture.split()
    dark=ImageChops.lighter(ImageChops.lighter(channels[0],channels[1]),channels[2]).point(lambda v:255 if v<120 else 0)
    delta=ImageChops.difference(picture,Image.new('RGB',picture.size,(238,238,238))).split()
    gray=ImageChops.lighter(ImageChops.lighter(delta[0],delta[1]),delta[2]).point(lambda v:255 if v==0 else 0)
    for dx,dy in ((24,24),(-24,24),(24,-24),(-24,-24)):dark=ImageChops.multiply(dark,ImageChops.offset(gray,dx,dy))
    return dark.crop((32,140,picture.width-32,picture.height-100)).histogram()[255]>1000

# Current Chrome's unhovered reload icon at 1080p; a stop/loading X must not pass.
RELOAD_ROWS=('................','................','......####...#..','....##########..',
    '...##......###..','...#.......###..','..##.....#####..','..##............',
    '..##............','..##........##..','...#........#...','...##......##...',
    '....########....','......#####.....','................','................')
def chrome_page_ready(picture):
    lower_link=sum(count for count,color in picture.crop((860,915,1100,960)).getcolors(240*45)
        if color[0]<100 and color[1]<100 and color[2]>150)
    reload_icon=all((max(picture.getpixel((87+x,87+y)))<160)==(pixel=='#')
        for y,row in enumerate(RELOAD_ROWS) for x,pixel in enumerate(row))
    return lower_link>100 and reload_icon and document_visible(picture)

blank=Image.new('RGB',(1920,1080),(30,20,60));assert not cursor_at(blank,(960,540))
for x,y,color in opaque:blank.putpixel((960+x,540+y),color)
assert cursor_at(blank,(960,540)) and not cursor_at(blank,(1020,540));del blank
sample=Image.new('RGB',(1920,1080),(238,238,238));assert not document_visible(sample)
assert sample_registers('RAX=0011223344556677\nRIP=000000000010abcd RFL=00000202 CPL=0 II=0')=={'rip':'0x10abcd','cpl':0}
assert sample_registers('RIP=000023456789abcd RFL=00000202 CPL=3 II=0')=={'rip':'0x23456789abcd','cpl':3}
test_elf=bytearray(1024);ident=b'\x7fELF\x02\x01\x01'+bytes(9)
struct.pack_into('<16sHHIQQQIHHHHHH',test_elf,0,ident,2,62,1,0x100000,64,512,0,64,56,2,64,3,0)
struct.pack_into('<II6Q',test_elf,64,1,5,256,0x100000,0x100000,4,4,4096)
struct.pack_into('<II6Q',test_elf,120,1,6,300,0x101000,0x101000,4,4096,4096)
struct.pack_into('<II4QII2Q',test_elf,576,0,1,6,0x100000,256,4,0,0,1,0)
struct.pack_into('<II4QII2Q',test_elf,640,0,8,3,0x101004,304,4092,0,0,4,0)
test_elf[256:260]=b'CODE';test_elf[300:304]=b'DATA'
test_addresses={'allocated':0x101008,'page_count':0x101010,'reserved':0x101014,'first_address':0x101018,'last_address':0x101020}
test_symbols='\n'.join(f'{i}: {address:016x} {PROFILE_SIZES[name]} OBJECT LOCAL DEFAULT 2 {name}' for i,(name,address) in enumerate(test_addresses.items(),1))
test_boot=bytes(2560)+kernel_build.flat_kernel(test_elf)+bytes(512)
assert profile_layout(test_elf,test_boot,test_symbols)==test_addresses
readonly=bytearray(test_elf);struct.pack_into('<I',readonly,124,4)
for image,boot_data,symbols in ((test_elf,test_boot,test_symbols+'\n'+test_symbols.splitlines()[0]),
        (test_elf,test_boot,test_symbols.replace('8 OBJECT','4 OBJECT',1)),
        (test_elf,test_boot,test_symbols.replace('OBJECT','FUNC',1)),
        (test_elf,test_boot,test_symbols.replace('0000000000101008','0000000000090000')),
        (readonly,test_boot,test_symbols),(test_elf,test_boot[:2560]+b'X'+test_boot[2561:],test_symbols)):
    try:profile_layout(image,boot_data,symbols)
    except ValueError:pass
    else:raise AssertionError('Unverified ELF counter address accepted')
del test_elf,test_boot,test_symbols,test_addresses,readonly
assert sample_words('0000000000184a88: 0x00080000 0x00000100\r\n',PROFILE_PAGE_COUNT,2)==[0x80000,0x100]
for invalid in ('0000000000184a89: 0x00080000 0x00000100','0000000000184a88: 0x00080000 0x00000100 0x00000000'):
    try:sample_words(invalid,PROFILE_PAGE_COUNT,2)
    except ValueError:pass
    else:raise AssertionError('Counter parser accepted an unrequested physical read')
sample.paste('black',(20,20,500,80));assert not document_visible(sample)
for y in range(260,390,26):
    for x in range(300,650,14):sample.paste('black',(x,y,x+5,y+10))
assert document_visible(sample) and not chrome_page_ready(sample)
sample.paste('blue',(910,930,1010,933));assert not chrome_page_ready(sample)
for y,row in enumerate(RELOAD_ROWS):
    for x,pixel in enumerate(row):sample.putpixel((87+x,87+y),(0,0,0) if pixel=='#' else (255,255,255))
assert chrome_page_ready(sample);del sample
if args.self_test:
    import os
    usage=process_usage(os.getpid());assert usage['host_cpu_seconds']>=0 and 0<usage['host_rss_bytes']<=usage['host_peak_rss_bytes']
    print('PASS exact cursor, document ink, lower link, completed-load gates and actual Windows process CPU/RSS query');raise SystemExit(0)
host.require_idle([disk,boot]);base_hash=host.digest(disk);boot_hash=host.digest(boot)
profile_addresses={'allocated':PROFILE_ALLOCATED,'page_count':PROFILE_PAGE_COUNT,'reserved':PROFILE_PAGE_COUNT+4,'first_address':PROFILE_FIRST,'last_address':PROFILE_LAST}
profile_elf=None
if args.sample_pc:
    if args.kernel_elf:
        elf_path=args.kernel_elf.resolve();elf=elf_path.read_bytes()
        symbols=subprocess.check_output([str(ROOT/'tools/i686-elf/bin/i686-elf-readelf.exe'),'-Ws',str(elf_path)],text=True)
        assert elf_path.read_bytes()==elf,'Kernel ELF changed while reading its symbols'
        profile_addresses=profile_layout(elf,boot.read_bytes(),symbols)
        profile_elf={'path':str(elf_path),'sha256':hashlib.sha256(elf).hexdigest(),'matched_boot_offset':2560,
            'matched_payload_bytes':len(kernel_build.flat_kernel(elf)),'allocator_addresses':{name:hex(address) for name,address in profile_addresses.items()}}
        del elf,symbols
    else:assert boot_hash==PROFILE_BOOT_SHA,'Pass --kernel-elf to verify allocator addresses for another boot image'
fs=host.fat.Fat32(disk)
try:
    desktop_hash=hashlib.sha256(host.read(fs,'sbin/init')).hexdigest();assert host.read(fs,'nexus/desktop.args').strip()==b'--aurora'
    if args.chrome:assert b'https://example.com/' in host.read(fs,'nexus/chrome-desktop.args').splitlines(),'Chrome workload requires the public Example Domain URL'
finally:fs.f.close()
serial=WORK/'serial.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(WORK);process=None;measurements=[];startup=None;profile=None
if (WORK/'result.json').exists():raise FileExistsError('Keep prior benchmark results; choose a new work directory')
def check():
    text=serial.read_text() if serial.exists() else ''
    assert not any(x in text for x in ('USER_FAULT','KERNEL_FAULT','DESKTOP RESOURCE LEAK','[64] FAIL:')),text[-4000:]
    return text
try:
    with (WORK/'qemu.log').open('wb') as output:
        process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe',*acceleration_options[args.accel],'-m','2048',
            '-drive',f'file={boot},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide,snapshot=on',
            '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,
            '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4','-netdev','user,id=network','-device','rtl8139,netdev=network'],stdout=output,stderr=subprocess.STDOUT)
    (WORK/'vm.json').write_text(json.dumps({'pid':process.pid,'disk':str(disk),'snapshot':True,'qmp':str(control.path),'accelerator':args.accel,'cpu':cpu_model},indent=2)+'\n')
    print('CURSOR BENCHMARK VM',process.pid,flush=True)
    until=time.monotonic()+120
    while 'AURORA READY persistent desktop' not in check():
        assert process.poll() is None and time.monotonic()<until,'Aurora startup failed or timed out'
        time.sleep(.1)
    with control.open() as monitor:
        command=monitor.command;position=[960,540]
        def capture():
            command('screendump',{'filename':str(WORK/'frame.ppm')})
            with Image.open(WORK/'frame.ppm') as image:return image.convert('RGB')
        def move(dx,dy):
            events=[]
            if dx:events.append({'type':'rel','data':{'axis':'x','value':dx}})
            if dy:events.append({'type':'rel','data':{'axis':'y','value':dy}})
            position[0]+=dx;position[1]+=dy;command('input-send-event',{'events':events})
        def visible(point,seconds=4):
            deadline=time.perf_counter()+seconds;captures=0
            while time.perf_counter()<deadline:
                check();picture=capture();captures+=1
                if cursor_at(picture,point):return picture,captures
                assert process.poll() is None,'VM exited while measuring cursor'
                time.sleep(.01)
            raise TimeoutError('Cursor never became visible at '+str(point))
        def move_to(x,y):
            while position!=[x,y]:
                move(max(-90,min(90,x-position[0])),max(-90,min(90,y-position[1])));time.sleep(.03)
        def click(x,y):
            move_to(x,y)
            for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.15)
        def wait(pattern,after=0,seconds=300):
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                match=re.search(pattern,check()[after:])
                if match:return match
                assert process.poll() is None,'VM exited before '+pattern
                time.sleep(.1)
            raise TimeoutError(pattern)
        original,_=visible(position)
        if args.chrome:
            original.save(WORK/'aurora-before-chrome.png');before=len(check())
            if args.startup:
                move_to(864,1040);usage=process_usage(process.pid);launched=time.perf_counter()
                startup={'dock_press_to_window_seconds':None,'dock_press_to_complete_https_seconds':None,
                    'cursor_probes':[],'host_stages':[{'stage':'dock_press','elapsed_seconds':0,**usage}],
                    'cursor_probe_timeout_seconds':10,'maximum_cursor_probe_rate_hz':1,
                    'measurement':'Host physical dock-button press to observed Chrome window and complete public HTTPS pixels; cursor samples include QMP/capture overhead.'}
                def save_startup():(WORK/'startup-progress.json').write_text(json.dumps(startup,indent=2)+'\n')
                def stage(name,elapsed):
                    startup['host_stages'].append({'stage':name,'elapsed_seconds':elapsed,**process_usage(process.pid)});save_startup()
                for down in (True,False):command('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':down}}]});time.sleep(.15)
                move_to(960,420);chrome_pid=None;prior=None;stable_since=None;last_page_observation=0;ready_picture=None
                last_pc=0;last_memory=0
                if args.sample_pc:
                    profile={'boot_sha256':boot_hash,'requested_pc_interval_seconds':.025,'memory_interval_seconds':1,
                        'kernel_elf':profile_elf,'allocator_addresses':{name:hex(address) for name,address in profile_addresses.items()},
                        'samples':[],'memory':[],'method':'Live HMP info registers without VM pause; only RIP/CPL retained. Physical reads restricted to verified allocator OBJECT symbols.'}
                def profile_startup():
                    global last_pc,last_memory
                    if not args.sample_pc:return
                    now=time.perf_counter()
                    if now-last_pc>=.025:
                        last_pc=now;registers=command('human-monitor-command',{'command-line':'info registers'})
                        profile['samples'].append({'after_dock_seconds':now-launched,**sample_registers(registers)});del registers
                    if now-last_memory>=1:
                        last_memory=now
                        if not profile['memory']:
                            address=profile_addresses['first_address'];first=sample_words(command('human-monitor-command',{'command-line':f'xp /1gx {address:#x}'}),address,1)[0]
                            address=profile_addresses['last_address'];last=sample_words(command('human-monitor-command',{'command-line':f'xp /1gx {address:#x}'}),address,1)[0]
                            profile['physical_pool']={'first_address':hex(first),'last_address':hex(last)}
                        address=profile_addresses['allocated'];allocated=sample_words(command('human-monitor-command',{'command-line':f'xp /1gx {address:#x}'}),address,1)[0]
                        address=profile_addresses['page_count']
                        if profile_addresses['reserved']==address+4:pages,reserved=sample_words(command('human-monitor-command',{'command-line':f'xp /2wx {address:#x}'}),address,2)
                        else:
                            pages=sample_words(command('human-monitor-command',{'command-line':f'xp /1wx {address:#x}'}),address,1)[0]
                            address=profile_addresses['reserved'];reserved=sample_words(command('human-monitor-command',{'command-line':f'xp /1wx {address:#x}'}),address,1)[0]
                        assert 0<=reserved<=pages and 0<=allocated<=pages-reserved
                        pool=profile['physical_pool'];assert int(pool['last_address'],16)-int(pool['first_address'],16)==pages*4096
                        profile['memory'].append({'after_dock_seconds':now-launched,'allocated_pages':allocated,'page_count':pages,'reserved_pages':reserved,'allocated_bytes':allocated*4096})
                        (WORK/'startup-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
                def observe_startup():
                    profile_startup()
                    text=check()[before:];now=time.perf_counter();elapsed=now-launched
                    if startup['dock_press_to_window_seconds'] is None and re.search('AURORA WINDOW app=google-chrome ',text):
                        startup['dock_press_to_window_seconds']=elapsed;stage('first_chrome_window',elapsed)
                    return text,now
                next_probe=launched;deadline=launched+600
                while time.perf_counter()<deadline:
                    text,now=observe_startup();match=re.search(r'AURORA LAUNCH app=chrome pid=(\d+)',text)
                    if match:chrome_pid=match.group(1)
                    if now<next_probe and (startup['dock_press_to_window_seconds'] is None or now-last_page_observation<.25):
                        assert process.poll() is None,'VM exited while launching Chrome';time.sleep(.025 if args.sample_pc else .05);continue
                    if now>=next_probe:
                        old=position.copy();sent=time.perf_counter();next_probe=sent+1;move(40 if position[0]==960 else -40,0)
                        sample={'from':old,'to':position.copy(),'sent_after_dock_seconds':sent-launched,
                            'phase':'before_window' if startup['dock_press_to_window_seconds'] is None else 'loading_page',
                            'sent_to_visible_ms':None,'timed_out':False,'captures':0}
                        probe_deadline=sent+10
                    else:sample=None;probe_deadline=now
                    while True:
                        if sample and time.perf_counter()>=probe_deadline:sample['timed_out']=True;break
                        old_timeout=monitor.timeout
                        if sample:monitor.timeout=min(old_timeout,max(.01,probe_deadline-time.perf_counter()))
                        try:picture=capture()
                        except TimeoutError:
                            if not sample:raise
                            sample['timed_out']=True;sample['capture_timeout']=True;break
                        finally:monitor.timeout=old_timeout
                        captured=time.perf_counter();text,now=observe_startup()
                        if sample:sample['captures']+=1
                        if startup['dock_press_to_window_seconds'] is not None and now-last_page_observation>=.25:
                            last_page_observation=now;region=picture.crop((880,450,1120,700)).tobytes()
                            if not chrome_page_ready(picture):stable_since=None
                            elif region!=prior or stable_since is None:stable_since=now
                            elif now-stable_since>=2 and startup['dock_press_to_complete_https_seconds'] is None:
                                startup['dock_press_to_complete_https_seconds']=now-launched;ready_picture=picture.copy();stage('complete_https_page',now-launched)
                            prior=region
                        if not sample:break
                        if cursor_at(picture,position) and captured<=probe_deadline:
                            sample['sent_to_visible_ms']=(captured-sent)*1000;break
                        if time.perf_counter()>=probe_deadline:sample['timed_out']=True;break
                        assert process.poll() is None,'VM exited during startup cursor probe'
                        time.sleep(.01)
                    if sample:
                        startup['cursor_probes'].append(sample);save_startup();print('STARTUP CURSOR',json.dumps(sample),flush=True)
                    if ready_picture is not None:
                        assert chrome_pid,'Chrome page appeared without a launch identity';picture=ready_picture;break
                    assert process.poll() is None,'VM exited while launching Chrome';time.sleep(.025 if args.sample_pc else .05)
                else:raise TimeoutError('Chrome window and complete public HTTPS page during startup benchmark')
                values=sorted(row['sent_to_visible_ms'] for row in startup['cursor_probes'] if row['sent_to_visible_ms'] is not None)
                startup['cursor_response_summary']={'samples':len(startup['cursor_probes']),'timeouts':sum(row['timed_out'] for row in startup['cursor_probes']),
                    'median_ms':statistics.median(values) if values else None,'p95_ms':values[math.ceil(.95*len(values))-1] if values else None,'max_ms':max(values) if values else None}
                if profile:
                    histogram=Counter((sample['cpl'],sample['rip']) for sample in profile['samples'])
                    profile['rip_histogram']=[{'cpl':cpl,'rip':rip,'count':count} for (cpl,rip),count in histogram.most_common()]
                    profile['cpl_counts']=dict(Counter(sample['cpl'] for sample in profile['samples']))
                    profile['peak_allocated_bytes']=max(sample['allocated_bytes'] for sample in profile['memory'])
                    (WORK/'startup-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
                    startup['profile']={'path':str(WORK/'startup-profile.json'),'samples':len(profile['samples']),
                        'cpl_counts':profile['cpl_counts'],'peak_allocated_bytes':profile['peak_allocated_bytes']}
                save_startup();print('CHROME STARTUP',json.dumps(startup['cursor_response_summary']),startup['dock_press_to_window_seconds'],startup['dock_press_to_complete_https_seconds'],flush=True)
                move_to(960,420);picture,_=visible(position,seconds=10)
            else:
                click(864,1040);chrome_pid=wait(r'AURORA LAUNCH app=chrome pid=(\d+)',before).group(1)
                wait('AURORA WINDOW app=google-chrome ',before);move_to(960,420);visible(position)
                # English arrives before later paragraphs: require the bottom link, completed load, and two seconds of stable pixels.
                deadline=time.monotonic()+300;prior=None;stable_since=None
                while time.monotonic()<deadline:
                    check();picture=capture();region=picture.crop((880,450,1120,700)).tobytes()
                    now=time.monotonic()
                    if not chrome_page_ready(picture):stable_since=None
                    elif region!=prior or stable_since is None:stable_since=now
                    elif now-stable_since>=2:break
                    prior=region;assert process.poll() is None,'Chrome exited while loading its page';time.sleep(.3)
                else:raise TimeoutError('Stable public HTTPS page pixels')
            picture.save(WORK/'chrome-https.png')
            # Each benchmark position must be over the actual page, not exposed Aurora wallpaper.
            for x in (900,960,1020,1080):
                for y in (540,600,660):
                    assert sum(count for count,color in picture.crop(cursor_box((x,y))).getcolors(520) if color==(238,238,238))>200,'Chrome page does not cover the cursor benchmark path'
            background=picture
        else:
            move(0,-60);move(0,-60);background,_=visible(position)
        background.save(WORK/'background.png')
        move(0,60);move(0,60);initial,_=visible(position);initial.save(WORK/'initial.png')
        deltas=[(60,0),(60,0),(0,60),(-60,0),(-60,0),(-60,0),(0,60),(60,0),(60,0),(60,0),(0,-60),(-60,0)]
        pauses=[.073,.137,.091,.181,.113,.057,.149,.103,.163,.083,.127,.061]
        for number,((dx,dy),pause) in enumerate(zip(deltas,pauses)):
            time.sleep(pause);old=position.copy();sent=time.perf_counter();move(dx,dy)
            picture,captures=visible(position);latency=(time.perf_counter()-sent)*1000
            assert picture.crop(cursor_box(old)).tobytes()==background.crop(cursor_box(old)).tobytes(),'Old cursor was not fully erased'
            picture.save(WORK/f'move-{number:02d}.png')
            row={'move':number,'from':old,'to':position.copy(),'pause_ms':pause*1000,'sent_to_visible_ms':latency,'captures':captures,'old_cursor_erased':True}
            measurements.append(row);print(json.dumps(row),flush=True)
        if args.chrome:
            if startup:stage('after_loaded_cursor_benchmark',time.perf_counter()-launched)
            before=len(check());command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':name}}} for name,down in (('alt',True),('f4',True),('f4',False),('alt',False))]})
            wait('AURORA CHILD EXIT pid='+chrome_pid+' status=0',before,seconds=120)
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                check();picture=capture()
                if picture.crop((400,450,850,850)).tobytes()==original.crop((400,450,850,850)).tobytes():break
                assert process.poll() is None,'Closing Chrome terminated Aurora';time.sleep(.1)
            else:raise TimeoutError('Chrome close did not restore Aurora wallpaper')
            picture.save(WORK/'aurora-after-chrome.png')
        click(16,14)
        for _ in range(3):
            for down in (True,False):command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':'down'}}}]});time.sleep(.15)
        command('input-send-event',{'events':[{'type':'key','data':{'down':down,'key':{'type':'qcode','data':'ret'}}} for down in (True,False)]})
    process.wait(timeout=30);text=check();assert process.returncode==33 and 'DESKTOP EXIT status=0' in text and 'INIT SESSION RETURNED' in text
    assert host.digest(disk)==base_hash and host.digest(boot)==boot_hash,'Snapshot benchmark changed a base image'
    values=sorted(row['sent_to_visible_ms'] for row in measurements)
    report={'disk':str(disk),'boot':str(boot),'snapshot':True,'base_sha256':base_hash,'boot_sha256':boot_hash,'desktop_sha256':desktop_hash,
        'workload':'chrome-public-https' if args.chrome else 'empty-desktop','public_document_pixels_verified':args.chrome,'chrome_close_returns_to_aurora':args.chrome,
        'chrome_load_complete_pixels_verified':args.chrome,'minimum_stable_page_seconds':2 if args.chrome else None,
        'accelerator':'QEMU default TCG' if args.accel=='tcg' else 'WHPX kernel-irqchip=off','cpu':cpu_model,
        'samples':measurements,'median_ms':statistics.median(values),'p95_ms':values[math.ceil(.95*len(values))-1],
        'max_ms':max(values),'opaque_cursor_pixels_matched':len(opaque),'normal_shutdown':True,'base_images_unchanged':True,
        'measurement':'Host physical-input send to first QMP framebuffer capture matching the actual opaque cursor; includes capture/poll overhead, not an FPS estimate.'}
    if startup:report['chrome_startup']=startup
    (WORK/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS cursor latency median/p95/max(ms)',report['median_ms'],report['p95_ms'],report['max_ms'],flush=True)
except BaseException as error:
    if profile:(WORK/'startup-profile.json').write_text(json.dumps(profile,indent=2)+'\n')
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'chrome_startup':startup,'loaded_cursor_samples':measurements},indent=2)+'\n');raise
finally:
    if process is not None:
        if process.poll() is None:
            try:
                with control.open() as monitor:monitor.command('quit')
                process.wait(timeout=5)
            except Exception:process.kill();process.wait()
        control.cleanup(process)
    assert host.digest(disk)==base_hash and host.digest(boot)==boot_hash,'Base image changed during cleanup'
    if profile_elf:assert host.digest(Path(profile_elf['path']))==profile_elf['sha256'],'Profile kernel ELF changed during the benchmark'

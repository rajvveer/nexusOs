"""Aurora and direct Chrome select sessions without rebuilding or editing disks."""
from pathlib import Path
from unittest.mock import patch
import ctypes
import importlib.util
import json
import runpy
from types import SimpleNamespace
ROOT=Path(__file__).resolve().parents[1];script=ROOT/'arch/x86_64/build.py'
spec=importlib.util.spec_from_file_location('aurora_launcher',script)
launcher=importlib.util.module_from_spec(spec);spec.loader.exec_module(launcher)

class Capabilities:
    def __init__(self,values=None,failed=None,short=None):
        self.values={0:1,0x1000:0,0x1001:1<<21};self.values.update(values or {})
        self.failed,self.short=failed,short;self.calls=[]
    def __call__(self,code,output,size,written):
        assert code in self.values and size==(8 if code==0x1001 else 4)
        self.calls.append(code)
        ctypes.cast(written,ctypes.POINTER(ctypes.c_uint32))[0]=size-1 if code==self.short else size
        kind=ctypes.c_uint64 if code==0x1001 else ctypes.c_uint32
        ctypes.cast(output,ctypes.POINTER(kind))[0]=self.values[code]
        return -1 if code==self.failed else 0

for values,failed,short,expected in [({},None,None,True),({0x1000:1},None,None,True),({0x1000:2},None,None,True),
        ({0:0},None,None,False),({0x1000:16},None,None,False),({0x1001:0},None,None,False),
        ({0x1001:(1<<20)|(1<<22)},None,None,False),
        *[({},code,None,False) for code in (0,0x1000,0x1001)],
        *[({},None,code,False) for code in (0,0x1000,0x1001)]]:
    query=Capabilities(values,failed,short)
    with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',return_value=SimpleNamespace(WHvGetCapability=query),create=True) as load:
        assert launcher.whpx_available()==expected
        load.assert_called_once_with('WinHvPlatform.dll',winmode=0x800)
for library in (OSError('DLL absent'),SimpleNamespace()):
    with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',side_effect=library if isinstance(library,Exception) else None,return_value=library,create=True):
        assert launcher.whpx_available() is False
with patch.object(launcher.sys,'platform','linux'),patch('ctypes.WinDLL',create=True) as load:
    assert launcher.whpx_available() is False;load.assert_not_called()
for features,expected in [((),''),((38,),',+ssse3,+sse4.1,+sse4.2,+popcnt'),((38,40),',+ssse3,+sse4.1,+sse4.2,+popcnt,+aes,+pclmulqdq,+xsave,+avx,+avx2,+fma,+f16c,+bmi1,+bmi2,+movbe,+abm')]:
    kernel32=SimpleNamespace(IsProcessorFeaturePresent=lambda feature,features=features:int(feature in features))
    with patch('ctypes.windll',SimpleNamespace(kernel32=kernel32),create=True):
        assert launcher.host_cpu_flags()==expected
FLAGS=launcher.host_cpu_flags()
for available in (False,True):
    with patch.object(launcher,'whpx_available',return_value=available) as capability:
        assert launcher.desktop_cpu_options()==(['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand'+FLAGS+launcher.whpx_security_flags()] if available else ['-cpu','max'])
        capability.assert_called_once()
with patch.object(launcher,'whpx_available') as capability:
    assert launcher.desktop_cpu_options(True)==['-cpu','max'];capability.assert_not_called()
class Throttling:
    def __init__(self,handle=77,result=1):
        self.handle,self.result,self.calls=handle,result,[]
        def open_process(access,inherit,pid):
            self.calls.append(('open',access,inherit,pid));return self.handle
        self.OpenProcess=open_process
    def SetProcessInformation(self,handle,kind,state,size):
        value=tuple(getattr(state._obj,field) for field in ('Version','ControlMask','StateMask'))
        self.calls.append(('set',handle.value,kind,value,size));return self.result
    def CloseHandle(self,handle):
        self.calls.append(('close',handle.value));return 1
for handle,result,expected in [(77,1,True),(77,0,False),(0,1,False)]:
    kernel32=Throttling(handle,result)
    with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',return_value=kernel32,create=True) as load:
        assert launcher.unthrottle(4321) is expected
        load.assert_called_once_with('kernel32',use_last_error=True)
    assert kernel32.calls[0]==('open',0x200,False,4321)
    if handle:assert kernel32.calls[1:]==[('set',77,4,(1,5,0),12),('close',77)],'VM keeps 1 ms timers and full-speed cores'
    else:assert len(kernel32.calls)==1
with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',side_effect=OSError('absent'),create=True):
    assert launcher.unthrottle(4321) is False
with patch.object(launcher.sys,'platform','linux'),patch('ctypes.WinDLL',create=True) as load:
    assert launcher.unthrottle(4321) is False;load.assert_not_called()
batch=(ROOT/'build.bat').read_bytes()
assert b'if "%1"=="run" goto run_desktop\r\n' in batch
assert b'if "%1"=="legacy" goto build_and_run\r\n' in batch
assert b':run_desktop\r\npython -I arch\\x86_64\\build.py desktop %2 %3 %4\r\n' in batch,'desktop flags (--gpu/--uefi) reach the launcher'
assert b'if "%1"=="vmware" goto run_vmware\r\n' in batch and b':run_vmware\r\npython -I arch\\x86_64\\build.py vmware %2 %3 %4\r\n' in batch
assert b'\n' not in batch.replace(b'\r\n',b''),'Windows build script must retain CRLF'
for action,published,available in [(a,p,c) for a,p in [('desktop',False),('desktop',True),('chrome',True),('chrome',False)] for c in (False,True)]:
    metadata=json.dumps({'aurora':{'verified':published}})
    query=Capabilities({0:int(available)})
    with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',return_value=SimpleNamespace(WHvGetCapability=query),create=True),patch('sys.argv',[str(script),action]),patch.object(Path,'is_file',return_value=True),patch.object(Path,'read_text',return_value=metadata),patch('subprocess.Popen') as execute:
        execute.return_value.wait.return_value=33;execute.return_value.pid=4321
        try:runpy.run_path(str(script),run_name='__main__')
        except SystemExit as error:
            assert action=='desktop' and not published and 'not been verified' in str(error)
            execute.assert_not_called();continue
        execute.assert_called_once();args=execute.call_args.args[0]
        assert args[args.index('-display')+1]=='sdl' and args[args.index('-m')+1]=='2048'
        assert args[args.index('-machine')+1]=='q35','modern PC chipset by default'
        startup=execute.call_args.kwargs.get('startupinfo')
        assert startup is not None and startup.wShowWindow==0 and startup.dwFlags&launcher.subprocess.STARTF_USESHOWWINDOW,'the SDL window starts hidden until the guest activated the USB tablet (QEMU 9.1 SDL grab deadlock)'
        assert any(item.startswith('if=none,id=root,file=') and item.endswith('desktop64.img,format=raw') for item in args) and 'nvme,serial=nexus-root,drive=root' in args
        assert any('nexus64.img,format=raw,if=floppy,readonly=on' in item for item in args) and '-bios' not in args
        assert 'e1000e,netdev=network' in args and 'virtio-vga,id=video0' in args
        assert 'usb-kbd,bus=xhci.0,display=video0' in args and 'usb-tablet,bus=xhci.0,display=video0' in args and 'qemu-xhci,id=xhci' in args
        assert ('name=opt/nexus/session,string=chrome' in args)==(action=='chrome' and published)
        assert args[args.index('-cpu')+1]==('qemu64,+rdrand'+FLAGS if available else 'max')
        assert ('-accel' in args)==available
        if available:assert args[args.index('-accel')+1]=='whpx,kernel-irqchip=off'
        assert args[args.index('-smp')+1]==str(launcher.desktop_cpu_count()) and 1<=launcher.desktop_cpu_count()<=4,'desktop runs on several CPUs'
        assert args[args.index('-device',args.index('-audiodev'))+1]=='AC97,audiodev=snd0' and args.count('AC97,audiodev=snd0')==1
        backend=args[args.index('-audiodev')+1].split(',')
        assert backend[0]=='dsound' and 'id=snd0' in backend and 'out.frequency=48000' in backend,'desktop audio uses the host DirectSound backend at 48 kHz'
for action in ('desktop','chrome'):
    for returncode,forced in [(r,f) for r in (0,33,1,37) for f in (False,True)]:
        query=Capabilities()
        with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',return_value=SimpleNamespace(WHvGetCapability=query),create=True) as load,patch('sys.argv',[str(script),action,*(['--tcg'] if forced else [])]),patch.object(Path,'is_file',return_value=True),patch.object(Path,'read_text',return_value=json.dumps({'aurora':{'verified':True}})),patch('subprocess.Popen') as execute:
            execute.return_value.wait.return_value=returncode;execute.return_value.pid=4321
            try:runpy.run_path(str(script),run_name='__main__')
            except SystemExit as error:assert returncode not in (0,33) and error.code==returncode
            else:assert returncode in (0,33)
            execute.assert_called_once()
            args=execute.call_args.args[0]
            assert args[args.index('-cpu')+1]==('max' if forced else 'qemu64,+rdrand'+FLAGS)
            assert ('-accel' in args)==(not forced)
            execute.return_value.wait.assert_called_once()
            libraries=[call.args[0] for call in load.call_args_list]
            assert libraries[-1]=='kernel32','the launched VM is opted out of power throttling'
            if forced:assert 'WinHvPlatform.dll' not in libraries
for flags in (['--gpu'], ['--uefi'], ['--legacy-devices'], ['--legacy-devices', '--uefi'], ['--gpu', '--uefi']):
    with patch.object(launcher.sys,'platform','win32'),patch('ctypes.WinDLL',return_value=SimpleNamespace(WHvGetCapability=Capabilities()),create=True),patch('sys.argv',[str(script),'desktop',*flags]),patch.object(Path,'is_file',return_value=True),patch.object(Path,'read_text',return_value=json.dumps({'aurora':{'verified':True}})),patch('subprocess.Popen') as execute,patch.object(Path,'write_bytes') as firmware,patch.object(Path,'read_bytes',return_value=b'fd'):
        execute.return_value.wait.return_value=0;execute.return_value.pid=4321
        runpy.run_path(str(script),run_name='__main__')
        args=[str(a) for a in execute.call_args.args[0]]
        gpu,uefi,legacy='--gpu' in flags,'--uefi' in flags,'--legacy-devices' in flags
        assert ('virtio-vga-gl,id=video0' in args)==gpu and args[args.index('-display')+1]==('sdl,gl=on' if gpu else 'sdl'),'virgl only with --gpu'
        assert ('-bios' in args)==uefi and (any('if=floppy' in a for a in args))==(not uefi),'UEFI boots OVMF + ESP instead of the floppy'
        if uefi:assert args[args.index('-bios')+1].endswith('ovmf-unified.fd') and any('nexus64-uefi.img' in a for a in args) and any('bootindex=0' in a for a in args)
        assert args[args.index('-machine')+1]==('pc' if legacy else 'q35')
        assert (execute.call_args.kwargs.get('startupinfo') is None)==legacy,'only a USB-tablet machine starts with a hidden window'
        assert ('rtl8139,netdev=network' in args)==legacy and ('e1000e,netdev=network' in args)==(not legacy)
        assert (any(a.endswith('desktop64.img,format=raw,if=ide') or a.endswith('desktop64.img,format=raw,if=ide,index=0') for a in args))==legacy
print('PASS desktop machine: modern PC by default (q35, NVMe, e1000e, USB HID), --gpu virgl, --uefi OVMF, --legacy-devices')
print('PASS Aurora publication gate and read-only host session selection; one QEMU command, no rebuild/staging')
print('PASS WHPX capability/CPU readiness (host SIMD flags) and forced TCG; errors never retry a launched guest')
print('PASS launched VM keeps 1 ms host timers and full-speed cores (Windows 11 power throttling opt-out)')

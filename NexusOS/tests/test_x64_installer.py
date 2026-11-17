"""Native installer: actual ar/XZ/tar inputs, rollback, links and persisted bytes."""
from pathlib import Path
import hashlib
import importlib.util
import io
import json
import lzma
import shutil
import subprocess
import sys
import tarfile
import zlib
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-installer')
WORK.mkdir(parents=True,exist_ok=True)
disk=WORK/'disk.img'
if disk.exists():raise FileExistsError(disk)
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
builder=module('installer_build',ROOT/'arch/x86_64/installer.py');builder.build()
fatput=module('installer_fatput',ROOT/'userland/fatput.py')
app=WORK/'test.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/install_test_user.c'),'-o',str(app)],check=True)
session=WORK/'session.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/install_session_user.c'),'-o',str(session)],check=True)
content=bytes((i*17+3)&255 for i in range(150123))
def tar(members,format=tarfile.GNU_FORMAT):
    output=io.BytesIO()
    with tarfile.open(fileobj=output,mode='w',format=format) as archive:
        for name,kind,value in members:
            info=tarfile.TarInfo(name);info.mode=0o755;info.type=kind
            if kind==tarfile.REGTYPE:info.size=len(value);archive.addfile(info,io.BytesIO(value))
            else:
                if kind in (tarfile.SYMTYPE,tarfile.LNKTYPE):info.linkname=value
                archive.addfile(info)
    return output.getvalue()
good=[('./',tarfile.DIRTYPE,b''),('./opt/demo/tool',tarfile.REGTYPE,content),('./usr/bin/tool',tarfile.SYMTYPE,'/opt/demo/tool'),('./relative',tarfile.SYMTYPE,'opt/demo/tool'),('./empty',tarfile.REGTYPE,b'')]
def package(payload,compressed=False):
    output=bytearray(b'!<arch>\n')
    for name,data in [('debian-binary',b'2.0\n'),('control.tar.xz',lzma.compress(tar([]))),('data.tar.xz',payload if compressed else lzma.compress(payload))]:
        header=f'{name+"/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(data):<10}`\n'.encode();assert len(header)==60
        output+=header+data+(b'\n' if len(data)&1 else b'')
    return bytes(output)
cases=[]
def case(name,data,status=1,destination=None,digest=None):
    path=WORK/(name+'.deb');path.write_bytes(data)
    cases.append((name,destination or name,status,digest or hashlib.sha256(data).hexdigest(),path))
valid=package(tar(good));case('good-gnu',valid,0)
case('good-posix',package(tar(good,tarfile.USTAR_FORMAT)),0)
long_name='opt/'+('long-directory/'*8)+'value'
case('prefix',package(tar([(long_name,tarfile.REGTYPE,b'prefix works')],tarfile.USTAR_FORMAT)),0)
case('bad-hash',valid,digest='0'*64)
case('traversal',package(tar([('ok',tarfile.REGTYPE,b'rollback'),('../outside',tarfile.REGTYPE,b'broken')])) )
case('absolute',package(tar([('/home/nexus/outside',tarfile.REGTYPE,b'broken')])) )
case('backslash',package(tar([('dir\\outside',tarfile.REGTYPE,b'broken')])) )
case('symlink-escape',package(tar([('link',tarfile.SYMTYPE,'../outside')])) )
case('symlink-parent',package(tar([('parent',tarfile.SYMTYPE,'/home/nexus'),('parent/outside',tarfile.REGTYPE,b'broken')])) )
case('duplicate',package(tar([('same',tarfile.REGTYPE,b'one'),('same',tarfile.REGTYPE,b'two')])) )
case('receipt-collision',package(tar([('.nexus-package-sha256',tarfile.REGTYPE,b'forged')])) )
case('hardlink',package(tar([('file',tarfile.LNKTYPE,'/home/nexus/outside')])) )
case('device',package(tar([('device',tarfile.CHRTYPE,b'')])) )
broken=bytearray(tar(good));broken[0]^=1;case('bad-tar-checksum',package(broken))
broken=bytearray(lzma.compress(tar(good)));broken[-8]^=1;case('bad-xz-checksum',package(broken,True))
large_dictionary=bytearray(lzma.compress(tar(good)))
assert large_dictionary[12:16]==b'\x02\x00\x21\x01'
large_dictionary[16]=34  # A 512 MiB dictionary plus decoder state exceeds the budget.
large_dictionary[20:24]=zlib.crc32(large_dictionary[12:20]).to_bytes(4,'little')
case('huge-dictionary',package(large_dictionary,True))
case('short-tar',package(tar(good)[:700]))
case('no-end-marker',package(tar([('small',tarfile.REGTYPE,b'x')])[:1024]))
case('truncated',valid[:-100]);case('trailing',valid+b'bad')
case('existing',valid,destination='existing');case('existing-link',valid,destination='existing-link')
case('repeat',valid,destination='good-gnu')
case('disk-full',package(tar([('large',tarfile.REGTYPE,bytes(8*1024*1024))])))
case('receipt-full',package(tar([])))  # Staging uses the final cluster; the receipt must roll back cleanly.
chrome=module('installer_packages',ROOT/'arch/x86_64/chrome.py')
hello=json.loads((ROOT/'arch/x86_64/hello-download-lock.json').read_text())
case('hello',chrome.fetch(hello).read_bytes(),0,digest=hello['sha256'])
session_config=WORK/'install-session';session_config.write_text('/home/nexus/installer-test/hello.deb\n/home/nexus/Applications/hello-session\n'+hello['sha256']+'\n/home/nexus/Applications/hello-session/usr/bin/hello\n\nHello, world!\n')
manifest=WORK/'install-tests';manifest.write_text(''.join(f'{name} {dest} {status} {digest}\n' for name,dest,status,digest,_ in cases))
sentinel=WORK/'outside';sentinel.write_bytes(b'preserve')
try:
    shutil.copyfile(OUT/'apps64.img',disk)
    args=['fatput',str(disk),'put',str(app),'sbin/init','put',str(OUT/'nexus-install.elf'),'bin/nexus-install',
        'put',str(OUT/'chrome-root/usr/lib/x86_64-linux-gnu/liblzma.so.5.8.1'),'lib/x86_64-linux-gnu/liblzma.so.5',
        'put',str(manifest),'nexus/install-tests','put',str(sentinel),'home/nexus/outside',
        'put',str(session),'bin/install-session','put',str(session_config),'nexus/install-session']
    for name,_,_,_,path in cases:args+=['put',str(path),'home/nexus/installer-test/'+name+'.deb']
    fatput.main(args)
    fs=fatput.Fat32(disk)
    try:free=sum(fs.fat_get(c)==0 for c in range(2,fs.nclusters+2))*fs.cl_bytes
    finally:fs.close()
    # Reach ENOSPC promptly without spending the test filling an otherwise empty volume.
    filler=WORK/'filler.bin'
    try:
        with filler.open('wb') as f:f.truncate(free-4*1024*1024)
        fatput.main(['fatput',str(disk),'put',str(filler),'home/nexus/filler'])
    finally:filler.unlink(missing_ok=True)
    with disk.open('rb') as f:protected=f.read(16*1024*1024)
    serial=WORK/'serial.log';serial.unlink(missing_ok=True)
    result=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','256',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-no-reboot',
        '-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=150)
    log=serial.read_text();print(log[-16000:])
    assert result.returncode==33 and f'INSTALL TEST ALL PASSED cases={len(cases)}' in log,(result.stderr,log)
    assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and '[64] INIT SESSION RETURNED' in log
    assert 'INSTALL SESSION PASSED' in log and 'INSTALLED APPLICATION EXECUTED path=/home/nexus/Applications/hello-session/usr/bin/hello' in log
    fs=fatput.Fat32(disk)
    try:
        def read(path):
            parts=path.split('/');entry=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1])[2]
            first=int.from_bytes(entry[20:22],'little')<<16|int.from_bytes(entry[26:28],'little');size=int.from_bytes(entry[28:32],'little')
            return fs.read_chain(first,size),entry
        for name in ('good-gnu','good-posix'):
            base='home/nexus/Applications/'+name+'/'
            assert read(base+'opt/demo/tool')[0]==content
            data,entry=read(base+'usr/bin/tool');assert entry[12]==0xA2 and data==b'../../opt/demo/tool'
            assert read(base+'empty')[0]==b''
            expected_digest=next(digest for case_name,_,_,digest,_ in cases if case_name==name)
            assert read(base+'.nexus-package-sha256')[0]==expected_digest.encode()+b'\n'
        assert read('home/nexus/Applications/prefix/'+long_name)[0]==b'prefix works'
        assert read('home/nexus/outside')[0]==read('home/nexus/Applications/existing')[0]==b'preserve'
    finally:fs.close()
    with disk.open('rb') as f:assert f.read(len(protected))==protected
    print(f'PASS native installer: {len(cases)} success/rejection cases, disk-full rollback, confined links, no overwrite, persisted exact bytes and real installed GNU Hello execution')
finally:disk.unlink(missing_ok=True)

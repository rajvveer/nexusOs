"""Install the guest-downloaded package in place; never stage its payload from the host."""
from pathlib import Path
import hashlib
import importlib.util
import json
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]);application=sys.argv[2] if len(sys.argv)>2 else 'chrome'
if application not in ('chrome','hello'):raise ValueError(application)
record=WORK/'download-result.json' if application=='chrome' else WORK/'installed-download/download-result.json'
download=json.loads(record.read_text());package=download['package']
lock=json.loads((ROOT/'arch/x86_64'/('download-lock.json' if application=='chrome' else 'hello-download-lock.json')).read_text())
assert package==lock and download['readback_sha256']==package['sha256']
assert download['guest_path']=='/home/nexus/Downloads/'+package['filename']
disk=WORK/'disk.img'
if not disk.is_file() and (WORK/'installation-result.json').is_file():
    disk=Path(json.loads((WORK/'installation-result.json').read_text())['disk'])
assert disk.is_file()
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
builder=module('download_installer',ROOT/'arch/x86_64/installer.py');builder.build()
fatput=module('download_fatput',ROOT/'userland/fatput.py')
artifacts=WORK/('install-'+application);artifacts.mkdir(exist_ok=True)
def read(fs,path):
    parts=path.lstrip('/').split('/');found=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1]);assert found,path
    entry=found[2];size=int.from_bytes(entry[28:32],'little');first=int.from_bytes(entry[20:22],'little')<<16|int.from_bytes(entry[26:28],'little')
    return fs.read_chain(first,size)
fs=fatput.Fat32(disk)
try:assert hashlib.sha256(read(fs,download['guest_path'])).hexdigest()==package['sha256']
finally:fs.close()
runner=artifacts/'session.elf'
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/install_session_user.c'),'-o',str(runner)],check=True)
destination='/home/nexus/Applications/'+application
entry=destination+('/opt/google/chrome/chrome' if application=='chrome' else '/usr/bin/hello')
expected='Google Chrome '+package['version'].rsplit('-',1)[0] if application=='chrome' else 'Hello, world!'
config=artifacts/'install-session';config.write_text('\n'.join([download['guest_path'],destination,package['sha256'],entry,'--version' if application=='chrome' else '',expected])+'\n')
fatput.main(['fatput',str(disk),'put',str(runner),'sbin/init','put',str(OUT/'nexus-install.elf'),'bin/nexus-install','put',str(config),'nexus/install-session'])
serial=artifacts/'serial.log';serial.unlink(missing_ok=True)
process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','2048',
    '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
    '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-no-reboot',
    '-device','isa-debug-exit,iobase=0xf4,iosize=4'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
print('NATIVE INSTALL VM',process.pid,'disk',disk,flush=True)
try:output=process.communicate(timeout=1800)[0]
finally:
    if process.poll() is None:process.kill();process.wait()
log=serial.read_text();print(log[-12000:])
assert process.returncode==33 and 'INSTALL SESSION PASSED' in log and 'INSTALLED APPLICATION EXECUTED path='+entry in log,(output,log)
assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and 'incomplete staging cleanup' not in log
fs=fatput.Fat32(disk)
try:
    actual=hashlib.sha256(read(fs,entry)).hexdigest()
    assert hashlib.sha256(read(fs,download['guest_path'])).hexdigest()==package['sha256'],'installer changed the downloaded package'
    if application=='chrome':
        verified_files=0
        for original in (OUT/'chrome-root/opt/google/chrome').rglob('*'):
            if not original.is_file():continue
            with original.open('rb') as f:expected_hash=hashlib.file_digest(f,'sha256').hexdigest()
            installed_path=destination+'/'+original.relative_to(OUT/'chrome-root').as_posix()
            assert hashlib.sha256(read(fs,installed_path)).hexdigest()==expected_hash,installed_path
            verified_files+=1
        assert verified_files>200,verified_files
finally:fs.close()
if application=='chrome':
    assert actual==hashlib.sha256((OUT/'chrome-root/opt/google/chrome/chrome').read_bytes()).hexdigest(),'installed Chrome differs from the original upstream ELF'
    fs=fatput.Fat32(disk)
    try:assert hashlib.sha256(read(fs,'/opt/google/chrome/chrome')).hexdigest()==actual,'bootstrap executable changed; refusing removal'
    finally:fs.close()
    # Retire only the verified duplicate in this private test disk; the original build image remains intact.
    fatput.main(['fatput',str(disk),'rm','opt/google/chrome/chrome'])
result={'package':package,'guest_path':destination,'entry':entry,'entry_sha256':actual,'source':'guest Chrome download; native nexus-install extraction and execution'}
if application=='chrome':result['bootstrap_executable_removed']=True
(WORK/('installation-result.json' if application=='chrome' else 'hello-installation-result.json')).write_text(json.dumps(result,indent=2)+'\n')
print('PASS guest download -> native package installation -> installed executable output; persisted bytes verified',flush=True)

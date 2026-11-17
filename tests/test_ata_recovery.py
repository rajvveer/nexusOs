"""Real QEMU ATA timeouts: recover the channel, retry exact data and verify on reboot."""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import time
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('work',nargs='?',type=Path)
parser.add_argument('--work',dest='work_option',type=Path);parser.add_argument('--boot',type=Path,default=OUT/'nexus64.img')
parser.add_argument('--accel',choices=('tcg','whpx'),default='tcg');parser.add_argument('--require-dma',action='store_true');args=parser.parse_args()
if args.work and args.work_option:parser.error('Specify either positional work or --work')
WORK=(args.work_option or args.work or nexus_paths.runs('native64-ata-recovery')).resolve()
WORK.mkdir(parents=True,exist_ok=True);disk=WORK/'disk.img';boot_image=args.boot.resolve();source=(OUT/'apps64.img').resolve()
if disk.exists():raise FileExistsError(disk)
assert not disk.is_symlink() and disk.resolve().parent==WORK and disk not in (source,boot_image)
acceleration={'tcg':['-cpu','max'],'whpx':['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand']}[args.accel]
def module(name,path):
    s=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(s);s.loader.exec_module(m);return m
fat=module('recovery_fat',ROOT/'userland/fatput.py');qmp=module('recovery_qmp',ROOT/'tests/qmp_local.py');host=module('recovery_host',ROOT/'arch/x86_64/aurora.py')
host.require_idle([source,boot_image,disk]);source_hash=host.digest(source);boot_hash=host.digest(boot_image)
if (WORK/'result.json').exists():raise FileExistsError('Keep prior results; choose a new work directory')
app=WORK/'test.elf';data=WORK/'initial.dat';data.write_bytes(bytes([0xa5])*8192)
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36','-O2','-fPIE','-pie','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/ata_recovery_user.c'),'-o',str(app)],check=True)
owned=False;process=None;dma_boots=[];dma_timeouts=[];report=None
try:
    with disk.open('xb') as target:
        owned=True
        with source.open('rb') as original:shutil.copyfileobj(original,target)
    assert fat.edit(['fatput',str(disk),'put',str(app),'sbin/init','put',str(data),'home/nexus/ata-recovery.dat'])==0
    with disk.open('rb') as file:protected=file.read(16*1024*1024)
    for boot in range(2):
        serial=WORK/f'boot-{boot}.log';serial.unlink(missing_ok=True);control=qmp.QmpMonitor(WORK)
        host.require_idle([disk,boot_image])
        process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe',*acceleration,'-m','128',
            '-drive',f'file={boot_image},format=raw,if=floppy,readonly=on',
            '-drive',f'file={disk},format=raw,if=ide,id=ata-test','-boot','a','-display','none',
            '-vga','none','-device','virtio-vga','-serial',f'file:{serial}','-qmp',control.argument,
            '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (WORK/'vm.json').write_text(json.dumps({'pid':process.pid,'disk':str(disk),'boot':str(boot_image),'accelerator':args.accel,'boot_number':boot},indent=2)+'\n')
        print('ATA RECOVERY VM',boot,process.pid,args.accel,flush=True)
        def wait(marker,after=0):
            deadline=time.monotonic()+50
            while time.monotonic()<deadline:
                log=serial.read_text() if serial.exists() else ''
                if marker in log[after:]:return
                if process.poll() is not None:raise AssertionError(log[-5000:])
                time.sleep(.01)
            raise TimeoutError(marker+': '+log[-3000:])
        try:
            if not boot:
                with control.open() as monitor:
                    def throttle(write=0,read=0):
                        monitor.command('block_set_io_throttle',{'device':'ata-test','bps':0,'bps_rd':read,'bps_wr':write,'iops':0,'iops_rd':0,'iops_wr':0})
                    for direction in ('WRITE','READ'):
                        wait('ATA RECOVERY '+direction+' READY');before=len(serial.read_text())
                        throttle(write=128 if direction=='WRITE' else 0,read=128 if direction=='READ' else 0)
                        wait('[ATA] command timeout',before)
                        throttle()
                        wait('ATA RECOVERY '+('WRITE PASSED' if direction=='WRITE' else 'ALL PASSED'),before)
            output=process.communicate(timeout=60)[0];log=serial.read_text()
            assert process.returncode==33 and 'INIT SESSION RETURNED' in log,(output,log[-5000:])
            dma_boots.append('[64] ATA DMA READY PIIX3' in log)
            dma_timeouts.append(log.count('[ATA] command timeout (DMA)'))
            if args.require_dma:
                assert dma_boots[-1],'DMA was required but not initialized'
                if not boot:assert log.count('[ATA] command timeout (DMA)')==2,'Both injected timeouts must exercise DMA'
            assert not any(x in log for x in ('KERNEL_FAULT','USER_FAULT','ATA RECOVERY FAIL','FILE_WRITEBACK failed'))
            if not boot:assert log.count('[ATA] channel recovered')==2 and 'ATA RECOVERY ALL PASSED' in log
            else:assert 'ATA RECOVERY REBOOT PASSED' in log and '[ATA] command timeout' not in log
        finally:
            if process.poll() is None:process.kill();process.wait()
            control.cleanup(process)
    fs=fat.Fat32(disk)
    try:
        entry=fs.find_name(fs.lookup_dir(['home','nexus']),'ata-recovery.dat')[2]
        first=int.from_bytes(entry[20:22],'little')<<16|int.from_bytes(entry[26:28],'little');size=int.from_bytes(entry[28:32],'little')
        assert size==8192 and fs.read_chain(first,size)==bytes((i*37+11)&255 for i in range(size))
    finally:fs.f.close()
    with disk.open('rb') as file:assert file.read(len(protected))==protected
    assert host.digest(source)==source_hash and host.digest(boot_image)==boot_hash,'An original input image changed'
    report={'passed':True,'boot':str(boot_image),'boot_sha256':boot_hash,'source_sha256':source_hash,'accelerator':args.accel,
        'dma_required':args.require_dma,'dma_initialized_each_boot':dma_boots,'dma_timeout_count_each_boot':dma_timeouts,'write_and_read_timeout_recovered':True,
        'persisted_bytes':8192,'persisted_sha256':hashlib.sha256(bytes((i*37+11)&255 for i in range(8192))).hexdigest(),
        'cold_reboot_verified':True,'protected_region_unchanged':True,'original_images_unchanged':True,'private_image_removed':True}
except BaseException as error:
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'boot':str(boot_image),'accelerator':args.accel,'dma_required':args.require_dma},indent=2)+'\n');raise
finally:
    if process is not None and process.poll() is None:process.kill();process.wait()
    if owned:
        assert disk.resolve().parent==WORK and not disk.is_symlink()
        disk.unlink(missing_ok=True)
    assert host.digest(source)==source_hash and host.digest(boot_image)==boot_hash,'Original input image changed during the test'
assert report and not disk.exists()
(WORK/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS actual QEMU write/read timeouts, channel reset/retry, exact persisted bytes, cold reboot and protected region; private disk removed',flush=True)

"""Real IDE write failures must retain dirty cache data after the application exits."""
from pathlib import Path
import argparse
import importlib.util
import json
import shutil
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'arch/x86_64/out'
WORK = nexus_paths.runs('native64-writeback-errors')
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('work',nargs='?',type=Path)
parser.add_argument('--work',dest='work_option',type=Path);parser.add_argument('--boot',type=Path,default=OUT/'nexus64.img')
parser.add_argument('--accel',choices=('tcg','whpx'),default='tcg');parser.add_argument('--require-dma',action='store_true');args=parser.parse_args()
if args.work and args.work_option:parser.error('Specify either positional work or --work')
WORK=(args.work_option or args.work or WORK).resolve();WORK.mkdir(parents=True,exist_ok=True)
import os;boot=args.boot.resolve();source=(OUT/('apps64-ext4.img' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'apps64.img')).resolve();disk=WORK/'disk.img'  # NEXUS_ROOT_FS=ext4
if disk.exists():raise FileExistsError(disk)
assert not disk.is_symlink() and disk.resolve().parent==WORK and disk not in (source,boot)
acceleration={'tcg':['-cpu','max'],'whpx':['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand']}[args.accel]
spec = importlib.util.spec_from_file_location('fatput', ROOT / 'userland' / ('ext4put.py' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'fatput.py'))
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
spec=importlib.util.spec_from_file_location('writeback_host',ROOT/'arch/x86_64/aurora.py')
host=importlib.util.module_from_spec(spec);spec.loader.exec_module(host)
host.require_idle([source,boot,disk]);source_hash=host.digest(source);boot_hash=host.digest(boot)
if (WORK/'result.json').exists():raise FileExistsError('Keep prior results; choose a new work directory')
owned=False;process=None;report=None
try:
    with disk.open('xb') as target:
        owned=True
        with source.open('rb') as original:shutil.copyfileobj(original,target)
    marker = WORK / 'marker'; marker.write_bytes(b'1')
    data = WORK / 'original.dat'; data.write_bytes(b'1' * 8192)
    assert fatput.edit(['fatput', str(disk), 'put', str(marker), 'nexus/test-writeback-errors',
                 'put', str(data), 'home/nexus/writeback-errors.dat'])==0
    before = host.digest(disk)
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    config = WORK / 'errors.cfg'
    config.write_text('[inject-error]\nevent = "none"\niotype = "write"\nerrno = "5"\nonce = "off"\nimmediately = "on"\n')
    device = {'driver': 'blkdebug', 'node-name': 'faulty', 'config': str(config),
              'image': {'driver': 'raw', 'file': {'driver': 'file', 'filename': str(disk)}}}
    host.require_idle([disk,boot])
    process = subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe', *acceleration, '-m', '128',
        '-drive', f'file={boot},format=raw,if=floppy,readonly=on',
        '-blockdev', json.dumps(device), '-device', 'ide-hd,drive=faulty,bus=ide.0,unit=0,werror=report,rerror=report', '-boot', 'a', '-display', 'none',
        '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}', '-no-reboot',
        '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (WORK/'vm.json').write_text(json.dumps({'pid':process.pid,'disk':str(disk),'boot':str(boot),'accelerator':args.accel},indent=2)+'\n')
    print('WRITEBACK ERROR VM',process.pid,args.accel,flush=True)
    output=process.communicate(timeout=60)[0];log=serial.read_text()
    assert process.returncode == 33, (process.returncode,output,log[-4000:])
    assert '[64] FILEMAP ERROR PASSED:' in log and '[64] WRITEBACK ERROR CAMPAIGN PASSED' in log, log
    assert ('[ATA] command error' in log or '[ATA] DMA command error' in log) and '[64] FILE_WRITEBACK failed; dirty data retained:' in log, log
    if args.require_dma:assert '[64] ATA DMA READY PIIX3' in log and '[ATA] DMA command error' in log,'Injected failures did not exercise DMA'
    assert not any(marker in log for marker in ('KERNEL_FAULT','USER_FAULT','[64] FAIL:')),log
    assert host.digest(disk)==before,'Failing writes changed the disk'
    assert host.digest(source)==source_hash and host.digest(boot)==boot_hash,'An original input image changed'
    report={'passed':True,'boot':str(boot),'boot_sha256':boot_hash,'source_sha256':source_hash,'accelerator':args.accel,
        'dma_required':args.require_dma,'dma_initialized':'[64] ATA DMA READY PIIX3' in log,'dma_command_errors':log.count('[ATA] DMA command error'),'write_failure_reported':True,
        'dirty_data_retained_after_exit':True,'disk_sha256_before_and_after':before,'original_images_unchanged':True,'private_image_removed':True}
except BaseException as error:
    (WORK/'failure.json').write_text(json.dumps({'error':str(error),'boot':str(boot),'accelerator':args.accel,'dma_required':args.require_dma},indent=2)+'\n');raise
finally:
    if process is not None and process.poll() is None:process.kill();process.wait()
    if owned:
        assert disk.resolve().parent==WORK and not disk.is_symlink()
        disk.unlink(missing_ok=True)
    assert host.digest(source)==source_hash and host.digest(boot)==boot_hash,'Original input image changed during the test'
assert report and not disk.exists()
(WORK/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS real QEMU IDE failures: fsync/msync/munmap/close errors, dirty data survives exit, disk unchanged; private disk removed')

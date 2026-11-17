"""Small real FAT fixtures prove Aurora publication preserves originals and secrets."""
from pathlib import Path
from unittest.mock import patch
import hashlib
import importlib.util
import json
import shutil
import struct
import tempfile
import zlib
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('aurora',ROOT/'arch/x86_64/aurora.py')
aurora=importlib.util.module_from_spec(spec);spec.loader.exec_module(aurora)
fat=aurora.fat
WORK=nexus_paths.runs('aurora-publication-check');WORK.mkdir(parents=True,exist_ok=True)

def record(parent,name,kind=1,mode=0o644,data=b'',uid=0):
    return struct.pack('<IBxHIII32s',parent,kind,mode,uid,0,len(data),name.encode())+data+b'\0'*((-len(data))&3)

def snapshot(payload,count,generation=1,tombs=0):
    header=struct.pack('<8I',0x5352584e,1,generation,count,len(payload),zlib.crc32(payload),0,tombs)
    return (header+struct.pack('<I',zlib.crc32(header))).ljust(512,b'\0')+payload.ljust(aurora.SLOT_BYTES-512,b'\0')

def invalid(payload,count):
    try:aurora.snapshot_records(snapshot(payload,count)+bytes(aurora.SLOT_BYTES))
    except ValueError:return
    raise AssertionError('Invalid snapshot accepted')

invalid(record(0,'cycle',2,0o755),1)
invalid(record(0xffffffff,'..'),1)
invalid(record(0xffffffff,'same')+record(0xffffffff,'SAME'),2)
invalid(record(0xffffffff,'file')+b'\0',1)
records,_=aurora.snapshot_records(snapshot(record(0xffffffff,'opaque',2,0o711)+record(0,'file',data=b'hidden ancestry'),2)+bytes(aurora.SLOT_BYTES))
assert not records[1]['readable'],'Unlistable ancestor leaked imported names'

with tempfile.TemporaryDirectory(prefix='fixture-',dir=WORK) as directory:
    work=Path(directory).resolve();assert work.is_relative_to(WORK.resolve())
    aurora.OUT=work/'out';aurora.OUT.mkdir()
    boot=aurora.OUT/'nexus64.img';boot.write_bytes(b'fake boot used only by host fixture')
    old_chrome=b'original Chrome executable';old_hello=b'original Hello executable'
    chrome_path='/home/nexus/Applications/chrome/opt/google/chrome/chrome'
    hello_path='/home/nexus/Applications/hello/usr/bin/hello'
    legacy=work/'legacy.img';legacy.write_bytes(bytes(aurora.BASE))
    fs=fat.Fat32(legacy,create=True)
    aurora.put_bytes(fs,'Downloads/old notes.txt',b'legacy FAT content')
    aurora.put_bytes(fs,'Docs/caf\u00e9.txt',b'UTF-8 path')
    aurora.put_bytes(fs,'passwd.db',b'legacy FAT sensitive account copy')
    cluster,offset,entry=fs.find_name(fs.rootc,'Docs');entry=bytearray(entry);entry[11]|=3;fs.put_entry(cluster,offset,entry)
    fs.close()
    payload=(record(0xffffffff,'Documents',2,0o755)+record(0,'public.txt',data=b'public old notes')+
        record(0,'private.txt',mode=0o600,data=b'do not expose')+record(0xffffffff,'Locked',2,0o700)+
        record(3,'world.txt',data=b'private ancestor')+record(0xffffffff,'etc',2,0o755)+
        record(5,'passwd.db',mode=0o644,data=b'account hashes never enter guest')+
        record(0xffffffff,'',0x80,data=b'/deleted.txt\0'))
    newer=bytearray(snapshot(payload,8,8,1));newer[520]^=1
    with legacy.open('r+b') as stream:
        stream.seek(aurora.SNAPSHOT_START);stream.write(snapshot(payload,8,7,1));stream.write(newer)
    parsed,description=aurora.snapshot_records(snapshot(payload,8,7,1)+newer)
    assert description['selected']==0 and description['generation']==7 and description['tombstones']==['/deleted.txt']
    assert next(item for item in parsed if item['path']=='Locked/world.txt')['readable'] is False
    legacy_hash=aurora.digest(legacy)

    baseline=work/'baseline.img';baseline.write_bytes(bytes(aurora.BASE));fs=fat.Fat32(baseline,create=True)
    for path,data in [('sbin/init',b'old desktop'),('bin/nexus-install',b'old installer'),('bin/xdg-open',b'old graphical installer'),
        ('nexus/desktop.args',(chrome_path+'\n--disable-gpu\nhttps://example.com/\n').encode()),
        (chrome_path,old_chrome),(hello_path,old_hello),('home/nexus/keep.txt',b'untouched data')]:
        aurora.put_bytes(fs,path.lstrip('/'),data)
    fs.close();aurora.storage.grow(baseline,64)
    metadata={'disk':'fixture','chrome':{'entry':chrome_path,'entry_sha256':hashlib.sha256(old_chrome).hexdigest()},
        'hello':{'entry':hello_path,'entry_sha256':hashlib.sha256(old_hello).hexdigest()}}
    artifacts={'sbin/init':work/'desktop.elf','bin/aurora-app':work/'aurora_app.elf'}
    for path in artifacts.values():path.write_bytes(('new '+path.name).encode())
    result={name:True for name in ('aurora_desktop_verified','chrome_verified','legacy_apps_verified','normal_shutdown')}
    result.update(boot_sha256=aurora.digest(boot),desktop_sha256=aurora.digest(artifacts['sbin/init']),aurora_app_sha256=aurora.digest(artifacts['bin/aurora-app']))
    verification=work/'verified.json';verification.write_text(json.dumps(result))
    original_edit=fat.edit;original_replace=aurora.os.replace
    for case in ('success','corrupt-file','corrupt-reserved','artifact-change','boot-change','late-busy','metadata-failure'):
        disk=work/(case+'.img');shutil.copyfile(baseline,disk);disk.with_suffix('.json').write_text(json.dumps(metadata))
        before_hash=aurora.digest(disk);before=aurora.manifest(disk);protected=aurora.reserved(disk);evidence=work/(case+'-evidence')
        def idle(paths):
            # This fixture never starts QEMU; inject a late ownership conflict at publication.
            idle.calls+=1
            if case=='late-busy' and idle.calls==2:raise RuntimeError('simulated attached image')
        idle.calls=0
        def edit(argv):
            value=original_edit(argv)
            if case=='corrupt-file':
                changed=work/'changed';changed.write_bytes(b'corruption')
                original_edit(['fatput',argv[1],'put',str(changed),'home/nexus/keep.txt'])
            elif case=='corrupt-reserved':
                with Path(argv[1]).open('r+b') as stream:stream.seek(aurora.RESERVED_START+200);stream.write(b'x')
            elif case=='artifact-change':artifacts['bin/aurora-app'].write_bytes(b'changed after verification')
            elif case=='boot-change':boot.write_bytes(b'changed after verification')
            return value
        def replace(source,target):
            if case=='metadata-failure' and Path(target)==disk.with_suffix('.json'):raise OSError('simulated metadata rename failure')
            return original_replace(source,target)
        error=None
        try:
            with patch.object(aurora,'require_idle',side_effect=idle),patch.object(fat,'edit',side_effect=edit),patch.object(aurora.os,'replace',side_effect=replace):
                publication=aurora.publish(disk,legacy,evidence,verification,artifacts)
        except (ValueError,RuntimeError) as caught:error=caught
        finally:
            artifacts['bin/aurora-app'].write_bytes(b'new aurora_app.elf');boot.write_bytes(b'fake boot used only by host fixture')
        assert aurora.digest(legacy)==legacy_hash
        assert not list(work.glob('.aurora-*.img')),'Leaked staging image'
        if case=='success':
            assert error is None,error
            after=aurora.manifest(disk)
            assert all(after.get(path)==entry for path,entry in before.items() if path not in ('sbin/init','nexus/desktop.args'))
            assert aurora.reserved(disk)==protected
            fs=fat.Fat32(disk)
            try:
                prefix=publication['legacy']['guest_prefix'].lstrip('/')
                assert aurora.read(fs,'nexus/desktop.args')==b'--aurora\n'
                assert aurora.read(fs,'nexus/chrome-desktop.args')==(chrome_path+'\n--disable-gpu\nhttps://example.com/\n').encode()
                assert aurora.read(fs,prefix+'/fat/Downloads/old notes.txt')==b'legacy FAT content'
                assert aurora.read(fs,prefix+'/ramfs/Documents/public.txt')==b'public old notes'
                assert not any(path.startswith(prefix) and any(secret in path for secret in ('passwd.db','private.txt','Locked')) for path in after)
                imported=json.loads(aurora.read(fs,prefix+'/import.json'))
                assert any(entry['path']=='ramfs/etc/passwd.db' for entry in imported['excluded'])
                assert 'account hashes never enter guest' not in json.dumps(imported)
            finally:fs.f.close()
            with legacy.open('rb') as stream:stream.seek(aurora.RESERVED_START);assert (evidence/'legacy-reserved.bin').read_bytes()==stream.read(aurora.BASE-aurora.RESERVED_START)
            assert json.loads(disk.with_suffix('.json').read_text())['aurora']['verified'] is True
            with patch.object(aurora,'require_idle'):
                second=aurora.publish(disk,legacy,work/'repeat-evidence',verification,artifacts)
            assert second['legacy']['reused_existing_import'] is True
            repeated=aurora.manifest(disk)
            assert all(repeated.get(path)==entry for path,entry in after.items() if path not in ('sbin/init','bin/aurora-app','nexus/desktop.args')),'Repeat publication overwrote imported data or arguments'
        elif case=='metadata-failure':
            assert error is not None and 'recover its metadata' in str(error)
            after=aurora.manifest(disk)
            assert all(after.get(path)==entry for path,entry in before.items() if path not in ('sbin/init','nexus/desktop.args'))
            assert aurora.reserved(disk)==protected
            assert json.loads(disk.with_suffix('.json').read_text())==metadata
            assert json.loads((evidence/'prepared-desktop64.json').read_text())['aurora']['verified'] is True
        else:
            assert error is not None,case
            assert aurora.digest(disk)==before_hash,'Refusal changed original image'
            assert json.loads(disk.with_suffix('.json').read_text())==metadata
        print('PASS Aurora publication',case)
        disk.unlink();disk.with_suffix('.json').unlink()
    print('PASS snapshot validation, permission/secret exclusions, original/reserved preservation, atomic failures and repeat import')

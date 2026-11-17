"""Explicit Aurora build and offline publication; launching never modifies images.

publish requires successful integration evidence for the exact runtime artifacts.
One disposable sparse image is edited and verified before atomic replacement.
Legacy images stay intact; restricted RAMFS/account data stays in host evidence.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import tempfile
import zlib

HERE=Path(__file__).resolve().parent;ROOT=HERE.parents[1];OUT=HERE/'out'
BASE=16*1024*1024;RESERVED_START=20480*512;SNAPSHOT_START=26624*512;SLOT_BYTES=3072*512

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);value=importlib.util.module_from_spec(spec);spec.loader.exec_module(value);return value

fat=module('aurora_fat',ROOT/'userland/fatput.py')
storage=module('aurora_storage',ROOT/'storage.py')

def digest(path):
    with Path(path).open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()

def require_idle(paths):
    commands=subprocess.check_output(['powershell','-NoProfile','-Command',"Get-CimInstance Win32_Process -Filter \"Name LIKE 'qemu%'\" | Select-Object -ExpandProperty CommandLine"],text=True)
    commands=commands.replace('\\','/').lower()
    for path in paths:
        if str(Path(path).resolve()).replace('\\','/').lower() in commands:
            raise RuntimeError('QEMU still uses '+str(path))

def component(name):
    if not name or name in ('.','..') or any(ord(c)<32 or c in '/\\:' for c in name):raise ValueError('Unsafe legacy filename')
    return name

def first_cluster(entry):return struct.unpack_from('<H',entry,20)[0]<<16|struct.unpack_from('<H',entry,26)[0]

def read(fs,path):
    parts=path.strip('/').split('/');found=fs.find_name(fs.lookup_dir(parts[:-1]),parts[-1])
    if not found:raise ValueError('Missing '+path)
    entry=found[2]
    if entry[11]&16:raise ValueError('Expected file '+path)
    return fs.read_chain(first_cluster(entry),struct.unpack_from('<I',entry,28)[0])

def inventory(fs):
    records={};directories={};seen=set()
    def visit(cluster,prefix):
        if cluster in seen:raise ValueError('Directory cycle or alias')
        seen.add(cluster);directories[cluster]=prefix;names=set()
        for name,_,_,entry,_ in fs.named_entries(cluster):
            if entry[0]==0:break
            if entry[0]==0xe5 or entry[11]==15 or entry[11]&8 or name in ('.','..'):continue
            component(name)
            if name.lower() in names:raise ValueError('Duplicate FAT filename')
            names.add(name.lower());path=prefix+name
            if len(path.encode('utf-8'))>=512:raise ValueError('Legacy path exceeds native path capacity')
            record={'directory':bool(entry[11]&16),'metadata':(entry[:20]+entry[22:26]).hex(),'native_kind':entry[12]}
            records[path]=record
            if entry[11]&16:visit(first_cluster(entry),path+'/');continue
            size=struct.unpack_from('<I',entry,28)[0];remaining=size;hash=hashlib.sha256()
            for cluster in fs.chain(first_cluster(entry)) if size else ():
                count=min(remaining,fs.cl_bytes);hash.update(fs.rd(fs.cl_off(cluster),count));remaining-=count
            if remaining:raise ValueError('Truncated FAT file '+path)
            record.update(bytes=size,sha256=hash.hexdigest())
    visit(fs.rootc,'');return records,directories

def manifest(path):
    fs=fat.Fat32(path)
    try:return inventory(fs)[0]
    finally:fs.f.close()

def reserved(path):
    with Path(path).open('rb') as stream:stream.seek(512);return stream.read(BASE-512)

def snapshot_records(raw):
    """Validate both NXRS slots and use the same newest-valid-slot rule as persist.c."""
    valid=[];status=[]
    for index in range(2):
        slot=raw[index*SLOT_BYTES:(index+1)*SLOT_BYTES]
        if len(slot)!=SLOT_BYTES:raise ValueError('Truncated snapshot region')
        if not any(slot[:36]):status.append('empty');continue
        try:
            magic,version,generation,count,size,crc,tick,tombs,hcrc=struct.unpack_from('<9I',slot)
            if magic!=0x5352584e or version!=1 or count>4096 or tombs>count or size>SLOT_BYTES-512:raise ValueError('invalid header')
            if zlib.crc32(slot[:32])!=hcrc or zlib.crc32(slot[512:512+size])!=crc:raise ValueError('CRC mismatch')
            payload=slot[512:512+size];records=[];paths=set();offset=0;deleted=[]
            for i in range(count):
                if len(payload)-offset<52:raise ValueError('truncated record')
                parent,kind,mode,uid,gid,length,name=struct.unpack_from('<IBxHIII32s',payload,offset);offset+=52
                padded=(length+3)&~3
                if padded>len(payload)-offset:raise ValueError('truncated payload')
                data=payload[offset:offset+length];offset+=padded
                if kind==0x80:
                    if not data or data[-1:]!=b'\0' or b'\0' in data[:-1]:raise ValueError('invalid tombstone')
                    path=data[:-1].decode('utf-8')
                    if not path.startswith('/'):raise ValueError('relative tombstone')
                    for part in path[1:].split('/'):component(part)
                    deleted.append(path);records.append(None);continue
                if kind not in (1,2) or length>256*1024 or (kind==2 and length) or b'\0' not in name:raise ValueError('invalid record')
                leaf=component(name.split(b'\0',1)[0].decode('utf-8'))
                ancestor=None
                if parent!=0xffffffff:
                    if parent>=i or not records[parent] or not records[parent]['directory']:raise ValueError('invalid parent')
                    ancestor=records[parent]
                path=(ancestor['path']+'/' if ancestor else '')+leaf
                if path.lower() in paths or len(path.encode('utf-8'))>=512:raise ValueError('duplicate/oversized path')
                paths.add(path.lower());mode&=0o777
                traversable=(ancestor is None or ancestor['traversable']) and (kind!=2 or mode&5==5)
                readable=traversable and bool(mode&4)
                records.append({'path':path,'directory':kind==2,'mode':mode,'uid':uid,'gid':gid,'data':data,
                    'traversable':traversable,'readable':readable,'sha256':hashlib.sha256(data).hexdigest()})
            if offset!=size or len(deleted)!=tombs:raise ValueError('snapshot count/length mismatch')
            valid.append((generation,-index,records,deleted));status.append('valid')
        except (ValueError,UnicodeError,struct.error) as error:status.append(str(error))
    if not valid:
        if any(state!='empty' for state in status):raise ValueError('No valid legacy snapshot: '+repr(status))
        return [],{'slots':status,'selected':None,'tombstones':[]}
    generation,negative_index,records,deleted=max(valid,key=lambda item:item[:2])
    return [record for record in records if record],{'slots':status,'selected':-negative_index,'generation':generation,'tombstones':deleted}

def sensitive(path):
    return any(part.lower() in ('passwd.db','shadow','gshadow','.ssh','.gnupg') for part in path.split('/'))

def put_bytes(fs,path,data):
    parts=path.split('/');parent=fs.lookup_dir(parts[:-1],create=True)
    if fs.find_name(parent,parts[-1]):raise ValueError('Refusing to overwrite '+path)
    first=fs.alloc((len(data)+fs.cl_bytes-1)//fs.cl_bytes);offset=0
    for cluster in fs.chain(first) if data else ():
        block=data[offset:offset+fs.cl_bytes];fs.wr(fs.cl_off(cluster),block);offset+=len(block)
    fs.create_entry(parent,parts[-1],0x20,first,len(data));fs.flush_fat()

def import_legacy(source,target,evidence):
    source=Path(source).resolve();source_hash=digest(source)
    prefix='home/nexus/Legacy/'+source_hash
    with source.open('rb') as stream:
        stream.seek(RESERVED_START);raw=stream.read(BASE-RESERVED_START)
    if len(raw)!=BASE-RESERVED_START:raise ValueError('Legacy image lacks its reserved regions')
    # Account records and restricted data stay outside the guest until real identity/ACL mapping exists.
    (evidence/'legacy-reserved.bin').write_bytes(raw)
    records,snapshot=snapshot_records(raw[SNAPSHOT_START-RESERVED_START:])
    src=fat.Fat32(source);dst=fat.Fat32(target)
    try:
        original,directories=inventory(src);excluded=[]
        for path,record in original.items():
            if record['native_kind'] in (0xa1,0xa2) or sensitive(path):excluded.append({'path':'fat/'+path,'reason':'special inode or sensitive data'})
        blocked={item['path'][4:] for item in excluded}
        public=[record for record in records if record['readable'] and not sensitive(record['path'])]
        excluded.extend({'path':'ramfs/'+record['path'],'reason':'original read/traverse permission or sensitive data'} for record in records if record not in public)
        report={'source':str(source),'source_sha256':source_hash,'source_retained':True,'guest_prefix':'/'+prefix,
            'snapshot':snapshot,'excluded':excluded,'permissions':'World-readable RAMFS data only; original uid/gid/modes retained as metadata, not remapped ACLs.',
            'raw_reserved_host_path':str(evidence/'legacy-reserved.bin'),'raw_reserved_sha256':hashlib.sha256(raw).hexdigest()}
        host_report={**report,'fat_entries':original,'ramfs_records':[{k:v for k,v in record.items() if k!='data'} for record in records]}
        (evidence/'legacy-import.json').write_text(json.dumps(host_report,indent=2)+'\n')
        parent=dst.lookup_dir(['home','nexus','Legacy'],create=True)
        if dst.find_name(parent,source_hash):
            previous=json.loads(read(dst,prefix+'/import.json'))
            if previous.get('source_sha256')!=source_hash:raise ValueError('Legacy namespace collision')
            report['reused_existing_import']=True
        else:
            dst.mkdir_name(parent,source_hash);fat_root=dst.lookup_dir((prefix+'/fat').split('/'),create=True)
            def visible(cluster):
                for item in named_entries(cluster):
                    path=directories[cluster]+item[0]
                    if not any(path==entry or path.startswith(entry+'/') for entry in blocked):yield item
            named_entries=src.named_entries;src.named_entries=visible
            storage.migrate(src,dst,src.rootc,fat_root)
            # mkdir_name normalizes attributes; imported directory metadata must remain exact.
            for path,record in original.items():
                if not record['directory'] or any(path==entry or path.startswith(entry+'/') for entry in blocked):continue
                parts=(prefix+'/fat/'+path).split('/');parent=dst.lookup_dir(parts[:-1]);cluster,offset,entry=dst.find_name(parent,parts[-1])
                updated=bytearray(entry);updated[11]=bytes.fromhex(record['metadata'])[11];dst.put_entry(cluster,offset,updated)
            for record in public:
                path=prefix+'/ramfs/'+record['path']
                if len(path.encode('utf-8'))>=512:raise ValueError('Imported snapshot path exceeds native capacity')
                if record['directory']:dst.lookup_dir(path.split('/'),create=True)
                else:put_bytes(dst,path,record['data'])
            put_bytes(dst,prefix+'/import.json',(json.dumps(report,indent=2)+'\n').encode())
        imported,_=inventory(dst)
        for path,expected in original.items():
            if any(path==entry or path.startswith(entry+'/') for entry in blocked):continue
            guest=prefix+'/fat/'+path
            if len(guest.encode('utf-8'))>=512 or imported[guest]!=expected:raise ValueError('Legacy FAT migration mismatch: '+path)
        for record in public:
            if not record['directory'] and hashlib.sha256(read(dst,prefix+'/ramfs/'+record['path'])).hexdigest()!=record['sha256']:
                raise ValueError('Legacy snapshot migration mismatch')
        return report
    finally:
        src.f.close();dst.close()
        if digest(source)!=source_hash:raise ValueError('Legacy source changed during import')

def build():
    require_idle([OUT])
    sdk=OUT/'desktop-sdk'
    if not (sdk/'generated.json').is_file():raise RuntimeError('Prepare the verified Wayland SDK before building Aurora')
    desktop=module('aurora_desktop',HERE/'desktop.py');compiler=module('aurora_compiler',HERE/'build.py')
    desktop.build()
    sources=['aurora_app.c','aurora_extra.c','aurora_graphics.c','aurora_text.c','aurora_settings.c','aurora_settings_displays.c','aurora_settings_users.c','aurora_settings_keyboard.c','aurora_settings_a11y.c','aurora_settings_upgrade.c','aurora_settings_sound.c','aurora_auth.c','aurora_network.c','aurora_wifi.c','aurora_places.c','aurora_settings_appearance.c','aurora_dialogs.c','aurora_notifications.c','aurora_authdialog.c','aurora_a11y_client.c','aurora_clipboard_app.c']   # System Settings pages: aurora_settings.h
    legacy=['macui','gui','font','gfx','theme','appui','calculator','calendar','paint','minesweeper']
    compiler.run(compiler.compiler(),'cc','-target','x86_64-linux-gnu.2.41','-O2','-fPIE','-pie','-DNEXUS_HOSTED=1','-DNEXUS_AURORA=1',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wall','-Wextra','-Werror','-I',sdk/'usr/include','-I',sdk,*desktop.text_flags(),
        '-I',OUT/'protocol-sdk',*[HERE/name for name in sources],OUT/'protocol-sdk/fractional-scale-v1-protocol.c',OUT/'protocol-sdk/viewporter-protocol.c',*[ROOT/'kernel'/(name+'.c') for name in legacy],sdk/'xdg-shell-protocol.c',
        OUT/'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1',
        OUT/'chrome-root/usr/lib/x86_64-linux-gnu/libxkbcommon.so.0.0.0','-o',OUT/'aurora_app.elf')
    compiler.run(compiler.compiler(),'cc','-target','x86_64-linux-gnu.2.41','-O2','-fPIE','-pie','-DNEXUS_HOSTED=1','-DNEXUS_AURORA=1',
        '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Wall','-Wextra','-Werror',
        ROOT/'tests/aurora_extra_test.c','-o',OUT/'aurora_extra_fixed_test.elf')

def publish(disk,legacy,evidence,result_file,artifacts=None):
    disk=Path(disk);legacy=Path(legacy)
    if disk.is_symlink() or legacy.is_symlink():raise ValueError('Refusing image symlinks')
    disk=disk.resolve();legacy=legacy.resolve();evidence=Path(evidence).resolve()
    if disk==legacy or disk.samefile(legacy):raise ValueError('Desktop and legacy source must be separate images')
    artifacts=artifacts or {'sbin/init':OUT/'desktop.elf','bin/aurora-app':OUT/'aurora_app.elf'}
    expected_fields={'sbin/init':'desktop_sha256','bin/aurora-app':'aurora_app_sha256'}
    result=json.loads(Path(result_file).read_text())
    for field in ('aurora_desktop_verified','chrome_verified','legacy_apps_verified','normal_shutdown'):
        if result.get(field) is not True:raise ValueError('Missing verified integration gate: '+field)
    for guest,path in artifacts.items():
        if guest not in expected_fields or digest(path)!=result[expected_fields[guest]]:raise ValueError('Unverified runtime '+guest)
    verified_artifacts={guest:result[field] for guest,field in expected_fields.items()}
    if set(artifacts)!=set(expected_fields) or digest(OUT/'nexus64.img')!=result['boot_sha256']:raise ValueError('Unverified boot/runtime set')
    require_idle([disk,legacy,OUT/'nexus64.img'])
    if list(disk.parent.glob('.aurora-*.img')):raise RuntimeError('A prior Aurora staging image needs ownership review before retry')
    evidence.mkdir(parents=True,exist_ok=False)
    before_hash=digest(disk);legacy_hash=digest(legacy);before=manifest(disk);protected=reserved(disk)
    (evidence/'before-files.json').write_text(json.dumps(before,indent=2)+'\n')
    (evidence/'before-reserved.sha256').write_text(hashlib.sha256(protected).hexdigest()+'\n')
    metadata_path=disk.with_suffix('.json');metadata=json.loads(metadata_path.read_text())
    (evidence/'before-desktop64.json').write_text(json.dumps(metadata,indent=2)+'\n')
    fs=fat.Fat32(disk)
    try:
        arguments=read(fs,'nexus/desktop.args')
        if arguments.strip()==b'--aurora':chrome_arguments=read(fs,'nexus/chrome-desktop.args')
        else:chrome_arguments=arguments
        if not chrome_arguments or metadata['chrome']['entry'].encode() not in chrome_arguments:raise ValueError('No preserved normal Chrome arguments')
        for name in ('chrome','hello'):
            if hashlib.sha256(read(fs,metadata[name]['entry'])).hexdigest()!=metadata[name]['entry_sha256']:raise ValueError('Original application changed: '+name)
    finally:fs.f.close()
    fd,name=tempfile.mkstemp(prefix='.aurora-',suffix='.img',dir=disk.parent);os.close(fd);staging=Path(name);metadata_staging=None
    try:
        with disk.open('rb') as old,staging.open('w+b') as new:
            storage.sparse(new);new.truncate(disk.stat().st_size)
            while block:=old.read(1024*1024):
                if block.strip(b'\0'):new.write(block)
                else:new.seek(len(block),1)
            new.flush();os.fsync(new.fileno())
        if digest(staging)!=before_hash:raise ValueError('Staging copy mismatch')
        legacy_report=import_legacy(legacy,staging,evidence)
        (evidence/'chrome-desktop.args').write_bytes(chrome_arguments)
        (evidence/'desktop.args').write_bytes(b'--aurora\n')
        additions={**artifacts,'nexus/desktop.args':evidence/'desktop.args'}
        if 'nexus/chrome-desktop.args' not in before:additions['nexus/chrome-desktop.args']=evidence/'chrome-desktop.args'
        elif before['nexus/chrome-desktop.args']['sha256']!=hashlib.sha256(chrome_arguments).hexdigest():raise ValueError('Chrome argument backup mismatch')
        if fat.edit(['fatput',str(staging),*[value for guest,path in additions.items() for value in ('put',str(path),guest)]])!=0:raise ValueError('Runtime staging failed')
        after=manifest(staging)
        if any(after.get(path)!=record for path,record in before.items() if path not in additions):raise ValueError('An unrelated original file changed')
        if reserved(staging)!=protected:raise ValueError('Reserved bytes changed')
        for guest,path in additions.items():
            expected=verified_artifacts.get(guest,digest(path))
            if after[guest]['sha256']!=expected or digest(path)!=expected:raise ValueError('Runtime readback mismatch: '+guest)
        require_idle([disk,legacy,OUT/'nexus64.img'])
        if digest(disk)!=before_hash or digest(legacy)!=legacy_hash:raise ValueError('A source image changed during publication')
        if digest(OUT/'nexus64.img')!=result['boot_sha256']:raise ValueError('Boot image changed during publication')
        metadata.update({field:result[field] for field in ('boot_sha256','desktop_sha256','aurora_app_sha256')})
        metadata['aurora']={'verified':True,'integration_evidence':str(Path(result_file).resolve()),'publication_evidence':str(evidence),
            'legacy':legacy_report,'chrome_arguments':'/nexus/chrome-desktop.args'}
        (evidence/'after-files.json').write_text(json.dumps(after,indent=2)+'\n')
        encoded_metadata=(json.dumps(metadata,indent=2)+'\n').encode()
        (evidence/'prepared-desktop64.json').write_bytes(encoded_metadata)
        fd,name=tempfile.mkstemp(prefix='.aurora-metadata-',suffix='.json',dir=metadata_path.parent);metadata_staging=Path(name)
        with os.fdopen(fd,'wb') as stream:stream.write(encoded_metadata);stream.flush();os.fsync(stream.fileno())
        os.replace(staging,disk)
        # The prepared metadata and manifests permit recovery if this second small rename fails.
        try:os.replace(metadata_staging,metadata_path)
        except OSError as error:raise RuntimeError('Desktop image published; recover its metadata from '+str(evidence/'prepared-desktop64.json')) from error
        report={'disk':str(disk),'legacy_source':str(legacy),'legacy_source_sha256':legacy_hash,'original_entries_preserved':len(before)-sum(path in before for path in additions),
            'updated_paths':list(additions),'reserved_preserved':True,'legacy':legacy_report,'one_staging_image':True}
        (evidence/'result.json').write_text(json.dumps(report,indent=2)+'\n')
        print('PASS verified Aurora publication; original files and legacy source preserved')
        return report
    finally:
        staging.unlink(missing_ok=True)
        if metadata_staging is not None:metadata_staging.unlink(missing_ok=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('action',choices=('build','publish'))
    parser.add_argument('--disk',type=Path,default=OUT/'desktop64.img');parser.add_argument('--legacy',type=Path,default=ROOT/'disk.img')
    parser.add_argument('--evidence',type=Path);parser.add_argument('--verified-result',type=Path);args=parser.parse_args()
    if args.action=='build':build()
    else:
        if not args.evidence or not args.verified_result:parser.error('publish needs --evidence and --verified-result')
        publish(args.disk,args.legacy,args.evidence,args.verified_result)

"""Native NIC/ARP/IPv4/TCP receive-window and DMA-wrap test on a private disk."""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import socket
import struct
import subprocess
import sys
import threading
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-network')
WORK.mkdir(parents=True,exist_ok=True)
server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(100)
port=server.getsockname()[1]
errors=[];finished=[]
def serve():
    try:
        with server:
            connection,_=server.accept()
            with connection:
                connection.settimeout(100);request=b''
                while not request.endswith(b'\n') and len(request)<64:
                    data=connection.recv(64-len(request))
                    if not data:raise AssertionError('guest closed before request')
                    request+=data
                assert request==b'NEXUS64-NET\n',request
                block=bytes((i*37+11)&255 for i in range(65536))
                for _ in range(32):connection.sendall(block)
                connection.shutdown(socket.SHUT_WR)
                assert connection.recv(1)==b'', 'guest sent unexpected trailing data'
                finished.append(True)
    except BaseException as error:errors.append(repr(error))
thread=threading.Thread(target=serve,daemon=True);thread.start()
import os;disk=WORK/'disk.img';shutil.copyfile(OUT/('apps64-ext4.img' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'apps64.img'),disk)  # NEXUS_ROOT_FS=ext4: ext4 root
marker=WORK/'port';marker.write_bytes(struct.pack('<H',port))
spec=importlib.util.spec_from_file_location('network_fatput',ROOT/'userland'/('ext4put.py' if os.environ.get('NEXUS_ROOT_FS') == 'ext4' else 'fatput.py'));fatput=importlib.util.module_from_spec(spec);spec.loader.exec_module(fatput)
fatput.main(['fatput',str(disk),'put',str(marker),'nexus/test-network'])
before=hashlib.sha256(disk.read_bytes()).digest()
serial=WORK/'serial.log';serial.unlink(missing_ok=True)
try:
    result=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe',*(['-accel','whpx,kernel-irqchip=off','-cpu','qemu64,+rdrand'] if os.environ.get('NEXUS_ACCEL') == 'whpx' else ['-cpu','max']),'-m','128',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}',
        '-netdev','user,id=network','-device','rtl8139,netdev=network',
        '-object',f'filter-dump,id=capture,netdev=network,file={WORK/"network.pcap"}',
        '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=110)
    log=serial.read_text();thread.join(timeout=5)
    assert result.returncode==33 and '[64] NETWORK CAMPAIGN PASSED' in log,(result.stdout,result.stderr,log)
    assert '[64] NATIVE NETWORK 2 MIB STREAM PASSED' in log and 'RTL8139 native DMA/IRQ ready' in log,log
    assert 'KERNEL_FAULT' not in log and 'USER_FAULT' not in log and '[64] FAIL:' not in log,log
    assert finished and not errors,(finished,errors)
    assert hashlib.sha256(disk.read_bytes()).digest()==before,'network DMA/test altered application disk'
    assert (WORK/'network.pcap').stat().st_size>2*1024*1024
finally:
    server.close()
print('PASS native RTL8139/ARP/IPv4/TCP: exact 2 MiB, receive-window updates, DMA wrapping, FIN/ownership and unchanged disk')

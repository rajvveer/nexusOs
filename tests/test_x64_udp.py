"""Native glibc UDP sockets and DNS, with actual QEMU Ethernet and private disks."""
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

ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'arch/x86_64/out'
WORK=Path(sys.argv[1]) if len(sys.argv)>1 else nexus_paths.runs('native64-udp')
WORK.mkdir(parents=True,exist_ok=True)
subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-linux-gnu.2.36',
    '-O2','-fPIE','-pie','-pthread','-Wall','-Wextra','-Werror',str(ROOT/'arch/x86_64/udp_user.c'),'-o',str(WORK/'udp-user.elf')],check=True)
server=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);server.bind(('127.0.0.1',0));server.settimeout(.25)
alternate=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);alternate.bind(('127.0.0.1',0))
stop=threading.Event();received=[];errors=[]
def serve():
    try:
        while not stop.is_set():
            try:data,peer=server.recvfrom(65535)
            except socket.timeout:continue
            received.append(data)
            if data==b'filter':alternate.sendto(b'bad-peer',peer);data=b'good'
            server.sendto(data,peer)
    except BaseException as error:
        if not stop.is_set():errors.append(repr(error))
thread=threading.Thread(target=serve,daemon=True);thread.start()
disk=WORK/'disk.img';shutil.copyfile(OUT/'apps64.img',disk)
port=WORK/'port';port.write_bytes(struct.pack('<H',server.getsockname()[1]))
spec=importlib.util.spec_from_file_location('udp_fatput',ROOT/'userland/fatput.py');fatput=importlib.util.module_from_spec(spec);spec.loader.exec_module(fatput)
fatput.main(['fatput',str(disk),'put',str(port),'nexus/udp-port','put',str(WORK/'udp-user.elf'),'sbin/init'])
before=hashlib.sha256(disk.read_bytes()).digest();serial=WORK/'serial.log';serial.unlink(missing_ok=True)
try:
    result=subprocess.run([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-cpu','max','-m','128',
        '-drive',f'file={OUT/"nexus64.img"},format=raw,if=floppy,readonly=on','-drive',f'file={disk},format=raw,if=ide',
        '-boot','a','-display','none','-vga','none','-device','virtio-vga','-serial',f'file:{serial}',
        '-netdev','user,id=network','-device','rtl8139,netdev=network',
        '-object',f'filter-dump,id=capture,netdev=network,file={WORK/"network.pcap"}',
        '-no-reboot','-device','isa-debug-exit,iobase=0xf4,iosize=4'],capture_output=True,timeout=90)
    log=serial.read_text()
    assert result.returncode==33 and '[64] INIT SESSION RETURNED' in log,(result.stdout,result.stderr,log)
    for marker in ('UDP MTU, VECTORS, PEEK/TRUNCATION, EMPTY PACKETS, SOURCE AND CONNECT/DISCONNECT PASSED',
        'UDP 32 CONCURRENT DESCRIPTORS AND EPOLL PASSED','UDP BLOCKING TIMEOUT AND SIGNAL PASSED',
        'SENDMMSG UNIX RIGHTS, RECORD BOUNDARIES AND BATCH LIMIT PASSED','GLIBC DNS RESOLVER AND NXDOMAIN PASSED','UDP SOCKET CAMPAIGN PASSED'):
        assert marker in log,(marker,log)
    assert 'USER_FAULT' not in log and 'KERNEL_FAULT' not in log and '[64] FAIL:' not in log,log
    assert not errors and b'orphan' in received and b'' in received,(errors,[len(data) for data in received])
    assert bytes((i*37+11)&255 for i in range(1472)) in received
    assert all(struct.pack('<I',0xABCD0000|i) in received for i in range(32))
    capture=(WORK/'network.pcap').read_bytes();offset=24;questions=[]
    while offset<len(capture):
        _,_,size,_=struct.unpack_from('<IIII',capture,offset);frame=capture[offset+16:offset+16+size];offset+=16+size
        if len(frame)<42 or frame[12:14]!=b'\x08\x00' or frame[23]!=17:continue
        udp=14+(frame[14]&15)*4;source,destination,length=struct.unpack_from('!HHH',frame,udp)
        if destination==53 and frame[30:34]==b'\x0a\x00\x02\x03':questions.append(frame[udp+8:udp+length])
    assert any(b'\x07example\x03com\0' in query for query in questions),'resolver did not send DNS through native Ethernet'
    assert hashlib.sha256(disk.read_bytes()).digest()==before,'UDP campaign changed the application disk'
finally:
    stop.set();thread.join(timeout=1);server.close();alternate.close()
print('PASS native UDP: checksummed full MTU, datagram/vector/peek/truncation/empty semantics, 32 concurrent sockets, epoll, timeout/signal, orphan delivery and real glibc DNS/NXDOMAIN')

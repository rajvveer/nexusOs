"""QMP over a host-local Unix socket, including Windows Python without AF_UNIX."""
from pathlib import Path
import ctypes
import json
import select
import socket
import time
import uuid

ws=ctypes.WinDLL('ws2_32')
ws.connect.argtypes=[ctypes.c_size_t,ctypes.c_void_p,ctypes.c_int]
ws.connect.restype=ctypes.c_int
ws.WSAGetLastError.argtypes=[]
ws.WSAGetLastError.restype=ctypes.c_int


class Address(ctypes.Structure):
    _fields_=[('family',ctypes.c_ushort),('path',ctypes.c_char*108)]


class QmpMonitor:
    def __init__(self,work=None,path=None,timeout=5):
        if (work is None)==(path is None):raise ValueError('Specify work directory or existing socket path')
        self.path=Path(path) if path is not None else Path(work)/('qmp-'+uuid.uuid4().hex[:12]+'.sock')
        self.path=self.path.resolve()
        encoded=str(self.path).encode('utf-8')
        if len(encoded)>=108 or any(c in str(self.path) for c in (',','\0')):raise ValueError('Invalid or overlong QMP socket path')
        self.address=Address(1,encoded)
        self.argument=f'unix:{self.path},server=on,wait=off'
        self.timeout=timeout
        self.socket=None
        self.buffer=bytearray()
        self.sequence=0
        self.owns_path=path is None
        if self.owns_path and self.path.exists():raise FileExistsError(self.path)

    def open(self):
        if self.socket is not None:raise RuntimeError('QMP already open')
        deadline=time.monotonic()+self.timeout
        while True:
            peer=socket.socket(1,socket.SOCK_STREAM)
            try:
                peer.setblocking(False)
                result=ws.connect(peer.fileno(),ctypes.byref(self.address),ctypes.sizeof(self.address))
                error=ws.WSAGetLastError() if result else 0
                if error in (10035,10036):
                    remaining=max(0,deadline-time.monotonic())
                    _,writable,failed=select.select([],[peer],[peer],remaining)
                    error=peer.getsockopt(socket.SOL_SOCKET,socket.SO_ERROR) if writable or failed else 10060
            except BaseException:peer.close();raise
            if not error:break
            peer.close()
            if error not in (2,3,10049,10061):raise OSError(error,'QMP local socket connect failed')
            if time.monotonic()>=deadline:raise TimeoutError('QMP local socket unavailable')
            time.sleep(.01)
        self.socket=peer
        try:
            self.buffer.clear()
            if 'QMP' not in self._read(deadline):raise RuntimeError('Missing QMP greeting')
            self.command('qmp_capabilities')
            return self
        except BaseException:self.close();raise

    def _read(self,deadline):
        while True:
            newline=self.buffer.find(b'\n')
            if newline>=0:
                line=bytes(self.buffer[:newline]);del self.buffer[:newline+1]
                if line.strip():return json.loads(line)
                continue
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('QMP response deadline')
            self.socket.settimeout(remaining)
            block=self.socket.recv(65536)
            if not block:raise EOFError('QMP disconnected')
            self.buffer.extend(block)

    def command(self,command,arguments=None):
        if self.socket is None:raise RuntimeError('QMP is not open')
        self.sequence+=1
        request={'execute':command,'id':self.sequence}
        if arguments is not None:request['arguments']=arguments
        deadline=time.monotonic()+self.timeout
        self.socket.settimeout(self.timeout)
        self.socket.sendall(json.dumps(request).encode()+b'\n')
        while True:
            reply=self._read(deadline)
            if reply.get('id')!=self.sequence:continue
            if 'error' in reply:raise RuntimeError(reply['error'])
            if 'return' in reply:return reply['return']

    def close(self):
        if self.socket is not None:self.socket.close();self.socket=None

    def cleanup(self,process):
        if process.poll() is None:raise RuntimeError('Cannot remove a live VM control socket')
        self.close()
        if self.owns_path:self.path.unlink(missing_ok=True)

    def __enter__(self):return self
    def __exit__(self,*args):self.close()

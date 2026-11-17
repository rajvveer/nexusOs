"""Run production networking code against deterministic host-side fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='nexus-network-') as directory:
    for source in ('tcp_native_test','net_wire_test','udp_wire_test','datagram_native_test','rtl8139_native_test','arp_native_test'):
        executable=Path(directory)/(source+'.exe')
        subprocess.run([str(ROOT/'tools/zig-x86_64-windows-0.15.2/zig.exe'),'cc','-target','x86_64-windows-gnu',
            '-std=c11','-O2','-Wall','-Wextra','-Werror',str(ROOT/'tests'/(source+'.c')),'-o',str(executable)],check=True)
        subprocess.run([str(executable)],check=True,timeout=15)

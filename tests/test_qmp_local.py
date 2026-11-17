"""Verify actual QEMU local control, including paused VM operation and reconnect."""
from pathlib import Path
import importlib.util
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

HERE=Path(__file__).resolve().parent
WORK=nexus_paths.runs('qmp-local');WORK.mkdir(exist_ok=True)
spec=importlib.util.spec_from_file_location('qmp_local',HERE/'qmp_local.py');qmp=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp)
monitor=qmp.QmpMonitor(WORK)
process=subprocess.Popen([r'C:\Program Files\qemu\qemu-system-x86_64.exe','-machine','none','-nodefaults','-display','none','-S','-qmp',monitor.argument],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
try:
    with monitor.open():
        assert monitor.command('query-status')['status']=='prelaunch'
        try:monitor.command('nexus-invalid-command')
        except RuntimeError as error:assert 'CommandNotFound' in str(error)
        else:raise AssertionError('invalid command accepted')
        try:monitor.cleanup(process)
        except RuntimeError:pass
        else:raise AssertionError('live socket removed')
    with monitor.open():
        assert monitor.command('query-status')['status']=='prelaunch'
        monitor.command('quit')
    output,error=process.communicate(timeout=5)
    assert process.returncode==0,(output,error)
finally:
    monitor.close()
    if process.poll() is None:process.kill();process.wait()
    monitor.cleanup(process)
assert not monitor.path.exists()
print('PASS QMP local socket: paused commands, error, reconnect and cleanup without a TCP listener')

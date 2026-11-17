"""The interactive Chrome launcher must reuse the installed disk without rebuilding it.
QEMU is never started: subprocess.Popen (the launcher) and subprocess.run (a rebuild) are both mocked."""
from pathlib import Path
import json
import runpy
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
script=ROOT/'arch/x86_64/build.py'
with patch('sys.argv',[str(script),'chrome']),patch.object(Path,'is_file',return_value=False),patch('subprocess.run') as execute,patch('subprocess.Popen') as launch:
    try:runpy.run_path(str(script),run_name='__main__')
    except SystemExit as error:assert 'No installed desktop image' in str(error)
    else:raise AssertionError('missing installed image accepted')
    execute.assert_not_called();launch.assert_not_called()
metadata=json.dumps({'aurora':{'verified':True}})
with patch('sys.argv',[str(script),'chrome']),patch.object(Path,'is_file',return_value=True),patch.object(Path,'read_text',return_value=metadata),patch('subprocess.run') as execute,patch('subprocess.Popen') as launch:
    launch.return_value.wait.return_value=33;launch.return_value.pid=4321
    runpy.run_path(str(script),run_name='__main__')
    execute.assert_not_called()
    launch.assert_called_once()
    args=launch.call_args.args[0]
    assert Path(args[0]).name=='qemu-system-x86_64.exe'
    assert args[args.index('-m')+1]=='2048' and args[args.index('-display')+1]=='sdl'
    drives=[args[i+1] for i,value in enumerate(args) if value=='-drive']
    assert any(value.startswith('if=none,id=root,file=') and value.endswith('desktop64.img,format=raw') and 'snapshot' not in value for value in drives)
    assert 'nvme,serial=nexus-root,drive=root' in args
    assert any('nexus64.img,format=raw,if=floppy,readonly=on' in value for value in drives)
    assert 'e1000e,netdev=network' in args and 'isa-debug-exit,iobase=0xf4,iosize=4' in args
    assert 'name=opt/nexus/session,string=chrome' in args
print('PASS Chrome launch: missing-image refusal, persistent installed disk (NVMe), native boot, network and interactive display; no rebuild')

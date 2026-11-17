"""Probe lifecycle failures use tiny fixtures and mocked QEMU; no OS image is used."""
from pathlib import Path
from types import ModuleType, SimpleNamespace
from unittest.mock import patch
import contextlib
import importlib.util
import io
import json
import shutil
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent)); import nexus_paths  # noqa: E402

ROOT=Path(__file__).resolve().parents[1]
WORK=nexus_paths.runs('probe-cleanup-check');WORK.mkdir(parents=True,exist_ok=True)
copyfile=shutil.copyfile;unlink=Path.unlink
spec_from_file=importlib.util.spec_from_file_location;module_from_spec=importlib.util.module_from_spec

class InjectedFailure(Exception):pass

def check(script,case,mode=None):
    desktop=script=='probe_desktop64.py';mode=mode or ('software' if desktop else 'headless')
    source=(ROOT/'tests'/script).read_text()
    with tempfile.TemporaryDirectory(prefix='case-',dir=WORK) as temporary:
        fixture=Path(temporary).resolve();assert fixture.is_relative_to(WORK.resolve())
        out=fixture/'arch/x86_64/out';out.mkdir(parents=True)
        work=fixture/'work';work.mkdir()
        original=b'Only a tiny fake OS image.\n'
        (out/'chrome64.img').write_bytes(original)
        private=work/('disk.img' if desktop else 'probe-disk.img')
        permanent=fixture/'permanent.img';permanent.write_bytes(original)
        package={'name':'fixture','filename':'fixture.deb','bytes':1,'sha256':'0'*64,'url':'https://example.invalid/fixture.deb'}
        (out.parent/'download-lock.json').write_text(json.dumps(package))
        if mode.startswith('installed-'):
            (work/'installation-result.json').write_text(json.dumps({'entry':'/home/nexus/Applications/chrome/opt/google/chrome/chrome',
                'package':package,'bootstrap_executable_removed':True,'disk':str(permanent)}))
        if case=='existing':private.write_bytes(b'Existing image must survive.\n')
        events=[];process=None;staged=[]

        class FakeProcess:
            pid=123;returncode=None
            def poll(self):
                if case in ('running','cannot-stop','keep-download') and 'observed' not in events:
                    events.append('observed');raise InjectedFailure('observation')
                return self.returncode
            def communicate(self,timeout=None):
                if case in ('running','cannot-stop'):
                    events.append('observed');raise InjectedFailure('observation')
                self.returncode=33;return (b'',None)
            def kill(self):
                events.append('kill')
                if case=='cannot-stop':raise InjectedFailure('cannot stop VM')
            def wait(self,timeout=None):events.append('wait');self.returncode=-9;return self.returncode

        def launch(args,**kwargs):
            nonlocal process
            assert str(fixture) in ' '.join(args),'A real image escaped the fixture'
            events.append('launch')
            if case=='popen':raise InjectedFailure('Popen')
            serial=Path(next(arg[5:] for arg in args if arg.startswith('file:')))
            serial.write_text('[64] CHROME PROBE RETURNED\n<html></html>\n'+('[64] CHROME DOWNLOAD VERIFIED fixture\n' if case=='keep-download' else ''))
            process=FakeProcess()
            if case=='success':process.returncode=33
            return process

        def copy(source,target):
            assert Path(source).resolve().is_relative_to(fixture) and Path(target)==private
            events.append('copy')
            if case=='copy':private.write_bytes(b'partial');raise InjectedFailure('partial copy')
            return copyfile(source,target)

        def remove(path,*args,**kwargs):
            if path==private:
                assert process is None or process.returncode is not None,'Deleted an attached image'
                events.append('unlink')
            assert path!=permanent and path!=out/'chrome64.img','Deleted a permanent image'
            return unlink(path,*args,**kwargs)

        def stage(kind,argv):
            staged.append(kind)
            assert Path(argv[1])==(permanent if mode.startswith('installed-') else private)
            if case=='stage':raise InjectedFailure('staging')
            return 0

        class Control:
            def __init__(self,directory):self.path=Path(directory)/'qmp.sock';self.argument='mocked-local-qmp'
            def cleanup(self,p):assert p.returncode is not None;events.append('qmp-cleanup')

        class Loader:
            def __init__(self,name):self.name=name
            def exec_module(self,module):
                if 'fatput' in self.name:
                    if case=='import':raise InjectedFailure('fatput import')
                    module.main=lambda argv:stage('main',argv)
                    module.edit=lambda argv:stage('edit',argv)
                else:module.QmpMonitor=Control

        def spec(name,path):
            if 'fatput' in name or name=='qmp_local':return SimpleNamespace(name=name,loader=Loader(name))
            return spec_from_file(name,path)

        class Server:
            server_port=12345
            def __init__(self,*args):pass
            def serve_forever(self):pass
            def shutdown(self):events.append('server-stop')
            def server_close(self):events.append('server-close')
        class Thread:
            def __init__(self,**kwargs):self.started=False
            def start(self):self.started=True
            def is_alive(self):return self.started

        error=None
        with patch('sys.argv',[script,str(work),mode]),patch('shutil.copyfile',side_effect=copy),patch.object(Path,'unlink',remove), \
             patch('importlib.util.spec_from_file_location',side_effect=spec), \
             patch('importlib.util.module_from_spec',side_effect=lambda s:ModuleType(s.name) if isinstance(s,SimpleNamespace) else module_from_spec(s)), \
             patch('subprocess.Popen',side_effect=launch),patch('http.server.ThreadingHTTPServer',Server),patch('threading.Thread',Thread), \
             contextlib.redirect_stdout(io.StringIO()):
            try:exec(compile(source,str(ROOT/'tests'/script),'exec'),{'__file__':str(fixture/'tests'/script),'__name__':'__main__'})
            except (InjectedFailure,FileExistsError) as failure:error=failure
        if case=='success':assert error is None
        else:assert error is not None,case
        if case=='existing':
            assert isinstance(error,FileExistsError) and private.read_bytes()==b'Existing image must survive.\n'
            assert not events,'Existing image was touched'
        elif case in ('cannot-stop','keep-download'):
            assert private.read_bytes()==original and 'unlink' not in events
        else:assert not private.exists(),(script,case,events)
        if case=='running':assert events.index('kill')<events.index('wait')<events.index('unlink')
        if case=='keep-download':assert events.index('kill')<events.index('wait') and 'server-close' in events
        if staged:assert set(staged)==({'main'} if mode.startswith('installed-') else {'edit'}),staged
        assert permanent.read_bytes()==original and (out/'chrome64.img').read_bytes()==original
        print('PASS',script,mode,case)

for script in ('probe_desktop64.py','test_chrome64.py'):
    for case in ('copy','import','stage','popen','running','cannot-stop','existing','success'):check(script,case)
check('probe_desktop64.py','popen','installed-https')
check('probe_desktop64.py','keep-download','download')
check('test_chrome64.py','popen','version')
source=(ROOT/'tests/test_chrome64.py').read_text()
assert "arguments.write_text('--headless\\n" in source and '\nstart = time.monotonic()\n' in source
print('PASS 19 lifecycle cases: partial copy/import/staging/spawn/VM failures, ownership, retention and no extra private staging clone')

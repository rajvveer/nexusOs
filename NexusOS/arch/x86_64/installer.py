"""Build NexusOS's user application installer, reusing the staged XZ runtime."""
from pathlib import Path, PurePosixPath
import importlib.util
import io
import json
import tarfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
OUT=HERE/'out'
SDK=OUT/'installer-sdk'

def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result

def build():
    chrome=module('installer_chrome',HERE/'chrome.py')
    compiler=module('installer_compiler',HERE/'build.py')
    for package in json.loads((HERE/'installer-sdk-lock.json').read_text()):
        with tarfile.open(fileobj=io.BytesIO(chrome.tar_payload(chrome.fetch(package)))) as archive:
            for member in archive:
                name=chrome.safe_name(member.name)
                if not member.isfile() or not (name.startswith('usr/include/') or name.endswith('/copyright')):continue
                target=SDK.joinpath(*PurePosixPath(name).parts);target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
    compiler.run(compiler.compiler(),'cc','-target','x86_64-linux-gnu.2.41','-O2','-fPIE','-pie','-DNEXUS_HOSTED=1',
        '-Wall','-Wextra','-Werror','-I',SDK/'usr/include',HERE/'install_user.c',ROOT/'kernel/crypto.c',
        OUT/'chrome-root/usr/lib/x86_64-linux-gnu/liblzma.so.5.8.1','-o',OUT/'nexus-install.elf')

def build_ui():
    desktop=module('installer_desktop',HERE/'desktop.py');desktop.prepare()
    compiler=module('installer_ui_compiler',HERE/'build.py');sdk=OUT/'desktop-sdk'
    compiler.run(compiler.compiler(),'cc','-target','x86_64-linux-gnu.2.41','-O2','-fPIE','-pie','-DNEXUS_HOSTED=1',
        '-Wall','-Wextra','-Werror','-I',sdk/'usr/include','-I',sdk,HERE/'installer_ui.c',sdk/'xdg-shell-protocol.c',ROOT/'kernel/crypto.c',
        OUT/'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1','-o',OUT/'nexus-installer-ui.elf')

if __name__=='__main__':build();build_ui()

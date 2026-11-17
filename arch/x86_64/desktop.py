"""Prepare verified Wayland/xkb headers and generate bindings on the native kernel.

Runs the unmodified Wayland scanner as an application on NexusOS.
Application libraries are reused from the existing Chrome staging; no foreign OS.
"""
from pathlib import Path, PurePosixPath
import hashlib
import importlib.util
import io
import json
import re
import shutil
import subprocess
import tarfile
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
SDK = OUT / 'desktop-sdk'
WORK = nexus_paths.runs('native64-protocols')


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec); spec.loader.exec_module(result); return result


def prepare():
    chrome = module('desktop_chrome', HERE / 'chrome.py')
    build = module('desktop_build', HERE / 'build.py')
    SDK.mkdir(exist_ok=True)
    for package in json.loads((HERE / 'desktop-sdk-lock.json').read_text()):
        path = chrome.fetch(package)
        with tarfile.open(fileobj=io.BytesIO(chrome.tar_payload(path))) as archive:
            for member in archive:
                name = chrome.safe_name(member.name)
                if not member.isfile() or not (name.startswith('usr/include/') or name == 'usr/bin/wayland-scanner' or
                    name == 'usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml' or name.endswith('/copyright')): continue
                destination = SDK.joinpath(*PurePosixPath(name).parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(archive.extractfile(member).read())
    generated = ['xdg-shell-server-protocol.h', 'xdg-shell-client-protocol.h', 'xdg-shell-protocol.c']
    manifest = SDK / 'generated.json'
    source_hash = hashlib.sha256((SDK / 'usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml').read_bytes()).hexdigest()
    generator_hash = hashlib.sha256((SDK / 'usr/bin/wayland-scanner').read_bytes()).hexdigest()
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if previous.get('source') == source_hash and previous.get('generator') == generator_hash and all((SDK / name).exists() and hashlib.sha256((SDK / name).read_bytes()).hexdigest() == previous.get(name) for name in generated):
            return
    WORK.mkdir(parents=True, exist_ok=True)
    build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
              HERE / 'protocols_user.c', '-o', SDK / 'protocols.elf')
    disk = WORK / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = module('desktop_fatput', ROOT / 'userland/fatput.py')
    runtime = OUT / 'chrome-root/usr/lib/x86_64-linux-gnu'
    libraries = {}
    def dependencies(path):
        text = subprocess.check_output([str(ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'), '-d', str(path)], text=True)
        for name in re.findall(r'Shared library: \[(.*?)\]', text):
            if PurePosixPath(name).name != name: raise ValueError('Invalid library name')
            if name in libraries: continue
            candidates = [runtime / name] if (runtime / name).exists() else list(runtime.glob(name + '.*'))
            if len(candidates) != 1: raise ValueError('Missing/ambiguous library ' + name)
            libraries[name] = candidates[0]; dependencies(candidates[0])
    dependencies(SDK / 'usr/bin/wayland-scanner')
    install = ['fatput', str(disk), 'put', str(SDK / 'protocols.elf'), 'sbin/init',
                'put', str(SDK / 'usr/bin/wayland-scanner'), 'bin/wayland-scanner',
                'put', str(SDK / 'usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'), 'etc/xdg-shell.xml']
    for name, path in sorted(libraries.items()): install += ['put', str(path), 'lib/x86_64-linux-gnu/' + name]
    fatput.main(install)
    serial = WORK / 'serial.log'; serial.unlink(missing_ok=True)
    p = subprocess.run([str(build.QEMU), '-cpu', 'max', '-m', '128',
        '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
        '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
        '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], timeout=60)
    log = serial.read_text()
    if p.returncode != 33 or '[64] PROTOCOL GENERATION PASSED' not in log or '[64] INIT SESSION RETURNED' not in log:
        raise AssertionError(log)
    if 'KERNEL_FAULT' in log or 'USER_FAULT' in log: raise AssertionError(log)
    fs = fatput.Fat32(disk)
    try:
        directory = fs.lookup_dir(['home','nexus'])
        for name in generated:
            entry = fs.find_name(directory, name)
            if entry is None: raise AssertionError('Missing generated ' + name)
            record = entry[2]
            first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
            size = int.from_bytes(record[28:32], 'little')
            data = fs.read_chain(first, size)
            assert len(data) == size and b'Generated by wayland-scanner' in data and b'xdg_wm_base' in data
            (SDK / name).write_bytes(data)
    finally: fs.close()
    manifest.write_text(json.dumps({'source':source_hash, 'generator':generator_hash, **{name:hashlib.sha256((SDK / name).read_bytes()).hexdigest() for name in generated}}, indent=2) + '\n')
    print('PASS official Wayland bindings generated on native NexusOS', flush=True)


# Protocols Aurora implements beyond xdg-shell (Wine's winewayland.drv, Xwayland, GTK/Qt/SDL clients). The XMLs come
# from the pinned wayland-protocols package of desktop-sdk-lock.json; the bindings are generated by the same verified
# wayland-scanner running on native NexusOS, into OUT/protocol-sdk (outside the shared desktop-sdk tree).
PROTOCOL_SDK = OUT / 'protocol-sdk'
EXTRA_PROTOCOLS = {
    'viewporter': 'stable/viewporter/viewporter.xml',
    'pointer-constraints-unstable-v1': 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
    'relative-pointer-unstable-v1': 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
    'xdg-output-unstable-v1': 'unstable/xdg-output/xdg-output-unstable-v1.xml',
    'xdg-decoration-unstable-v1': 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
    'xwayland-shell-v1': 'staging/xwayland-shell/xwayland-shell-v1.xml',
    'fractional-scale-v1': 'staging/fractional-scale/fractional-scale-v1.xml',   # HiDPI: per-surface preferred scale (desk)
    'text-input-unstable-v3': 'unstable/text-input/text-input-unstable-v3.xml',  # input methods (desk)
    'xdg-activation-v1': 'staging/xdg-activation/xdg-activation-v1.xml',         # app-requested activation, Wine restore (desk)
    'idle-inhibit-unstable-v1': 'unstable/idle-inhibit/idle-inhibit-unstable-v1.xml',   # video/presentation idle inhibitors (desk2)
}
# Protocols that are not in wayland-protocols (input methods: wlroots' official repository), kept in the tree and pinned
# by size + SHA-256 in protocols/protocols-lock.json.
LOCAL_PROTOCOLS = {
    'input-method-unstable-v2': 'protocols/input-method-unstable-v2.xml',
    'virtual-keyboard-unstable-v1': 'protocols/virtual-keyboard-unstable-v1.xml',
}
ALL_PROTOCOLS = {**EXTRA_PROTOCOLS, **LOCAL_PROTOCOLS}


def prepare_protocols():
    chrome = module('desktop_chrome', HERE / 'chrome.py')
    build = module('desktop_build', HERE / 'build.py')
    package = next(p for p in json.loads((HERE / 'desktop-sdk-lock.json').read_text()) if p['name'] == 'wayland-protocols')
    sources = {}
    with tarfile.open(fileobj=io.BytesIO(chrome.tar_payload(chrome.fetch(package)))) as archive:
        for member in archive:
            name = chrome.safe_name(member.name)
            for key, relative in EXTRA_PROTOCOLS.items():
                if member.isfile() and name == 'usr/share/wayland-protocols/' + relative: sources[key] = archive.extractfile(member).read()
    if sorted(sources) != sorted(EXTRA_PROTOCOLS): raise ValueError('wayland-protocols lacks ' + ', '.join(sorted(set(EXTRA_PROTOCOLS) - set(sources))))
    pins = {entry['name']: entry for entry in json.loads((HERE / 'protocols/protocols-lock.json').read_text())}
    for key, relative in LOCAL_PROTOCOLS.items():
        data = (HERE / relative).read_bytes(); pin = pins[PurePosixPath(relative).name]
        if len(data) != pin['bytes'] or hashlib.sha256(data).hexdigest() != pin['sha256']: raise ValueError('unpinned protocol file ' + relative)
        sources[key] = data
    scanner = SDK / 'usr/bin/wayland-scanner'
    outputs = [key + '-' + suffix for key in ALL_PROTOCOLS for suffix in ('server-protocol.h', 'client-protocol.h', 'protocol.c')]
    expected = {'package': package['sha256'], 'generator': hashlib.sha256(scanner.read_bytes()).hexdigest(),
                'sources': {key: hashlib.sha256(data).hexdigest() for key, data in sorted(sources.items())}}
    manifest = PROTOCOL_SDK / 'generated.json'
    if manifest.exists():
        previous = json.loads(manifest.read_text())
        if all(previous.get(k) == v for k, v in expected.items()) and all((PROTOCOL_SDK / name).exists() and
                hashlib.sha256((PROTOCOL_SDK / name).read_bytes()).hexdigest() == previous.get('outputs', {}).get(name) for name in outputs):
            return
    (PROTOCOL_SDK / 'xml').mkdir(parents=True, exist_ok=True)
    for key, data in sources.items(): (PROTOCOL_SDK / 'xml' / (key + '.xml')).write_bytes(data)
    work = PROTOCOL_SDK / 'work'; work.mkdir(exist_ok=True)
    build.run(build.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
              HERE / 'protocols_user.c', '-o', work / 'protocols.elf')
    (work / 'protocols.list').write_bytes(''.join(f'{key} /etc/protocols/{key}.xml\n' for key in ALL_PROTOCOLS).encode())
    disk = work / 'disk.img'; shutil.copyfile(OUT / 'apps64.img', disk)
    fatput = module('desktop_fatput', ROOT / 'userland/fatput.py')
    runtime = OUT / 'chrome-root/usr/lib/x86_64-linux-gnu'
    libraries = {}
    def dependencies(path):
        text = subprocess.check_output([str(ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'), '-d', str(path)], text=True)
        for name in re.findall(r'Shared library: \[(.*?)\]', text):
            if PurePosixPath(name).name != name: raise ValueError('Invalid library name')
            if name in libraries: continue
            candidates = [runtime / name] if (runtime / name).exists() else list(runtime.glob(name + '.*'))
            if len(candidates) != 1: raise ValueError('Missing/ambiguous library ' + name)
            libraries[name] = candidates[0]; dependencies(candidates[0])
    dependencies(scanner)
    install = ['fatput', str(disk), 'put', str(work / 'protocols.elf'), 'sbin/init', 'put', str(scanner), 'bin/wayland-scanner',
               'put', str(work / 'protocols.list'), 'etc/protocols.list']
    for key in ALL_PROTOCOLS: install += ['put', str(PROTOCOL_SDK / 'xml' / (key + '.xml')), f'etc/protocols/{key}.xml']
    for name, path in sorted(libraries.items()): install += ['put', str(path), 'lib/x86_64-linux-gnu/' + name]
    fatput.edit(install)
    serial = work / 'serial.log'; serial.unlink(missing_ok=True)
    try:
        p = subprocess.run([str(build.QEMU), '-cpu', 'max', '-m', '256',
            '-drive', f'file={OUT / "nexus64.img"},format=raw,if=floppy,readonly=on', '-drive', f'file={disk},format=raw,if=ide',
            '-boot', 'a', '-display', 'none', '-vga', 'none', '-device', 'virtio-vga', '-serial', f'file:{serial}',
            '-no-reboot', '-device', 'isa-debug-exit,iobase=0xf4,iosize=4'], timeout=180)
        log = serial.read_text(errors='replace')
        if p.returncode != 33 or f'[64] PROTOCOL LIST GENERATED {len(ALL_PROTOCOLS)}' not in log or '[64] INIT SESSION RETURNED' not in log:
            raise AssertionError(log)
        if 'KERNEL_FAULT' in log or 'USER_FAULT' in log: raise AssertionError(log)
        fs = fatput.Fat32(str(disk))
        try:
            directory = fs.lookup_dir(['home', 'nexus']); digests = {}
            for name in outputs:
                entry = fs.find_name(directory, name)
                if entry is None: raise AssertionError('Missing generated ' + name)
                record = entry[2]
                first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
                size = int.from_bytes(record[28:32], 'little')
                data = fs.read_chain(first, size)
                key = next(k for k in ALL_PROTOCOLS if name.startswith(k + '-'))
                interface = key.split('-unstable')[0].removesuffix('-v1').replace('-', '_').encode()
                assert len(data) == size and b'Generated by wayland-scanner' in data and interface in data, name
                (PROTOCOL_SDK / name).write_bytes(data); digests[name] = hashlib.sha256(data).hexdigest()
        finally: fs.close()
    finally:
        disk.unlink(missing_ok=True)
    manifest.write_bytes((json.dumps({**expected, 'outputs': digests}, indent=2) + '\n').encode())
    print('PASS', len(ALL_PROTOCOLS), 'Aurora protocol bindings generated on native NexusOS', flush=True)


def font_header():
    """out/aurora_font_data.h: the Aileron UI face (fonts/Aileron-Regular.ttf, CC0) as a C array for aurora_text.c."""
    data = (HERE / 'fonts/Aileron-Regular.ttf').read_bytes()
    if hashlib.sha256(data).hexdigest() != '69853909b940023570964e29cffe30da95aea8de3627736b5cd15ab30143169f': raise ValueError('unexpected Aileron-Regular.ttf')
    text = '/* Generated by desktop.py from fonts/Aileron-Regular.ttf (Aileron Regular, CC0 1.0). */\nstatic const unsigned char aurora_font_aileron[%d] = {\n' % len(data)
    text += ''.join(','.join(str(b) for b in data[i:i + 32]) + ',\n' for i in range(0, len(data), 32)) + '};\n'
    target = OUT / 'aurora_font_data.h'
    if not target.exists() or target.read_text() != text: target.write_text(text, newline='\n')


def text_flags():
    """Compiler flags for aurora_text.c: generated font data and the pinned FreeType/HarfBuzz/fontconfig headers."""
    font_header()
    include = module('desktop_desk_install', HERE / 'desk_install.py').prepare_sdk()
    return ['-I', OUT, '-I', include, '-I', include / 'freetype2', '-I', include / 'harfbuzz']


def build():
    prepare()
    prepare_protocols()
    compiler = module('desktop_compiler', HERE / 'build.py')
    text = text_flags()
    protocols = [PROTOCOL_SDK / (key + '-protocol.c') for key in ALL_PROTOCOLS if key != 'xwayland-shell-v1']
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-DNEXUS_HOSTED=1', '-DNEXUS_AURORA=1',
        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, '-I', PROTOCOL_SDK, *text, HERE / 'desktop.c', HERE / 'aurora_graphics.c', HERE / 'aurora_dmabuf.c',
        HERE / 'aurora_protocols.c', HERE / 'aurora_wineapps.c', HERE / 'aurora_outputs.c', HERE / 'aurora_control.c', HERE / 'aurora_wm.c',
        HERE / 'aurora_text.c', HERE / 'aurora_lock.c', HERE / 'aurora_auth.c', HERE / 'aurora_keyboard.c', HERE / 'aurora_a11y.c', HERE / 'aurora_ime.c',
        HERE / 'aurora_power.c', HERE / 'aurora_netstatus.c', HERE / 'aurora_services.c', HERE / 'aurora_media.c', HERE / 'aurora_sound.c', HERE / 'aurora_session.c', HERE / 'aurora_idle.c', HERE / 'aurora_capture.c', HERE / 'aurora_notices.c', HERE / 'aurora_reader.c', HERE / 'aurora_sak.c', HERE / 'aurora_clipboard.c', *protocols,
        HERE / 'aurora_xwm.c', PROTOCOL_SDK / 'xwayland-shell-v1-protocol.c',   # rootless Xwayland window manager (xwm)
        *[ROOT / 'kernel' / (name + '.c') for name in ('macui', 'dock', 'menubar', 'spotlight')], SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-server.so.0.23.1',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libxkbcommon.so.0.0.0', '-o', OUT / 'desktop.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'desktop_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libxkbcommon.so.0.0.0', '-o', OUT / 'desktop_user.elf')
    print('BUILD native Nexus desktop:', (OUT / 'desktop.elf').stat().st_size, 'bytes', flush=True)
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'subsurface_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'subsurface_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-DNEXUS_HOSTED=1', '-Wall', '-Wextra', '-Werror',
        HERE / 'inspect_user.c', ROOT / 'kernel/crypto.c', '-o', OUT / 'inspect_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'popup_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'popup_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'aurora_protocol_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'aurora_protocol_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
        '-I', SDK / 'usr/include', '-I', SDK, '-I', PROTOCOL_SDK, HERE / 'wine_protocols_user.c', SDK / 'xdg-shell-protocol.c', *protocols,
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'wine_protocols_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-DNEXUS_HOSTED=1', '-Wall', '-Wextra', '-Werror',   # Software app (apt)
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'software_ui.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libxkbcommon.so.0.0.0',
        '-o', OUT / 'nexus_software.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # input-method test pair (tests/test_aurora_ime.py)
        '-I', SDK / 'usr/include', '-I', SDK, '-I', PROTOCOL_SDK, HERE / 'ime_user.c', SDK / 'xdg-shell-protocol.c', *protocols,
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'ime_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # idle-inhibit client (tests/test_aurora_idle.py)
        '-I', SDK / 'usr/include', '-I', SDK, '-I', PROTOCOL_SDK, HERE / 'idle_user.c', SDK / 'xdg-shell-protocol.c', *protocols,
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'idle_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # drag-and-drop pair (tests/test_aurora_dnd.py)
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'dnd_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'dnd_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # clipboard history client (tests/test_aurora_clipboard.py)
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'clip_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'clip_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.41', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # X11 window manager test client (tests/test_aurora_xwm.py)
        '-I', SDK / 'usr/include', '-I', SDK, HERE / 'xwm_test_user.c', SDK / 'xdg-shell-protocol.c',
        OUT / 'chrome-root/usr/lib/x86_64-linux-gnu/libwayland-client.so.0.23.1', '-o', OUT / 'xwm_test_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # /usr/libexec/nexus-auth (sysinstall.py services --aurora: 04755 root)
                 HERE / 'nexus_auth_user.c', HERE / 'aurora_auth.c', '-o', OUT / 'nexus-auth.elf')
    for name, source in (('nexus-session', 'nexus_session_user.c'), ('session_test_user', 'session_test_user.c')):   # session launcher (04755 root), tests/test_x64_sessions.py
        compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                     HERE / source, HERE / 'aurora_auth.c', '-o', OUT / (name + '.elf'))
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # tests/test_x64_auth_ext4.py
                 HERE / 'auth_test_user.c', '-o', OUT / 'auth_test_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # /usr/bin/pkexec (04755 root): run a program as root after admin auth
                 HERE / 'pkexec_user.c', HERE / 'aurora_auth.c', '-o', OUT / 'pkexec.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # /usr/libexec/nexus-polkitd: authorization broker (PolicyKit1 subset)
                 HERE / 'nexus_polkitd.c', HERE / 'aurora_auth.c', '-o', OUT / 'nexus-polkitd.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',   # tests/test_x64_polkit.py
                 HERE / 'polkit_test_user.c', '-o', OUT / 'polkit_test_user.elf')
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIC', '-shared', '-Wall', '-Wextra', '-Werror',
                 HERE / 'auth_preload.c', '-o', OUT / 'auth_preload.so')
    for name in ('x11_session_user', 'x11_test_user', 'wine_fs_user', 'wine_runner_user'):   # Xwayland session launcher, X test client, Wine prefix FS check, Wine campaign runner
        compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-fPIE', '-pie', '-Wall', '-Wextra', '-Werror',
                     HERE / (name + '.c'), '-o', OUT / (name + '.elf'))


if __name__ == '__main__':
    build()

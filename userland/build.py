"""Build and install static i386 apps. Usage: python -I userland/build.py [disk.img]."""
from pathlib import Path
import concurrent.futures
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
USER = ROOT / 'userland'
OUT = USER / 'out'
TOOLS = ROOT / 'tools'
CC = TOOLS / 'i686-elf/bin/i686-elf-gcc.exe'
PICO = TOOLS / 'picolibc'
SYSROOT = PICO / 'sysroot/picolibc'
VERSION = '1.8.12'
SHA256 = '64e8c412e1c40fa6eb1a72d2b5cdbcbfe6ceca4cbea454edbad54557ffc747fa'


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT, **kwargs)


def bootstrap():
    if (SYSROOT / 'lib/libc.a').exists():
        return
    venv = TOOLS / 'userland-build'
    py = venv / 'Scripts/python.exe'
    if not py.exists():
        run([sys.executable, '-m', 'venv', venv])
    run([py, '-m', 'pip', 'install', 'meson==1.12.1', 'ninja==1.13.2'])
    PICO.mkdir(exist_ok=True)
    archive = PICO / f'picolibc-{VERSION}.tar.xz'
    if not archive.exists():
        urllib.request.urlretrieve(f'https://github.com/picolibc/picolibc/releases/download/{VERSION}/{archive.name}', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise ValueError('Picolibc archive checksum mismatch')
    source = PICO / f'picolibc-{VERSION}'
    if not source.exists():
        with tarfile.open(archive) as tar:
            tar.extractall(PICO, filter='data')
    tc = (TOOLS / 'i686-elf/bin').as_posix()
    cross = PICO / 'nexus.cross'
    cross.write_text(f"""[binaries]
c = '{tc}/i686-elf-gcc.exe'
ar = '{tc}/i686-elf-ar.exe'
strip = '{tc}/i686-elf-strip.exe'
[host_machine]
system = 'none'
cpu_family = 'x86'
cpu = 'i686'
endian = 'little'
[properties]
needs_exe_wrapper = true
skip_sanity_check = true
[built-in options]
c_std = 'gnu11'
c_args = ['-m32', '-ffreestanding', '-fno-stack-protector', '-ffunction-sections', '-fdata-sections']
""", encoding='utf-8')
    env = dict(os.environ, PATH=str(venv / 'Scripts') + os.pathsep + os.environ['PATH'])
    meson = venv / 'Scripts/meson.exe'
    run([meson, 'setup', PICO / 'build', source, '--cross-file', cross, '--prefix', '/picolibc',
         '-Dmultilib=false', '-Dpicocrt=false', '-Dsemihost=false', '-Dtests=false',
         '-Dthread-local-storage=false', '-Dsingle-thread=true', '-Dposix-console=true',
         '-Dio-long-long=true', '-Dnewlib-global-errno=true', '-Ddefault_library=static', '-Dc_std=gnu11'], env=env)
    run([venv / 'Scripts/ninja.exe', '-C', PICO / 'build', '-j', '8'], env=env)
    run([meson, 'install', '-C', PICO / 'build', '--destdir', PICO / 'sysroot'], env=env)


def main():
    bootstrap()
    OUT.mkdir(exist_ok=True)
    flags = ['-std=gnu11', '-O2', '-m32', '-ffreestanding', '-fno-stack-protector',
             '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra',
             '-I' + str(USER / 'libc/include'), '-isystem', str(SYSROOT / 'include')]

    def compile(src):
        obj = OUT / (src.stem + '.o')
        run([CC, *flags, '-c', src, '-o', obj])
        return obj

    platform = compile(USER / 'libc/src/platform.c')
    crt = compile(USER / 'libc/src/crt0.S')
    lua = USER / 'vendor/lua-5.4.7/src'
    sources = [p for p in sorted(lua.glob('*.c')) if p.name != 'luac.c']
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        lua_objects = list(pool.map(compile, sources))
    programs = {'lua': lua_objects, 'kilo': [compile(USER / 'vendor/kilo.c')]}
    for src in sorted((USER / 'apps').glob('*.c')):
        programs[src.stem] = [compile(src)]
    for name, objs in programs.items():
        run([CC, '-m32', '-nostdlib', '-static', '-s', '-Wl,--gc-sections',
             '-T', USER / 'libc/user.ld', crt, platform, *objs,
             '-L' + str(SYSROOT / 'lib'), '-lc', '-lgcc', '-o', OUT / (name + '.elf')])
        print(f'{name}: {(OUT / (name + ".elf")).stat().st_size:,} bytes', flush=True)
    disk = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / 'disk.img'
    args = [sys.executable, '-I', USER / 'fatput.py', disk]
    for name in programs:
        args += ['put', OUT / (name + '.elf'), 'bin/' + name]
    args += ['put', USER / 'tests/check.lua', 'tests/check.lua']
    if 'core' in programs:
        for name in ('sh', 'ls', 'cat', 'echo', 'wc', 'grep', 'head', 'tail', 'cp', 'mv', 'rm', 'mkdir', 'pwd', 'sort', 'uniq', 'tr', 'hexdump'):
            args += ['put', OUT / 'core.elf', 'bin/' + name]
    if 'game' in programs:
        for name in ('2048', 'tetris'):
            args += ['put', OUT / 'game.elf', 'bin/' + name]
    run(args)


if __name__ == '__main__':
    main()

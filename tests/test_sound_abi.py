"""Compile tests/sound_abi_check.c for x86_64-linux-gnu: arch/x86_64/sound_abi.h must match <sound/asound.h>."""
from pathlib import Path
import importlib.util
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('nexus_build', ROOT / 'arch/x86_64/build.py')
build = importlib.util.module_from_spec(spec); spec.loader.exec_module(build)
with tempfile.TemporaryDirectory() as work:
    subprocess.run([str(build.compiler()), 'cc', '-target', 'x86_64-linux-gnu', '-Wall', '-Wextra', '-Werror', '-c',
                    str(ROOT / 'tests/sound_abi_check.c'), '-o', str(Path(work) / 'abi.o')], check=True)
print('PASS ALSA ABI: hw/sw params, status, sync_ptr, info, xferi, card info, elem list and ioctl numbers match asound.h')

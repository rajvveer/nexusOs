"""Prepare a desktop64 image (your own copy, never the real desktop disk) for tests/test_x64_aurora_power.py, in place.
    python -I tests/stage_desktop_disk.py DISK <host_file> <fat_path> [<host_file> <fat_path> ...]
    python -I tests/stage_desktop_disk.py DISK --reserve <fat_path> <megabytes>
e.g. `DISK arch/x86_64/out/desktop.elf sbin/init` and `DISK --reserve nexus/hiber.img 512` (the hibernation file:
clusters are allocated but not zero-filled, so a sparse copy stays sparse; hibernate.c overwrites the area anyway)."""
import importlib.util, sys
from pathlib import Path

spec = importlib.util.spec_from_file_location('fatput', Path(__file__).resolve().parents[1] / 'userland/fatput.py')
fatput = importlib.util.module_from_spec(spec); spec.loader.exec_module(fatput)
fs = fatput.Fat32(sys.argv[1])
try:
    if sys.argv[2] == '--reserve':
        dirs, leaf = fs.split(sys.argv[3]); size = int(sys.argv[4]) << 20
        directory = fs.lookup_dir(dirs, create=True)
        assert fs.find_name(directory, leaf) is None, 'exists already'
        clusters = (size + fs.cl_bytes - 1) // fs.cl_bytes
        # Largest free runs first (each ascending): hibernate.c maps at most 512 extents, and a first-fit chain on a
        # fragmented volume (desk's disk: 686 runs for 512 MiB) has more. Clusters are not zero-filled.
        free_runs, start = [], None
        for c in range(2, fs.nclusters + 2):
            if fs.fat_get(c) == 0:
                if start is None: start = c
            elif start is not None: free_runs.append((start, c - start)); start = None
        if start is not None: free_runs.append((start, fs.nclusters + 2 - start))
        picked, need = [], clusters
        for run_start, length in sorted(free_runs, key=lambda r: -r[1]):
            take = min(length, need); picked.append((run_start, take)); need -= take
            if not need: break
        assert not need, 'FAT32 volume full (%d clusters needed)' % clusters
        got = [c for run_start, take in sorted(picked) for c in range(run_start, run_start + take)]
        for i, c in enumerate(got): fs.fat_set(c, got[i + 1] if i + 1 < len(got) else fatput.EOC)
        first = got[0]
        fs.create_entry(directory, leaf, 0x20, first, size)
        fs.flush_fat()
        chain = list(fs.chain(first)); runs = 1 + sum(1 for a, b in zip(chain, chain[1:]) if b != a + 1)
        print('reserved', sys.argv[3], size, 'bytes,', clusters, 'clusters in', runs, 'runs')
    else:
        pairs = sys.argv[2:]
        for i in range(0, len(pairs), 2): fs.put(pairs[i], pairs[i + 1])
finally:
    fs.close()

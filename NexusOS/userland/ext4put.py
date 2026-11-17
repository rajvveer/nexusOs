#!/usr/bin/env python3
"""ext4put.py -- copy files, directories and symlinks into a NexusOS ext4 image (offline).

    python -I ext4put.py <image> [options] <command> [args] [[options] <command> [args] ...]

Commands:
    put <host_file> <path>          copy a file (replaces an existing non-directory)
    tree <host_dir> <path>          copy a host directory recursively
    mkdir <path>                    create a directory (and missing parents)
    symlink <target> <path>         create a symbolic link (fast if the target is < 60 bytes)
    link <existing> <path>          create a hard link
    mknod <path> p|s|c|b [major minor]
    chmod <octal> <path> | chown <uid>:<gid> <path> | touch <epoch-seconds> <path>
    rm <path>                       remove a file, symlink or empty directory
    ls [path] | cat <path> | stat <path>
    mark-error                      set the 'errors' state (the kernel then mounts read-only)
Options (apply to the following commands until changed):
    --mode OCT   permission bits for put/mkdir/tree (default: host bits; executables 0755, else 0644)
    --uid N --gid N   owner (default 0:0)
    --mtime SECONDS   timestamps (default: host file mtime / now)
Parent directories are created on demand as 0755 owned by the current --uid/--gid.
The volume is marked "in use" while editing; a journal that needs recovery is refused.
"""
import importlib.util
import os
import stat
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('ext4lib', os.path.join(HERE, 'ext4lib.py'))
ext4lib = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ext4lib)


def guess_mode(path):
    """0755 for ELF executables/shared objects with an interpreter and for #! scripts, else 0644."""
    try:
        with open(path, 'rb') as f:
            head = f.read(4096)
    except OSError:
        return 0o644
    return 0o755 if is_executable_image(head) else 0o644


def is_executable_image(head):
    if head[:2] == b'#!':
        return True
    if head[:4] != b'\x7fELF' or len(head) < 64:
        return False
    import struct
    if head[4] == 2:
        e_type = struct.unpack_from('<H', head, 16)[0]
        phoff = struct.unpack_from('<Q', head, 32)[0]
        phentsize, phnum = struct.unpack_from('<HH', head, 54)
    else:
        e_type = struct.unpack_from('<H', head, 16)[0]
        phoff = struct.unpack_from('<I', head, 28)[0]
        phentsize, phnum = struct.unpack_from('<HH', head, 42)
    if e_type == 2:
        return True
    if e_type != 3:
        return False
    # ET_DYN: a PIE executable has PT_INTERP; the dynamic loader itself has an entry and no PT_DYNAMIC soname use
    for i in range(phnum):
        off = phoff + i * phentsize
        if off + 4 > len(head):
            break
        if struct.unpack_from('<I', head, off)[0] == 3:
            return True
    return False


def run(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    image = argv[0]
    readonly_cmds = {'ls', 'cat', 'stat'}
    cmds = [a for a in argv[1:] if not a.startswith('--')]
    writing = not cmds or cmds[0] not in readonly_cmds
    fs = ext4lib.Writer(image) if writing else ext4lib.Ext4(image)
    opts = {'mode': None, 'uid': 0, 'gid': 0, 'mtime': None}
    try:
        i = 1
        args = argv
        while i < len(args):
            a = args[i]
            if a.startswith('--'):
                key, _, val = a[2:].partition('=')
                if not val:
                    i += 1
                    val = args[i]
                opts[key] = int(val, 8) if key == 'mode' else int(val)
                i += 1
                continue
            t = opts['mtime'] * 10**9 if opts['mtime'] is not None else None
            if a == 'put':
                host, guest = args[i + 1], args[i + 2]
                mode = opts['mode'] if opts['mode'] is not None else guess_mode(host)
                fs.ensure_dir(fs.split(guest)[:-1], 0o755, opts['uid'], opts['gid'])
                fs.put_file(host, guest, mode, opts['uid'], opts['gid'], t)
                i += 3
            elif a == 'tree':
                host, guest = args[i + 1], args[i + 2]
                put_tree(fs, host, guest, opts)
                i += 3
            elif a == 'mkdir':
                mode = opts['mode'] if opts['mode'] is not None else 0o755
                fs.ensure_dir(fs.split(args[i + 1]), mode, opts['uid'], opts['gid'], t)
                i += 2
            elif a == 'symlink':
                fs.ensure_dir(fs.split(args[i + 2])[:-1], 0o755, opts['uid'], opts['gid'])
                fs.symlink(args[i + 1], args[i + 2], opts['uid'], opts['gid'], t)
                i += 3
            elif a == 'link':
                fs.link(args[i + 1], args[i + 2])
                i += 3
            elif a == 'mknod':
                kind = args[i + 2]
                mode = (opts['mode'] if opts['mode'] is not None else 0o644)
                if kind in ('c', 'b'):
                    rdev = (int(args[i + 3]) << 8) | int(args[i + 4])
                    fs.mknod(args[i + 1], (ext4lib.S_IFCHR if kind == 'c' else ext4lib.S_IFBLK) | mode, rdev, opts['uid'], opts['gid'])
                    i += 5
                else:
                    fs.mknod(args[i + 1], (ext4lib.S_IFIFO if kind == 'p' else ext4lib.S_IFSOCK) | mode, 0, opts['uid'], opts['gid'])
                    i += 3
            elif a == 'chmod':
                fs.setattr(args[i + 2], mode=int(args[i + 1], 8))
                i += 3
            elif a == 'chown':
                u, _, g = args[i + 1].partition(':')
                fs.setattr(args[i + 2], uid=int(u), gid=int(g or u))
                i += 3
            elif a == 'touch':
                fs.setattr(args[i + 2], mtime_ns=int(args[i + 1]) * 10**9)
                i += 3
            elif a == 'rm':
                fs.remove(args[i + 1])
                i += 2
            elif a == 'mark-error':
                fs.sb.state = fs.sb.state | ext4lib.STATE_ERROR
                i += 1
            elif a == 'ls':
                path = args[i + 1] if i + 1 < len(args) else '/'
                ino = fs.lookup(path)
                if not ino:
                    raise SystemExit('not found: ' + path)
                for name, child, ft in fs.dir_entries(ino):
                    inode = fs.read_inode(child)
                    print('%7o %5d %5d %10d %s' % (inode.mode, inode.uid, inode.gid, inode.size, name.decode(errors='replace')))
                i += 2
            elif a == 'cat':
                ino = fs.lookup(args[i + 1])
                if not ino:
                    raise SystemExit('not found: ' + args[i + 1])
                sys.stdout.buffer.write(fs.read_file(fs.read_inode(ino)))
                i += 2
            elif a == 'stat':
                ino = fs.lookup(args[i + 1])
                if not ino:
                    raise SystemExit('not found: ' + args[i + 1])
                n = fs.read_inode(ino)
                print('ino=%d mode=%o uid=%d gid=%d links=%d size=%d blocks=%d flags=%#x mtime=%d' %
                      (ino, n.mode, n.uid, n.gid, n.links, n.size, n.blocks, n.flags, n.get_time('mtime')))
                i += 2
            else:
                raise SystemExit('unknown command ' + a)
    finally:
        fs.close()
    return 0


def put_tree(fs, host, guest, opts):
    t = opts['mtime'] * 10**9 if opts['mtime'] is not None else None
    base = fs.ensure_dir(fs.split(guest), opts['mode'] or 0o755, opts['uid'], opts['gid'], t)
    for root, dirs, files in os.walk(host):
        rel = os.path.relpath(root, host)
        gdir = guest.rstrip('/') + ('' if rel == '.' else '/' + rel.replace(os.sep, '/'))
        for d in sorted(dirs):
            fs.ensure_dir(fs.split(gdir + '/' + d), 0o755, opts['uid'], opts['gid'], t)
        for name in sorted(files):
            src = os.path.join(root, name)
            if os.path.islink(src):
                fs.symlink(os.readlink(src), gdir + '/' + name, opts['uid'], opts['gid'], t)
                continue
            mode = opts['mode'] if opts['mode'] is not None else guess_mode(src)
            fs.put_file(src, gdir + '/' + name, mode, opts['uid'], opts['gid'], t)
    return base


def main(argv):
    """fatput-compatible entry (tests): main(['fatput', image, 'put', host, guest, 'mkdir', guest, ...]).
    Paths below home/nexus and run/user/1000 belong to 1000:1000, everything else to root (fat2ext4 policy)."""
    image, cmds, args, i = argv[1], argv[2:], [argv[1]], 0
    while i < len(cmds):
        n = 3 if cmds[i] == 'put' else 2
        guest = cmds[i + n - 1].strip('/')
        user = guest.startswith(('home/nexus', 'run/user/1000'))
        args += ['--uid', '1000' if user else '0', '--gid', '1000' if user else '0'] + cmds[i:i + n]
        i += n
    return run(args)


edit = main

if __name__ == '__main__':
    sys.exit(run(sys.argv[1:]))

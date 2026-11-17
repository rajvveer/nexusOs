#!/usr/bin/env python3
"""Build Wine 10.0's winewayland.so with NexusOS's minimize support and install it over Debian's on a desktop disk.

    python -I arch/x86_64/winewayland_build.py build [--cache DIR] [--work DIR]      # -> out/winewayland.so (+ .patch), mshta
    python -I arch/x86_64/winewayland_build.py build-mshta                           # -> out/wine-mshta-{x86_64,i386}.exe only
    python -I arch/x86_64/winewayland_build.py install <disk.img>                    # replaces the Debian files (backups kept)

Also an HTA host (arch/x86_64/wine_mshta.c): Wine 10.0's mshta.exe is an upstream stub and mshtml's RunHTMLApplication a
FIXME, so .hta files did nothing. Ours hosts the WebBrowser control, runs the HTA's scripts and closes on window.close();
it is built for x86_64 and x86 with zig (windows-gnu), marked as a Wine builtin (the 17-byte "Wine builtin DLL"
signature at offset 0x40, as winebuild writes it) and installed over the Debian stubs in
usr/lib/x86_64-linux-gnu/wine/{x86_64,i386}-windows/mshta.exe (originals kept as mshta.exe.debian); existing prefixes
load it too, their system32/syswow64 copies being builtin placeholders.

Why: Wine 10.0's Wayland driver has no ShowWindow hook and never calls xdg_toplevel.set_minimized. Minimizing a window
(title-bar button, Win+Down, ShowWindow(SW_MINIMIZE)) is then done the Win32 way: the window moves to -32000,-32000 and
shrinks to the SM_CXMINIMIZED caption size, and the driver commits that 160x27 buffer, so on Wayland the window just
turns into a small caption bar on screen. The patch (PATCH below, also written as out/winewayland-minimize.patch):
  * the window config records WS_MINIMIZE; while it is set the toplevel is minimized with xdg_toplevel.set_minimized
    (once per minimize) and no iconic-size contents are committed (the compositor keeps the last real frame, which
    Aurora shows in its dock);
  * when the compositor gives a minimized window keyboard focus again (dock click, Alt+Tab), the window gets
    WM_SYSCOMMAND/SC_RESTORE, so Win32 restores it and the next frame is the normal one;
  * when Win32 restores it by itself (ShowWindow(SW_RESTORE), SetForegroundWindow), xdg-shell has no unminimize
    request: the driver asks with xdg_activation_v1 (a token for the surface, then activate), which Aurora grants a
    client for its own minimized window.
  * OpenGL contexts (opengl.c): the driver checks that a context handed to it is one it made (wglMakeCurrent,
    wglMakeContextCurrentARB, wglDeleteContext, wglShareLists, the share context of wglCreateContextAttribsARB) and
    fails with ERROR_INVALID_HANDLE instead of dereferencing it; a context whose attributes or EGL context could not be
    made is not returned. Seen with Notepad++ x86 (WoW64): wined3d's GL probe reached the driver with context 1 and
    the process crashed in its UI thread (NULL+0x19 in wayland_wglMakeContextCurrentARB), so the window never got
    input again; now the probe fails and wined3d/Direct2D fall back.
Everything else is Wine 10.0 unchanged. The unix library is compiled from the pinned upstream tarball (dl.winehq.org)
and linked against the win32u.so/ntdll.so of the Debian libwine package the disk has (wine-lock.json), with the same
NEEDED libraries as Debian's build; its unix-call table matches the PE side (winewayland.drv) of that package because
both come from the same Wine 10.0 sources. Wine is LGPL-2.1+: the patch and this script are the corresponding source.
"""
from pathlib import Path, PurePosixPath
import argparse
import difflib
import hashlib
import importlib.util
import io
import json
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
_nexus_paths_spec = importlib.util.spec_from_file_location('nexus_paths', Path(__file__).resolve().parent / 'nexus_paths.py')
nexus_paths = importlib.util.module_from_spec(_nexus_paths_spec); _nexus_paths_spec.loader.exec_module(nexus_paths)   # data locations

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'out'
WINE_SOURCE = {'url': 'https://dl.winehq.org/wine/source/10.0/wine-10.0.tar.xz', 'bytes': 32011244,
               'sha256': 'c5e0b3f5f7efafb30e9cd4d9c624b85c583171d33549d933cd3402f341ac3601'}
DEFAULT_CACHE = nexus_paths.file('NEXUS_DESK_CACHE', OUT / 'desk-packages')
UNIX_SOURCES = ['display.c', 'opengl.c', 'vulkan.c', 'wayland.c', 'waylanddrv_main.c', 'wayland_keyboard.c', 'wayland_output.c',
                'wayland_pointer.c', 'wayland_surface.c', 'window.c', 'window_surface.c']
GUEST = 'usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winewayland.so'
MSHTA = {'x86_64': ('x86_64-windows-gnu', 'usr/lib/x86_64-linux-gnu/wine/x86_64-windows/mshta.exe'),
         'i386': ('x86-windows-gnu', 'usr/lib/x86_64-linux-gnu/wine/i386-windows/mshta.exe')}
BUILTIN_SIGNATURE = b'Wine builtin DLL\0'   # dlls/ntdll and the server: a PE with this at 0x40 is a Wine builtin
CONFIG_H = '''/* NexusOS: the configure results winewayland.so's sources use (Debian 10.0 x86_64 build). */
#ifndef __WINE_CONFIG_H
#define __WINE_CONFIG_H
#define PACKAGE_VERSION "10.0"
#define SONAME_LIBEGL "libEGL.so.1"
#define HAVE_LIBWAYLAND_EGL 1
#define SONAME_LIBVULKAN "libvulkan.so.1"
#define HAVE_UNISTD_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_LINUX_INPUT_H 1
#define HAVE_DLFCN_H 1
#endif
'''
PATCH = [   # (file, old, new): exact text substitutions on Wine 10.0
    ('waylanddrv.h', '''    BOOL visible;
    BOOL managed;
};''', '''    BOOL visible;
    BOOL managed;
    BOOL minimized;   /* NexusOS: WS_MINIMIZE (xdg_toplevel.set_minimized) */
};'''),
    ('waylanddrv.h', '''    int content_width, content_height;
    HCURSOR hcursor;
};''', '''    int content_width, content_height;
    HCURSOR hcursor;
    BOOL minimize_requested;   /* NexusOS: set_minimized sent for the current minimize */
};'''),
    ('window.c', '''    conf->managed = data->managed;
}''', '''    conf->managed = data->managed;
    conf->minimized = (style & WS_MINIMIZE) != 0;
}'''),
    ('window.c', '''    else
    {
        surface->processing.processed = TRUE;
    }
}''', '''    else
    {
        surface->processing.processed = TRUE;
    }

    /* NexusOS: a minimized Win32 window is a minimized toplevel (the compositor hides it; restoring it from there
     * gives it keyboard focus, see keyboard_handle_enter). xdg-shell has no unminimize request. */
    if (surface->window.minimized && !surface->minimize_requested)
    {
        xdg_toplevel_set_minimized(surface->xdg_toplevel);
        surface->minimize_requested = TRUE;
    }
    else if (!surface->window.minimized)
    {
        /* Win32 restored it (SW_RESTORE, SetForegroundWindow...): ask the compositor to show it again. */
        if (surface->minimize_requested) request_activation(surface);
        surface->minimize_requested = FALSE;
    }
}'''),
    ('window.c', '''static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{''', '''/* NexusOS: xdg_activation_v1 token for a window Win32 restored from minimized; activate it once the token is done. */
static void activation_token_done(void *data, struct xdg_activation_token_v1 *token, const char *text)
{
    struct wayland_win_data *win_data;

    if ((win_data = wayland_win_data_get((HWND)data)))
    {
        if (win_data->wayland_surface && win_data->wayland_surface->wl_surface && process_wayland.xdg_activation_v1)
            xdg_activation_v1_activate(process_wayland.xdg_activation_v1, text, win_data->wayland_surface->wl_surface);
        wayland_win_data_release(win_data);
    }
    xdg_activation_token_v1_destroy(token);
    wl_display_flush(process_wayland.wl_display);
}

static const struct xdg_activation_token_v1_listener activation_token_listener = { activation_token_done };

static void request_activation(struct wayland_surface *surface)
{
    struct xdg_activation_token_v1 *token;

    if (!process_wayland.xdg_activation_v1 || !surface->wl_surface) return;
    if (!(token = xdg_activation_v1_get_activation_token(process_wayland.xdg_activation_v1))) return;
    xdg_activation_token_v1_add_listener(token, &activation_token_listener, surface->hwnd);
    xdg_activation_token_v1_set_surface(token, surface->wl_surface);
    xdg_activation_token_v1_commit(token);
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{'''),
    ('waylanddrv.h', '''#include "xdg-shell-client-protocol.h"''', '''#include "xdg-shell-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"   /* NexusOS */'''),
    ('waylanddrv.h', '''    struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;''', '''    struct zwp_relative_pointer_manager_v1 *zwp_relative_pointer_manager_v1;
    struct xdg_activation_v1 *xdg_activation_v1;   /* NexusOS: restore minimized windows */'''),
    ('wayland.c', '''            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
''', '''            wl_registry_bind(registry, id, &zwp_relative_pointer_manager_v1_interface, 1);
    }
    else if (strcmp(interface, "xdg_activation_v1") == 0)   /* NexusOS */
    {
        process_wayland.xdg_activation_v1 =
            wl_registry_bind(registry, id, &xdg_activation_v1_interface, 1);
    }
'''),
    ('window.c', '''    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface_reconfigure(wayland_surface))''', '''    if (!(data = wayland_win_data_get(hwnd))) return FALSE;

    if ((wayland_surface = data->wayland_surface) && wayland_surface->window.minimized)
    {
        /* NexusOS: no iconic (caption-sized) frames for a minimized toplevel: the compositor keeps its last frame. */
        wayland_win_data_release(data);
        return TRUE;
    }

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface_reconfigure(wayland_surface))'''),
    ('wayland_surface.c', '''    needs_contents = surface->window.visible &&''', '''    needs_contents = surface->window.visible && !surface->window.minimized &&'''),
    ('wayland_keyboard.c', '''    keyboard->focused_hwnd = hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    NtUserPostMessage(keyboard->focused_hwnd, WM_INPUTLANGCHANGEREQUEST, 0 /*FIXME*/,
                      (LPARAM)keyboard_hkl);
''', '''    keyboard->focused_hwnd = hwnd;
    pthread_mutex_unlock(&keyboard->mutex);

    NtUserPostMessage(keyboard->focused_hwnd, WM_INPUTLANGCHANGEREQUEST, 0 /*FIXME*/,
                      (LPARAM)keyboard_hkl);

    /* NexusOS: the compositor restored a minimized window (dock, window switcher): restore it in Win32 too. */
    if (NtUserGetWindowLongW(hwnd, GWL_STYLE) & WS_MINIMIZE)
        NtUserPostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
'''),
    # OpenGL: only contexts this driver made are used (see the module comment)
    ('opengl.c', '''static BOOL wgl_context_make_current(struct wgl_context *ctx, HDC draw_hdc, HDC read_hdc)
{''', '''/* NexusOS: true when ctx is one of this driver's contexts (opengl32's WoW64 path was seen passing 1) */
static BOOL wgl_context_valid(struct wgl_context *ctx)
{
    struct wgl_context *c;
    BOOL found = FALSE;

    pthread_mutex_lock(&gl_object_mutex);
    LIST_FOR_EACH_ENTRY(c, &gl_contexts, struct wgl_context, entry)
        if (c == ctx) { found = TRUE; break; }
    pthread_mutex_unlock(&gl_object_mutex);
    return found;
}

static BOOL wgl_context_make_current(struct wgl_context *ctx, HDC draw_hdc, HDC read_hdc)
{'''),
    ('opengl.c', '''    if (!wgl_context_populate_attribs(ctx, attribs))
    {
        ctx->attribs[0] = EGL_NONE;
        goto out;
    }''', '''    if (!wgl_context_populate_attribs(ctx, attribs))
    {
        free(ctx);   /* NexusOS: never hand out a context that is not in gl_contexts */
        ctx = NULL;
        goto out;
    }'''),
    ('opengl.c', '''                                      share ? share->context : EGL_NO_CONTEXT,
                                      ctx->attribs);

    pthread_mutex_lock(&gl_object_mutex);
    list_add_head(&gl_contexts, &ctx->entry);''', '''                                      share ? share->context : EGL_NO_CONTEXT,
                                      ctx->attribs);
    if (!ctx->context)   /* NexusOS: no EGL context, no WGL context */
    {
        ERR("Failed to create EGL context\\n");
        free(ctx);
        ctx = NULL;
        goto out;
    }

    pthread_mutex_lock(&gl_object_mutex);
    list_add_head(&gl_contexts, &ctx->entry);'''),
    ('opengl.c', '''    TRACE("hdc=%p share=%p attribs=%p\\n", hdc, share, attribs);
    return create_context(hdc, share, attribs);''', '''    TRACE("hdc=%p share=%p attribs=%p\\n", hdc, share, attribs);
    if (share && !wgl_context_valid(share))
    {
        WARN("invalid share context %p\\n", share);
        RtlSetLastWin32Error(ERROR_INVALID_OPERATION);
        return NULL;
    }
    return create_context(hdc, share, attribs);'''),
    ('opengl.c', '''static BOOL wayland_wglDeleteContext(struct wgl_context *ctx)
{
    struct wgl_pbuffer *pb;
''', '''static BOOL wayland_wglDeleteContext(struct wgl_context *ctx)
{
    struct wgl_pbuffer *pb;

    if (!wgl_context_valid(ctx))
    {
        WARN("invalid context %p\\n", ctx);
        RtlSetLastWin32Error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
'''),
    ('opengl.c', '''    ret = wgl_context_make_current(ctx, draw_hdc, read_hdc);
    if (!ret) RtlSetLastWin32Error(ERROR_INVALID_HANDLE);''', '''    if (!wgl_context_valid(ctx))
    {
        WARN("invalid context %p\\n", ctx);
        RtlSetLastWin32Error(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    ret = wgl_context_make_current(ctx, draw_hdc, read_hdc);
    if (!ret) RtlSetLastWin32Error(ERROR_INVALID_HANDLE);'''),
    ('opengl.c', '''static BOOL wayland_wglShareLists(struct wgl_context *orig, struct wgl_context *dest)
{
    struct wgl_context *keep, *clobber;
''', '''static BOOL wayland_wglShareLists(struct wgl_context *orig, struct wgl_context *dest)
{
    struct wgl_context *keep, *clobber;

    if (!wgl_context_valid(orig) || !wgl_context_valid(dest))
    {
        WARN("invalid context %p or %p\\n", orig, dest);
        RtlSetLastWin32Error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
'''),
]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def sha256(data): return hashlib.sha256(data).hexdigest()


def wine_tarball(cache):
    """The pinned Wine 10.0 source tarball (cache, else the shared NexusOS copy, else dl.winehq.org), size + SHA-256 checked."""
    candidates = [cache / 'wine-10.0.tar.xz', nexus_paths.file('NEXUS_WINE_SOURCE', nexus_paths.cache('wine-src') / 'wine-10.0.tar.xz')]
    for path in candidates:
        if path.is_file() and path.stat().st_size == WINE_SOURCE['bytes'] and sha256(path.read_bytes()) == WINE_SOURCE['sha256']: return path
    wi = module('ww_wine_install', HERE / 'wine_install.py')
    data = wi.fetch_url(WINE_SOURCE['url'], WINE_SOURCE['bytes'], ('https://dl.winehq.org/',))
    if len(data) != WINE_SOURCE['bytes'] or sha256(data) != WINE_SOURCE['sha256']: raise SystemExit('Wine source verification failed')
    cache.mkdir(parents=True, exist_ok=True); (cache / 'wine-10.0.tar.xz').write_bytes(data)
    return cache / 'wine-10.0.tar.xz'


def build(cache, work):
    wi = module('ww_wine_install', HERE / 'wine_install.py'); ai = wi.ai
    desk = module('ww_desk_install', HERE / 'desk_install.py')
    compiler = module('ww_build', HERE / 'build.py')
    include = desk.prepare_sdk()
    if work.exists(): shutil.rmtree(work)
    src = work / 'wine'; (src / 'include').mkdir(parents=True)
    with tarfile.open(wine_tarball(cache)) as archive:   # include/ and dlls/winewayland.drv/ only
        for member in archive:
            name = ai.safe_member(member.name)
            if not member.isfile() or not name.startswith(('wine-10.0/include/', 'wine-10.0/dlls/winewayland.drv/')): continue
            target = src.joinpath(*PurePosixPath(name).parts[1:]); target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(archive.extractfile(member).read())
    (src / 'include/config.h').write_text(CONFIG_H, newline='\n')
    drv = src / 'dlls/winewayland.drv'
    original, diff = {}, []
    for name, old, new in PATCH:
        path = drv / name; text = path.read_text(encoding='utf-8'); original.setdefault(name, text)
        if text.count(old) != 1: raise SystemExit('patch anchor not found once in %s:\n%s' % (name, old[:200]))
        path.write_text(text.replace(old, new), encoding='utf-8', newline='\n')
    for name, text in original.items():   # one unified diff per file (git apply / patch -p1 in the Wine tree)
        diff += difflib.unified_diff(text.splitlines(True), (drv / name).read_text(encoding='utf-8').splitlines(True),
                                     'a/dlls/winewayland.drv/' + name, 'b/dlls/winewayland.drv/' + name)
    (OUT / 'winewayland-minimize.patch').write_text(''.join(diff), newline='\n')
    # Debian's libwine: win32u.so / ntdll.so to link against, and the original winewayland.so for comparison
    lock = json.loads((HERE / 'wine-lock.json').read_text())
    libs = work / 'lib'; libs.mkdir()
    wanted = {'libwine': ['win32u.so', 'ntdll.so', 'winewayland.so'], 'libxkbregistry0': ['libxkbregistry.so.0']}
    for entry in lock['packages']:
        if entry['name'] not in wanted: continue
        deb = wi.fetch_package(entry, cache)
        with ai.open_payload(entry, deb) as archive:
            for member in archive:
                name = ai.safe_member(member.name); leaf = PurePosixPath(name).name
                if member.isfile() and '/x86_64-unix/' in name and leaf in wanted[entry['name']]: (libs / leaf).write_bytes(archive.extractfile(member).read())
                elif member.isfile() and leaf.startswith('libxkbregistry.so.0'): (libs / 'libxkbregistry.so.0').write_bytes(archive.extractfile(member).read())
    for leaf in ('win32u.so', 'ntdll.so', 'winewayland.so', 'libxkbregistry.so.0'):
        if not (libs / leaf).is_file(): raise SystemExit('missing ' + leaf + ' from the Debian packages')
    (libs / 'winewayland.debian.so').write_bytes((libs / 'winewayland.so').read_bytes()); (libs / 'winewayland.so').unlink()
    runtime = OUT / 'chrome-root/usr/lib/x86_64-linux-gnu'
    protocols = OUT / 'protocol-sdk'; sdk = OUT / 'desktop-sdk'
    generated = [sdk / 'xdg-shell-protocol.c'] + [protocols / (k + '-protocol.c') for k in ('viewporter', 'pointer-constraints-unstable-v1', 'relative-pointer-unstable-v1', 'xdg-output-unstable-v1', 'xdg-activation-v1')]
    target = OUT / 'winewayland.so'
    compiler.run(compiler.compiler(), 'cc', '-target', 'x86_64-linux-gnu.2.36', '-O2', '-g0', '-fPIC', '-shared', '-fasynchronous-unwind-tables',
                 '-D__WINESRC__', '-DWINE_UNIX_LIB', '-D_REENTRANT', '-DWINE_NO_LONG_TYPES', '-fno-strict-aliasing', '-Wno-attributes',
                 '-I', drv, '-I', src / 'include', '-I', sdk / 'usr/include', '-I', sdk, '-I', protocols, '-I', include,
                 *[drv / s for s in UNIX_SOURCES], *generated,
                 libs / 'win32u.so', libs / 'ntdll.so', runtime / 'libwayland-client.so.0.23.1', runtime / 'libwayland-egl.so.1.23.1',
                 runtime / 'libxkbcommon.so.0.0.0', libs / 'libxkbregistry.so.0', '-lm', '-o', target)
    readelf = ROOT / 'tools/i686-elf/bin/i686-elf-readelf.exe'
    def needed(path): return sorted(re.findall(r'Shared library: \[(.*?)\]', subprocess.check_output([str(readelf), '-d', str(path)], text=True)))
    def exports(path):
        text = subprocess.check_output([str(readelf), '--dyn-syms', '-W', str(path)], text=True)
        return sorted({m.group(1) for m in re.finditer(r'\s(?:FUNC|OBJECT)\s+GLOBAL\s+DEFAULT\s+\d+\s+(\S+)', text)})
    ours, theirs = needed(target), needed(libs / 'winewayland.debian.so')
    if set(ours) - set(theirs) - {'libc.so.6', 'libm.so.6'}: raise SystemExit('unexpected NEEDED %r (Debian: %r)' % (ours, theirs))
    if set(exports(libs / 'winewayland.debian.so')) - set(exports(target)): raise SystemExit('missing exports %r' % (set(exports(libs / 'winewayland.debian.so')) - set(exports(target))))
    info = {'sha256': sha256(target.read_bytes()), 'debian_sha256': sha256((libs / 'winewayland.debian.so').read_bytes()), 'needed': ours, 'debian_needed': theirs,
            'source': WINE_SOURCE, 'patch_sha256': sha256((OUT / 'winewayland-minimize.patch').read_bytes())}
    (OUT / 'winewayland.json').write_text(json.dumps(info, indent=1) + '\n')
    print('BUILD winewayland.so', target.stat().st_size, 'bytes; NEEDED', ', '.join(ours), flush=True)
    build_mshta()


def build_mshta():
    """out/wine-mshta-{x86_64,i386}.exe: wine_mshta.c for both architectures, marked as Wine builtins."""
    compiler = module('ww_build', HERE / 'build.py'); info = {}
    for arch, (target, _) in MSHTA.items():
        exe = OUT / ('wine-mshta-%s.exe' % arch)
        compiler.run(compiler.compiler(), 'cc', '-target', target, '-O2', '-g0', '-Wall', '-Wextra', '-Werror', '-municode', '-Wl,--subsystem,windows',
                     HERE / 'wine_mshta.c', '-lole32', '-loleaut32', '-luuid', '-lshlwapi', '-luser32', '-o', exe)
        data = bytearray(exe.read_bytes())
        if data[:2] != b'MZ' or int.from_bytes(data[0x3c:0x40], 'little') < 0x40 + len(BUILTIN_SIGNATURE): raise SystemExit('unexpected PE layout of ' + exe.name)
        data[0x40:0x40 + len(BUILTIN_SIGNATURE)] = BUILTIN_SIGNATURE   # in the DOS stub, before the PE header
        exe.write_bytes(bytes(data)); info[arch] = {'sha256': sha256(bytes(data)), 'bytes': len(data), 'guest': MSHTA[arch][1]}
        exe.with_suffix('.pdb').unlink(missing_ok=True)
    (OUT / 'wine-mshta.json').write_text(json.dumps(info, indent=1) + '\n')
    print('BUILD mshta', ', '.join('%s %d bytes' % (a, v['bytes']) for a, v in info.items()), flush=True)
    return info


def overlay_files():
    """[(host file, disk path)] of everything install() puts on a disk, for staging into a qcow2 overlay
    (tests/x64_overlay.py) instead of writing a disk."""
    files = [(OUT / 'winewayland.so', GUEST)]
    for arch, (_, guest) in MSHTA.items(): files.append((OUT / ('wine-mshta-%s.exe' % arch), guest))
    for host, _ in files:
        if not host.is_file(): raise SystemExit('missing %s: run winewayland_build.py build' % host)
    return files


def install_mshta(disk):
    """Our HTA host over the Debian stubs (kept as mshta.exe.debian); a file that is neither the stub nor ours stays."""
    fatput = module('ww_fatput_mshta', ROOT / 'userland/fatput.py')
    info = json.loads((OUT / 'wine-mshta.json').read_text()); args = ['fatput', str(disk)]; staged = []
    for arch, (_, guest) in MSHTA.items():
        exe = OUT / ('wine-mshta-%s.exe' % arch)
        if sha256(exe.read_bytes()) != info[arch]['sha256']: raise SystemExit(exe.name + ' changed since it was built')
        fs = fatput.Fat32(str(disk), readonly=True)
        try:
            directory = fs.lookup_dir(guest.split('/')[:-1]); found = fs.find_name(directory, 'mshta.exe')
            if not found: raise SystemExit('no %s on this disk' % guest)
            r = found[2]; current = fs.read_chain(int.from_bytes(r[20:22], 'little') << 16 | int.from_bytes(r[26:28], 'little'), int.from_bytes(r[28:32], 'little'))
            has_backup = bool(fs.find_name(directory, 'mshta.exe.debian'))
        finally: fs.close()
        if sha256(current) == info[arch]['sha256']: continue
        if b'mshta.exe is a stub!' not in current: raise SystemExit('%s is not Wine\'s stub: not replaced' % guest)
        if not has_backup:
            keep = OUT / ('mshta-debian-%s.exe' % arch); keep.write_bytes(current); staged.append(keep); args += ['put', str(keep), guest + '.debian']
        args += ['put', str(exe), guest]
    if len(args) > 2 and fatput.edit(args) != 0: raise SystemExit('fatput failed')
    for p in staged: p.unlink(missing_ok=True)
    print('mshta (HTA host) installed on' if len(args) > 2 else 'mshta (HTA host) already on', disk)


def install(disk):
    """Replace /usr/lib/x86_64-linux-gnu/wine/x86_64-unix/winewayland.so on a FAT desktop disk (Debian's kept as
    winewayland.so.debian); only when the disk's file is Debian's or ours. Then the HTA host (install_mshta)."""
    install_winewayland(disk); install_mshta(disk)


def install_winewayland(disk):
    info = json.loads((OUT / 'winewayland.json').read_text()); data = (OUT / 'winewayland.so').read_bytes()
    if sha256(data) != info['sha256']: raise SystemExit('out/winewayland.so changed since it was built')
    fatput = module('ww_fatput', ROOT / 'userland/fatput.py')
    fs = fatput.Fat32(str(disk), readonly=True)
    try:
        directory = fs.lookup_dir(GUEST.split('/')[:-1]); found = fs.find_name(directory, 'winewayland.so')
        if not found: raise SystemExit('no Wine on this disk (wine_install.py first)')
        record = found[2]; first = int.from_bytes(record[20:22], 'little') << 16 | int.from_bytes(record[26:28], 'little')
        current = sha256(fs.read_chain(first, int.from_bytes(record[28:32], 'little')))
        backup = fs.find_name(directory, 'winewayland.so.debian')
        if backup:   # an earlier NexusOS build is installed: Debian's original must be the backup
            b = backup[2]; kept = sha256(fs.read_chain(int.from_bytes(b[20:22], 'little') << 16 | int.from_bytes(b[26:28], 'little'), int.from_bytes(b[28:32], 'little')))
            if kept != info['debian_sha256']: raise SystemExit('winewayland.so.debian is not Debian\'s file: not replaced')
    finally: fs.close()
    if current == info['sha256']: print('winewayland.so already patched'); return
    if current != info['debian_sha256'] and not backup: raise SystemExit('the disk has an unknown winewayland.so (%s): not replaced' % current)
    staged = OUT / 'winewayland-debian.so'
    args = ['fatput', str(disk)]
    if not backup:
        fs = fatput.Fat32(str(disk), readonly=True)
        try: original = fs.read_chain(first, int.from_bytes(record[28:32], 'little'))
        finally: fs.close()
        staged.write_bytes(original); args += ['put', str(staged), GUEST + '.debian']
    args += ['put', str(OUT / 'winewayland.so'), GUEST]
    if fatput.edit(args) != 0: raise SystemExit('fatput failed')
    staged.unlink(missing_ok=True)
    print('installed patched winewayland.so on', disk, '(Debian original kept as winewayland.so.debian)')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0]); ap.add_argument('action', choices=('build', 'build-mshta', 'install'))
    ap.add_argument('disk', nargs='?'); ap.add_argument('--cache', default=str(DEFAULT_CACHE)); ap.add_argument('--work', default=str(OUT / 'winewayland-work'))
    a = ap.parse_args()
    if a.action == 'build': build(Path(a.cache).resolve(), Path(a.work).resolve())
    elif a.action == 'build-mshta': build_mshta()
    else:
        if not a.disk: ap.error('install needs the disk image')
        install(Path(a.disk).resolve())

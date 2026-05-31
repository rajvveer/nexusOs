/* ============================================================================
 * NexusOS — Win32 Compatibility Shim (Implementation) — Phase 34
 * ============================================================================
 * Implements basic kernel32, user32, and gdi32 functions via NexusOS APIs.
 * Includes the PE import resolver to bind functions to their implementations.
 * ============================================================================ */

#include "win32.h"
#include "window.h"
#include "gui.h"
#include "gfx.h"
#include "font.h"
#include "string.h"
#include "keyboard.h"
#include "mouse.h"
#include "vga.h"
#include "heap.h"
#include "vfs.h"
#include "process.h"
#include "rtc.h"
#include "registry.h"
#include "framebuffer.h"

extern volatile uint32_t system_ticks;

/* ============================================================================
 * Internal State
 * ============================================================================ */

#define WIN32_MAX_WINDOWS  16
#define WIN32_MAX_CLASSES   8
#define WIN32_MAX_HANDLES  32
#define WIN32_MAX_EVENTS   64

typedef struct {
    bool     active;
    HWND     hwnd;
    HWND     parent;
    int      nx_win_id;
    int      x, y;
    int      width, height;
    WNDPROC  wndproc;
    char     class_name[32];
    char     title[64];
    bool     is_visible;
} win32_window_t;

typedef struct {
    bool     active;
    WNDCLASS wc;
    char     class_name[32];
} win32_class_t;

typedef struct {
    bool     active;
    HANDLE   handle;
    fs_node_t* node;
    uint32_t offset;
} win32_handle_t;

static win32_window_t win32_windows[WIN32_MAX_WINDOWS];
static win32_class_t  win32_classes[WIN32_MAX_CLASSES];
static win32_handle_t win32_handles[WIN32_MAX_HANDLES];
static MSG            win32_msg_queue[WIN32_MAX_EVENTS];

static int      win32_eq_head = 0, win32_eq_tail = 0;
static uint32_t win32_next_id = 1000;
static uint32_t win32_last_error = 0;
static bool     win32_initialized = false;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static uint32_t alloc_id(void) { return win32_next_id++; }

static win32_window_t* find_window(HWND hwnd) {
    if (!hwnd) return NULL;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        if (win32_windows[i].active && win32_windows[i].hwnd == hwnd)
            return &win32_windows[i];
    }
    return NULL;
}

static win32_class_t* find_class(LPCSTR name) {
    if (!name) return NULL;
    for (int i = 0; i < WIN32_MAX_CLASSES; i++) {
        if (win32_classes[i].active && strcmp(win32_classes[i].class_name, name) == 0)
            return &win32_classes[i];
    }
    return NULL;
}

static win32_handle_t* find_handle(HANDLE h) {
    if (!h) return NULL;
    for (int i = 0; i < WIN32_MAX_HANDLES; i++) {
        if (win32_handles[i].active && win32_handles[i].handle == h)
            return &win32_handles[i];
    }
    return NULL;
}

static void push_msg(MSG* msg) {
    int next = (win32_eq_head + 1) % WIN32_MAX_EVENTS;
    if (next == win32_eq_tail) return; /* Queue full */
    win32_msg_queue[win32_eq_head] = *msg;
    win32_eq_head = next;
}

static bool get_content_origin(win32_window_t* w, int* ox, int* oy) {
    if (!w || w->nx_win_id < 0) return false;
    window_t* nw = window_get(w->nx_win_id);
    if (!nw) return false;
    *ox = nw->px + 1;
    *oy = nw->py + WIN_TITLEBAR_H + 1;
    return true;
}

/* ============================================================================
 * Callbacks from NexusOS Window Manager
 * ============================================================================ */

static void win32_win_draw(int id, int cx, int cy, int cw, int ch) {
    (void)cx; (void)cy; (void)cw; (void)ch;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        if (win32_windows[i].active && win32_windows[i].nx_win_id == id) {
            MSG msg;
            memset(&msg, 0, sizeof(msg));
            msg.hwnd = win32_windows[i].hwnd;
            msg.message = WM_PAINT;
            push_msg(&msg);
            break;
        }
    }
}

static void win32_win_key(int id, char key) {
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        if (win32_windows[i].active && win32_windows[i].nx_win_id == id) {
            MSG msg;
            memset(&msg, 0, sizeof(msg));
            msg.hwnd = win32_windows[i].hwnd;
            msg.message = WM_KEYDOWN;
            msg.wParam = (WPARAM)key;
            push_msg(&msg);
            
            /* Give WM_CHAR as well */
            msg.message = WM_CHAR;
            push_msg(&msg);
            break;
        }
    }
}

/* ============================================================================
 * Kernel32
 * ============================================================================ */

HMODULE GetModuleHandleA(LPCSTR lpModuleName) {
    (void)lpModuleName;
    /* In our simple OS, the .exe base is usually available, but we'll return a fake handle */
    return (HMODULE)0x400000;
}

void ExitProcess(UINT uExitCode) {
    (void)uExitCode;
    process_exit();
}

DWORD GetLastError(void) {
    return win32_last_error;
}

void SetLastError(DWORD dwErrCode) {
    win32_last_error = dwErrCode;
}

DWORD GetTickCount(void) {
    /* Translate system ticks to ms */
    return (DWORD)(system_ticks * 55); /* 18.2Hz = ~55ms per tick */
}

void Sleep(DWORD dwMilliseconds) {
    uint32_t ticks = dwMilliseconds / 55;
    uint32_t end = system_ticks + ticks;
    while (system_ticks < end) {
        __asm__ volatile("hlt");
    }
}

LPSTR GetCommandLineA(void) {
    return (LPSTR)"app.exe";
}

void OutputDebugStringA(LPCSTR lpOutputString) {
    if (lpOutputString) {
        vga_print_color("[WIN32 DBG] ", VGA_COLOR(VGA_LIGHT_BLUE, VGA_BLACK));
        vga_print(lpOutputString);
        vga_print("\n");
    }
}

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, 
                   void* lpSecurityAttributes, DWORD dwCreationDisposition, 
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {
    (void)dwDesiredAccess; (void)dwShareMode; (void)lpSecurityAttributes;
    (void)dwCreationDisposition; (void)dwFlagsAndAttributes; (void)hTemplateFile;

    if (!lpFileName) { SetLastError(87); return (HANDLE)-1; } /* ERROR_INVALID_PARAMETER */

    fs_node_t* root = vfs_get_root();
    fs_node_t* node = vfs_finddir(root, (char*)lpFileName);
    if (!node) { SetLastError(2); return (HANDLE)-1; } /* ERROR_FILE_NOT_FOUND */

    int slot = -1;
    for (int i = 0; i < WIN32_MAX_HANDLES; i++) {
        if (!win32_handles[i].active) { slot = i; break; }
    }
    if (slot < 0) { SetLastError(6); return (HANDLE)-1; } /* ERROR_INVALID_HANDLE */

    win32_handle_t* wh = &win32_handles[slot];
    wh->active = true;
    wh->handle = (HANDLE)alloc_id();
    wh->node = node;
    wh->offset = 0;
    
    return wh->handle;
}

BOOL ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead, 
              DWORD* lpNumberOfBytesRead, void* lpOverlapped) {
    (void)lpOverlapped;
    win32_handle_t* wh = find_handle(hFile);
    if (!wh || !wh->node) return FALSE;

    int32_t rd = vfs_read(wh->node, wh->offset, nNumberOfBytesToRead, (uint8_t*)lpBuffer);
    if (rd >= 0) {
        wh->offset += rd;
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = (DWORD)rd;
        return TRUE;
    }
    return FALSE;
}

BOOL WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite, 
               DWORD* lpNumberOfBytesWritten, void* lpOverlapped) {
    (void)lpOverlapped;
    win32_handle_t* wh = find_handle(hFile);
    if (!wh || !wh->node) return FALSE;

    int32_t wr = vfs_write(wh->node, wh->offset, nNumberOfBytesToWrite, (uint8_t*)lpBuffer);
    if (wr >= 0) {
        wh->offset += wr;
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = (DWORD)wr;
        return TRUE;
    }
    return FALSE;
}

BOOL CloseHandle(HANDLE hObject) {
    win32_handle_t* wh = find_handle(hObject);
    if (!wh) return FALSE;
    wh->active = false;
    return TRUE;
}

DWORD GetFileSize(HANDLE hFile, DWORD* lpFileSizeHigh) {
    if (lpFileSizeHigh) *lpFileSizeHigh = 0;
    win32_handle_t* wh = find_handle(hFile);
    if (!wh || !wh->node) return 0xFFFFFFFF;
    return wh->node->size;
}

DWORD GetCurrentProcessId(void) {
    process_t* current = process_get_current();
    return current ? current->pid : 0;
}

HANDLE GetProcessHeap(void) {
    return (HANDLE)0x1000; /* Dummy heap handle */
}

void* HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD dwBytes) {
    (void)hHeap; (void)dwFlags;
    return kmalloc(dwBytes);
}

BOOL HeapFree(HANDLE hHeap, DWORD dwFlags, void* lpMem) {
    (void)hHeap; (void)dwFlags;
    kfree(lpMem);
    return TRUE;
}

/* ============================================================================
 * User32
 * ============================================================================ */

WORD RegisterClassA(const WNDCLASS* lpWndClass) {
    if (!lpWndClass || !lpWndClass->lpszClassName) return 0;
    
    int slot = -1;
    for (int i = 0; i < WIN32_MAX_CLASSES; i++) {
        if (!win32_classes[i].active) { slot = i; break; }
    }
    if (slot < 0) return 0;
    
    win32_class_t* wc = &win32_classes[slot];
    wc->active = true;
    wc->wc = *lpWndClass;
    strncpy(wc->class_name, lpWndClass->lpszClassName, 31);
    wc->class_name[31] = '\0';
    return (WORD)(slot + 1);
}

HWND CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, 
                     DWORD dwStyle, int X, int Y, int nWidth, int nHeight, 
                     HWND hWndParent, HANDLE hMenu, HINSTANCE hInstance, void* lpParam) {
    (void)dwExStyle; (void)dwStyle; (void)hMenu; (void)hInstance; (void)lpParam;
    
    win32_class_t* wc = find_class(lpClassName);
    if (!wc) return NULL;
    
    int slot = -1;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) {
        if (!win32_windows[i].active) { slot = i; break; }
    }
    if (slot < 0) return NULL;
    
    win32_window_t* ww = &win32_windows[slot];
    ww->active = true;
    ww->hwnd = (HWND)alloc_id();
    ww->parent = hWndParent;
    ww->wndproc = wc->wc.lpfnWndProc;
    strncpy(ww->class_name, lpClassName, 31);
    ww->class_name[31] = '\0';
    
    if (lpWindowName) {
        strncpy(ww->title, lpWindowName, 63);
        ww->title[63] = '\0';
    } else {
        ww->title[0] = '\0';
    }
    
    ww->x = X == CW_USEDEFAULT ? 100 : X;
    ww->y = Y == CW_USEDEFAULT ? 100 : Y;
    ww->width = nWidth == CW_USEDEFAULT ? 400 : nWidth;
    ww->height = nHeight == CW_USEDEFAULT ? 300 : nHeight;
    ww->nx_win_id = -1;
    ww->is_visible = false;
    
    /* Send WM_CREATE */
    if (ww->wndproc) {
        ww->wndproc(ww->hwnd, WM_CREATE, 0, 0);
    }
    
    return ww->hwnd;
}

BOOL ShowWindow(HWND hWnd, int nCmdShow) {
    win32_window_t* ww = find_window(hWnd);
    if (!ww) return FALSE;
    
    if (nCmdShow == SW_SHOWNORMAL || nCmdShow == SW_SHOW) {
        if (!ww->is_visible) {
            int cx = ww->x / 8;
            int cy = ww->y / 16;
            int cw = (ww->width + WIN_TITLEBAR_H + 4) / 8;
            int ch = (ww->height + WIN_TITLEBAR_H + 4) / 16;
            if (cw < 10) cw = 10;
            if (ch < 5) ch = 5;
            if (cx < 2) cx = 2;
            if (cy < 2) cy = 2;
            
            int nid = window_create(ww->title, cx, cy, cw, ch, win32_win_draw, win32_win_key);
            if (nid >= 0) {
                ww->nx_win_id = nid;
                ww->is_visible = true;
                
                /* Request an initial paint */
                InvalidateRect(hWnd, NULL, TRUE);
            }
        }
    } else if (nCmdShow == SW_HIDE) {
        if (ww->is_visible && ww->nx_win_id >= 0) {
            window_destroy(ww->nx_win_id);
            ww->nx_win_id = -1;
            ww->is_visible = false;
        }
    }
    return TRUE;
}

BOOL UpdateWindow(HWND hWnd) {
    win32_window_t* ww = find_window(hWnd);
    if (ww && ww->wndproc) {
        ww->wndproc(hWnd, WM_PAINT, 0, 0);
    }
    return TRUE;
}

void win32_pump_events(void) {
    /* Called by GetMessage loop to sleep if empty */
}

BOOL GetMessageA(MSG* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax) {
    (void)hWnd; (void)wMsgFilterMin; (void)wMsgFilterMax;
    if (!lpMsg) return FALSE;
    
    while (win32_eq_head == win32_eq_tail) {
        win32_pump_events();
        __asm__ volatile("hlt");
    }
    
    *lpMsg = win32_msg_queue[win32_eq_tail];
    win32_eq_tail = (win32_eq_tail + 1) % WIN32_MAX_EVENTS;
    
    if (lpMsg->message == WM_QUIT) return FALSE;
    return TRUE;
}

BOOL TranslateMessage(const MSG* lpMsg) {
    (void)lpMsg;
    /* Do basic key translation if needed, but our kernel provides char keys */
    return TRUE;
}

LRESULT DispatchMessageA(const MSG* lpMsg) {
    if (!lpMsg) return 0;
    win32_window_t* ww = find_window(lpMsg->hwnd);
    if (ww && ww->wndproc) {
        return ww->wndproc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
    }
    return 0;
}

void PostQuitMessage(int nExitCode) {
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.message = WM_QUIT;
    msg.wParam = (WPARAM)nExitCode;
    push_msg(&msg);
}

LRESULT DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
    (void)wParam; (void)lParam;
    if (Msg == WM_CLOSE) {
        DestroyWindow(hWnd);
        return 0;
    }
    if (Msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return 0;
}

int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType) {
    (void)hWnd; (void)uType;
    /* Basic dialog display via msgbox in GUI subsystem or simple printf for now */
    if (fb_is_vesa()) {
        /* If GUI is up, we can use GUI messaging or simply alert */
        /* For this shim, let's just create a small NexusOS window if parent is null */
        int nid = window_create(lpCaption ? (char*)lpCaption : "Message", 20, 10, 40, 10, NULL, NULL);
        if (nid >= 0) {
            window_t* w = window_get(nid);
            if (w) {
                font_draw_string(w->px + 10, w->py + WIN_TITLEBAR_H + 20, lpText ? lpText : "", 0xFFFFFF, 0x000000);
            }
            /* Quick hack: wait for a key to close */
            window_focus(nid);
            /* This is a simple non-blocking draw for now. Full MB requires nested message loop. */
            return IDOK;
        }
    } else {
        vga_print_color("\n[MESSAGEBOX] ", VGA_COLOR(VGA_LIGHT_CYAN, VGA_BLACK));
        vga_print((char*)(lpCaption ? lpCaption : "Info"));
        vga_print(":\n ");
        vga_print((char*)(lpText ? lpText : ""));
        vga_print("\n");
        return IDOK;
    }
    return IDOK;
}

BOOL SetWindowTextA(HWND hWnd, LPCSTR lpString) {
    win32_window_t* ww = find_window(hWnd);
    if (!ww || !lpString) return FALSE;
    strncpy(ww->title, lpString, 63);
    ww->title[63] = '\0';
    if (ww->nx_win_id >= 0) {
        window_t* nw = window_get(ww->nx_win_id);
        if (nw) {
            strncpy(nw->title, lpString, WIN_TITLE_MAX - 1);
            nw->title[WIN_TITLE_MAX - 1] = '\0';
        }
    }
    return TRUE;
}

BOOL GetClientRect(HWND hWnd, RECT* lpRect) {
    win32_window_t* ww = find_window(hWnd);
    if (!ww || !lpRect) return FALSE;
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = ww->width;
    lpRect->bottom = ww->height;
    return TRUE;
}

BOOL InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase) {
    (void)lpRect; (void)bErase;
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hWnd;
    msg.message = WM_PAINT;
    push_msg(&msg);
    return TRUE;
}

HDC BeginPaint(HWND hWnd, PAINTSTRUCT* lpPaint) {
    if (lpPaint) {
        memset(lpPaint, 0, sizeof(PAINTSTRUCT));
        GetClientRect(hWnd, &lpPaint->rcPaint);
    }
    return (HDC)hWnd; /* Use HWND as HDC identifier for simple mapping */
}

BOOL EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint) {
    (void)hWnd; (void)lpPaint;
    return TRUE;
}

BOOL DestroyWindow(HWND hWnd) {
    win32_window_t* ww = find_window(hWnd);
    if (!ww) return FALSE;
    
    if (ww->wndproc) {
        ww->wndproc(hWnd, WM_DESTROY, 0, 0);
    }
    
    if (ww->nx_win_id >= 0) {
        window_destroy(ww->nx_win_id);
    }
    ww->active = false;
    return TRUE;
}

HDC GetDC(HWND hWnd) {
    return (HDC)hWnd;
}

int ReleaseDC(HWND hWnd, HDC hDC) {
    (void)hWnd; (void)hDC;
    return 1;
}

/* ============================================================================
 * GDI32
 * ============================================================================ */

/* Global GDI context state (simple context for shim) */
static DWORD gdi_current_color = 0xFFFFFF;
static DWORD gdi_current_bk    = 0x000000;
static DWORD gdi_current_brush = 0xFFFFFF;
static int   gdi_pen_x = 0, gdi_pen_y = 0;

BOOL TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c) {
    (void)c;
    win32_window_t* ww = find_window((HWND)hdc);
    if (!ww || !lpString) return FALSE;
    
    int ox, oy;
    if (get_content_origin(ww, &ox, &oy)) {
        font_draw_string(ox + x, oy + y, lpString, gdi_current_color, gdi_current_bk);
    }
    return TRUE;
}

DWORD SetBkColor(HDC hdc, DWORD color) {
    (void)hdc;
    DWORD old = gdi_current_bk;
    gdi_current_bk = color;
    return old;
}

DWORD SetTextColor(HDC hdc, DWORD color) {
    (void)hdc;
    DWORD old = gdi_current_color;
    gdi_current_color = color;
    return old;
}

HGDIOBJ GetStockObject(int i) {
    if (i == NULL_BRUSH) return (HGDIOBJ)-1;
    return (HGDIOBJ)i;
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h) {
    (void)hdc;
    return h; /* Dummy */
}

HBRUSH CreateSolidBrush(DWORD color) {
    gdi_current_brush = color;
    return (HBRUSH)color;
}

BOOL DeleteObject(HGDIOBJ ho) {
    (void)ho;
    return TRUE;
}

int FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr) {
    win32_window_t* ww = find_window((HWND)hDC);
    if (!ww || !lprc) return 0;
    
    DWORD color = (hbr == (HBRUSH)WHITE_BRUSH) ? 0xFFFFFF :
                  (hbr == (HBRUSH)BLACK_BRUSH) ? 0x000000 :
                  (hbr == (HBRUSH)GRAY_BRUSH)  ? 0x808080 :
                  (DWORD)hbr;
                  
    int ox, oy;
    if (get_content_origin(ww, &ox, &oy)) {
        gfx_fill_rect(ox + lprc->left, oy + lprc->top, 
                      lprc->right - lprc->left, lprc->bottom - lprc->top, color);
    }
    return 1;
}

BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom) {
    win32_window_t* ww = find_window((HWND)hdc);
    if (!ww) return FALSE;
    int ox, oy;
    if (get_content_origin(ww, &ox, &oy)) {
        gfx_draw_rect(ox + left, oy + top, right - left, bottom - top, gdi_current_color);
    }
    return TRUE;
}

BOOL MoveToEx(HDC hdc, int x, int y, POINT* lppt) {
    (void)hdc;
    if (lppt) { lppt->x = gdi_pen_x; lppt->y = gdi_pen_y; }
    gdi_pen_x = x;
    gdi_pen_y = y;
    return TRUE;
}

BOOL LineTo(HDC hdc, int x, int y) {
    win32_window_t* ww = find_window((HWND)hdc);
    if (!ww) return FALSE;
    int ox, oy;
    if (get_content_origin(ww, &ox, &oy)) {
        gfx_draw_line(ox + gdi_pen_x, oy + gdi_pen_y, ox + x, oy + y, gdi_current_color);
    }
    gdi_pen_x = x;
    gdi_pen_y = y;
    return TRUE;
}

DWORD SetPixel(HDC hdc, int x, int y, DWORD color) {
    win32_window_t* ww = find_window((HWND)hdc);
    if (!ww) return 0;
    int ox, oy;
    if (get_content_origin(ww, &ox, &oy)) {
        fb_putpixel(ox + x, oy + y, color);
    }
    return color;
}

/* ============================================================================
 * Import Resolver (Bridge from pe.c)
 * ============================================================================ */

typedef struct {
    const char* func_name;
    void* addr;
} win32_export_t;

static const win32_export_t kernel32_exports[] = {
    {"GetModuleHandleA", (void*)GetModuleHandleA},
    {"ExitProcess", (void*)ExitProcess},
    {"GetLastError", (void*)GetLastError},
    {"SetLastError", (void*)SetLastError},
    {"GetTickCount", (void*)GetTickCount},
    {"Sleep", (void*)Sleep},
    {"GetCommandLineA", (void*)GetCommandLineA},
    {"OutputDebugStringA", (void*)OutputDebugStringA},
    {"CreateFileA", (void*)CreateFileA},
    {"ReadFile", (void*)ReadFile},
    {"WriteFile", (void*)WriteFile},
    {"CloseHandle", (void*)CloseHandle},
    {"GetFileSize", (void*)GetFileSize},
    {"GetCurrentProcessId", (void*)GetCurrentProcessId},
    {"GetProcessHeap", (void*)GetProcessHeap},
    {"HeapAlloc", (void*)HeapAlloc},
    {"HeapFree", (void*)HeapFree},
    {NULL, NULL}
};

static const win32_export_t user32_exports[] = {
    {"RegisterClassA", (void*)RegisterClassA},
    {"CreateWindowExA", (void*)CreateWindowExA},
    {"ShowWindow", (void*)ShowWindow},
    {"UpdateWindow", (void*)UpdateWindow},
    {"GetMessageA", (void*)GetMessageA},
    {"TranslateMessage", (void*)TranslateMessage},
    {"DispatchMessageA", (void*)DispatchMessageA},
    {"PostQuitMessage", (void*)PostQuitMessage},
    {"DefWindowProcA", (void*)DefWindowProcA},
    {"MessageBoxA", (void*)MessageBoxA},
    {"SetWindowTextA", (void*)SetWindowTextA},
    {"GetClientRect", (void*)GetClientRect},
    {"InvalidateRect", (void*)InvalidateRect},
    {"BeginPaint", (void*)BeginPaint},
    {"EndPaint", (void*)EndPaint},
    {"DestroyWindow", (void*)DestroyWindow},
    {"GetDC", (void*)GetDC},
    {"ReleaseDC", (void*)ReleaseDC},
    {NULL, NULL}
};

static const win32_export_t gdi32_exports[] = {
    {"TextOutA", (void*)TextOutA},
    {"SetBkColor", (void*)SetBkColor},
    {"SetTextColor", (void*)SetTextColor},
    {"GetStockObject", (void*)GetStockObject},
    {"SelectObject", (void*)SelectObject},
    {"CreateSolidBrush", (void*)CreateSolidBrush},
    {"DeleteObject", (void*)DeleteObject},
    {"FillRect", (void*)FillRect},
    {"Rectangle", (void*)Rectangle},
    {"MoveToEx", (void*)MoveToEx},
    {"LineTo", (void*)LineTo},
    {"SetPixel", (void*)SetPixel},
    {NULL, NULL}
};

static const win32_export_t advapi32_exports[] = {
    {"RegOpenKeyExA", (void*)RegOpenKeyExA},
    {"RegCreateKeyExA", (void*)RegCreateKeyExA},
    {"RegCloseKey", (void*)RegCloseKey},
    {"RegQueryValueExA", (void*)RegQueryValueExA},
    {"RegSetValueExA", (void*)RegSetValueExA},
    {NULL, NULL}
};

uint32_t win32_resolve_import(const char* dll_name, const char* func_name) {
    if (!dll_name || !func_name) return 0;
    
    char name_lower[64];
    strncpy(name_lower, dll_name, 63);
    for (int i = 0; name_lower[i]; i++) {
        if (name_lower[i] >= 'A' && name_lower[i] <= 'Z')
            name_lower[i] += 32;
    }
    
    const win32_export_t* list = NULL;
    if (strstr(name_lower, "kernel32")) list = kernel32_exports;
    else if (strstr(name_lower, "user32")) list = user32_exports;
    else if (strstr(name_lower, "gdi32")) list = gdi32_exports;
    else if (strstr(name_lower, "advapi32")) list = advapi32_exports;
    
    if (list) {
        for (int i = 0; list[i].func_name != NULL; i++) {
            if (strcmp(list[i].func_name, func_name) == 0) {
                return (uint32_t)list[i].addr;
            }
        }
    }
    
    vga_print_color("[!!] ", VGA_COLOR(VGA_LIGHT_RED, VGA_BLACK));
    vga_print("Win32: Unresolved import ");
    vga_print(dll_name);
    vga_print(" -> ");
    vga_print((char*)func_name);
    vga_print("\n");
    return 0; /* Unresolved imports will crash the PE if called */
}

/* ============================================================================
 * Internal Subsytem
 * ============================================================================ */

void win32_init(void) {
    memset(win32_windows, 0, sizeof(win32_windows));
    memset(win32_classes, 0, sizeof(win32_classes));
    memset(win32_handles, 0, sizeof(win32_handles));
    memset(win32_msg_queue, 0, sizeof(win32_msg_queue));
    
    win32_eq_head = 0;
    win32_eq_tail = 0;
    win32_next_id = 1000;
    win32_last_error = 0;
    win32_initialized = true;

    vga_print_color("[OK] ", VGA_COLOR(VGA_LIGHT_GREEN, VGA_BLACK));
    vga_print("Win32 API Layer initialized (Phase 34)\n");
}

int win32_count_active_windows(void) {
    int w = 0;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) if (win32_windows[i].active) w++;
    return w;
}
int win32_count_active_classes(void) {
    int c = 0;
    for (int i = 0; i < WIN32_MAX_CLASSES; i++) if (win32_classes[i].active) c++;
    return c;
}
int win32_count_active_handles(void) {
    int h = 0;
    for (int i = 0; i < WIN32_MAX_HANDLES; i++) if (win32_handles[i].active) h++;
    return h;
}

static char win32_status_buf[80];
const char* win32_get_status(void) {
    int w = 0, c = 0, h = 0;
    for (int i = 0; i < WIN32_MAX_WINDOWS; i++) if (win32_windows[i].active) w++;
    for (int i = 0; i < WIN32_MAX_CLASSES; i++) if (win32_classes[i].active) c++;
    for (int i = 0; i < WIN32_MAX_HANDLES; i++) if (win32_handles[i].active) h++;

    char n1[12], n2[12], n3[12];
    int_to_str(w, n1);
    int_to_str(c, n2);
    int_to_str(h, n3);

    strcpy(win32_status_buf, "Win32: ");
    strcat(win32_status_buf, n1);
    strcat(win32_status_buf, " windows, ");
    strcat(win32_status_buf, n2);
    strcat(win32_status_buf, " classes, ");
    strcat(win32_status_buf, n3);
    strcat(win32_status_buf, " handles");
    return win32_status_buf;
}

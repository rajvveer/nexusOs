/* ============================================================================
 * NexusOS — Win32 Compatibility Shim (Header) — Phase 34
 * ============================================================================
 * Defines Win32 types, constants, and API mapped to NexusOS primitives.
 * ============================================================================ */

#ifndef WIN32_H
#define WIN32_H

#include "types.h"

/* ============================================================================
 * Basic Types
 * ============================================================================ */
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t  BYTE;
typedef int      BOOL;
typedef int32_t  LONG;
typedef uint32_t UINT;

typedef void*    HANDLE;
typedef HANDLE   HMODULE;
typedef HANDLE   HINSTANCE;
typedef HANDLE   HWND;
typedef HANDLE   HDC;
typedef HANDLE   HBRUSH;
typedef HANDLE   HGDIOBJ;

typedef uint32_t WPARAM;
typedef uint32_t LPARAM;
typedef uint32_t LRESULT;

typedef char*    LPSTR;
typedef const char* LPCSTR;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

/* ============================================================================
 * Constants
 * ============================================================================ */

/* Window Styles */
#define WS_OVERLAPPED       0x00000000L
#define WS_POPUP            0x80000000L
#define WS_CHILD            0x40000000L
#define WS_MINIMIZE         0x20000000L
#define WS_VISIBLE          0x10000000L
#define WS_DISABLED         0x08000000L
#define WS_CLIPSIBLINGS     0x04000000L
#define WS_CLIPCHILDREN     0x02000000L
#define WS_MAXIMIZE         0x01000000L
#define WS_CAPTION          0x00C00000L     /* WS_BORDER | WS_DLGFRAME  */
#define WS_BORDER           0x00800000L
#define WS_DLGFRAME         0x00400000L
#define WS_VSCROLL          0x00200000L
#define WS_HSCROLL          0x00100000L
#define WS_SYSMENU          0x00080000L
#define WS_THICKFRAME       0x00040000L
#define WS_MINIMIZEBOX      0x00020000L
#define WS_MAXIMIZEBOX      0x00010000L
#define WS_GROUP            0x00020000L
#define WS_TABSTOP          0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

#define CW_USEDEFAULT       ((int)0x80000000)

/* Class Styles */
#define CS_HREDRAW          0x0002
#define CS_VREDRAW          0x0001
#define CS_OWNDC            0x0020
#define CS_DBLCLKS          0x0008

/* Show Window commands */
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_NORMAL           1
#define SW_SHOWMINIMIZED    2
#define SW_SHOWMAXIMIZED    3
#define SW_MAXIMIZE         3
#define SW_SHOWNOACTIVATE   4
#define SW_SHOW             5
#define SW_MINIMIZE         6

/* Window Messages */
#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_ENABLE           0x000A
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202

/* MessageBox Types & Returns */
#define MB_OK               0x00000000L
#define MB_OKCANCEL         0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL      0x00000003L
#define MB_YESNO            0x00000004L
#define MB_RETRYCANCEL      0x00000005L

#define IDOK                1
#define IDCANCEL            2
#define IDABORT             3
#define IDRETRY             4
#define IDIGNORE            5
#define IDYES               6
#define IDNO                7

/* GDI Colors */
#define RGB(r,g,b)          ((DWORD)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))

/* Stock Objects */
#define WHITE_BRUSH         0
#define LTGRAY_BRUSH        1
#define GRAY_BRUSH          2
#define DKGRAY_BRUSH        3
#define BLACK_BRUSH         4
#define NULL_BRUSH          5

/* ============================================================================
 * Structures
 * ============================================================================ */

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra;
    int       cbWndExtra;
    HINSTANCE hInstance;
    HANDLE    hIcon;
    HANDLE    hCursor;
    HBRUSH    hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
} WNDCLASS;

typedef struct {
    HWND    hwnd;
    UINT    message;
    WPARAM  wParam;
    LPARAM  lParam;
    DWORD   time;
    struct {
        LONG x;
        LONG y;
    } pt;
} MSG;

typedef struct {
    LONG x;
    LONG y;
} POINT;

typedef struct {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
} RECT;

typedef struct {
    HDC  hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/* Kernel32 */
HMODULE GetModuleHandleA(LPCSTR lpModuleName);
void    ExitProcess(UINT uExitCode);
DWORD   GetLastError(void);
void    SetLastError(DWORD dwErrCode);
DWORD   GetTickCount(void);
void    Sleep(DWORD dwMilliseconds);
LPSTR   GetCommandLineA(void);
void    OutputDebugStringA(LPCSTR lpOutputString);

HANDLE  CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, void* lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);
BOOL    ReadFile(HANDLE hFile, void* lpBuffer, DWORD nNumberOfBytesToRead, DWORD* lpNumberOfBytesRead, void* lpOverlapped);
BOOL    WriteFile(HANDLE hFile, const void* lpBuffer, DWORD nNumberOfBytesToWrite, DWORD* lpNumberOfBytesWritten, void* lpOverlapped);
BOOL    CloseHandle(HANDLE hObject);
DWORD   GetFileSize(HANDLE hFile, DWORD* lpFileSizeHigh);
DWORD   GetCurrentProcessId(void);
HANDLE  GetProcessHeap(void);
void*   HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD dwBytes);
BOOL    HeapFree(HANDLE hHeap, DWORD dwFlags, void* lpMem);

/* User32 */
WORD    RegisterClassA(const WNDCLASS* lpWndClass);
HWND    CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HANDLE hMenu, HINSTANCE hInstance, void* lpParam);
BOOL    ShowWindow(HWND hWnd, int nCmdShow);
BOOL    UpdateWindow(HWND hWnd);
BOOL    GetMessageA(MSG* lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax);
BOOL    TranslateMessage(const MSG* lpMsg);
LRESULT DispatchMessageA(const MSG* lpMsg);
void    PostQuitMessage(int nExitCode);
LRESULT DefWindowProcA(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
int     MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType);
BOOL    SetWindowTextA(HWND hWnd, LPCSTR lpString);
BOOL    GetClientRect(HWND hWnd, RECT* lpRect);
BOOL    InvalidateRect(HWND hWnd, const RECT* lpRect, BOOL bErase);
HDC     BeginPaint(HWND hWnd, PAINTSTRUCT* lpPaint);
BOOL    EndPaint(HWND hWnd, const PAINTSTRUCT* lpPaint);
BOOL    DestroyWindow(HWND hWnd);
HDC     GetDC(HWND hWnd);
int     ReleaseDC(HWND hWnd, HDC hDC);

/* GDI32 */
BOOL    TextOutA(HDC hdc, int x, int y, LPCSTR lpString, int c);
DWORD   SetBkColor(HDC hdc, DWORD color);
DWORD   SetTextColor(HDC hdc, DWORD color);
HGDIOBJ GetStockObject(int i);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h);
HBRUSH  CreateSolidBrush(DWORD color);
BOOL    DeleteObject(HGDIOBJ ho);
int     FillRect(HDC hDC, const RECT* lprc, HBRUSH hbr);
BOOL    Rectangle(HDC hdc, int left, int top, int right, int bottom);
BOOL    MoveToEx(HDC hdc, int x, int y, POINT* lppt);
BOOL    LineTo(HDC hdc, int x, int y);
DWORD   SetPixel(HDC hdc, int x, int y, DWORD color);

/* ============================================================================
 * Internal Subsytem
 * ============================================================================ */

void    win32_init(void);
const char* win32_get_status(void);
uint32_t win32_resolve_import(const char* dll_name, const char* func_name);
void    win32_pump_events(void);

#endif /* WIN32_H */

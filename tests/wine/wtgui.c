/* NexusOS Wine campaign: Win32 GUI test program (x86_64-windows-gnu and x86-windows-gnu, zig cc).
 *
 *   wtgui.exe [--title TEXT]
 *
 * One main window: a menu bar (File: New Window, Dialog..., Open File..., Exit; Help: About), a painted test area
 * with four colour bars, an edit box, a button, a check box and a list box. Everything that happens is printed to
 * stdout as "GUI ..." lines, which tests/test_wine.py matches while it drives the window through the Aurora
 * compositor with QMP input:
 *   GUI READY bits=N hwnd=H client=X,Y size=W,H window=L,T,R,B caption=CXSIZE,CYSIZE,CYCAPTION frame=X,Y buttons=...
 *     (client: client-area origin inside the window; window: GetWindowRect; buttons: WM_GETTITLEBARINFOEX, zero on Wine)
 *   GUI CLICK x=.. y=.. (painted area, client coordinates)     GUI CHAR c=..      GUI EDIT text=...
 *   GUI BUTTON pressed  GUI CHECK state=N   GUI SIZE type=N w=.. h=..   GUI ACTIVATE state=N   GUI FOCUS
 *   GUI WINDOW n=2 created / closed   GUI DIALOG open / closed result=N   GUI MENU id=N   GUI EXIT code=N
 *   GUI NCDOWN / NCDBLCLK hit=HT.. x= y=   GUI SYSCOMMAND cmd=SC_..   GUI ICONIC ... (geometry 300 ms after minimizing)
 * Keyboard shortcuts inside the main window (for the test): F2 opens the modal dialog, F3 a second window,
 * F5 maximizes, F6 restores, F7 minimizes, F8 closes, F9 prints GEOMETRY, F11 minimizes and restores itself 2 s later
 * (ShowWindow(SW_RESTORE) from a timer: a restore the program asks for). With WTGUI_LOG=FILE in the environment the
 * lines are also appended to FILE (programs started from the desktop have no console). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_EDIT 101
#define ID_BUTTON 102
#define ID_CHECK 103
#define ID_LIST 104
#define ID_NEWWIN 201
#define ID_DIALOG 202
#define ID_OPEN 203
#define ID_EXIT 204
#define ID_ABOUT 205
#define AREA_X 10
#define AREA_Y 10
#define AREA_W 400
#define AREA_H 150
static HWND main_window, edit, button, check, list, second_window;
static HINSTANCE instance;
static int windows_open;
static const int BITS = (int)(sizeof(void*) * 8);

static void say(const char* format, ...) {
    char text[1024]; va_list list_args; va_start(list_args, format); vsnprintf(text, sizeof(text), format, list_args); va_end(list_args);
    printf("GUI %s\n", text); fflush(stdout);
    const char* log = getenv("WTGUI_LOG");   /* started from the desktop (no console): the same lines into a file */
    if (log && log[0]) { FILE* f = fopen(log, "a"); if (f) { fprintf(f, "GUI %s\n", text); fclose(f); } }
}
static void report_geometry(HWND hwnd, const char* label) {
    RECT window, client; POINT origin = {0, 0};
    GetWindowRect(hwnd, &window); GetClientRect(hwnd, &client); ClientToScreen(hwnd, &origin);
    TITLEBARINFOEX bar = {.cbSize = sizeof(bar)}; SendMessageW(hwnd, WM_GETTITLEBARINFOEX, 0, (LPARAM)&bar);
    char buttons[256] = ""; const char* names[] = {"min", "max", "help", "close"};
    for (int i = 0; i < 4; i++) {
        RECT* r = &bar.rgrect[i + 2]; char part[64];
        snprintf(part, sizeof(part), " %s:%ld,%ld,%ld,%ld", names[i], r->left - window.left, r->top - window.top, r->right - window.left, r->bottom - window.top);
        strcat(buttons, part);
    }
    say("%s bits=%d hwnd=%p client=%ld,%ld size=%ld,%ld window=%ld,%ld,%ld,%ld caption=%d,%d,%d frame=%d,%d buttons=%s", label, BITS, (void*)hwnd,
        origin.x - window.left, origin.y - window.top, client.right, client.bottom, window.left, window.top, window.right, window.bottom,
        GetSystemMetrics(SM_CXSIZE), GetSystemMetrics(SM_CYSIZE), GetSystemMetrics(SM_CYCAPTION),
        GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER), buttons);
}
/* in-memory dialog template: caption, a static text and an OK button */
static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    if (message == WM_INITDIALOG) { report_geometry(dlg, "DIALOG open"); return TRUE; }
    if (message == WM_COMMAND && (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) { EndDialog(dlg, LOWORD(wparam)); return TRUE; }
    return FALSE;
}
static INT_PTR show_dialog(HWND owner) {
    static WORD buffer[512]; WORD* p = buffer; memset(buffer, 0, sizeof(buffer));
    DLGTEMPLATE* t = (DLGTEMPLATE*)p;
    t->style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_SETFONT | DS_CENTER; t->cdit = 2; t->cx = 200; t->cy = 80;
    p = (WORD*)(t + 1); *p++ = 0; *p++ = 0;                                     /* no menu, default class */
    const wchar_t* caption = L"Wine Test Dialog"; wcscpy((wchar_t*)p, caption); p += wcslen(caption) + 1;
    *p++ = 9; wcscpy((wchar_t*)p, L"Tahoma"); p += 7;                         /* DS_SETFONT */
    p = (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    DLGITEMTEMPLATE* item = (DLGITEMTEMPLATE*)p; item->style = WS_CHILD | WS_VISIBLE | SS_LEFT; item->x = 10; item->y = 10; item->cx = 180; item->cy = 20; item->id = 1;
    p = (WORD*)(item + 1); *p++ = 0xFFFF; *p++ = 0x0082;                       /* static */
    const wchar_t* text = L"Modal dialog from a template. Press Enter."; wcscpy((wchar_t*)p, text); p += wcslen(text) + 1; *p++ = 0;
    p = (WORD*)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    item = (DLGITEMTEMPLATE*)p; item->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP; item->x = 75; item->y = 55; item->cx = 50; item->cy = 14; item->id = IDOK;
    p = (WORD*)(item + 1); *p++ = 0xFFFF; *p++ = 0x0080;                       /* button */
    wcscpy((wchar_t*)p, L"OK"); p += 3; *p++ = 0;
    INT_PTR result = DialogBoxIndirectW(instance, t, owner, dialog_proc);
    say("DIALOG closed result=%d", (int)result); return result;
}
static void paint(HWND hwnd) {
    PAINTSTRUCT ps; HDC dc = BeginPaint(hwnd, &ps);
    static const COLORREF colors[4] = {RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 255)};
    HBRUSH background = CreateSolidBrush(RGB(0, 0x80, 0x80)); RECT area = {AREA_X, AREA_Y, AREA_X + AREA_W, AREA_Y + AREA_H};
    FillRect(dc, &area, background); DeleteObject(background);
    for (int i = 0; i < 4; i++) {
        HBRUSH brush = CreateSolidBrush(colors[i]); RECT bar = {AREA_X + 20 + i * 95, AREA_Y + 40, AREA_X + 20 + i * 95 + 80, AREA_Y + 110};
        FillRect(dc, &bar, brush); DeleteObject(brush);
    }
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, AREA_X + 20, AREA_Y + 12, L"NexusOS Wine GUI test \x00e9\x03a9\x65e5", 25);
    EndPaint(hwnd, &ps);
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    int number = hwnd == main_window ? 1 : 2;
    switch (message) {
    case WM_CREATE:
        if (!main_window) {
            edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, AREA_X, AREA_Y + AREA_H + 10, 260, 24, hwnd, (HMENU)ID_EDIT, instance, NULL);
            button = CreateWindowExW(0, L"BUTTON", L"Press me", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, AREA_X + 270, AREA_Y + AREA_H + 10, 130, 26, hwnd, (HMENU)ID_BUTTON, instance, NULL);
            check = CreateWindowExW(0, L"BUTTON", L"Check me", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, AREA_X, AREA_Y + AREA_H + 44, 160, 24, hwnd, (HMENU)ID_CHECK, instance, NULL);
            list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | LBS_NOTIFY, AREA_X + 170, AREA_Y + AREA_H + 44, 230, 70, hwnd, (HMENU)ID_LIST, instance, NULL);
            const wchar_t* items[] = {L"alpha", L"beta", L"gamma"};
            for (int i = 0; i < 3; i++) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)items[i]);
        }
        windows_open++; return 0;
    case WM_PAINT: paint(hwnd); return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
        say("CLICK window=%d x=%d y=%d", number, x, y);
        if (x >= AREA_X && x < AREA_X + AREA_W && y >= AREA_Y && y < AREA_Y + AREA_H) SetFocus(edit);
        return 0;
    }
    case WM_RBUTTONDOWN: say("RCLICK window=%d x=%d y=%d", number, (short)LOWORD(lparam), (short)HIWORD(lparam)); return 0;
    case WM_MOUSEWHEEL: say("WHEEL delta=%d", (short)HIWORD(wparam)); return 0;
    case WM_KEYDOWN:
        say("KEY window=%d vk=0x%02x", number, (unsigned)wparam);
        if (wparam == VK_F2) show_dialog(hwnd);
        else if (wparam == VK_F3) SendMessageW(hwnd, WM_COMMAND, ID_NEWWIN, 0);
        else if (wparam == VK_F5) ShowWindow(hwnd, SW_MAXIMIZE);
        else if (wparam == VK_F6) ShowWindow(hwnd, SW_RESTORE);
        else if (wparam == VK_F7) ShowWindow(hwnd, SW_MINIMIZE);
        else if (wparam == VK_F8) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        else if (wparam == VK_F9) report_geometry(hwnd, "GEOMETRY");
        else if (wparam == VK_F11) { ShowWindow(hwnd, SW_MINIMIZE); SetTimer(hwnd, 2, 2000, NULL); }
        return 0;
    case WM_CHAR: say("CHAR window=%d c=0x%02x", number, (unsigned)wparam); return 0;
    case WM_SIZE:
        say("SIZE window=%d type=%u w=%u h=%u", number, (unsigned)wparam, LOWORD(lparam), HIWORD(lparam));
        if (wparam == SIZE_MINIMIZED) SetTimer(hwnd, 1, 300, NULL);   /* report where the iconic window went */
        return 0;
    case WM_TIMER: if (wparam == 1) { KillTimer(hwnd, 1); report_geometry(hwnd, "ICONIC"); }
        else if (wparam == 2) { KillTimer(hwnd, 2); ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        return 0;
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
        say("NC%s window=%d hit=%u x=%d y=%d", message == WM_NCLBUTTONDOWN ? "DOWN" : "DBLCLK", number, (unsigned)wparam, (short)LOWORD(lparam), (short)HIWORD(lparam));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SYSCOMMAND: say("SYSCOMMAND window=%d cmd=0x%04x", number, (unsigned)(wparam & 0xfff0)); return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_ACTIVATE: say("ACTIVATE window=%d state=%u", number, (unsigned)LOWORD(wparam)); return DefWindowProcW(hwnd, message, wparam, lparam);
    case WM_SETFOCUS: say("FOCUS window=%d", number); return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_EDIT: if (HIWORD(wparam) == EN_CHANGE) { char text[256]; GetWindowTextA(edit, text, sizeof(text)); say("EDIT text=%s", text); } return 0;
        case ID_BUTTON: if (HIWORD(wparam) == BN_CLICKED) say("BUTTON pressed"); return 0;
        case ID_CHECK: if (HIWORD(wparam) == BN_CLICKED) say("CHECK state=%d", (int)SendMessageW(check, BM_GETCHECK, 0, 0)); return 0;
        case ID_LIST: if (HIWORD(wparam) == LBN_SELCHANGE) say("LIST selected=%d", (int)SendMessageW(list, LB_GETCURSEL, 0, 0)); return 0;
        case ID_NEWWIN:
            say("MENU id=%d", ID_NEWWIN);
            if (!second_window) {
                second_window = CreateWindowExW(0, L"NexusWineTest", L"Wine Second Window", WS_OVERLAPPEDWINDOW, 200, 200, 360, 240, NULL, NULL, instance, NULL);
                ShowWindow(second_window, SW_SHOW); UpdateWindow(second_window); say("WINDOW n=2 created"); report_geometry(second_window, "WINDOW2");
            }
            return 0;
        case ID_DIALOG: say("MENU id=%d", ID_DIALOG); show_dialog(hwnd); return 0;
        case ID_OPEN: {
            say("MENU id=%d", ID_OPEN); wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = {.lStructSize = sizeof(ofn), .hwndOwner = hwnd, .lpstrFile = file, .nMaxFile = MAX_PATH, .lpstrFilter = L"All files\0*.*\0", .Flags = OFN_EXPLORER};
            BOOL ok = GetOpenFileNameW(&ofn); say("OPENFILE result=%d file=%ls error=%lu", ok, file, CommDlgExtendedError()); return 0;
        }
        case ID_EXIT: say("MENU id=%d", ID_EXIT); PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case ID_ABOUT: say("MENU id=%d", ID_ABOUT); MessageBoxW(hwnd, L"NexusOS Wine GUI test", L"About", MB_OK | MB_ICONINFORMATION); say("ABOUT closed"); return 0;
        }
        break;
    case WM_CLOSE: say("CLOSE window=%d", number); DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        windows_open--;
        if (hwnd == second_window) { second_window = NULL; say("WINDOW n=2 closed"); }
        if (hwnd == main_window) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE hinstance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)show; instance = hinstance;
    setvbuf(stdout, NULL, _IONBF, 0);
    const wchar_t* title = L"Wine GUI Test";
    wchar_t custom[128];
    if (command_line && !wcsncmp(command_line, L"--title ", 8)) { wcsncpy(custom, command_line + 8, 127); custom[127] = 0; title = custom; }
    WNDCLASSW wc = {.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS, .lpfnWndProc = window_proc, .hInstance = hinstance,
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1), .lpszClassName = L"NexusWineTest",
                    .hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION)};
    if (!RegisterClassW(&wc)) { say("FAIL RegisterClass %lu", GetLastError()); return 1; }
    HMENU bar = CreateMenu(), file_menu = CreatePopupMenu(), help_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, ID_NEWWIN, L"&New Window\tF3"); AppendMenuW(file_menu, MF_STRING, ID_DIALOG, L"&Dialog...\tF2");
    AppendMenuW(file_menu, MF_STRING, ID_OPEN, L"&Open File..."); AppendMenuW(file_menu, MF_SEPARATOR, 0, NULL); AppendMenuW(file_menu, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(help_menu, MF_STRING, ID_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file_menu, L"&File"); AppendMenuW(bar, MF_POPUP, (UINT_PTR)help_menu, L"&Help");
    main_window = CreateWindowExW(0, L"NexusWineTest", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 460, 420, NULL, bar, hinstance, NULL);
    if (!main_window) { say("FAIL CreateWindow %lu", GetLastError()); return 1; }
    ShowWindow(main_window, SW_SHOWNORMAL); UpdateWindow(main_window);
    report_geometry(main_window, "READY");
    MSG msg; int code = 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == edit && msg.wParam >= VK_F2 && msg.wParam <= VK_F11) { SendMessageW(main_window, WM_KEYDOWN, msg.wParam, msg.lParam); continue; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    code = (int)msg.wParam;
    say("EXIT code=%d", code);
    return code;
}

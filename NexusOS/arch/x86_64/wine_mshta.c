/* mshta.exe for NexusOS's Wine 10.0, whose own mshta is an upstream stub ("mshta.exe is a stub!") and whose
 * mshtml!RunHTMLApplication is a FIXME. This is a small HTA host: a top-level window holding the WebBrowser control
 * (ieframe + mshtml, in place, the way hhctrl hosts help topics) that loads the .hta given on the command line, runs
 * its scripts and closes when the script calls window.close() (DWebBrowserEvents2::WindowClosing). The window title
 * follows the document's title (polled: Wine's ieframe sends no TitleChange). <body onload> is honoured even though
 * Wine's mshtml loses it after an <HTA:APPLICATION> tag in the head (body_onload below).
 *
 * Exit code: 0 the HTA closed itself (window.close), 3 the window was closed from outside, 1 nothing could be loaded.
 * Limit: Wine's mshtml checks every ActiveXObject against the zone policy and the safe-for-scripting category (its
 * secmgr.c has no host override), so objects that are not safe for scripting (Scripting.FileSystemObject,
 * WScript.Shell) stay refused, unlike on Windows where an HTA is trusted; XMLHttpRequest, the DOM and safe objects work.
 *
 * Built by arch/x86_64/winewayland_build.py (zig cc, x86_64 and x86 windows-gnu) and installed over the Debian package's
 * stub in the Wine library folders, marked as a Wine builtin ("Wine builtin DLL" at offset 0x40, as winebuild does).
 * It prints its progress on stderr ("mshta: ..."), which Wine passes to the terminal that started it. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <exdisp.h>
#include <exdispid.h>
#include <mshtmhst.h>
#include <mshtml.h>
#include <shlwapi.h>
#include <stdio.h>
#ifndef INTERNET_MAX_URL_LENGTH
#define INTERNET_MAX_URL_LENGTH 2084
#endif

struct host {
    IOleClientSite client;
    IOleInPlaceSite site;
    IOleInPlaceFrame frame;
    IDocHostUIHandler ui;
    IDispatch events;
    HWND hwnd;
    IOleObject* ole;
    IWebBrowser2* web;
    DWORD cookie;
    int exit_code;
};
static struct host host = {.exit_code = 3};

#define HOST_FROM(iface, field) (&host)
static HRESULT query(REFIID riid, void** out) {
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IOleClientSite)) *out = &host.client;
    else if (IsEqualIID(riid, &IID_IOleWindow) || IsEqualIID(riid, &IID_IOleInPlaceSite)) *out = &host.site;
    else if (IsEqualIID(riid, &IID_IOleInPlaceFrame) || IsEqualIID(riid, &IID_IOleInPlaceUIWindow)) *out = &host.frame;
    else if (IsEqualIID(riid, &IID_IDocHostUIHandler)) *out = &host.ui;
    else return E_NOINTERFACE;
    return S_OK;
}
static ULONG STDMETHODCALLTYPE add_ref(void* self) { (void)self; return 2; }   /* one static object: never freed */
static ULONG STDMETHODCALLTYPE release(void* self) { (void)self; return 1; }

/* ---------------------------------------------------------------- IOleClientSite */
static HRESULT STDMETHODCALLTYPE cs_qi(IOleClientSite* s, REFIID riid, void** out) { (void)s; return query(riid, out); }
static ULONG STDMETHODCALLTYPE cs_add(IOleClientSite* s) { return add_ref(s); }
static ULONG STDMETHODCALLTYPE cs_rel(IOleClientSite* s) { return release(s); }
static HRESULT STDMETHODCALLTYPE cs_save(IOleClientSite* s) { (void)s; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cs_moniker(IOleClientSite* s, DWORD a, DWORD w, IMoniker** m) { (void)s; (void)a; (void)w; *m = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE cs_container(IOleClientSite* s, IOleContainer** c) { (void)s; *c = NULL; return E_NOINTERFACE; }
static HRESULT STDMETHODCALLTYPE cs_show(IOleClientSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE cs_onshow(IOleClientSite* s, BOOL b) { (void)s; (void)b; return S_OK; }
static HRESULT STDMETHODCALLTYPE cs_layout(IOleClientSite* s) { (void)s; return E_NOTIMPL; }
static IOleClientSiteVtbl cs_vtbl = {cs_qi, cs_add, cs_rel, cs_save, cs_moniker, cs_container, cs_show, cs_onshow, cs_layout};

/* ---------------------------------------------------------------- IOleInPlaceSite */
static HRESULT STDMETHODCALLTYPE ip_qi(IOleInPlaceSite* s, REFIID riid, void** out) { (void)s; return query(riid, out); }
static ULONG STDMETHODCALLTYPE ip_add(IOleInPlaceSite* s) { return add_ref(s); }
static ULONG STDMETHODCALLTYPE ip_rel(IOleInPlaceSite* s) { return release(s); }
static HRESULT STDMETHODCALLTYPE ip_window(IOleInPlaceSite* s, HWND* w) { (void)s; *w = host.hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_help(IOleInPlaceSite* s, BOOL b) { (void)s; (void)b; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ip_can(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_activate(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_ui(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_context(IOleInPlaceSite* s, IOleInPlaceFrame** frame, IOleInPlaceUIWindow** doc, RECT* pos, RECT* clip, OLEINPLACEFRAMEINFO* info) {
    (void)s; *frame = &host.frame; *doc = NULL;
    GetClientRect(host.hwnd, pos); *clip = *pos;
    info->cb = sizeof(*info); info->fMDIApp = FALSE; info->hwndFrame = host.hwnd; info->haccel = NULL; info->cAccelEntries = 0;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ip_scroll(IOleInPlaceSite* s, SIZE e) { (void)s; (void)e; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ip_uideact(IOleInPlaceSite* s, BOOL u) { (void)s; (void)u; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_deact(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_discard(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_undo(IOleInPlaceSite* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ip_posrect(IOleInPlaceSite* s, LPCRECT r) { (void)s; (void)r; return S_OK; }
static IOleInPlaceSiteVtbl ip_vtbl = {ip_qi, ip_add, ip_rel, ip_window, ip_help, ip_can, ip_activate, ip_ui, ip_context, ip_scroll, ip_uideact, ip_deact, ip_discard, ip_undo, ip_posrect};

/* ---------------------------------------------------------------- IOleInPlaceFrame */
static HRESULT STDMETHODCALLTYPE fr_qi(IOleInPlaceFrame* s, REFIID riid, void** out) { (void)s; return query(riid, out); }
static ULONG STDMETHODCALLTYPE fr_add(IOleInPlaceFrame* s) { return add_ref(s); }
static ULONG STDMETHODCALLTYPE fr_rel(IOleInPlaceFrame* s) { return release(s); }
static HRESULT STDMETHODCALLTYPE fr_window(IOleInPlaceFrame* s, HWND* w) { (void)s; *w = host.hwnd; return S_OK; }
static HRESULT STDMETHODCALLTYPE fr_help(IOleInPlaceFrame* s, BOOL b) { (void)s; (void)b; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_border(IOleInPlaceFrame* s, LPRECT r) { (void)s; (void)r; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_request(IOleInPlaceFrame* s, LPCBORDERWIDTHS w) { (void)s; (void)w; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_setborder(IOleInPlaceFrame* s, LPCBORDERWIDTHS w) { (void)s; (void)w; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_active(IOleInPlaceFrame* s, IOleInPlaceActiveObject* o, LPCOLESTR n) { (void)s; (void)o; (void)n; return S_OK; }
static HRESULT STDMETHODCALLTYPE fr_insert(IOleInPlaceFrame* s, HMENU m, LPOLEMENUGROUPWIDTHS w) { (void)s; (void)m; (void)w; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_setmenu(IOleInPlaceFrame* s, HMENU m, HOLEMENU h, HWND w) { (void)s; (void)m; (void)h; (void)w; return S_OK; }
static HRESULT STDMETHODCALLTYPE fr_remove(IOleInPlaceFrame* s, HMENU m) { (void)s; (void)m; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE fr_status(IOleInPlaceFrame* s, LPCOLESTR t) { (void)s; (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE fr_modeless(IOleInPlaceFrame* s, BOOL e) { (void)s; (void)e; return S_OK; }
static HRESULT STDMETHODCALLTYPE fr_accel(IOleInPlaceFrame* s, LPMSG m, WORD id) { (void)s; (void)m; (void)id; return S_FALSE; }
static IOleInPlaceFrameVtbl fr_vtbl = {fr_qi, fr_add, fr_rel, fr_window, fr_help, fr_border, fr_request, fr_setborder, fr_active, fr_insert, fr_setmenu, fr_remove, fr_status, fr_modeless, fr_accel};

/* ---------------------------------------------------------------- IDocHostUIHandler: an application window, not a browser */
static HRESULT STDMETHODCALLTYPE ui_qi(IDocHostUIHandler* s, REFIID riid, void** out) { (void)s; return query(riid, out); }
static ULONG STDMETHODCALLTYPE ui_add(IDocHostUIHandler* s) { return add_ref(s); }
static ULONG STDMETHODCALLTYPE ui_rel(IDocHostUIHandler* s) { return release(s); }
static HRESULT STDMETHODCALLTYPE ui_menu(IDocHostUIHandler* s, DWORD id, POINT* p, IUnknown* c, IDispatch* o) { (void)s; (void)id; (void)p; (void)c; (void)o; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE ui_info(IDocHostUIHandler* s, DOCHOSTUIINFO* info) {
    (void)s; if (info->cbSize < sizeof(*info)) return E_INVALIDARG;
    info->dwFlags = DOCHOSTUIFLAG_NO3DBORDER | DOCHOSTUIFLAG_ENABLE_INPLACE_NAVIGATION; info->dwDoubleClick = DOCHOSTUIDBLCLK_DEFAULT;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE ui_show(IDocHostUIHandler* s, DWORD id, IOleInPlaceActiveObject* a, IOleCommandTarget* c, IOleInPlaceFrame* f, IOleInPlaceUIWindow* d) {
    (void)s; (void)id; (void)a; (void)c; (void)f; (void)d; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_hide(IDocHostUIHandler* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_update(IDocHostUIHandler* s) { (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_modeless(IDocHostUIHandler* s, BOOL e) { (void)s; (void)e; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_docact(IDocHostUIHandler* s, BOOL a) { (void)s; (void)a; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_frameact(IDocHostUIHandler* s, BOOL a) { (void)s; (void)a; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_resize(IDocHostUIHandler* s, LPCRECT r, IOleInPlaceUIWindow* w, BOOL f) { (void)s; (void)r; (void)w; (void)f; return S_OK; }
static HRESULT STDMETHODCALLTYPE ui_accel(IDocHostUIHandler* s, LPMSG m, const GUID* g, DWORD id) { (void)s; (void)m; (void)g; (void)id; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE ui_key(IDocHostUIHandler* s, LPOLESTR* k, DWORD r) { (void)s; (void)r; *k = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE ui_drop(IDocHostUIHandler* s, IDropTarget* d, IDropTarget** o) { (void)s; (void)d; *o = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ui_external(IDocHostUIHandler* s, IDispatch** d) { (void)s; *d = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ui_url(IDocHostUIHandler* s, DWORD t, LPWSTR in, LPWSTR* out) { (void)s; (void)t; (void)in; *out = NULL; return S_FALSE; }
static HRESULT STDMETHODCALLTYPE ui_data(IDocHostUIHandler* s, IDataObject* in, IDataObject** out) { (void)s; (void)in; *out = NULL; return S_FALSE; }
static IDocHostUIHandlerVtbl ui_vtbl = {ui_qi, ui_add, ui_rel, ui_menu, ui_info, ui_show, ui_hide, ui_update, ui_modeless, ui_docact, ui_frameact, ui_resize, ui_accel, ui_key, ui_drop, ui_external, ui_url, ui_data};

/* ---------------------------------------------------------------- DWebBrowserEvents2 */
static HRESULT STDMETHODCALLTYPE ev_qi(IDispatch* s, REFIID riid, void** out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &DIID_DWebBrowserEvents2)) { *out = s; return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE ev_add(IDispatch* s) { return add_ref(s); }
static ULONG STDMETHODCALLTYPE ev_rel(IDispatch* s) { return release(s); }
static HRESULT STDMETHODCALLTYPE ev_count(IDispatch* s, UINT* n) { (void)s; *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE ev_info(IDispatch* s, UINT i, LCID l, ITypeInfo** t) { (void)s; (void)i; (void)l; *t = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE ev_names(IDispatch* s, REFIID r, LPOLESTR* n, UINT c, LCID l, DISPID* d) { (void)s; (void)r; (void)n; (void)c; (void)l; (void)d; return E_NOTIMPL; }
/* <body onload="..."> of an HTA: <HTA:APPLICATION> in the head makes the HTML parser start the body early, the real
 * <body> tag's attributes are then merged into it, and Wine's mshtml never binds a handler for such an attribute. When
 * the body has an onload attribute but no onload handler, its script runs once the document is complete (what the
 * load event would have done). A handler set by the script itself (window.onload = ...) is left alone. */
static void body_onload(void) {
    IDispatch* disp = NULL; IHTMLDocument2* doc = NULL; IHTMLElement* body = NULL; IHTMLBodyElement* b = NULL; IHTMLWindow2* window = NULL;
    VARIANT attr, handler, result; VariantInit(&attr); VariantInit(&handler); VariantInit(&result);
    BSTR name = SysAllocString(L"onload");
    if (!host.web || FAILED(IWebBrowser2_get_Document(host.web, &disp)) || !disp) goto done;
    if (FAILED(IDispatch_QueryInterface(disp, &IID_IHTMLDocument2, (void**)&doc)) || FAILED(IHTMLDocument2_get_body(doc, &body)) || !body) goto done;
    if (FAILED(IHTMLElement_QueryInterface(body, &IID_IHTMLBodyElement, (void**)&b)) || FAILED(IHTMLBodyElement_get_onload(b, &handler))) goto done;
    if (V_VT(&handler) != VT_NULL && V_VT(&handler) != VT_EMPTY) goto done;   /* bound: the load event runs it */
    if (FAILED(IHTMLElement_getAttribute(body, name, 0, &attr)) || V_VT(&attr) != VT_BSTR || !V_BSTR(&attr) || !V_BSTR(&attr)[0]) goto done;
    if (FAILED(IHTMLDocument2_get_parentWindow(doc, &window)) || !window) goto done;
    BSTR language = SysAllocString(L"JScript");
    HRESULT hr = IHTMLWindow2_execScript(window, V_BSTR(&attr), language, &result);
    if (FAILED(hr)) { SysFreeString(language); language = SysAllocString(L"VBScript"); VariantClear(&result); hr = IHTMLWindow2_execScript(window, V_BSTR(&attr), language, &result); }
    SysFreeString(language);
    fprintf(stderr, "mshta: body onload run by the host (%08lx)\n", (unsigned long)hr); fflush(stderr);
done:
    VariantClear(&attr); VariantClear(&handler); VariantClear(&result); SysFreeString(name);
    if (window) IHTMLWindow2_Release(window);
    if (b) IHTMLBodyElement_Release(b);
    if (body) IHTMLElement_Release(body);
    if (doc) IHTMLDocument2_Release(doc);
    if (disp) IDispatch_Release(disp);
}
static HRESULT STDMETHODCALLTYPE ev_invoke(IDispatch* s, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS* p, VARIANT* res, EXCEPINFO* e, UINT* a) {
    (void)s; (void)r; (void)l; (void)f; (void)res; (void)e; (void)a;
    if (id == DISPID_NAVIGATECOMPLETE2 || id == DISPID_DOCUMENTCOMPLETE || id == DISPID_NAVIGATEERROR || id == DISPID_WINDOWCLOSING || id == DISPID_FILEDOWNLOAD)
        { fprintf(stderr, "mshta: event %ld\n", (long)id); fflush(stderr); }
    if (id == DISPID_WINDOWCLOSING && p && p->cArgs >= 2) {   /* window.close() from the HTA's script: allowed, the application ends */
        VARIANT* cancel = &p->rgvarg[0];
        if (V_VT(cancel) == (VT_BYREF | VT_BOOL)) *V_BOOLREF(cancel) = VARIANT_FALSE;
        host.exit_code = 0; PostMessageW(host.hwnd, WM_CLOSE, 0, 0);
    } else if (id == DISPID_TITLECHANGE && p && p->cArgs >= 1 && V_VT(&p->rgvarg[0]) == VT_BSTR && V_BSTR(&p->rgvarg[0])) {
        SetWindowTextW(host.hwnd, V_BSTR(&p->rgvarg[0])); fprintf(stderr, "mshta: title %ls\n", V_BSTR(&p->rgvarg[0])); fflush(stderr);
    } else if (id == DISPID_DOCUMENTCOMPLETE) {
        static BOOL loaded;
        if (!loaded) { loaded = TRUE; body_onload(); }
        SetTimer(host.hwnd, 1, 500, NULL);   /* the document's title (ieframe sends no TitleChange) */
    }
    return S_OK;
}
/* the window takes the document's title (checked after the load and then twice a second for a while) */
static void follow_title(void) {
    IDispatch* disp = NULL; IHTMLDocument2* doc = NULL; BSTR title = NULL;
    if (!host.web || FAILED(IWebBrowser2_get_Document(host.web, &disp)) || !disp) return;
    if (SUCCEEDED(IDispatch_QueryInterface(disp, &IID_IHTMLDocument2, (void**)&doc))) {
        if (SUCCEEDED(IHTMLDocument2_get_title(doc, &title)) && title && title[0]) {
            WCHAR now[512]; GetWindowTextW(host.hwnd, now, 512);
            if (lstrcmpW(now, title)) { SetWindowTextW(host.hwnd, title); fprintf(stderr, "mshta: title %ls\n", title); fflush(stderr); }
        }
        SysFreeString(title); IHTMLDocument2_Release(doc);
    }
    IDispatch_Release(disp);
}
static IDispatchVtbl ev_vtbl = {ev_qi, ev_add, ev_rel, ev_count, ev_info, ev_names, ev_invoke};

/* ---------------------------------------------------------------- window */
static void resize(void) {
    IOleInPlaceObject* inplace; RECT rc; GetClientRect(host.hwnd, &rc);
    if (host.ole && SUCCEEDED(IOleObject_QueryInterface(host.ole, &IID_IOleInPlaceObject, (void**)&inplace))) {
        IOleInPlaceObject_SetObjectRects(inplace, &rc, &rc); IOleInPlaceObject_Release(inplace);
    }
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_SIZE: resize(); return 0;
    case WM_TIMER: { static unsigned ticks; follow_title(); if (++ticks > 120) { KillTimer(hwnd, 1); ticks = 0; } return 0; }
    /* the application ends with its window: no teardown of the browser (closing it from inside its own script call
     * hung), the process exits after the message loop (ExitProcess) */
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); PostQuitMessage(host.exit_code); return 0;
    case WM_DESTROY: PostQuitMessage(host.exit_code); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

/* the first argument of the command line (quoted or not) */
static void first_argument(const WCHAR* line, WCHAR* out, size_t size) {
    while (*line == L' ' || *line == L'\t') line++;
    size_t n = 0; WCHAR end = L' ';
    if (*line == L'"') { end = L'"'; line++; }
    while (*line && *line != end && (end == L'"' || *line != L'\t') && n + 1 < size) out[n++] = *line++;
    out[n] = 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR command_line, int show) {
    (void)previous;
    WCHAR argument[MAX_PATH * 2], full[MAX_PATH * 2], url[INTERNET_MAX_URL_LENGTH]; DWORD length = INTERNET_MAX_URL_LENGTH;
    first_argument(command_line, argument, sizeof(argument) / sizeof(argument[0]));
    if (!argument[0]) { MessageBoxW(NULL, L"Usage: mshta file.hta", L"mshta", MB_OK); return 1; }
    if (PathIsURLW(argument)) lstrcpynW(url, argument, INTERNET_MAX_URL_LENGTH);
    else {
        if (!GetFullPathNameW(argument, sizeof(full) / sizeof(full[0]), full, NULL) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) return 1;
        if (FAILED(UrlCreateFromPathW(full, url, &length, 0))) return 1;
    }
    if (FAILED(OleInitialize(NULL))) return 1;
    host.client.lpVtbl = &cs_vtbl; host.site.lpVtbl = &ip_vtbl; host.frame.lpVtbl = &fr_vtbl; host.ui.lpVtbl = &ui_vtbl; host.events.lpVtbl = &ev_vtbl;
    WNDCLASSW wc = {0}; wc.lpfnWndProc = window_proc; wc.hInstance = instance; wc.lpszClassName = L"NexusOSHTA";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    host.hwnd = CreateWindowExW(0, wc.lpszClassName, PathFindFileNameW(argument), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, instance, NULL);
    if (!host.hwnd) return 1;
    HRESULT hr = CoCreateInstance(&CLSID_WebBrowser, NULL, CLSCTX_INPROC_SERVER, &IID_IOleObject, (void**)&host.ole);
    fprintf(stderr, "mshta: %ls -> WebBrowser %08lx\n", url, (unsigned long)hr); fflush(stderr);
    if (FAILED(hr)) { DestroyWindow(host.hwnd); return 1; }
    IOleObject_SetClientSite(host.ole, &host.client);
    RECT rc; GetClientRect(host.hwnd, &rc);
    if (FAILED(IOleObject_DoVerb(host.ole, OLEIVERB_INPLACEACTIVATE, NULL, &host.client, 0, host.hwnd, &rc)) ||
        FAILED(IOleObject_QueryInterface(host.ole, &IID_IWebBrowser2, (void**)&host.web))) { host.exit_code = 1; DestroyWindow(host.hwnd); goto loop; }
    IConnectionPointContainer* cpc; IConnectionPoint* cp;
    if (SUCCEEDED(IWebBrowser2_QueryInterface(host.web, &IID_IConnectionPointContainer, (void**)&cpc))) {
        if (SUCCEEDED(IConnectionPointContainer_FindConnectionPoint(cpc, &DIID_DWebBrowserEvents2, &cp))) { IConnectionPoint_Advise(cp, (IUnknown*)&host.events, &host.cookie); IConnectionPoint_Release(cp); }
        IConnectionPointContainer_Release(cpc);
    }
    VARIANT target; VariantInit(&target); V_VT(&target) = VT_BSTR; V_BSTR(&target) = SysAllocString(url);
    VARIANT empty; VariantInit(&empty);
    hr = IWebBrowser2_Navigate2(host.web, &target, &empty, &empty, &empty, &empty);
    fprintf(stderr, "mshta: navigate %08lx (events %s)\n", (unsigned long)hr, host.cookie ? "connected" : "not connected"); fflush(stderr);
    if (FAILED(hr)) { host.exit_code = 1; DestroyWindow(host.hwnd); }
    VariantClear(&target);
    if (IsWindow(host.hwnd)) { ShowWindow(host.hwnd, show == SW_HIDE ? SW_SHOWNORMAL : show); UpdateWindow(host.hwnd); }
loop:;
    MSG msg; int code = host.exit_code;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        IOleInPlaceActiveObject* active = NULL; BOOL done = FALSE;
        if (host.web && msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST &&
            SUCCEEDED(IWebBrowser2_QueryInterface(host.web, &IID_IOleInPlaceActiveObject, (void**)&active))) {
            done = IOleInPlaceActiveObject_TranslateAccelerator(active, &msg) == S_OK; IOleInPlaceActiveObject_Release(active);
        }
        if (!done) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (msg.message == WM_QUIT) break;
    }
    code = host.exit_code;
    fprintf(stderr, "mshta: exit %d (%s)\n", code, code == 0 ? "window.close" : code == 3 ? "window closed" : "not loaded"); fflush(stderr);
    ExitProcess((UINT)code);
}

/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Program entry: single instance, tray icon, global hotkey and the capture flow.
 */
#include "dofram.h"
#include "gdip.h"
#include "resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

HINSTANCE g_inst;

#define HOTKEY_ID       0x4446
#define TIMER_CAPTURE   1
#define TIMER_START     2
#define WM_APP_CAPTURE  (WM_APP + 20)
#define WM_APP_SETTINGS (WM_APP + 21)

enum { IDM_REGION = 1, IDM_FULL, IDM_DELAY, IDM_FOLDER, IDM_SETTINGS, IDM_INSTALL, IDM_UNINSTALL, IDM_UPDATES,
       IDM_ABOUT, IDM_EXIT };

#define ABOUT_BTN_UPDATES 200

static HWND g_host;
static HANDLE g_mutex;
static NOTIFYICONDATAW g_nid;
static UINT g_taskbar_created;
static bool g_hotkey_ok;
static bool g_busy;
static bool g_pending_full;
static bool g_in_settings;
static bool g_in_dialog;   /* About or an install action is showing */
static wchar_t g_balloon_path[MAX_PATH];
static int g_start_action; /* 0 none, 1 capture, 2 settings */

/* ------------------------------------------------------------------ */
/* Small UI helpers                                                      */
/* ------------------------------------------------------------------ */

int ui_scale_dpi(int px, int dpi) { return MulDiv(px, dpi, 96); }
int ui_scale(int px) { return ui_scale_dpi(px, (int)GetDpiForSystem()); }

HFONT ui_font(int pt, bool bold, int dpi)
{
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

HICON app_icon(bool large)
{
    static HICON small_icon, large_icon;
    HICON *slot = large ? &large_icon : &small_icon;
    if (!*slot) {
        int s = GetSystemMetrics(large ? SM_CXICON : SM_CXSMICON);
        *slot = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, s, s, LR_DEFAULTCOLOR);
        if (!*slot) *slot = LoadIconW(NULL, IDI_APPLICATION);
    }
    return *slot;
}

void app_error(HWND owner, const wchar_t *fmt, ...)
{
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    MessageBoxW(owner, buf, APP_NAME, MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ */
/* Tray icon and notifications                                           */
/* ------------------------------------------------------------------ */

static void tray_tooltip(void)
{
    Hotkey h = hotkey_parse(g_cfg.hotkey);
    wchar_t k[64];
    hotkey_format(&h, k, 64);
    swprintf(g_nid.szTip, 128, L"Dofram  \x2022  %s", g_hotkey_ok ? k : L"no hotkey");
}

static void tray_add(void)
{
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_host;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = app_icon(false);
    tray_tooltip();
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

static void tray_update_tip(void)
{
    tray_tooltip();
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void app_notify(const wchar_t *title, const wchar_t *text, const wchar_t *path, bool warning)
{
    if (!g_cfg.notifications && !warning) return;
    if (path) wcsncpy_s(g_balloon_path, MAX_PATH, path, _TRUNCATE);
    else g_balloon_path[0] = 0;
    g_nid.uFlags = NIF_INFO | NIF_SHOWTIP;
    wcsncpy_s(g_nid.szInfoTitle, 64, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, 256, (text && text[0]) ? text : L" ", _TRUNCATE);
    g_nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static bool apply_hotkey(bool quiet)
{
    UnregisterHotKey(g_host, HOTKEY_ID);
    Hotkey h = hotkey_parse(g_cfg.hotkey);
    g_hotkey_ok = hotkey_valid(&h) && RegisterHotKey(g_host, HOTKEY_ID, hotkey_modifiers(&h), h.vk);
    tray_update_tip();
    if (!g_hotkey_ok && !quiet) {
        wchar_t k[64], msg[256];
        hotkey_format(&h, k, 64);
        swprintf(msg, 256, L"%s may be in use by another program. Choose a different key in Settings.", k);
        app_notify(L"Could not register the hotkey", msg, NULL, true);
    }
    return g_hotkey_ok;
}

/* ------------------------------------------------------------------ */
/* Saving and copying                                                    */
/* ------------------------------------------------------------------ */

void app_open_folder(const wchar_t *folder, const wchar_t *select_file)
{
    wchar_t args[MAX_PATH + 32];
    if (select_file && select_file[0] && GetFileAttributesW(select_file) != INVALID_FILE_ATTRIBUTES) {
        swprintf(args, MAX_PATH + 32, L"/select,\"%s\"", select_file);
        ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    } else if (folder && folder[0]) {
        util_ensure_dir(folder);
        ShellExecuteW(NULL, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
    }
}

bool app_copy_image(Img *img, HWND owner)
{
    if (!img_copy_to_clipboard(img, owner)) {
        app_error(owner, L"Could not copy to the clipboard. Another program may be holding it open.");
        return false;
    }
    wchar_t t[64];
    swprintf(t, 64, L"%d \x00D7 %d px", img->w, img->h);
    app_notify(L"Copied to the clipboard", t, NULL, false);
    return true;
}

bool app_save_image(Img *img, bool also_copy, HWND owner)
{
    wchar_t path[MAX_PATH];
    if (!util_next_save_path(path, MAX_PATH)) {
        app_error(owner, L"The screenshot folder cannot be used:\n%s\n\nChoose another folder in Settings.", g_cfg.folder);
        return false;
    }
    if (!img_save(img, path, g_cfg.jpeg_quality)) {
        app_error(owner, L"Could not save the screenshot:\n%s", path);
        return false;
    }
    bool copied = also_copy && img_copy_to_clipboard(img, owner);
    app_notify(copied ? L"Saved and copied to the clipboard" : L"Saved", path, path, false);
    return true;
}

bool app_save_image_as(Img *img, HWND owner)
{
    wchar_t file[MAX_PATH], next[MAX_PATH];
    file[0] = 0;
    if (util_next_save_path(next, MAX_PATH)) {
        const wchar_t *base = wcsrchr(next, L'\\');
        wcsncpy_s(file, MAX_PATH, base ? base + 1 : next, _TRUNCATE);
        wchar_t *dot = wcsrchr(file, L'.');
        if (dot) *dot = 0;
    }
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0JPEG image (*.jpg)\0*.jpg\0BMP image (*.bmp)\0*.bmp\0";
    ofn.nFilterIndex = g_cfg.jpg ? 2 : 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_cfg.folder;
    ofn.lpstrDefExt = g_cfg.jpg ? L"jpg" : L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return false;
    const wchar_t *dot = wcsrchr(file, L'.');
    if (!dot || (_wcsicmp(dot, L".png") && _wcsicmp(dot, L".jpg") && _wcsicmp(dot, L".jpeg") && _wcsicmp(dot, L".bmp"))) {
        const wchar_t *ext = ofn.nFilterIndex == 2 ? L".jpg" : ofn.nFilterIndex == 3 ? L".bmp" : L".png";
        wcscat_s(file, MAX_PATH, ext);
    }
    if (!img_save(img, file, g_cfg.jpeg_quality)) {
        app_error(owner, L"Could not save the screenshot:\n%s", file);
        return false;
    }
    app_notify(L"Saved", file, file, false);
    return true;
}

/* ------------------------------------------------------------------ */
/* Capture flow                                                          */
/* ------------------------------------------------------------------ */

static void handle_result(Img *img)
{
    switch (g_cfg.after) {
    case AFTER_CLIPBOARD:
        app_copy_image(img, g_host);
        img_free(img);
        break;
    case AFTER_SAVE:
    case AFTER_BOTH:
        if (app_save_image(img, g_cfg.after == AFTER_BOTH || g_cfg.copy_on_save, g_host)) img_free(img);
        else editor_window_open(img); /* still lets the user save it elsewhere */
        break;
    default:
        editor_window_open(img);
        break;
    }
}

void app_overlay_done(Outcome outcome, Img *result)
{
    g_busy = false;
    if (!result) return;
    switch (outcome) {
    case OUTCOME_COPY:
        app_copy_image(result, g_host);
        img_free(result);
        break;
    case OUTCOME_SAVE:
        if (app_save_image(result, g_cfg.copy_on_save, g_host)) img_free(result);
        else editor_window_open(result);
        break;
    case OUTCOME_WINDOW:
        editor_window_open(result);
        break;
    case OUTCOME_QUICK:
        handle_result(result);
        break;
    default:
        img_free(result);
        break;
    }
}

static void do_capture(bool whole)
{
    Rc vs = rc_make(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    Img *full = img_capture_screen(vs);
    if (!full) {
        g_busy = false;
        app_error(NULL, L"Could not take the screenshot (not enough memory?).");
        return;
    }
    bool in_place = g_cfg.after == AFTER_EDITOR;
    if (whole && !in_place) {
        g_busy = false;
        handle_result(full);
        return;
    }
    Rc all = rc_make(0, 0, vs.w, vs.h);
    if (!overlay_open(full, vs, !in_place, whole ? &all : NULL)) {
        g_busy = false;
        app_error(NULL, L"Could not open the capture overlay.");
    }
}

static void capture(int delay_ms, bool whole)
{
    if (g_busy) return;
    g_busy = true;
    g_pending_full = whole;
    SetTimer(g_host, TIMER_CAPTURE, (UINT)(delay_ms > 0 ? delay_ms : 1), NULL);
}

/* ------------------------------------------------------------------ */
/* Menu, settings, exit                                                  */
/* ------------------------------------------------------------------ */

static void show_settings(void)
{
    if (g_in_settings) return;
    g_in_settings = true;
    UnregisterHotKey(g_host, HOTKEY_ID); /* do not fire while a new key is being recorded */
    settings_dialog(g_host);
    g_in_settings = false;
    apply_hotkey(false);
}

static HRESULT CALLBACK about_cb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data)
{
    (void)hwnd; (void)wp; (void)data;
    if (msg == TDN_HYPERLINK_CLICKED && lp && !wcscmp((const wchar_t *)lp, DOFRAM_URL_WSTR))
        ShellExecuteW(g_host, L"open", DOFRAM_URL_WSTR, NULL, NULL, SW_SHOWNORMAL);
    return S_OK;
}

static void show_about(void)
{
    TASKDIALOG_BUTTON buttons[1] = { { ABOUT_BTN_UPDATES, L"Check for Updates on GitHub" } };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_host;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"About Dofram";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = L"Dofram " APP_VERSION;
    tc.pszContent = L"A small screenshot tool for Windows with on-screen editing.\n\n"
                    DOFRAM_COPYRIGHT_WSTR L"\n\n"
                    L"Free software under the GNU General Public License, version 3 only (GPL-3.0-only). "
                    L"There is NO WARRANTY, to the extent permitted by law.\n\n"
                    L"<a href=\"" DOFRAM_URL_WSTR L"\">github.com/dogukansahil/Dofram</a>";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.cButtons = 1;
    tc.pButtons = buttons;
    tc.nDefaultButton = IDOK;
    tc.pfCallback = about_cb;
    int button = 0;
    SetForegroundWindow(g_host);
    if (SUCCEEDED(TaskDialogIndirect(&tc, &button, NULL, NULL)) && button == ABOUT_BTN_UPDATES)
        upd_check_async(g_host);
}

static BOOL CALLBACK close_editor(HWND h, LPARAM lp)
{
    (void)lp;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"DoframEditor") || !wcscmp(cls, L"DoframOverlay")) SendMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static BOOL CALLBACK find_editor(HWND h, LPARAM lp)
{
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"DoframEditor") || !wcscmp(cls, L"DoframOverlay")) { *(bool *)lp = true; return FALSE; }
    return TRUE;
}

/* Closes editor windows, so unsaved edits get their prompt. Returns true when all closed. */
static bool close_editors(void)
{
    DWORD tid = GetCurrentThreadId();
    EnumThreadWindows(tid, close_editor, 0);
    bool left = false;
    EnumThreadWindows(tid, find_editor, (LPARAM)&left);
    return !left;
}

/* Exits only when every editor window closed. */
static void exit_app(void)
{
    if (close_editors()) DestroyWindow(g_host);
}

/* Gives up the hotkey, the tray icon and the single-instance lock, so that
 * the installed copy can take over before this process exits. */
static void hand_over(void)
{
    if (g_host) {
        UnregisterHotKey(g_host, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
    }
    if (g_mutex) {
        CloseHandle(g_mutex);
        g_mutex = NULL;
    }
}

static void install_action(bool uninstall)
{
    if (!close_editors()) return; /* the user kept an editor open */
    bool quit = uninstall ? inst_interactive_uninstall(g_host) : inst_interactive_install(g_host, hand_over);
    if (quit) {
        hand_over();
        DestroyWindow(g_host);
    }
}

static void show_menu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    Hotkey h = hotkey_parse(g_cfg.hotkey);
    wchar_t item[96], k[64];
    hotkey_format(&h, k, 64);
    if (g_hotkey_ok) swprintf(item, 96, L"Capture area\t%s", k);
    else wcscpy_s(item, 96, L"Capture area");
    AppendMenuW(m, MF_STRING, IDM_REGION, item);
    AppendMenuW(m, MF_STRING, IDM_FULL, L"All screens");
    AppendMenuW(m, MF_STRING, IDM_DELAY, L"Capture area in 3 seconds");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_FOLDER, L"Open screenshot folder");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings\x2026");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    if (inst_running_installed()) AppendMenuW(m, MF_STRING, IDM_UNINSTALL, L"Uninstall Dofram\x2026");
    else AppendMenuW(m, MF_STRING, IDM_INSTALL, L"Install Dofram on This PC\x2026");
    AppendMenuW(m, MF_STRING, IDM_UPDATES, L"Check for Updates on GitHub");
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About Dofram");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(m, IDM_REGION, FALSE);
    SetForegroundWindow(g_host);
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY;
    flags |= GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = TrackPopupMenuEx(m, flags, x, y, g_host, NULL);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (g_in_dialog && (cmd == IDM_INSTALL || cmd == IDM_UNINSTALL || cmd == IDM_ABOUT)) return;
    bool modal = cmd == IDM_INSTALL || cmd == IDM_UNINSTALL || cmd == IDM_ABOUT;
    if (modal) g_in_dialog = true;
    switch (cmd) {
    case IDM_REGION: capture(250, false); break;
    case IDM_FULL: capture(250, true); break;
    case IDM_DELAY: capture(3000, false); break;
    case IDM_FOLDER: app_open_folder(g_cfg.folder, NULL); break;
    case IDM_SETTINGS: show_settings(); break;
    case IDM_INSTALL: install_action(false); break;
    case IDM_UNINSTALL: install_action(true); break;
    case IDM_UPDATES: upd_check_async(g_host); break;
    case IDM_ABOUT: show_about(); break;
    case IDM_EXIT: exit_app(); break;
    }
    if (modal) g_in_dialog = false;
}

static LRESULT CALLBACK host_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) {
        tray_add();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HOTKEY_ID) capture(0, false);
        return 0;
    case WM_APP_CAPTURE:
        capture(100, false);
        return 0;
    case WM_APP_SETTINGS:
        show_settings();
        return 0;
    case WM_APP_QUIT:
        exit_app();
        return 0;
    case WM_APP_UPDATE:
        upd_show_result(g_host, (UpdateResult *)lp);
        return 0;
    case WM_TIMER:
        KillTimer(h, wp);
        if (wp == TIMER_CAPTURE) do_capture(g_pending_full);
        else if (wp == TIMER_START) {
            if (g_start_action == 1) capture(0, false);
            else if (g_start_action == 2) show_settings();
        }
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            capture(150, false);
            break;
        case WM_CONTEXTMENU:
            show_menu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        case NIN_BALLOONUSERCLICK:
            if (g_balloon_path[0]) app_open_folder(g_cfg.folder, g_balloon_path);
            break;
        }
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(h, HOTKEY_ID);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                           */
/* ------------------------------------------------------------------ */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    AppArgs args;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    args_parse(argc, argv, &args);
    if (argv) LocalFree(argv);
    bool tray = args.tray, want_capture = args.capture, want_settings = args.settings;

    /* --install / --uninstall run on their own and never start the tray app. */
    if (args.install || args.uninstall) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        INITCOMMONCONTROLSEX icc0 = { sizeof(icc0), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc0);
        int code = 0;
        inst_handle_command_line(&args, &code);
        CoUninitialize();
        return code;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\Dofram_SingleInstance");
    g_mutex = mutex;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Already running: hand the request to the running copy and exit. */
        HWND other = NULL;
        for (int i = 0; i < 20 && !other; i++) {
            other = FindWindowW(HOST_CLASS, NULL);
            if (!other) Sleep(100);
        }
        if (other && !tray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, want_settings ? WM_APP_SETTINGS : WM_APP_CAPTURE, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR gdip_token = 0;
    if (GdiplusStartup(&gdip_token, &gsi, NULL) != GP_OK) {
        MessageBoxW(NULL, L"GDI+ could not be started.", APP_NAME, MB_OK | MB_ICONERROR);
        return 1;
    }

    settings_load();

    /* A plain start of a copy outside Program Files offers to install it. */
    if (inst_startup_offer(&args, hand_over)) {
        GdiplusShutdown(gdip_token);
        CoUninitialize();
        hand_over();
        return 0;
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = host_proc;
    wc.hInstance = inst;
    wc.lpszClassName = HOST_CLASS;
    wc.hIcon = app_icon(true);
    wc.hIconSm = app_icon(false);
    RegisterClassExW(&wc);
    editor_register(inst);

    /* A hidden top-level window: receives the hotkey, tray and second-instance messages. */
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_host) return 1;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_host, g_taskbar_created, MSGFLT_ALLOW, NULL);

    tray_add();
    bool hotkey_ok = apply_hotkey(false);
    util_startup_sync(g_cfg.start_with_windows);

    if (want_capture) { g_start_action = 1; SetTimer(g_host, TIMER_START, 300, NULL); }
    else if (want_settings) { g_start_action = 2; SetTimer(g_host, TIMER_START, 100, NULL); }
    else if (!tray && hotkey_ok) {
        Hotkey h = hotkey_parse(g_cfg.hotkey);
        wchar_t k[64], msg[200];
        hotkey_format(&h, k, 64);
        swprintf(msg, 200, L"Hotkey: %s   \x2022   Clicking the tray icon also captures.", k);
        app_notify(L"Dofram is running", msg, NULL, false);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    GdiplusShutdown(gdip_token);
    CoUninitialize();
    hand_over();
    return (int)m.wParam;
}

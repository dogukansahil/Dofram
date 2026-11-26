/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Settings dialog (layout in dofram.rc) and the key-combination capture box.
 */
#include "dofram.h"
#include "resource.h"

#include <commctrl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define IDC_HINT_FIRST 1100
#define IDC_HINT_LAST  1199

static Hotkey g_value;

static const wchar_t *const AFTER_NAMES[AFTER_COUNT] = {
    L"Edit on the screen", L"Edit in a separate window", L"Copy to the clipboard",
    L"Save to the folder", L"Save and copy to the clipboard"
};

static const wchar_t *const TOOL_NAMES[TOOL_COUNT] = {
    L"Select / move", L"Blur", L"Mosaic", L"Redaction box", L"Frame", L"Arrow", L"Pen", L"Highlighter", L"Text"
};

static bool win_down(void) { return GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0; }

static void hotkey_show(HWND box)
{
    wchar_t t[64];
    hotkey_format(&g_value, t, 64);
    SetWindowTextW(box, t);
}

static void hotkey_take(HWND box, UINT vk)
{
    g_value.ctrl = GetKeyState(VK_CONTROL) < 0;
    g_value.alt = GetKeyState(VK_MENU) < 0;
    g_value.shift = GetKeyState(VK_SHIFT) < 0;
    g_value.win = win_down();
    g_value.vk = vk;
    hotkey_show(box);
}

static bool is_modifier(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

/* Edit control that shows the pressed key combination instead of typing it. */
static LRESULT CALLBACK hotkey_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    switch (msg) {
    case WM_GETDLGCODE: {
        MSG *m = (MSG *)lp;
        if (m && m->message == WM_KEYDOWN && m->wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0)
            return DefSubclassProc(h, msg, wp, lp);
        return DLGC_WANTALLKEYS;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = (UINT)wp;
        if (is_modifier(vk)) {
            wchar_t t[64] = L"";
            if (GetKeyState(VK_CONTROL) < 0) wcscat_s(t, 64, L"Ctrl+");
            if (GetKeyState(VK_MENU) < 0) wcscat_s(t, 64, L"Alt+");
            if (GetKeyState(VK_SHIFT) < 0) wcscat_s(t, 64, L"Shift+");
            if (win_down()) wcscat_s(t, 64, L"Win+");
            wcscat_s(t, 64, L"\x2026");
            SetWindowTextW(h, t);
            return 0;
        }
        bool mods = GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0 || GetKeyState(VK_SHIFT) < 0 || win_down();
        if ((vk == VK_BACK || vk == VK_DELETE) && !mods) {
            memset(&g_value, 0, sizeof(g_value));
            hotkey_show(h);
            return 0;
        }
        hotkey_take(h, vk);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        if (wp == VK_SNAPSHOT) { hotkey_take(h, VK_SNAPSHOT); return 0; } /* PrintScreen only sends key-up */
        wchar_t t[64];
        GetWindowTextW(h, t, 64);
        size_t n = wcslen(t);
        if (!hotkey_valid(&g_value) || (n && t[n - 1] == L'\x2026')) hotkey_show(h);
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
    case WM_CONTEXTMENU:
    case WM_PASTE:
        return 0;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, hotkey_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void fill_combo(HWND dlg, int id, const wchar_t *const *items, int n, int sel)
{
    HWND c = GetDlgItem(dlg, id);
    for (int i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(c, CB_SETCURSEL, (WPARAM)sel, 0);
}

static int combo_sel(HWND dlg, int id)
{
    LRESULT r = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return r < 0 ? 0 : (int)r;
}

static void init_dialog(HWND dlg)
{
    SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)app_icon(true));
    SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)app_icon(false));
    g_value = hotkey_parse(g_cfg.hotkey);
    HWND hk = GetDlgItem(dlg, IDC_HOTKEY);
    SetWindowSubclass(hk, hotkey_proc, 1, 0);
    hotkey_show(hk);

    fill_combo(dlg, IDC_AFTER, AFTER_NAMES, AFTER_COUNT, g_cfg.after);
    static const wchar_t *const formats[2] = { L"PNG (lossless)", L"JPG (smaller files)" };
    fill_combo(dlg, IDC_FORMAT, formats, 2, g_cfg.jpg ? 1 : 0);
    fill_combo(dlg, IDC_TOOL, TOOL_NAMES, TOOL_COUNT, g_cfg.default_tool);

    SetDlgItemTextW(dlg, IDC_FOLDER, g_cfg.folder);
    SetDlgItemTextW(dlg, IDC_PATTERN, g_cfg.pattern);
    CheckDlgButton(dlg, IDC_STARTUP, g_cfg.start_with_windows ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_COPYONSAVE, g_cfg.copy_on_save ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CLOSESAVE, g_cfg.close_after_save ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_CLOSECOPY, g_cfg.close_after_copy ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_NOTIFY, g_cfg.notifications ? BST_CHECKED : BST_UNCHECKED);

    wchar_t path[MAX_PATH], t[MAX_PATH + 32];
    settings_path(path, MAX_PATH);
    swprintf(t, MAX_PATH + 32, L"Settings file: %s", path);
    SetDlgItemTextW(dlg, IDC_CFGPATH, t);

    RECT r, w;
    GetWindowRect(dlg, &r);
    HMONITOR m = MonitorFromWindow(dlg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    w = mi.rcWork;
    SetWindowPos(dlg, NULL, w.left + ((w.right - w.left) - (r.right - r.left)) / 2,
                 w.top + ((w.bottom - w.top) - (r.bottom - r.top)) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetForegroundWindow(dlg);
}

static bool apply(HWND dlg)
{
    if (!hotkey_valid(&g_value)) {
        app_error(dlg, L"Choose a hotkey: click the box and press a key combination.");
        return false;
    }
    wchar_t raw[MAX_PATH], dir[MAX_PATH], pattern[128];
    GetDlgItemTextW(dlg, IDC_FOLDER, raw, MAX_PATH);
    util_expand(raw, dir, MAX_PATH);
    if (!dir[0] || !util_ensure_dir(dir)) {
        app_error(dlg, L"This folder cannot be used:\n%s", dir);
        return false;
    }
    GetDlgItemTextW(dlg, IDC_PATTERN, pattern, 128);
    bool blank = true;
    for (wchar_t *c = pattern; *c; c++) if (*c != L' ' && *c != L'\t') blank = false;

    hotkey_format(&g_value, g_cfg.hotkey, 64);
    g_cfg.after = (AfterCapture)combo_sel(dlg, IDC_AFTER);
    wcscpy_s(g_cfg.folder, MAX_PATH, dir);
    wcscpy_s(g_cfg.pattern, 128, blank ? L"Dofram_{date}_{time}" : pattern);
    g_cfg.jpg = combo_sel(dlg, IDC_FORMAT) == 1;
    g_cfg.default_tool = (Tool)combo_sel(dlg, IDC_TOOL);
    g_cfg.start_with_windows = IsDlgButtonChecked(dlg, IDC_STARTUP) == BST_CHECKED;
    g_cfg.copy_on_save = IsDlgButtonChecked(dlg, IDC_COPYONSAVE) == BST_CHECKED;
    g_cfg.close_after_save = IsDlgButtonChecked(dlg, IDC_CLOSESAVE) == BST_CHECKED;
    g_cfg.close_after_copy = IsDlgButtonChecked(dlg, IDC_CLOSECOPY) == BST_CHECKED;
    g_cfg.notifications = IsDlgButtonChecked(dlg, IDC_NOTIFY) == BST_CHECKED;
    if (!settings_save()) app_error(dlg, L"The settings file could not be written.");
    if (!util_startup_set(g_cfg.start_with_windows)) app_error(dlg, L"The Windows start-up entry could not be changed.");
    return true;
}

static void make_shortcut(HWND dlg, bool desktop)
{
    wchar_t path[MAX_PATH];
    if (util_create_shortcut(desktop, path, MAX_PATH)) {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Shortcut created:\n%s", path);
        MessageBoxW(dlg, msg, APP_NAME, MB_OK | MB_ICONINFORMATION);
    } else {
        app_error(dlg, L"The shortcut could not be created.");
    }
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        init_dialog(dlg);
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        HWND c = (HWND)lp;
        int id = GetDlgCtrlID(c);
        HDC dc = (HDC)wp;
        if (id == IDC_HOTKEY) {
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            return (INT_PTR)GetSysColorBrush(COLOR_WINDOW);
        }
        if ((id >= IDC_HINT_FIRST && id <= IDC_HINT_LAST) || id == IDC_CFGPATH) {
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            if (apply(dlg)) EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case IDC_BROWSE: {
            wchar_t raw[MAX_PATH], dir[MAX_PATH];
            GetDlgItemTextW(dlg, IDC_FOLDER, raw, MAX_PATH);
            util_expand(raw, dir, MAX_PATH);
            if (util_pick_folder(dlg, dir, MAX_PATH)) SetDlgItemTextW(dlg, IDC_FOLDER, dir);
            return TRUE;
        }
        case IDC_OPEN: {
            wchar_t raw[MAX_PATH], dir[MAX_PATH];
            GetDlgItemTextW(dlg, IDC_FOLDER, raw, MAX_PATH);
            util_expand(raw, dir, MAX_PATH);
            app_open_folder(dir, NULL);
            return TRUE;
        }
        case IDC_DESKTOP: make_shortcut(dlg, true); return TRUE;
        case IDC_STARTMENU: make_shortcut(dlg, false); return TRUE;
        }
        break;
    }
    return FALSE;
}

bool settings_dialog(HWND owner)
{
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, dialog_proc, 0) == IDOK;
}

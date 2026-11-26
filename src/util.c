/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * File names, folders, Windows start-up entry, .lnk shortcuts and common dialogs.
 */
#define COBJMACROS
#include "dofram.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

wchar_t *util_strdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

void util_exe_path(wchar_t *out, int cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cap);
    if (n == 0 || n >= (DWORD)cap) out[0] = 0;
}

void util_expand(const wchar_t *in, wchar_t *out, int cap)
{
    DWORD n = ExpandEnvironmentStringsW(in, out, (DWORD)cap);
    if (n == 0 || n > (DWORD)cap) wcsncpy_s(out, cap, in, _TRUNCATE);
}

bool util_ensure_dir(const wchar_t *dir)
{
    DWORD a = GetFileAttributesW(dir);
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int r = SHCreateDirectoryExW(NULL, dir, NULL);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

static void replace_all(wchar_t *s, int cap, const wchar_t *what, const wchar_t *with)
{
    wchar_t tmp[512];
    size_t wl = wcslen(what);
    wchar_t *p;
    while ((p = wcsstr(s, what)) != NULL) {
        *p = 0;
        swprintf(tmp, 512, L"%s%s%s", s, with, p + wl);
        wcsncpy_s(s, cap, tmp, _TRUNCATE);
    }
}

/* Builds the next free file name in the save folder from the name pattern. */
bool util_next_save_path(wchar_t *out, int cap)
{
    if (!util_ensure_dir(g_cfg.folder)) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[256], v[32];
    wcsncpy_s(name, 256, g_cfg.pattern, _TRUNCATE);
    swprintf(v, 32, L"%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    replace_all(name, 256, L"{date}", v);
    swprintf(v, 32, L"%02d-%02d-%02d", t.wHour, t.wMinute, t.wSecond);
    replace_all(name, 256, L"{time}", v);
    swprintf(v, 32, L"%03d", t.wMilliseconds);
    replace_all(name, 256, L"{ms}", v);
    bool blank = true;
    for (wchar_t *c = name; *c; c++) {
        if (*c < 32 || wcschr(L"<>:\"/\\|?*", *c)) *c = L'_';
        if (!iswspace(*c)) blank = false;
    }
    if (blank) wcscpy_s(name, 256, L"Dofram");
    const wchar_t *ext = g_cfg.jpg ? L".jpg" : L".png";
    swprintf(out, cap, L"%s\\%s%s", g_cfg.folder, name, ext);
    for (int i = 2; GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES && i < 100000; i++)
        swprintf(out, cap, L"%s\\%s_%d%s", g_cfg.folder, name, i, ext);
    return true;
}

/* ------------------------------------------------------------------ */
/* Start with Windows                                                    */
/* ------------------------------------------------------------------ */

/* The start-up entry starts the installed copy when there is one, unless this
 * copy is portable (it then keeps its own settings and starts itself). */
static void startup_command(wchar_t *out, int cap)
{
    wchar_t exe[MAX_PATH], installed[MAX_PATH];
    util_exe_path(exe, MAX_PATH);
    if (!settings_is_portable() && !inst_running_installed()) {
        inst_installed_exe(installed, MAX_PATH);
        DWORD a = GetFileAttributesW(installed);
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) wcscpy_s(exe, MAX_PATH, installed);
    }
    swprintf(out, cap, L"\"%s\" --tray", exe);
}

bool util_startup_set(bool enabled)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (enabled) {
        wchar_t cmd[MAX_PATH + 32];
        startup_command(cmd, MAX_PATH + 32);
        r = RegSetValueExW(k, APP_NAME, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, APP_NAME);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* Fixes the registered path if the executable was moved. */
void util_startup_sync(bool enabled)
{
    wchar_t cur[MAX_PATH + 32], want[MAX_PATH + 32];
    DWORD size = sizeof(cur);
    bool present = RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, cur, &size) == ERROR_SUCCESS;
    startup_command(want, MAX_PATH + 32);
    if (enabled && (!present || wcscmp(cur, want) != 0)) util_startup_set(true);
    else if (!enabled && present) util_startup_set(false);
}

/* ------------------------------------------------------------------ */
/* Shortcuts                                                             */
/* ------------------------------------------------------------------ */

bool util_create_shortcut(bool desktop, wchar_t *out_path, int cap)
{
    PWSTR folder = NULL;
    if (FAILED(SHGetKnownFolderPath(desktop ? &FOLDERID_Desktop : &FOLDERID_Programs, 0, NULL, &folder))) return false;
    swprintf(out_path, cap, L"%s\\Dofram screenshot.lnk", folder);
    CoTaskMemFree(folder);

    wchar_t exe[MAX_PATH], dir[MAX_PATH];
    util_exe_path(exe, MAX_PATH);
    wcscpy_s(dir, MAX_PATH, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;

    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
        IShellLinkW_SetPath(link, exe);
        IShellLinkW_SetArguments(link, L"--capture");
        IShellLinkW_SetWorkingDirectory(link, dir);
        IShellLinkW_SetIconLocation(link, exe, 0);
        IShellLinkW_SetDescription(link, L"Take a screenshot of a selected area");
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            ok = SUCCEEDED(IPersistFile_Save(file, out_path, TRUE));
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                               */
/* ------------------------------------------------------------------ */

bool util_pick_folder(HWND owner, wchar_t *path, int cap)
{
    IFileOpenDialog *dlg = NULL;
    bool ok = false;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return false;
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Folder for saved screenshots");
    IShellItem *start = NULL;
    if (path[0] && SUCCEEDED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&start))) {
        IFileOpenDialog_SetFolder(dlg, start);
        IShellItem_Release(start);
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR name = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &name))) {
                wcsncpy_s(path, cap, name, _TRUNCATE);
                CoTaskMemFree(name);
                ok = true;
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

static COLORREF g_custom_colors[16];

bool util_pick_color(HWND owner, uint32_t *rgb)
{
    CHOOSECOLORW cc;
    memset(&cc, 0, sizeof(cc));
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = owner;
    cc.rgbResult = RGB((*rgb >> 16) & 0xFF, (*rgb >> 8) & 0xFF, *rgb & 0xFF);
    cc.lpCustColors = g_custom_colors;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    *rgb = ((uint32_t)GetRValue(cc.rgbResult) << 16) | ((uint32_t)GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
    return true;
}

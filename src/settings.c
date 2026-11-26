/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Settings file (UTF-8 key=value) and hotkey parsing.
 * A "dofram.ini" next to the executable switches to portable mode;
 * otherwise the file lives in %APPDATA%\Dofram\dofram.ini.
 */
#include "dofram.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

Settings g_cfg;

static const wchar_t *const AFTER_KEYS[AFTER_COUNT] = { L"editor", L"window", L"clipboard", L"save", L"both" };

const wchar_t *after_key(AfterCapture a) { return (a >= 0 && a < AFTER_COUNT) ? AFTER_KEYS[a] : L"editor"; }

/* ------------------------------------------------------------------ */
/* Hotkeys                                                               */
/* ------------------------------------------------------------------ */

typedef struct { UINT vk; const wchar_t *name; } KeyName;

static const KeyName KEY_NAMES[] = {
    { VK_SNAPSHOT, L"PrintScreen" }, { VK_PAUSE, L"Pause" }, { VK_SCROLL, L"ScrollLock" },
    { VK_INSERT, L"Insert" }, { VK_DELETE, L"Delete" }, { VK_HOME, L"Home" }, { VK_END, L"End" },
    { VK_PRIOR, L"PageUp" }, { VK_NEXT, L"PageDown" }, { VK_LEFT, L"Left" }, { VK_RIGHT, L"Right" },
    { VK_UP, L"Up" }, { VK_DOWN, L"Down" }, { VK_RETURN, L"Enter" }, { VK_SPACE, L"Space" },
    { VK_TAB, L"Tab" }, { VK_BACK, L"Backspace" }, { VK_ESCAPE, L"Escape" },
    { VK_MULTIPLY, L"NumMultiply" }, { VK_ADD, L"NumPlus" }, { VK_SUBTRACT, L"NumMinus" },
    { VK_DECIMAL, L"NumDecimal" }, { VK_DIVIDE, L"NumDivide" },
    { VK_OEM_1, L"Semicolon" }, { VK_OEM_PLUS, L"Plus" }, { VK_OEM_COMMA, L"Comma" },
    { VK_OEM_MINUS, L"Minus" }, { VK_OEM_PERIOD, L"Period" }, { VK_OEM_2, L"Slash" },
    { VK_OEM_3, L"Backtick" }, { VK_OEM_4, L"OpenBracket" }, { VK_OEM_5, L"Backslash" },
    { VK_OEM_6, L"CloseBracket" }, { VK_OEM_7, L"Quote" }, { VK_OEM_102, L"OemBackslash" },
};

static void key_name(UINT vk, wchar_t *out, int cap)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { swprintf(out, cap, L"%c", (wchar_t)vk); return; }
    if (vk >= VK_F1 && vk <= VK_F24) { swprintf(out, cap, L"F%u", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { swprintf(out, cap, L"Num%u", vk - VK_NUMPAD0); return; }
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++)
        if (KEY_NAMES[i].vk == vk) { wcsncpy_s(out, cap, KEY_NAMES[i].name, _TRUNCATE); return; }
    swprintf(out, cap, L"0x%02X", vk);
}

static UINT key_from_name(const wchar_t *s)
{
    if (!s[0]) return 0;
    if (!s[1]) {
        wchar_t c = towupper(s[0]);
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    }
    if ((s[0] == 'F' || s[0] == 'f') && iswdigit(s[1])) {
        int n = _wtoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (_wcsnicmp(s, L"Num", 3) == 0 && iswdigit(s[3]) && !s[4]) return VK_NUMPAD0 + (s[3] - '0');
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (UINT)wcstoul(s + 2, NULL, 16) & 0xFF;
    if (_wcsicmp(s, L"PrtSc") == 0) return VK_SNAPSHOT;
    for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++)
        if (_wcsicmp(KEY_NAMES[i].name, s) == 0) return KEY_NAMES[i].vk;
    return 0;
}

bool hotkey_valid(const Hotkey *h) { return h->vk != 0; }

UINT hotkey_modifiers(const Hotkey *h)
{
    UINT m = MOD_NOREPEAT;
    if (h->alt) m |= MOD_ALT;
    if (h->ctrl) m |= MOD_CONTROL;
    if (h->shift) m |= MOD_SHIFT;
    if (h->win) m |= MOD_WIN;
    return m;
}

void hotkey_format(const Hotkey *h, wchar_t *out, int cap)
{
    out[0] = 0;
    if (!hotkey_valid(h)) { wcsncpy_s(out, cap, L"(none)", _TRUNCATE); return; }
    if (h->ctrl) wcscat_s(out, cap, L"Ctrl+");
    if (h->alt) wcscat_s(out, cap, L"Alt+");
    if (h->shift) wcscat_s(out, cap, L"Shift+");
    if (h->win) wcscat_s(out, cap, L"Win+");
    wchar_t k[32];
    key_name(h->vk, k, 32);
    wcscat_s(out, cap, k);
}

Hotkey hotkey_parse(const wchar_t *text)
{
    Hotkey h;
    memset(&h, 0, sizeof(h));
    wchar_t buf[64];
    wcsncpy_s(buf, 64, text ? text : L"", _TRUNCATE);
    wchar_t *ctx = NULL;
    for (wchar_t *tok = wcstok_s(buf, L"+", &ctx); tok; tok = wcstok_s(NULL, L"+", &ctx)) {
        while (iswspace(*tok)) tok++;
        size_t n = wcslen(tok);
        while (n > 0 && iswspace(tok[n - 1])) tok[--n] = 0;
        if (!n) continue;
        if (_wcsicmp(tok, L"Ctrl") == 0 || _wcsicmp(tok, L"Control") == 0) h.ctrl = true;
        else if (_wcsicmp(tok, L"Alt") == 0) h.alt = true;
        else if (_wcsicmp(tok, L"Shift") == 0) h.shift = true;
        else if (_wcsicmp(tok, L"Win") == 0) h.win = true;
        else h.vk = key_from_name(tok);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* File                                                                  */
/* ------------------------------------------------------------------ */

void settings_path(wchar_t *out, int cap)
{
    wchar_t exe[MAX_PATH];
    util_exe_path(exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (slash) {
        slash[1] = 0;
        swprintf(out, cap, L"%sdofram.ini", exe);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return;
    }
    PWSTR appdata = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata))) {
        swprintf(out, cap, L"%s\\Dofram\\dofram.ini", appdata);
        CoTaskMemFree(appdata);
    } else {
        swprintf(out, cap, L"dofram.ini");
    }
}

bool settings_is_portable(void)
{
    wchar_t exe[MAX_PATH], ini[MAX_PATH];
    util_exe_path(exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    swprintf(ini, MAX_PATH, L"%sdofram.ini", exe);
    return GetFileAttributesW(ini) != INVALID_FILE_ATTRIBUTES;
}

static void defaults(Settings *s)
{
    memset(s, 0, sizeof(*s));
    wcscpy_s(s->hotkey, 64, L"Ctrl+Shift+X");
    s->after = AFTER_EDITOR;
    PWSTR pics = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, 0, NULL, &pics))) {
        swprintf(s->folder, MAX_PATH, L"%s\\Dofram", pics);
        CoTaskMemFree(pics);
    }
    wcscpy_s(s->pattern, 128, L"Dofram_{date}_{time}");
    s->jpg = false;
    s->jpeg_quality = 92;
    s->start_with_windows = false;
    s->copy_on_save = true;
    s->close_after_save = true;
    s->close_after_copy = true;
    s->notifications = true;
    s->default_tool = TOOL_BLUR;
    s->tool_size = 4;
    s->ask_to_install = true;
}

static bool to_bool(const wchar_t *v)
{
    return _wcsicmp(v, L"1") == 0 || _wcsicmp(v, L"true") == 0 || _wcsicmp(v, L"yes") == 0 || _wcsicmp(v, L"on") == 0;
}

static wchar_t *trim(wchar_t *s)
{
    while (iswspace(*s)) s++;
    size_t n = wcslen(s);
    while (n > 0 && iswspace(s[n - 1])) s[--n] = 0;
    return s;
}

static void apply_line(Settings *s, wchar_t *line)
{
    line = trim(line);
    if (!line[0] || line[0] == '#' || line[0] == ';') return;
    wchar_t *eq = wcschr(line, L'=');
    if (!eq || eq == line) return;
    *eq = 0;
    wchar_t *k = trim(line), *v = trim(eq + 1);
    if (!wcscmp(k, L"hotkey")) wcsncpy_s(s->hotkey, 64, v, _TRUNCATE);
    else if (!wcscmp(k, L"after_capture")) {
        for (int i = 0; i < AFTER_COUNT; i++)
            if (!_wcsicmp(v, AFTER_KEYS[i])) s->after = (AfterCapture)i;
    } else if (!wcscmp(k, L"save_folder")) { if (v[0]) util_expand(v, s->folder, MAX_PATH); }
    else if (!wcscmp(k, L"file_name")) { if (v[0]) wcsncpy_s(s->pattern, 128, v, _TRUNCATE); }
    else if (!wcscmp(k, L"format")) s->jpg = _wcsicmp(v, L"jpg") == 0 || _wcsicmp(v, L"jpeg") == 0;
    else if (!wcscmp(k, L"jpeg_quality")) s->jpeg_quality = iclamp(_wtoi(v) ? _wtoi(v) : 92, 30, 100);
    else if (!wcscmp(k, L"start_with_windows")) s->start_with_windows = to_bool(v);
    else if (!wcscmp(k, L"copy_on_save")) s->copy_on_save = to_bool(v);
    else if (!wcscmp(k, L"close_after_save")) s->close_after_save = to_bool(v);
    else if (!wcscmp(k, L"close_after_copy")) s->close_after_copy = to_bool(v);
    else if (!wcscmp(k, L"notifications")) s->notifications = to_bool(v);
    else if (!wcscmp(k, L"default_tool")) s->default_tool = tool_from_key(v, TOOL_BLUR);
    else if (!wcscmp(k, L"tool_size")) s->tool_size = iclamp(_wtoi(v) ? _wtoi(v) : 4, 1, 10);
    else if (!wcscmp(k, L"ask_to_install")) s->ask_to_install = to_bool(v);
}

void settings_load(void)
{
    defaults(&g_cfg);
    wchar_t path[MAX_PATH];
    settings_path(path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > 1024 * 1024) { CloseHandle(f); return; }
    DWORD n = (DWORD)size.QuadPart, got = 0;
    char *raw = (char *)malloc((size_t)n + 1);
    if (!raw) { CloseHandle(f); return; }
    if (!ReadFile(f, raw, n, &got, NULL)) got = 0;
    CloseHandle(f);
    raw[got] = 0;
    const char *start = raw;
    if (got >= 3 && (BYTE)raw[0] == 0xEF && (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF) start += 3;
    int wn = MultiByteToWideChar(CP_UTF8, 0, start, -1, NULL, 0);
    wchar_t *text = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(wn > 0 ? wn : 1));
    if (text && wn > 0) {
        MultiByteToWideChar(CP_UTF8, 0, start, -1, text, wn);
        wchar_t *ctx = NULL;
        for (wchar_t *line = wcstok_s(text, L"\r\n", &ctx); line; line = wcstok_s(NULL, L"\r\n", &ctx))
            apply_line(&g_cfg, line);
    }
    free(text);
    free(raw);
}

bool settings_save(void)
{
    wchar_t path[MAX_PATH];
    settings_path(path, MAX_PATH);
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, path);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; util_ensure_dir(dir); }

    wchar_t *buf = (wchar_t *)malloc(sizeof(wchar_t) * 8192);
    if (!buf) return false;
    swprintf(buf, 8192,
             L"# Dofram settings. You can edit this file while Dofram is not running.\r\n"
             L"hotkey=%s\r\n"
             L"# editor (edit on screen) | window (separate window) | clipboard | save | both\r\n"
             L"after_capture=%s\r\n"
             L"save_folder=%s\r\n"
             L"# {date} {time} {ms} are replaced when saving\r\n"
             L"file_name=%s\r\n"
             L"# png | jpg\r\n"
             L"format=%s\r\n"
             L"jpeg_quality=%d\r\n"
             L"start_with_windows=%d\r\n"
             L"copy_on_save=%d\r\n"
             L"close_after_save=%d\r\n"
             L"close_after_copy=%d\r\n"
             L"notifications=%d\r\n"
             L"# select | blur | pixelate | box | frame | arrow | pen | highlight | text\r\n"
             L"default_tool=%s\r\n"
             L"tool_size=%d\r\n"
             L"# 0 = never offer to install Dofram when it runs from outside Program Files\r\n"
             L"ask_to_install=%d\r\n",
             g_cfg.hotkey, after_key(g_cfg.after), g_cfg.folder, g_cfg.pattern, g_cfg.jpg ? L"jpg" : L"png",
             g_cfg.jpeg_quality, g_cfg.start_with_windows, g_cfg.copy_on_save, g_cfg.close_after_save,
             g_cfg.close_after_copy, g_cfg.notifications, tool_key(g_cfg.default_tool), g_cfg.tool_size,
             g_cfg.ask_to_install);

    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
    char *utf8 = (char *)malloc((size_t)(n > 0 ? n : 1));
    bool ok = false;
    if (utf8 && n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, n, NULL, NULL);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(f, utf8, (DWORD)(n - 1), &wrote, NULL) && wrote == (DWORD)(n - 1);
            CloseHandle(f);
        }
    }
    free(utf8);
    free(buf);
    return ok;
}

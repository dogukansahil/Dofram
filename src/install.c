/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Command line parsing and the self-installer. Dofram.exe installs itself:
 * it copies itself to %ProgramFiles%\Dofram, adds a Start menu shortcut and
 * registers in "Installed apps". A copy with a dofram.ini next to it stays
 * portable and never offers to install. No external installer technology.
 *
 * Machine-wide changes (Program Files, HKLM) are made by an elevated copy of
 * this executable started with "--install" or "--uninstall". Per-user changes
 * (the start-up entry, settings) are made by the non-elevated process, so
 * they land in the right user's profile.
 */
#define COBJMACROS
#include "dofram.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Dofram"
#define APPPATH_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Dofram.exe"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define SETUP_TITLE   L"Dofram Setup"
#define BTN_YES       100
#define BTN_NO        101
#define EXIT_DECLINED 10      /* the user said no */
#define EXIT_CANCELLED 1223   /* ERROR_CANCELLED: the UAC prompt was declined */

/* ------------------------------------------------------------------ */
/* Command line                                                          */
/* ------------------------------------------------------------------ */

static bool arg_is(const wchar_t *a, const wchar_t *flag)
{
    return CompareStringOrdinal(a, -1, flag, -1, TRUE) == CSTR_EQUAL;
}

void args_parse(int argc, wchar_t **argv, AppArgs *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 1; argv && i < argc; i++) {
        const wchar_t *a = argv[i];
        if (!a) continue;
        if (arg_is(a, L"--tray")) out->tray = true;
        else if (arg_is(a, L"--capture")) out->capture = true;
        else if (arg_is(a, L"--settings")) out->settings = true;
        else if (arg_is(a, L"--install")) out->install = true;
        else if (arg_is(a, L"--uninstall")) out->uninstall = true;
        else if (arg_is(a, L"--quiet")) out->quiet = true;
        else if (arg_is(a, L"--portable")) out->portable = true;
        else if (arg_is(a, L"--elevated")) out->elevated_child = true;
    }
}

/* ------------------------------------------------------------------ */
/* Pure decisions (unit tested)                                          */
/* ------------------------------------------------------------------ */

InstallOffer inst_offer_kind(const AppArgs *args, const InstallState *st)
{
    if (args->install || args->uninstall) return OFFER_NONE;
    /* Started by the start-up entry, a capture shortcut or with options: do not interrupt. */
    if (args->portable || args->tray || args->capture || args->settings) return OFFER_NONE;
    if (st->portable_ini || st->running_installed || !st->ask_enabled) return OFFER_NONE;
    if (st->installed) {
        if (st->installed_version && wcscmp(st->installed_version, DOFRAM_VERSION_WSTR) == 0) return OFFER_NONE;
        return OFFER_UPDATE;
    }
    return OFFER_INSTALL;
}

bool inst_path_in_dir(const wchar_t *path, const wchar_t *dir)
{
    if (!path || !dir || !path[0] || !dir[0]) return false;
    const wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash || !slash[1]) return false;
    int n = (int)wcslen(dir);
    while (n > 0 && dir[n - 1] == L'\\') n--;
    if (n == 0 || (int)(slash - path) != n) return false;
    return CompareStringOrdinal(path, n, dir, n, TRUE) == CSTR_EQUAL;
}

bool inst_command_targets(const wchar_t *command, const wchar_t *exe)
{
    if (!command || !exe || !exe[0]) return false;
    while (*command == L' ' || *command == L'\t') command++;
    const wchar_t *start = command, *end;
    if (*command == L'"') {
        start = command + 1;
        end = wcschr(start, L'"');
        if (!end) return false;
    } else {
        end = start;
        while (*end && *end != L' ' && *end != L'\t') end++;
    }
    int n = (int)(end - start);
    return n > 0 && CompareStringOrdinal(start, n, exe, -1, TRUE) == CSTR_EQUAL;
}

/* ------------------------------------------------------------------ */
/* Paths and state                                                       */
/* ------------------------------------------------------------------ */

void inst_install_dir(wchar_t *out, int cap)
{
    PWSTR pf = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &pf)) && pf) {
        swprintf(out, cap, L"%s\\Dofram", pf);
        CoTaskMemFree(pf);
    } else {
        swprintf(out, cap, L"C:\\Program Files\\Dofram");
    }
}

void inst_installed_exe(wchar_t *out, int cap)
{
    wchar_t dir[MAX_PATH];
    inst_install_dir(dir, MAX_PATH);
    swprintf(out, cap, L"%s\\Dofram.exe", dir);
}

static bool file_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t fa[MAX_PATH], fb[MAX_PATH];
    DWORD na = GetFullPathNameW(a, MAX_PATH, fa, NULL), nb = GetFullPathNameW(b, MAX_PATH, fb, NULL);
    if (!na || na >= MAX_PATH || !nb || nb >= MAX_PATH) return false;
    return CompareStringOrdinal(fa, -1, fb, -1, TRUE) == CSTR_EQUAL;
}

bool inst_running_installed(void)
{
    wchar_t self[MAX_PATH], target[MAX_PATH];
    util_exe_path(self, MAX_PATH);
    inst_installed_exe(target, MAX_PATH);
    return self[0] && same_path(self, target);
}

static bool uninstall_key_exists(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

bool inst_installed_version(wchar_t *version, int cap)
{
    if (cap > 0) version[0] = 0;
    DWORD bytes = (DWORD)(cap * (int)sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"DisplayVersion", RRF_RT_REG_SZ, NULL, version, &bytes) !=
        ERROR_SUCCESS && cap > 0)
        version[0] = 0;
    wchar_t exe[MAX_PATH];
    inst_installed_exe(exe, MAX_PATH);
    return file_exists(exe);
}

bool inst_is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION e;
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size)) elevated = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

/* Runs this executable elevated with args and waits, keeping the caller's
 * windows responsive. Returns false when the UAC prompt was declined. */
static bool run_elevated(HWND owner, const wchar_t *args, DWORD *exit_code)
{
    wchar_t self[MAX_PATH];
    util_exe_path(self, MAX_PATH);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r != WAIT_OBJECT_0 + 1) break;
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exit_code) *exit_code = code;
    return true;
}

/* ------------------------------------------------------------------ */
/* Registry and shortcut helpers                                         */
/* ------------------------------------------------------------------ */

static void reg_sz(const wchar_t *key, const wchar_t *name, const wchar_t *value)
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_SZ, value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

static void reg_dword(const wchar_t *key, const wchar_t *name, DWORD value)
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, key, name, REG_DWORD, &value, sizeof(value));
}

static bool start_menu_link(wchar_t *out, int cap)
{
    PWSTR dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &dir)) || !dir) return false;
    swprintf(out, cap, L"%s\\Dofram.lnk", dir);
    CoTaskMemFree(dir);
    return true;
}

static bool create_link(const wchar_t *lnk, const wchar_t *target, const wchar_t *workdir)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    bool ok = false;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
        IShellLinkW_SetPath(link, target);
        IShellLinkW_SetWorkingDirectory(link, workdir);
        IShellLinkW_SetIconLocation(link, target, 0);
        IShellLinkW_SetDescription(link, L"Screenshot tool with on-screen editing");
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            ok = SUCCEEDED(IPersistFile_Save(file, lnk, TRUE));
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

/* True when the .lnk at path points at exe. */
static bool link_targets(const wchar_t *path, const wchar_t *exe)
{
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    bool hit = false;
    if (!file_exists(path)) return false;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            wchar_t target[MAX_PATH];
            if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ)) &&
                SUCCEEDED(IShellLinkW_GetPath(link, target, MAX_PATH, NULL, SLGP_RAWPATH)))
                hit = same_path(target, exe);
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return hit;
}

static void fail(bool quiet, const wchar_t *what)
{
    DWORD err = GetLastError();
    if (quiet) return;
    wchar_t msg[512];
    swprintf(msg, 512, L"%s\n\nWindows error code: %lu", what, err);
    MessageBoxW(NULL, msg, SETUP_TITLE, MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

/* ------------------------------------------------------------------ */
/* Install (elevated)                                                    */
/* ------------------------------------------------------------------ */

static int install_machine(bool quiet)
{
    wchar_t dir[MAX_PATH], self[MAX_PATH], target[MAX_PATH], old[MAX_PATH];
    inst_install_dir(dir, MAX_PATH);
    inst_installed_exe(target, MAX_PATH);
    util_exe_path(self, MAX_PATH);
    if (!self[0]) return 1;

    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fail(quiet, L"Could not create the installation folder.");
        return 2;
    }
    if (!same_path(self, target)) {
        swprintf(old, MAX_PATH, L"%s\\Dofram.old.exe", dir);
        DeleteFileW(old);
        /* A running executable cannot be overwritten, but it can be renamed. */
        if (file_exists(target) && !MoveFileExW(target, old, MOVEFILE_REPLACE_EXISTING)) {
            fail(quiet, L"Could not replace the installed Dofram.exe. Exit Dofram from its tray icon and try again.");
            return 3;
        }
        if (!CopyFileW(self, target, FALSE)) {
            fail(quiet, L"Could not copy Dofram.exe to Program Files.");
            if (file_exists(old)) MoveFileExW(old, target, MOVEFILE_REPLACE_EXISTING);
            return 4;
        }
        if (file_exists(old) && !DeleteFileW(old)) MoveFileExW(old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    wchar_t lnk[MAX_PATH];
    if (start_menu_link(lnk, MAX_PATH)) create_link(lnk, target, dir);

    /* "Installed apps" entry */
    wchar_t value[MAX_PATH + 64];
    reg_sz(UNINSTALL_KEY, L"DisplayName", L"Dofram");
    reg_sz(UNINSTALL_KEY, L"DisplayVersion", DOFRAM_VERSION_WSTR);
    reg_sz(UNINSTALL_KEY, L"Publisher", DOFRAM_PUBLISHER_WSTR);
    reg_sz(UNINSTALL_KEY, L"DisplayIcon", target);
    reg_sz(UNINSTALL_KEY, L"InstallLocation", dir);
    reg_sz(UNINSTALL_KEY, L"URLInfoAbout", DOFRAM_URL_WSTR);
    reg_sz(UNINSTALL_KEY, L"HelpLink", DOFRAM_URL_WSTR);
    swprintf(value, MAX_PATH + 64, L"\"%s\" --uninstall", target);
    reg_sz(UNINSTALL_KEY, L"UninstallString", value);
    swprintf(value, MAX_PATH + 64, L"\"%s\" --uninstall --quiet", target);
    reg_sz(UNINSTALL_KEY, L"QuietUninstallString", value);
    reg_dword(UNINSTALL_KEY, L"NoModify", 1);
    reg_dword(UNINSTALL_KEY, L"NoRepair", 1);
    reg_dword(UNINSTALL_KEY, L"VersionMajor", DOFRAM_VERSION_MAJOR);
    reg_dword(UNINSTALL_KEY, L"VersionMinor", DOFRAM_VERSION_MINOR);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(target, GetFileExInfoStandard, &fa))
        reg_dword(UNINSTALL_KEY, L"EstimatedSize", (fa.nFileSizeLow + 1023) / 1024); /* KB */
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t date[16];
    swprintf(date, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
    reg_sz(UNINSTALL_KEY, L"InstallDate", date);

    /* "Dofram" in the Run dialog */
    reg_sz(APPPATH_KEY, NULL, target);
    reg_sz(APPPATH_KEY, L"Path", dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Uninstall                                                             */
/* ------------------------------------------------------------------ */

typedef struct { DWORD self; const wchar_t *exe; HANDLE waits[8]; int nwaits; } CloseCtx;

static BOOL CALLBACK close_installed_proc(HWND h, LPARAM lp)
{
    CloseCtx *c = (CloseCtx *)lp;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, HOST_CLASS) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == c->self || c->nwaits >= 8) return TRUE;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!p) return TRUE;
    wchar_t image[MAX_PATH];
    DWORD n = MAX_PATH;
    /* Only the copy in Program Files is asked to exit; portable copies keep running. */
    if (QueryFullProcessImageNameW(p, 0, image, &n) && same_path(image, c->exe)) {
        PostMessageW(h, WM_APP_QUIT, 0, 0);
        c->waits[c->nwaits++] = p;
    } else {
        CloseHandle(p);
    }
    return TRUE;
}

/* Politely asks a running installed copy to exit (it may still ask about
 * unsaved edits) and waits a few seconds. Nothing is ever terminated. */
static void close_running_installed(void)
{
    wchar_t exe[MAX_PATH];
    inst_installed_exe(exe, MAX_PATH);
    CloseCtx c;
    memset(&c, 0, sizeof(c));
    c.self = GetCurrentProcessId();
    c.exe = exe;
    EnumWindows(close_installed_proc, (LPARAM)&c);
    if (c.nwaits) WaitForMultipleObjects((DWORD)c.nwaits, c.waits, TRUE, 8000);
    for (int i = 0; i < c.nwaits; i++) CloseHandle(c.waits[i]);
}

/* Current user's side: the start-up entry and shortcuts that point at the
 * installed copy, and the settings file when asked. Screenshots are never touched. */
static void uninstall_user(bool remove_settings)
{
    wchar_t exe[MAX_PATH], cur[MAX_PATH + 32], path[MAX_PATH];
    inst_installed_exe(exe, MAX_PATH);

    DWORD size = sizeof(cur);
    if (RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME, RRF_RT_REG_SZ, NULL, cur, &size) == ERROR_SUCCESS &&
        inst_command_targets(cur, exe))
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUN_KEY, APP_NAME);

    const KNOWNFOLDERID *folders[2] = { &FOLDERID_Desktop, &FOLDERID_Programs };
    for (int i = 0; i < 2; i++) {
        PWSTR dir = NULL;
        if (FAILED(SHGetKnownFolderPath(folders[i], 0, NULL, &dir)) || !dir) continue;
        swprintf(path, MAX_PATH, L"%s\\Dofram screenshot.lnk", dir);
        CoTaskMemFree(dir);
        if (link_targets(path, exe)) DeleteFileW(path);
    }

    if (remove_settings) {
        PWSTR appdata = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata)) && appdata) {
            swprintf(path, MAX_PATH, L"%s\\Dofram\\dofram.ini", appdata);
            DeleteFileW(path);
            swprintf(path, MAX_PATH, L"%s\\Dofram", appdata);
            RemoveDirectoryW(path); /* only when empty */
            CoTaskMemFree(appdata);
        }
    }
}

/* Removes a file that may still be in use: a hidden command interpreter keeps
 * trying for about a minute, then removes the folder if it is empty. */
static bool delete_later(const wchar_t *file, const wchar_t *dir)
{
    wchar_t sys[MAX_PATH], cmd[MAX_PATH * 4];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmd, MAX_PATH * 4,
             L"\"%s\\cmd.exe\" /d /q /c \"(for /l %%i in (1,1,60) do @if exist \"%s\" (ping -n 2 127.0.0.1 >nul"
             L" & del /f /q \"%s\" 2>nul)) & rmdir \"%s\" 2>nul\"",
             sys, file, file, dir);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static int uninstall_machine(void)
{
    wchar_t dir[MAX_PATH], target[MAX_PATH], path[MAX_PATH];
    inst_install_dir(dir, MAX_PATH);
    inst_installed_exe(target, MAX_PATH);

    if (start_menu_link(path, MAX_PATH)) DeleteFileW(path);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, APPPATH_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, APPPATH_KEY);

    swprintf(path, MAX_PATH, L"%s\\Dofram.old.exe", dir);
    DeleteFileW(path);
    if (file_exists(target) && !DeleteFileW(target)) {
        /* In use (this process or the copy that started it): delete it once it exits. */
        if (!delete_later(target, dir)) {
            MoveFileExW(target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            MoveFileExW(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } else {
        RemoveDirectoryW(dir);
    }
    return 0;
}

static HRESULT CALLBACK shield_cb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data)
{
    (void)wp; (void)lp; (void)data;
    if (msg == TDN_CREATED && !inst_is_elevated()) SendMessageW(hwnd, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, BTN_YES, TRUE);
    return S_OK;
}

/* Asks before uninstalling. Returns false when the user said no. */
static bool confirm_uninstall(HWND owner, bool *remove_settings)
{
    TASKDIALOG_BUTTON buttons[2] = {
        { BTN_YES, L"Uninstall" },
        { BTN_NO, L"Cancel" },
    };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = owner;
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"Uninstall Dofram";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = L"Remove Dofram from this PC?";
    tc.pszContent = L"The program, its Start menu shortcut and its \"Installed apps\" entry are removed. "
                    L"Your screenshots are not touched.";
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.nDefaultButton = BTN_NO;
    tc.pszVerificationText = L"Also delete my Dofram settings";
    tc.pfCallback = shield_cb;
    int button = 0;
    BOOL settings = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, NULL, &settings)) || button != BTN_YES) return false;
    *remove_settings = settings != FALSE;
    return true;
}

/* ------------------------------------------------------------------ */
/* Command line entry                                                    */
/* ------------------------------------------------------------------ */

bool inst_handle_command_line(const AppArgs *args, int *exit_code)
{
    if (!args->install && !args->uninstall) return false;
    bool quiet = args->quiet;
    bool elevated = inst_is_elevated();

    if (args->install) {
        if (!elevated) {
            DWORD code = 1;
            if (!run_elevated(NULL, quiet ? L"--install --quiet --elevated" : L"--install --elevated", &code))
                code = EXIT_CANCELLED;
            *exit_code = (int)code;
            return true;
        }
        int r = install_machine(quiet);
        if (r == 0 && !quiet)
            MessageBoxW(NULL, L"Dofram is installed. You can start it from the Start menu.", SETUP_TITLE,
                        MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        *exit_code = r;
        return true;
    }

    /* --uninstall: the elevated child started by another Dofram process only
     * does the machine part; everything else happens in the first process. */
    if (args->elevated_child && elevated) {
        *exit_code = uninstall_machine();
        return true;
    }
    wchar_t ver[64];
    if (!inst_installed_version(ver, 64) && !uninstall_key_exists()) {
        if (!quiet)
            MessageBoxW(NULL, L"Dofram is not installed on this PC. A portable copy can simply be deleted.",
                        L"Uninstall Dofram", MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        *exit_code = 0;
        return true;
    }
    bool remove_settings = false;
    if (!quiet && !confirm_uninstall(NULL, &remove_settings)) {
        *exit_code = EXIT_DECLINED;
        return true;
    }
    close_running_installed();
    uninstall_user(remove_settings);
    DWORD code = 0;
    if (elevated) code = (DWORD)uninstall_machine();
    else if (!run_elevated(NULL, L"--uninstall --quiet --elevated", &code)) code = EXIT_CANCELLED;
    if (!quiet && code == 0)
        MessageBoxW(NULL, L"Dofram has been removed from this PC.", L"Uninstall Dofram",
                    MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
    *exit_code = (int)code;
    return true;
}

/* ------------------------------------------------------------------ */
/* Offer and tray menu actions                                           */
/* ------------------------------------------------------------------ */

bool inst_launch_installed(void)
{
    wchar_t exe[MAX_PATH], dir[MAX_PATH], cmd[MAX_PATH + 4];
    inst_installed_exe(exe, MAX_PATH);
    inst_install_dir(dir, MAX_PATH);
    swprintf(cmd, MAX_PATH + 4, L"\"%s\"", exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

/* Installs through an elevated copy, then hands over to the installed copy.
 * Returns true when this process should exit. */
static bool install_and_hand_over(HWND owner, void (*release)(void))
{
    DWORD code = 1;
    if (!run_elevated(owner, L"--install --quiet --elevated", &code)) return false;
    if (code != 0) {
        MessageBoxW(owner, L"Dofram could not be installed. You can keep using this copy.", SETUP_TITLE,
                    MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        return false;
    }
    if (release) release();
    if (!inst_launch_installed())
        MessageBoxW(owner, L"Dofram is installed, but it could not be started. Start it from the Start menu.",
                    SETUP_TITLE, MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
    return true;
}

bool inst_startup_offer(const AppArgs *args, void (*release)(void))
{
    wchar_t ver[64];
    InstallState st;
    st.portable_ini = settings_is_portable();
    st.running_installed = inst_running_installed();
    st.ask_enabled = g_cfg.ask_to_install;
    st.installed = inst_installed_version(ver, 64);
    st.installed_version = ver;
    InstallOffer kind = inst_offer_kind(args, &st);
    if (kind == OFFER_NONE) return false;

    TASKDIALOG_BUTTON buttons[2];
    buttons[0].nButtonID = BTN_YES;
    buttons[0].pszButtonText = kind == OFFER_UPDATE
        ? L"Update the installed copy\nReplaces the Dofram in Program Files with this version."
        : L"Install Dofram\nCopies Dofram to Program Files and adds it to the Start menu.";
    buttons[1].nButtonID = BTN_NO;
    buttons[1].pszButtonText = L"Run without installing\nUse this copy as a portable app.";

    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Dofram";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = kind == OFFER_UPDATE ? L"Update Dofram on this PC?" : L"Install Dofram on this PC?";
    tc.pszContent = L"Dofram is a single program file. Installing it takes a second and needs administrator "
                    L"approval. You can uninstall it any time from Settings \x2192 Apps.";
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.nDefaultButton = BTN_YES;
    tc.pszVerificationText = L"Don't ask me again";
    tc.pfCallback = shield_cb;

    int button = 0;
    BOOL never = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, NULL, &never))) return false;
    if (never && button != BTN_YES) {
        g_cfg.ask_to_install = false;
        settings_save();
    }
    if (button != BTN_YES) return false;
    return install_and_hand_over(NULL, release);
}

bool inst_interactive_install(HWND owner, void (*release)(void))
{
    if (inst_running_installed()) {
        MessageBoxW(owner, L"This copy of Dofram is already the installed one.", APP_NAME,
                    MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        return false;
    }
    return install_and_hand_over(owner, release);
}

bool inst_interactive_uninstall(HWND owner)
{
    wchar_t ver[64];
    if (!inst_installed_version(ver, 64) && !uninstall_key_exists()) {
        MessageBoxW(owner, L"Dofram is not installed on this PC. A portable copy can simply be deleted.", APP_NAME,
                    MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
        return false;
    }
    bool remove_settings = false;
    if (!confirm_uninstall(owner, &remove_settings)) return false;
    DWORD code = 1;
    if (!run_elevated(owner, L"--uninstall --quiet --elevated", &code)) return false;
    if (code != 0) {
        MessageBoxW(owner, L"Dofram could not be uninstalled.", L"Uninstall Dofram",
                    MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
        return false;
    }
    uninstall_user(remove_settings);
    MessageBoxW(owner, L"Dofram has been removed from this PC and will now exit.", L"Uninstall Dofram",
                MB_ICONINFORMATION | MB_OK | MB_SETFOREGROUND);
    return true;
}

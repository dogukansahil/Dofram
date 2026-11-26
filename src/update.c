/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * "Check for Updates on GitHub": asks the GitHub API for the latest release,
 * only when the user picks the menu item (never on its own). The request
 * carries a generic User-Agent and nothing else. Nothing is downloaded or
 * run; a newer version only offers to open its release page in the browser.
 */
#include "dofram.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winhttp.h>

#define UPDATE_TITLE L"Dofram update"
#define BODY_MAX     (512 * 1024)

/* ------------------------------------------------------------------ */
/* Pure helpers (unit tested)                                            */
/* ------------------------------------------------------------------ */

/* Reads "v1.2.3", "1.2" or "1.2.3-beta" into v. Returns false when there is no number. */
bool upd_parse_version(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (!s) return false;
    while (*s == ' ') s++;
    if (*s == 'v' || *s == 'V') s++;
    if (*s < '0' || *s > '9') return false;
    for (int i = 0; i < 3; i++) {
        long n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s - '0');
            if (n > 1000000) return false;
            s++;
        }
        v[i] = (int)n;
        if (*s != '.' || s[1] < '0' || s[1] > '9') break;
        s++;
    }
    return true;
}

int upd_compare(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* Finds "key": "value" at any level of a JSON text and copies the value.
 * Tags and GitHub URLs need no escapes, so a value with a backslash is rejected. */
bool upd_json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    if (!json || !key || !out || cap == 0) return false;
    out[0] = 0;
    if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) return false;
    size_t plen = strlen(pat);
    for (const char *p = strstr(json, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + plen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != ':') continue; /* the text appeared as a value, not as a key */
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        if (*q != '"') return false;
        q++;
        size_t n = 0;
        while (*q && *q != '"') {
            if (*q == '\\' || n + 1 >= cap) { out[0] = 0; return false; }
            out[n++] = *q++;
        }
        out[n] = 0;
        if (*q != '"') { out[0] = 0; return false; }
        return true;
    }
    return false;
}

/* Only pages of this project are opened from an API answer. */
bool upd_release_url_ok(const char *url)
{
    static const char prefix[] = DOFRAM_URL_STR "/";
    if (!url || strncmp(url, prefix, sizeof(prefix) - 1) != 0) return false;
    for (const char *c = url; *c; c++)
        if ((unsigned char)*c <= ' ' || *c == '"' || *c == '\\' || (unsigned char)*c >= 0x7F) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Request                                                               */
/* ------------------------------------------------------------------ */

static volatile LONG g_busy;

static DWORD WINAPI update_thread(LPVOID arg)
{
    HWND notify = (HWND)arg;
    UpdateResult *r = (UpdateResult *)calloc(1, sizeof(*r));
    if (!r) {
        InterlockedExchange(&g_busy, 0);
        return 0;
    }
    HINTERNET s = WinHttpOpen(L"Dofram/" DOFRAM_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 10000, 15000);
    HINTERNET c = s ? WinHttpConnect(s, DOFRAM_API_HOST_WSTR, INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", DOFRAM_API_LATEST_WSTR, NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : NULL;
    if (q) {
        /* No cookies or stored credentials are sent. */
        DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
        WinHttpSetOption(q, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    }
    if (q && WinHttpSendRequest(q, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA,
                                0, 0, 0) &&
        WinHttpReceiveResponse(q, NULL)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)status;
        if (status == 200) {
            size_t len = 0;
            char *body = (char *)malloc(BODY_MAX + 1);
            DWORD got = 0;
            while (body && len < BODY_MAX && WinHttpReadData(q, body + len, (DWORD)(BODY_MAX - len), &got) && got > 0)
                len += got;
            if (body) {
                body[len] = 0;
                if (!upd_json_string(body, "tag_name", r->tag, sizeof(r->tag))) r->status = 0;
                if (!upd_json_string(body, "html_url", r->url, sizeof(r->url))) r->url[0] = 0;
                free(body);
            } else {
                r->status = 0;
            }
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!PostMessageW(notify, WM_APP_UPDATE, 0, (LPARAM)r)) {
        free(r);
        InterlockedExchange(&g_busy, 0);
    }
    return 0;
}

bool upd_check_async(HWND notify)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return false; /* already asking */
    HANDLE t = CreateThread(NULL, 0, update_thread, (LPVOID)notify, 0, NULL);
    if (!t) {
        InterlockedExchange(&g_busy, 0);
        return false;
    }
    CloseHandle(t);
    return true;
}

static void open_page(HWND owner, const char *url)
{
    wchar_t w[512];
    if (url && upd_release_url_ok(url) && MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 512) > 0)
        ShellExecuteW(owner, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    else
        ShellExecuteW(owner, L"open", DOFRAM_URL_WSTR L"/releases", NULL, NULL, SW_SHOWNORMAL);
}

void upd_show_result(HWND owner, UpdateResult *r)
{
    InterlockedExchange(&g_busy, 0);
    if (!r) return;
    const UINT fg = MB_SETFOREGROUND | MB_TOPMOST;
    int have[3] = { DOFRAM_VERSION_MAJOR, DOFRAM_VERSION_MINOR, DOFRAM_VERSION_PATCH }, got[3];
    if (r->status == 200 && upd_parse_version(r->tag, got)) {
        if (upd_compare(got, have) > 0) {
            wchar_t msg[256];
            swprintf(msg, 256, L"Dofram %d.%d.%d is available. You have %s.\n\nOpen the release page on GitHub?",
                     got[0], got[1], got[2], APP_VERSION);
            if (MessageBoxW(owner, msg, UPDATE_TITLE, MB_ICONINFORMATION | MB_YESNO | fg) == IDYES)
                open_page(owner, r->url);
        } else {
            MessageBoxW(owner, L"You have the latest version (" DOFRAM_VERSION_WSTR L").", UPDATE_TITLE,
                        MB_ICONINFORMATION | MB_OK | fg);
        }
    } else if (r->status == 404) {
        MessageBoxW(owner, L"No release has been published on GitHub yet.", UPDATE_TITLE, MB_ICONINFORMATION | MB_OK | fg);
    } else {
        wchar_t msg[256];
        if (r->status == 0)
            swprintf(msg, 256, L"Could not reach GitHub. Check your internet connection.\n\nOpen the releases page in your browser?");
        else
            swprintf(msg, 256, L"GitHub did not answer as expected (HTTP %d).\n\nOpen the releases page in your browser?", r->status);
        if (MessageBoxW(owner, msg, UPDATE_TITLE, MB_ICONWARNING | MB_YESNO | fg) == IDYES) open_page(owner, NULL);
    }
    free(r);
}

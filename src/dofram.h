/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Shared declarations for all modules.
 */
#ifndef DOFRAM_H
#define DOFRAM_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#include "version.h"

#define APP_NAME     DOFRAM_NAME_WSTR
#define APP_VERSION  DOFRAM_VERSION_WSTR

/* Private window messages. */
#define WM_APP_TRAY        (WM_APP + 1)
#define WM_APP_TEXT_COMMIT (WM_APP + 2)
#define WM_APP_TEXT_CANCEL (WM_APP + 3)
#define WM_APP_QUIT        (WM_APP + 4)   /* asks a running copy to exit (sent by the uninstaller) */
#define WM_APP_UPDATE      (WM_APP + 5)   /* the update check has an answer; lParam = UpdateResult* */

#define HOST_CLASS L"DoframHostWindow"

/* ------------------------------------------------------------------ */
/* Rectangles: x, y, width, height (right and bottom are exclusive).    */
/* ------------------------------------------------------------------ */

typedef struct { int x, y, w, h; } Rc;

static inline Rc rc_make(int x, int y, int w, int h) { Rc r = { x, y, w, h }; return r; }
static inline int rc_right(Rc r) { return r.x + r.w; }
static inline int rc_bottom(Rc r) { return r.y + r.h; }
static inline bool rc_empty(Rc r) { return r.w <= 0 || r.h <= 0; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline Rc rc_ltrb(int l, int t, int r, int b) { return rc_make(l, t, r - l, b - t); }

static inline Rc rc_intersect(Rc a, Rc b)
{
    int l = imax(a.x, b.x), t = imax(a.y, b.y);
    int r = imin(rc_right(a), rc_right(b)), btm = imin(rc_bottom(a), rc_bottom(b));
    if (r <= l || btm <= t) return rc_make(0, 0, 0, 0);
    return rc_ltrb(l, t, r, btm);
}

/* Bounding box of two rectangles; an empty rectangle is ignored. */
static inline Rc rc_union(Rc a, Rc b)
{
    if (rc_empty(a)) return b;
    if (rc_empty(b)) return a;
    return rc_ltrb(imin(a.x, b.x), imin(a.y, b.y),
                   imax(rc_right(a), rc_right(b)), imax(rc_bottom(a), rc_bottom(b)));
}

static inline bool rc_has(Rc r, POINT p)
{
    return p.x >= r.x && p.x < rc_right(r) && p.y >= r.y && p.y < rc_bottom(r);
}

static inline bool rc_contains(Rc outer, Rc inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           rc_right(inner) <= rc_right(outer) && rc_bottom(inner) <= rc_bottom(outer);
}

static inline bool rc_overlaps(Rc a, Rc b)
{
    return a.x < rc_right(b) && b.x < rc_right(a) && a.y < rc_bottom(b) && b.y < rc_bottom(a);
}

static inline Rc rc_inflate(Rc r, int dx, int dy) { return rc_make(r.x - dx, r.y - dy, r.w + 2 * dx, r.h + 2 * dy); }
static inline Rc rc_offset(Rc r, int dx, int dy) { return rc_make(r.x + dx, r.y + dy, r.w, r.h); }
static inline bool rc_equal(Rc a, Rc b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
static inline RECT rc_to_rect(Rc r) { RECT o = { r.x, r.y, r.x + r.w, r.y + r.h }; return o; }
static inline Rc rc_from_rect(RECT r) { return rc_ltrb(r.left, r.top, r.right, r.bottom); }
static inline POINT pt(int x, int y) { POINT p = { x, y }; return p; }

/* ------------------------------------------------------------------ */
/* Images: 32-bit top-down DIB sections (memory layout B, G, R, A).     */
/* ------------------------------------------------------------------ */

typedef struct Img {
    int w, h;
    uint32_t *px;     /* w * h pixels, row stride = w */
    HBITMAP hbm;
    HDC dc;           /* memory DC with hbm selected */
    HGDIOBJ old;
    void *gp;         /* GpBitmap wrapping px (created on demand) */
} Img;

Img *img_new(int w, int h);
void img_free(Img *img);
Img *img_crop(const Img *src, Rc r);
void img_copy_rect(Img *dst, const Img *src, Rc r);
Img *img_capture_screen(Rc screen);
void *img_graphics(Img *img, Rc clip, bool antialias);   /* returns GpGraphics*, free with GdipDeleteGraphics */

void fx_opaque(Img *img);
void fx_fill(Img *img, Rc r, uint32_t rgb);
void fx_darken(Img *img, int alpha);
void fx_blur(Img *img, Rc r, int radius);
void fx_pixelate(Img *img, Rc r, int block);
void fx_multiply(Img *img, Rc r, uint32_t rgb);

bool img_save(Img *img, const wchar_t *path, int jpeg_quality);
bool img_copy_to_clipboard(Img *img, HWND owner);

/* ------------------------------------------------------------------ */
/* Edit objects                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    TOOL_SELECT, TOOL_BLUR, TOOL_PIXELATE, TOOL_BOX, TOOL_FRAME,
    TOOL_ARROW, TOOL_PEN, TOOL_HIGHLIGHT, TOOL_TEXT, TOOL_COUNT
} Tool;

typedef struct Op {
    Tool kind;
    uint32_t color;          /* 0xRRGGBB */
    int size;                /* level 1..10 */
    Rc r, r_orig;            /* region tools */
    POINT a, b, a_orig, b_orig; /* arrow */
    POINT *pts;              /* pen */
    int npts, cap;
    wchar_t *text;           /* text, lines separated by '\n' */
    POINT p;                 /* text origin */
    Rc text_ext;             /* text extent relative to p */
} Op;

Op *op_new(Tool kind, uint32_t color, int size);
Op *op_clone(const Op *o);
void op_free(Op *o);
Rc op_bounds(const Op *o);
Rc op_frame(const Op *o);
bool op_meaningful(const Op *o);
bool op_hit(const Op *o, POINT p, int tol);
void op_move(Op *o, int dx, int dy);
int op_handles(const Op *o, POINT out[8]);
void op_begin_handle_drag(Op *o);
void op_drag_handle(Op *o, int index, int dx, int dy);
void op_apply(const Op *o, Img *img, Rc clip);
void op_pen_add(Op *o, POINT p);
void op_set_text(Op *o, const wchar_t *text);
void op_set_size(Op *o, int size);
bool op_is_region(const Op *o);

float op_line_width(int level);
int op_blur_radius(int level);
int op_pixel_block(int level);
float op_font_px(int level);

void geo_handles(Rc r, POINT out[8]);
Rc geo_resize(Rc o, int handle, int dx, int dy);
Rc geo_norm(POINT a, POINT b);

const wchar_t *tool_key(Tool t);
Tool tool_from_key(const wchar_t *s, Tool fallback);

/* ------------------------------------------------------------------ */
/* Settings                                                              */
/* ------------------------------------------------------------------ */

typedef enum { AFTER_EDITOR, AFTER_WINDOW, AFTER_CLIPBOARD, AFTER_SAVE, AFTER_BOTH, AFTER_COUNT } AfterCapture;

typedef struct {
    bool ctrl, alt, shift, win;
    UINT vk;
} Hotkey;

typedef struct {
    wchar_t hotkey[64];
    AfterCapture after;
    wchar_t folder[MAX_PATH];
    wchar_t pattern[128];
    bool jpg;
    int jpeg_quality;
    bool start_with_windows;
    bool copy_on_save;
    bool close_after_save;
    bool close_after_copy;
    bool notifications;
    Tool default_tool;
    int tool_size;
    bool ask_to_install;     /* offer to install on start when running a copy outside Program Files */
} Settings;

extern Settings g_cfg;

void settings_load(void);
bool settings_save(void);
void settings_path(wchar_t *out, int cap);
bool settings_is_portable(void);      /* a dofram.ini lives next to the executable */
const wchar_t *after_key(AfterCapture a);

Hotkey hotkey_parse(const wchar_t *text);
void hotkey_format(const Hotkey *h, wchar_t *out, int cap);
UINT hotkey_modifiers(const Hotkey *h);
bool hotkey_valid(const Hotkey *h);

/* ------------------------------------------------------------------ */
/* Editor (frozen-screen overlay and separate window)                    */
/* ------------------------------------------------------------------ */

typedef enum { OUTCOME_NONE, OUTCOME_COPY, OUTCOME_SAVE, OUTCOME_WINDOW, OUTCOME_QUICK } Outcome;

bool editor_register(HINSTANCE inst);
/* Takes ownership of shot. initial may be NULL. */
HWND overlay_open(Img *shot, Rc virt, bool quick, const Rc *initial);
/* Takes ownership of img. */
HWND editor_window_open(Img *img);

/* ------------------------------------------------------------------ */
/* Application services (app.c)                                         */
/* ------------------------------------------------------------------ */

extern HINSTANCE g_inst;

void app_overlay_done(Outcome outcome, Img *result);
void app_notify(const wchar_t *title, const wchar_t *text, const wchar_t *path, bool warning);
bool app_save_image(Img *img, bool also_copy, HWND owner);
bool app_save_image_as(Img *img, HWND owner);
bool app_copy_image(Img *img, HWND owner);
void app_open_folder(const wchar_t *folder, const wchar_t *select_file);
HICON app_icon(bool large);
int ui_scale(int px);                 /* 96 DPI pixels to system DPI */
int ui_scale_dpi(int px, int dpi);
HFONT ui_font(int pt, bool bold, int dpi);
void app_error(HWND owner, const wchar_t *fmt, ...);

/* Settings dialog (settingsdlg.c). Returns true when saved. */
bool settings_dialog(HWND owner);

/* Utilities (util.c) */
bool util_next_save_path(wchar_t *out, int cap);
bool util_ensure_dir(const wchar_t *dir);
void util_expand(const wchar_t *in, wchar_t *out, int cap);
void util_exe_path(wchar_t *out, int cap);
bool util_startup_set(bool enabled);
void util_startup_sync(bool enabled);
bool util_create_shortcut(bool desktop, wchar_t *out_path, int cap);
bool util_pick_folder(HWND owner, wchar_t *path, int cap);
bool util_pick_color(HWND owner, uint32_t *rgb);
wchar_t *util_strdup(const wchar_t *s);

/* ------------------------------------------------------------------ */
/* Command line (install.c)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    bool tray, capture, settings;    /* normal start options */
    bool install, uninstall, quiet;  /* self-installer */
    bool portable;                   /* do not offer to install this time */
    bool elevated_child;             /* internal: started elevated by another Dofram process */
} AppArgs;

void args_parse(int argc, wchar_t **argv, AppArgs *out);

/* ------------------------------------------------------------------ */
/* Self-installer (install.c)                                           */
/* Installs to %ProgramFiles%\Dofram, adds a Start menu shortcut and an  */
/* "Installed apps" entry. No external installer technology.            */
/* ------------------------------------------------------------------ */

typedef enum { OFFER_NONE, OFFER_INSTALL, OFFER_UPDATE } InstallOffer;

typedef struct {
    bool portable_ini;          /* dofram.ini next to the executable */
    bool running_installed;     /* this process is %ProgramFiles%\Dofram\Dofram.exe */
    bool ask_enabled;           /* settings: ask_to_install */
    bool installed;             /* an installation is registered and its file exists */
    const wchar_t *installed_version;
} InstallState;

/* Pure decision: whether a start with these arguments shows the install offer. */
InstallOffer inst_offer_kind(const AppArgs *args, const InstallState *st);
/* Pure: true when path names a file directly inside dir (case-insensitive). */
bool inst_path_in_dir(const wchar_t *path, const wchar_t *dir);
/* Pure: true when a "Run" command line starts the executable at exe. */
bool inst_command_targets(const wchar_t *command, const wchar_t *exe);

void inst_install_dir(wchar_t *out, int cap);
void inst_installed_exe(wchar_t *out, int cap);
bool inst_running_installed(void);
bool inst_installed_version(wchar_t *version, int cap);
bool inst_is_elevated(void);

/* Handles --install / --uninstall. Returns true when the process should exit with *exit_code. */
bool inst_handle_command_line(const AppArgs *args, int *exit_code);
/* Shows the start-up offer when it applies. Returns true when the installed
 * copy was started and this process should exit (release the single-instance
 * mutex before starting it: release_mutex is called first). */
bool inst_startup_offer(const AppArgs *args, void (*release_mutex)(void));
/* Tray menu actions. Both return true when this process should exit now. */
bool inst_interactive_install(HWND owner, void (*release_mutex)(void));
bool inst_interactive_uninstall(HWND owner);
/* Starts the installed copy (no arguments). */
bool inst_launch_installed(void);

/* ------------------------------------------------------------------ */
/* Update check (update.c): asks GitHub for the latest release, only    */
/* when the user picks the menu item. Nothing is downloaded or run.     */
/* ------------------------------------------------------------------ */

typedef struct {
    int status;        /* HTTP status, 0 when GitHub could not be reached */
    char tag[64];      /* "v1.2.3" */
    char url[512];     /* release page */
} UpdateResult;

bool upd_parse_version(const char *s, int v[3]);
int upd_compare(const int a[3], const int b[3]);   /* <0, 0, >0 */
bool upd_json_string(const char *json, const char *key, char *out, size_t cap);
bool upd_release_url_ok(const char *url);
/* Starts the request on a worker thread; the answer arrives as WM_APP_UPDATE at notify. */
bool upd_check_async(HWND notify);
/* Shows the answer and frees r. */
void upd_show_result(HWND owner, UpdateResult *r);

#endif

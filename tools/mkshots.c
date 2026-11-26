/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mkshots: generates the README screenshots without capturing the real
 * screen. A neutral "desktop" with made-up content (a fake order page with an
 * example e-mail address and a test card number) is drawn in code and fed to
 * the real editor as the frozen screenshot. The editor is driven on hidden
 * windows with synthetic input, exactly like tests\test_editor.c, and its
 * rendered frames are written as PNG. Nothing of the user's desktop appears.
 *
 * Usage: mkshots.exe <outdir>     (writes screenshot.png and editor-window.png)
 *
 * Build (from the repository root, in a Visual Studio command prompt):
 *   rc /nologo /fo build\tools\dofram.res src\dofram.rc
 *   type nul > build\tools\dofram.ini
 *   cl /nologo /std:c11 /utf-8 /W4 /wd4100 /O2 /MT /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_CRT_SECURE_NO_WARNINGS /Isrc tools\mkshots.c src\image.c src\ops.c src\settings.c src\util.c build\tools\dofram.res /Fobuild\tools\ /Fdbuild\tools\ /Febuild\tools\mkshots.exe /link /SUBSYSTEM:CONSOLE /MANIFEST:NO user32.lib gdi32.lib gdiplus.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib advapi32.lib
 * The empty dofram.ini next to the exe keeps settings out of %APPDATA%.
 */
#include "../src/editor.c"
#include "../src/settingsdlg.c"

HINSTANCE g_inst;
static wchar_t g_out[MAX_PATH];
static int g_dpi = 120;

/* ---- stubs for app.c services ---- */
int ui_scale_dpi(int px, int dpi) { return MulDiv(px, dpi, 96); }
int ui_scale(int px) { return px; }
HFONT ui_font(int pt, bool bold, int dpi)
{
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}
HICON app_icon(bool large) { (void)large; return LoadIconW(NULL, IDI_APPLICATION); }
void app_error(HWND owner, const wchar_t *fmt, ...) { (void)owner; wprintf(L"app_error: %s\n", fmt); }
void app_notify(const wchar_t *t, const wchar_t *x, const wchar_t *p, bool w) { (void)t; (void)x; (void)p; (void)w; }
void app_open_folder(const wchar_t *f, const wchar_t *s) { (void)f; (void)s; }
bool app_save_image(Img *img, bool c, HWND o) { (void)img; (void)c; (void)o; return true; }
bool app_save_image_as(Img *img, HWND o) { (void)img; (void)o; return true; }
bool app_copy_image(Img *img, HWND o) { (void)img; (void)o; return true; }
void app_overlay_done(Outcome o, Img *r) { (void)o; if (r) img_free(r); }

/* ---- synthetic input ---- */
static void down(Editor *e, POINT p) { on_mouse_down(e, p, false, false); }
static void up(Editor *e, POINT p) { on_mouse_up(e, p, true); }
/* Drags between two points given in image coordinates. */
static void idrag(Editor *e, int x1, int y1, int x2, int y2)
{
    POINT a = to_scr(e, pt(x1, y1)), b = to_scr(e, pt(x2, y2));
    down(e, a);
    for (int i = 1; i <= 8; i++) on_mouse_move(e, pt(a.x + (b.x - a.x) * i / 8, a.y + (b.y - a.y) * i / 8));
    up(e, b);
}

static void tool(Editor *e, Tool t, int size)
{
    set_tool(e, t);
    e->sel = NULL;
    e->size = size;
}

/* ---- drawing the fake desktop ---- */
static GpGraphics *g_g;
static Img *g_img;

static void m_rounded(GpPath *p, float x, float y, float w, float h, float rad)
{
    float d = rad * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
}

static void m_rr(float x, float y, float w, float h, float rad, ARGB c)
{
    GpPath *p = NULL;
    GpBrush *b = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &p);
    m_rounded(p, x, y, w, h, rad);
    GdipCreateSolidFill(c, &b);
    GdipFillPath(g_g, b, p);
    GdipDeleteBrush(b);
    GdipDeletePath(p);
}

static void m_rect(float x, float y, float w, float h, ARGB c)
{
    GpBrush *b = NULL;
    GdipCreateSolidFill(c, &b);
    GdipFillRectangle(g_g, b, x, y, w, h);
    GdipDeleteBrush(b);
}

static void m_circle(float cx, float cy, float r, ARGB c) { m_rr(cx - r, cy - r, 2 * r, 2 * r, r, c); }

static void m_text(int x, int y, const wchar_t *s, int px, int weight, COLORREF c)
{
    GdipFlush(g_g, 1);
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old = SelectObject(g_img->dc, f);
    SetBkMode(g_img->dc, TRANSPARENT);
    SetTextColor(g_img->dc, c);
    TextOutW(g_img->dc, x, y, s, (int)wcslen(s));
    SelectObject(g_img->dc, old);
    DeleteObject(f);
    GdiFlush();
}

/* Scene geometry shared with the edit script (image coordinates, 1600 x 900). */
#define SW 1600
#define SH 900
#define BAR_X0 470
#define BAR_STEP 72
#define BAR_W 44
#define BAR_BASE 700
static const int BAR_H[12] = { 90, 130, 110, 160, 140, 190, 170, 120, 215, 150, 130, 175 };
#define TALL 8

static Img *make_desktop(void)
{
    Img *d = img_new(SW, SH);
    g_img = d;
    /* wallpaper gradient */
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            double u = (double)x / SW, v = (double)y / SH, t = 0.65 * u + 0.35 * v;
            double r = 28 + 80 * t, g = 58 + 22 * t, b = 120 + 40 * t - 70 * u * v;
            double dx = (x - 1250.0) / 700.0, dy = (y - 180.0) / 500.0, dd = sqrt(dx * dx + dy * dy);
            if (dd < 1) { double k = (1 - dd) * (1 - dd); r += 90 * k; g += 60 * k; b += 40 * k; }
            d->px[y * SW + x] = 0xFF000000u | ((uint32_t)iclamp((int)r, 0, 255) << 16) | ((uint32_t)iclamp((int)g, 0, 255) << 8) | (uint32_t)iclamp((int)b, 0, 255);
        }
    g_g = (GpGraphics *)img_graphics(d, rc_make(0, 0, SW, SH), true);
    GdipSetPixelOffsetMode(g_g, GP_PIXOFF_HALF);

    /* soft shapes on the wallpaper */
    m_circle(160, 700, 260, 0x14FFFFFF);
    m_circle(1500, 120, 190, 0x10FFFFFF);

    /* window shadow, then the window */
    for (int i = 24; i >= 1; i--) m_rr((float)(196 - i), (float)(70 - i + 14), (float)(1208 + 2 * i), (float)(700 + 2 * i), (float)(12 + i), (ARGB)(((int)(3 + (24 - i) * 0.1)) << 24));
    m_rr(196, 70, 1208, 700, 12, 0xFFF7F8FB);
    m_rr(196, 70, 1208, 44, 12, 0xFFE9ECF3);
    m_rect(196, 100, 1208, 14, 0xFFE9ECF3);
    m_rect(196, 114, 1208, 1, 0xFFD3D8E3);
    m_circle(222, 92, 6, 0xFFE06C5F);
    m_circle(244, 92, 6, 0xFFE5B84E);
    m_circle(266, 92, 6, 0xFF62C554);
    /* sidebar */
    m_rect(196, 115, 190, 655, 0xFFEEF1F7);
    m_rect(386, 115, 1, 655, 0xFFD9DEE8);
    m_rr(206, 160, 170, 36, 8, 0xFFD6E3FB);
    /* status pill */
    m_rr(628, 290, 112, 32, 16, 0xFFD5F2DC);
    /* chart card */
    m_rr(430, 400, 940, 350, 12, 0xFFFFFFFF);
    m_rr(430.5f, 400.5f, 939, 349, 12, 0x00FFFFFF);
    for (int i = 0; i < 4; i++) m_rect(450, (float)(BAR_BASE - 60 - i * 60), 900, 1, 0xFFE6E9F0);
    for (int i = 0; i < 12; i++)
        m_rr((float)(BAR_X0 + i * BAR_STEP), (float)(BAR_BASE - BAR_H[i]), BAR_W, (float)BAR_H[i], 6, i == TALL ? 0xFF3478F6 : 0xFFB5CBF3);
    m_rect(450, BAR_BASE, 900, 2, 0xFFC9CFDC);
    /* sidebar items */
    for (int i = 0; i < 6; i++) m_rr(224, (float)(218 + i * 44), (float)(90 + (i * 23 % 50)), 12, 6, 0xFFC4CBDA);
    
    /* taskbar with generic icons */
    m_rect(0, SH - 52, SW, 52, 0xE60F1424);
    for (int i = 0; i < 6; i++) m_rr((float)(630 + i * 56), SH - 42, 32, 32, 8, i == 1 ? 0xFF5B8DEF : (i == 3 ? 0xFF3FB68B : 0xFF55607C));
    GdipFlush(g_g, 1);

    m_text(760, 76, L"Orders", 17, FW_NORMAL, RGB(70, 78, 98));
    m_text(236, 168, L"Overview", 20, FW_SEMIBOLD, RGB(44, 84, 190));
    m_text(430, 128, L"Order #2041", 34, FW_SEMIBOLD, RGB(28, 34, 50));
    m_text(430, 196, L"Customer", 21, FW_NORMAL, RGB(110, 118, 136));
    m_text(620, 194, L"alex@example.com", 25, FW_NORMAL, RGB(28, 34, 50));
    m_text(430, 246, L"Card number", 21, FW_NORMAL, RGB(110, 118, 136));
    m_text(620, 244, L"4111 1111 1111 1111", 25, FW_NORMAL, RGB(28, 34, 50));
    m_text(430, 296, L"Status", 21, FW_NORMAL, RGB(110, 118, 136));
    m_text(654, 293, L"Shipped", 21, FW_SEMIBOLD, RGB(36, 120, 60));
    m_text(430, 348, L"Delivery window: Tuesday, 9:00 to 12:00", 23, FW_NORMAL, RGB(28, 34, 50));
    m_text(450, 414, L"Usage this month", 21, FW_SEMIBOLD, RGB(28, 34, 50));
    static const wchar_t *mon[12] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    for (int i = 0; i < 12; i++) m_text(BAR_X0 + i * BAR_STEP + 4, BAR_BASE + 10, mon[i], 16, FW_NORMAL, RGB(110, 118, 136));
    m_text(666, SH - 40, L"", 10, FW_NORMAL, 0);
    m_text(SW - 90, SH - 42, L"10:42", 17, FW_NORMAL, RGB(235, 238, 248));
    m_text(SW - 100, SH - 24, L"6/24/2026", 14, FW_NORMAL, RGB(190, 198, 218));

    GdipDeleteGraphics(g_g);
    g_g = NULL;
    GdiFlush();
    fx_opaque(d);
    return d;
}

/* ---- the edits (coordinates are desktop pixels; off moves them into the shown image) ---- */
static void do_edits(Editor *e, int ox, int oy)
{
    tool(e, TOOL_BLUR, 3);
    idrag(e, 612 - ox, 188 - oy, 866 - ox, 230 - oy);
    tool(e, TOOL_BOX, 5);
    idrag(e, 612 - ox, 238 - oy, 884 - ox, 280 - oy);
    tool(e, TOOL_HIGHLIGHT, 5);
    idrag(e, 424 - ox, 346 - oy, 842 - ox, 380 - oy);
    int bx = BAR_X0 + TALL * BAR_STEP, by = BAR_BASE - BAR_H[TALL];
    tool(e, TOOL_FRAME, 4);
    idrag(e, bx - 12 - ox, by - 12 - oy, bx + BAR_W + 12 - ox, BAR_BASE + 10 - oy);
    tool(e, TOOL_ARROW, 5);
    idrag(e, bx + 170 - ox, by - 62 - oy, bx + BAR_W + 22 - ox, by + 6 - oy);
    tool(e, TOOL_TEXT, 3);
    POINT a = to_scr(e, pt(bx + 130 - ox, by - 112 - oy));
    down(e, a);
    if (e->edit) {
        SetWindowTextW(e->edit, L"Fixed in 1.0");
        commit_text(e);
    }
    set_tool(e, TOOL_SELECT);
    /* select the frame so its handles show */
    for (int i = 0; i < e->nobjs; i++)
        if (e->objs[i]->kind == TOOL_FRAME) e->sel = e->objs[i];
}

static bool save_png(Img *img, const wchar_t *name)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\%s", g_out, name);
    bool ok = img_save(img, p, 90);
    wprintf(L"%s %s (%dx%d)\n", ok ? L"Wrote" : L"FAILED", p, img->w, img->h);
    return ok;
}

static void make_overlay_shot(Img *desktop)
{
    Img *shot = img_crop(desktop, rc_make(0, 0, SW, SH));
    Editor *e = editor_new(shot, false);
    e->virt = rc_make(0, 0, SW, SH);
    e->dpi = g_dpi;
    e->dim = img_new(SW, SH);
    img_copy_rect(e->dim, shot, e->full);
    fx_darken(e->dim, 145);
    make_fonts(e);
    CreateWindowExW(WS_EX_TOOLWINDOW, OVERLAY_CLASS, L"shot", WS_POPUP, -20000, -20000, SW, SH, NULL, NULL, g_inst, e);
    e->has_mouse = false;
    idrag(e, 196, 100, 1404, 770);
    do_edits(e, 0, 0);
    e->cur = pt(1500, 600);
    paint(e, client_rect(e));
    save_png(e->back, L"screenshot.png");
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

static void make_window_shot(Img *desktop)
{
    const int ox = 196, oy = 100;
    Img *img = img_crop(desktop, rc_make(ox, oy, 1208, 670));
    Editor *e = editor_new(img, true);
    e->crop = e->full;
    e->mode = MODE_EDITING;
    e->dpi = g_dpi;
    make_fonts(e);
    CreateWindowExW(0, WINDOW_CLASS, L"shot", WS_POPUP, -20000, -20000, 1320, 850, NULL, NULL, g_inst, e);
    layout_bar(e);
    e->zoom = 1.0;
    clamp_view(e);
    do_edits(e, ox, oy);
    paint(e, client_rect(e));
    save_png(e->back, L"editor-window.png");
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { fwprintf(stderr, L"Usage: mkshots.exe <outdir>\n"); return 1; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = GetModuleHandleW(NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, NULL);
    editor_register(g_inst);
    settings_load();
    _wfullpath(g_out, argv[1], MAX_PATH);
    util_ensure_dir(g_out);

    Img *desktop = make_desktop();
    save_png(desktop, L"_desktop.png");
    make_overlay_shot(desktop);
    make_window_shot(desktop);
    img_free(desktop);
    GdiplusShutdown(tok);
    return 0;
}

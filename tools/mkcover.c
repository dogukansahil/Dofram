/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mkcover: draws the 1280 x 640 repository cover (README banner and GitHub
 * social preview) and writes it as PNG. Everything is drawn here with GDI+
 * and GDI: the logo uses the same geometry as tools\makeicon.c, the product
 * mock-up is made of simple shapes, and the text is set in Segoe UI, which
 * ships with Windows. No third-party code or artwork is involved.
 *
 * Usage: mkcover.exe <out.png>
 * Build (from the repository root, in a Visual Studio command prompt):
 *   cl /nologo /O2 /W4 /utf-8 /DUNICODE /D_UNICODE tools\mkcover.c /Fobuild\tools\ /Febuild\tools\mkcover.exe /link user32.lib gdi32.lib gdiplus.lib ole32.lib
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gdip.h"

/* A few more flat GDI+ functions that gdip.h does not declare. */
__declspec(dllimport) GpStatus WINAPI GdipSetClipPath(GpGraphics *g, GpPath *path, INT combine);
__declspec(dllimport) GpStatus WINAPI GdipResetClip(GpGraphics *g);

#define W 1280
#define H 640

static const CLSID CLSID_EncPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

static BYTE *g_px;       /* BGRA, top-down, shared by GDI and GDI+ */
static HDC g_dc;
static GpGraphics *g_g;

#define ARGBC(a, rgb) ((ARGB)(((DWORD)(a) << 24) | ((rgb) & 0xFFFFFF)))

/* ---- GDI+ helpers ------------------------------------------------------- */

static void rounded(GpPath *p, float x, float y, float w, float h, float rad)
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

static void fill_rr(float x, float y, float w, float h, float rad, ARGB c)
{
    GpPath *p = NULL;
    GpBrush *b = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &p);
    rounded(p, x, y, w, h, rad);
    GdipCreateSolidFill(c, &b);
    GdipFillPath(g_g, b, p);
    GdipDeleteBrush(b);
    GdipDeletePath(p);
}

static void stroke_rr(float x, float y, float w, float h, float rad, ARGB c, float pw)
{
    GpPath *p = NULL;
    GpPen *pen = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &p);
    rounded(p, x, y, w, h, rad);
    GdipCreatePen1(c, pw, GP_UNIT_WORLD, &pen);
    GdipDrawPath(g_g, pen, p);
    GdipDeletePen(pen);
    GdipDeletePath(p);
}

static void fill_rect(float x, float y, float w, float h, ARGB c)
{
    GpBrush *b = NULL;
    GdipCreateSolidFill(c, &b);
    GdipFillRectangle(g_g, b, x, y, w, h);
    GdipDeleteBrush(b);
}

static void fill_circle(float cx, float cy, float r, ARGB c)
{
    fill_rr(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

static void line(float x1, float y1, float x2, float y2, ARGB c, float pw)
{
    GpPen *pen = NULL;
    GdipCreatePen1(c, pw, GP_UNIT_WORLD, &pen);
    GdipSetPenStartCap(pen, GP_CAP_ROUND);
    GdipSetPenEndCap(pen, GP_CAP_ROUND);
    GdipSetPenLineJoin(pen, GP_JOIN_ROUND);
    GdipDrawLine(g_g, pen, x1, y1, x2, y2);
    GdipDeletePen(pen);
}

/* Soft shadow: stacked translucent rounded rectangles. */
static void shadow(float x, float y, float w, float h, float rad, float spread, int alpha)
{
    int n = 24;
    for (int i = n; i >= 1; i--) {
        float k = spread * i / n;
        float a = alpha * (1.0f - (float)i / (n + 1));
        fill_rr(x - k, y - k + spread * 0.45f, w + 2 * k, h + 2 * k, rad + k, ARGBC((int)(a / 13.0f + 0.5f), 0x000000));
    }
}

/* ---- text (GDI on the shared DIB) -------------------------------------- */

static void gdi_begin(void) { GdipFlush(g_g, 1); }
static void gdi_end(void) { GdiFlush(); }

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

static int text_w(const wchar_t *s, int px, int weight)
{
    HFONT f = make_font(px, weight);
    HGDIOBJ old = SelectObject(g_dc, f);
    SIZE sz;
    GetTextExtentPoint32W(g_dc, s, (int)wcslen(s), &sz);
    SelectObject(g_dc, old);
    DeleteObject(f);
    return sz.cx;
}

/* (x, y) is the top of the line box. */
static void text(int x, int y, const wchar_t *s, int px, int weight, COLORREF c)
{
    HFONT f = make_font(px, weight);
    HGDIOBJ old = SelectObject(g_dc, f);
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, c);
    TextOutW(g_dc, x, y, s, (int)wcslen(s));
    SelectObject(g_dc, old);
    DeleteObject(f);
}

#define RGBH(v) RGB(((v) >> 16) & 255, ((v) >> 8) & 255, (v) & 255)

/* ---- logo (same geometry as tools\makeicon.c) -------------------------- */

static void logo(float ox, float oy, float s)
{
    float pad = s * 0.04f;
    GpRectF bg = { ox + pad, oy + pad, s - 2 * pad, s - 2 * pad };
    GpPath *path = NULL;
    GpBrush *grad = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &path);
    rounded(path, bg.X, bg.Y, bg.Width, bg.Height, s * 0.22f);
    GdipCreateLineBrushFromRectWithAngle(&bg, 0xFF3478F6, 0xFF183A96, 45.0f, TRUE, GP_WRAP_TILE, &grad);
    GdipFillPath(g_g, grad, path);
    GdipDeleteBrush(grad);
    GdipDeletePath(path);

    float m = s * 0.2f, len = s * 0.2f, pw = s * 0.075f;
    GpPen *pen = NULL;
    GdipCreatePen1(0xFFFFFFFF, pw, GP_UNIT_WORLD, &pen);
    GdipSetPenStartCap(pen, GP_CAP_ROUND);
    GdipSetPenEndCap(pen, GP_CAP_ROUND);
    GdipSetPenLineJoin(pen, GP_JOIN_ROUND);
    float a = ox + m, b = ox + s - m, a2 = oy + m, b2 = oy + s - m;
    GpPointF c1[3] = { { a, a2 + len }, { a, a2 }, { a + len, a2 } };
    GpPointF c2[3] = { { b - len, a2 }, { b, a2 }, { b, a2 + len } };
    GpPointF c3[3] = { { b, b2 - len }, { b, b2 }, { b - len, b2 } };
    GpPointF c4[3] = { { a + len, b2 }, { a, b2 }, { a, b2 - len } };
    GdipDrawLines(g_g, pen, c1, 3);
    GdipDrawLines(g_g, pen, c2, 3);
    GdipDrawLines(g_g, pen, c3, 3);
    GdipDrawLines(g_g, pen, c4, 3);
    GdipDeletePen(pen);

    int n = 3;
    float area = s * 0.36f, x0 = ox + (s - area) / 2, y0 = oy + (s - area) / 2, cell = area / n;
    static const ARGB shades[3] = { 0xFFFFFFFF, 0xFFAACDFF, 0xFF78AAFA };
    GdipSetSmoothingMode(g_g, GP_SMOOTH_NONE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) fill_rect(x0 + x * cell, y0 + y * cell, cell + 0.5f, cell + 0.5f, shades[(x + y * 2) % 3]);
    GdipSetSmoothingMode(g_g, GP_SMOOTH_ANTIALIAS);
}

/* ---- background --------------------------------------------------------- */

static void background(void)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double u = (double)x / W, v = (double)y / H;
            /* base: deep navy, slightly lighter toward the lower right */
            double r = 9 + 16 * u * v, g = 13 + 22 * u * v, b = 26 + 46 * (0.35 * u + 0.65 * v);
            /* soft blue glow behind the product mock-up */
            double dx = (x - 980.0) / 520.0, dy = (y - 320.0) / 380.0, d = sqrt(dx * dx + dy * dy);
            double glow = d < 1 ? (1 - d) * (1 - d) : 0;
            r += 14 * glow;
            g += 40 * glow;
            b += 92 * glow;
            /* second faint glow, top left */
            dx = (x - 160.0) / 460.0;
            dy = (y - 110.0) / 300.0;
            d = sqrt(dx * dx + dy * dy);
            glow = d < 1 ? (1 - d) * (1 - d) : 0;
            r += 6 * glow;
            g += 14 * glow;
            b += 34 * glow;
            /* dot grid */
            if ((x % 32) == 0 && (y % 32) == 0) { r += 10; g += 14; b += 22; }
            BYTE *p = g_px + ((size_t)y * W + x) * 4;
            p[0] = (BYTE)(b > 255 ? 255 : b);
            p[1] = (BYTE)(g > 255 ? 255 : g);
            p[2] = (BYTE)(r > 255 ? 255 : r);
            p[3] = 255;
        }
}

/* ---- product mock-up ---------------------------------------------------- */

static void handle(float cx, float cy)
{
    fill_rect(cx - 5, cy - 5, 10, 10, 0xFF0096FF);
    fill_rect(cx - 3.5f, cy - 3.5f, 7, 7, 0xFFFFFFFF);
}

static void arrow(float x1, float y1, float x2, float y2, ARGB c, float pw)
{
    double ang = atan2(y2 - y1, x2 - x1), hl = pw * 4.2, hw = pw * 2.4;
    float bx = (float)(x2 - cos(ang) * hl * 0.8), by = (float)(y2 - sin(ang) * hl * 0.8);
    line(x1, y1, bx, by, c, pw);
    GpPointF tip[3] = {
        { x2, y2 },
        { (float)(x2 - cos(ang) * hl + sin(ang) * hw), (float)(y2 - sin(ang) * hl - cos(ang) * hw) },
        { (float)(x2 - cos(ang) * hl - sin(ang) * hw), (float)(y2 - sin(ang) * hl + cos(ang) * hw) },
    };
    GpBrush *b = NULL;
    GdipCreateSolidFill(c, &b);
    GdipFillPolygon(g_g, b, tip, 3, GP_FILL_WINDING);
    GdipDeleteBrush(b);
}

static void mockup(void)
{
    const float px = 742, py = 78, pw = 482, ph = 456;

    shadow(px, py, pw, ph, 18, 46, 255);

    /* the panel is clipped to a rounded rectangle */
    GpPath *clip = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &clip);
    rounded(clip, px, py, pw, ph, 18);
    GdipSetClipPath(g_g, clip, GP_COMBINE_REPLACE);

    /* wallpaper */
    GpRectF wr = { px, py, pw, ph };
    GpBrush *wall = NULL;
    GdipCreateLineBrushFromRectWithAngle(&wr, 0xFF3E7BD8, 0xFF1C2F6B, 60.0f, TRUE, GP_WRAP_TILE, &wall);
    GdipFillRectangle(g_g, wall, px, py, pw, ph);
    GdipDeleteBrush(wall);

    /* a window on the "desktop" */
    float wx = px + 28, wy = py + 30, ww = pw - 56, wh = 330;
    fill_rr(wx, wy, ww, wh, 10, 0xFFF4F6FA);
    fill_rect(wx, wy + 30, ww, 1, 0xFFD5DAE4);
    fill_circle(wx + 18, wy + 15, 4.5f, 0xFFE06C5F);
    fill_circle(wx + 34, wy + 15, 4.5f, 0xFFE5B84E);
    fill_circle(wx + 50, wy + 15, 4.5f, 0xFF62C554);

    gdi_begin();
    text((int)wx + 22, (int)wy + 46, L"Order #2041", 19, FW_SEMIBOLD, RGB(32, 38, 52));
    text((int)wx + 22, (int)wy + 82, L"Contact", 13, FW_NORMAL, RGB(110, 118, 135));
    text((int)wx + 22, (int)wy + 128, L"Payment", 13, FW_NORMAL, RGB(110, 118, 135));
    text((int)wx + 112, (int)wy + 79, L"alex@example.com", 17, FW_NORMAL, RGB(32, 38, 52));
    text((int)wx + 112, (int)wy + 125, L"4111 1111 1111 1111", 17, FW_NORMAL, RGB(32, 38, 52));
    text((int)wx + 22, (int)wy + 174, L"Usage this month", 13, FW_NORMAL, RGB(110, 118, 135));
    gdi_end();

    /* bar chart */
    static const float bars[7] = { 0.38f, 0.55f, 0.46f, 0.74f, 0.62f, 0.9f, 0.7f };
    float cx0 = wx + 26, cy0 = wy + wh - 22, bw = 34, gap = 16, chh = 112;
    for (int i = 0; i < 7; i++) {
        ARGB c = i == 5 ? 0xFF3478F6 : 0xFFA9C4F2;
        fill_rr(cx0 + i * (bw + gap), cy0 - chh * bars[i], bw, chh * bars[i], 5, c);
    }

    /* dim everything outside the selection */
    float sx = wx - 14, sy = wy + 36, sw = ww + 28, sh = 296;
    ARGB dim = ARGBC(120, 0x05080F);
    fill_rect(px, py, pw, sy - py, dim);
    fill_rect(px, sy + sh, pw, py + ph - (sy + sh), dim);
    fill_rect(px, sy, sx - px, sh, dim);
    fill_rect(sx + sw, sy, px + pw - (sx + sw), sh, dim);

    /* edits inside the selection: blur block, black box, marker, arrow, frame, label */
    float ex = wx + 108, ey = wy + 74;
    GdipSetSmoothingMode(g_g, GP_SMOOTH_NONE);
    static const ARGB blur[6] = { 0xFFB9C3D6, 0xFFD3DAE6, 0xFFA5B0C7, 0xFFC6CEDE, 0xFFAEB9CE, 0xFFDDE2EC };
    for (int yy = 0; yy < 3; yy++)
        for (int xx = 0; xx < 11; xx++)
            fill_rect(ex - 4 + xx * 14.5f, ey - 3 + yy * 12, 15, 12.5f, blur[(xx * 5 + yy * 3 + xx / 3) % 6]);
    GdipSetSmoothingMode(g_g, GP_SMOOTH_ANTIALIAS);
    fill_rr(wx + 108, wy + 122, 178, 30, 3, 0xFF0A0A0C);
    fill_rr(wx + 22, wy + 170, 120, 22, 2, ARGBC(120, 0xFFE63C));
    stroke_rr(cx0 + 5 * (bw + gap) - 6, cy0 - chh * 0.9f - 6, bw + 12, chh * 0.9f + 12, 6, 0xFFE5342C, 3.0f);
    arrow(wx + 372, wy + 148, cx0 + 5 * (bw + gap) + bw + 8, cy0 - chh * 0.9f - 4, 0xFFE5342C, 4.0f);
    gdi_begin();
    text((int)wx + 304, (int)wy + 114, L"Fixed in 1.0", 19, FW_SEMIBOLD, RGB(229, 52, 44));
    gdi_end();

    /* selection outline, size label, handles */
    stroke_rr(sx, sy, sw, sh, 0, 0xFF0096FF, 2.0f);
    fill_rect(sx - 1, sy - 24, 92, 22, ARGBC(225, 0x0078D7));
    gdi_begin();
    text((int)sx + 8, (int)sy - 24, L"370 \x00D7 286", 14, FW_SEMIBOLD, RGB(255, 255, 255));
    gdi_end();
    float hx[3] = { sx, sx + sw / 2, sx + sw }, hy[3] = { sy, sy + sh / 2, sy + sh };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            if (i != 1 || j != 1) handle(hx[i], hy[j]);

    GdipResetClip(g_g);
    GdipDeletePath(clip);

    /* toolbar under the selection */
    float tw = 340, th = 40, tx = sx + sw - tw, ty = sy + sh + 12;
    if (ty + th > py + ph - 8) ty = py + ph - th - 8;
    shadow(tx, ty, tw, th, 8, 16, 190);
    fill_rr(tx, ty, tw, th, 8, 0xFF1E2230);
    stroke_rr(tx + 0.5f, ty + 0.5f, tw - 1, th - 1, 8, 0xFF3A4156, 1.0f);
    /* tool glyphs */
    float gx = tx + 22, gy = ty + th / 2;
    for (int i = 0; i < 8; i++) {
        float x = gx + i * 33;
        int act = i == 3;
        if (act) fill_rr(x - 14, gy - 14, 28, 28, 6, 0xFF2F6FE0);
        ARGB c = act ? 0xFFFFFFFF : 0xFFB6BED2;
        switch (i) {
        case 0: line(x - 6, gy - 7, x + 5, gy + 3, c, 2); line(x - 6, gy - 7, x - 6, gy + 5, c, 2); line(x - 6, gy - 7, x + 3, gy - 7, c, 2); break;
        case 1: stroke_rr(x - 7, gy - 7, 14, 14, 3, c, 2); break;
        case 2: for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) fill_rect(x - 7 + a * 5, gy - 7 + b * 5, 4, 4, ((a + b) & 1) ? c : ARGBC(130, 0xB6BED2)); break;
        case 3: fill_rr(x - 7, gy - 6, 14, 12, 2, c); break;
        case 4: stroke_rr(x - 7, gy - 6, 14, 12, 2, c, 2); break;
        case 5: line(x - 7, gy + 6, x + 6, gy - 6, c, 2); line(x + 6, gy - 6, x + 1, gy - 6, c, 2); line(x + 6, gy - 6, x + 6, gy - 1, c, 2); break;
        case 6: line(x - 7, gy + 3, x - 2, gy - 5, c, 2); line(x - 2, gy - 5, x + 2, gy + 4, c, 2); line(x + 2, gy + 4, x + 7, gy - 4, c, 2); break;
        case 7: fill_rr(x - 7, gy - 3, 14, 7, 2, c); break;
        }
    }
    fill_rect(tx + 22 + 8 * 33 - 12, ty + 9, 1, th - 18, 0xFF3A4156);
    fill_circle(tx + tw - 24, gy, 8, 0xFFE5342C);
    fill_circle(tx + tw - 24, gy, 8.5f, ARGBC(60, 0xFFFFFF));

    /* subtle rim */
    stroke_rr(px + 0.5f, py + 0.5f, pw - 1, ph - 1, 18, ARGBC(70, 0x9DB4E8), 1.0f);
}

/* ---- left column -------------------------------------------------------- */

static void chip(int x, int y, const wchar_t *label)
{
    int w = text_w(label, 22, FW_SEMIBOLD) + 40, h = 42;
    fill_rr((float)x, (float)y, (float)w, (float)h, 21, ARGBC(255, 0x0F1A33));
    stroke_rr(x + 0.5f, y + 0.5f, (float)w - 1, (float)h - 1, 21, 0xFF22325A, 1.0f);
    gdi_begin();
    text(x + 20, y + 5, label, 22, FW_SEMIBOLD, RGB(214, 226, 252));
    gdi_end();
}

static int chip_w(const wchar_t *label) { return text_w(label, 22, FW_SEMIBOLD) + 40; }

static void left_column(void)
{
    logo(96, 118, 112);
    gdi_begin();
    text(232, 118, L"Dofram", 124, FW_SEMIBOLD, RGB(246, 249, 255));
    text(99, 284, L"Screenshot, mark up, share.", 40, FW_SEMIBOLD, RGB(226, 235, 255));
    text(99, 338, L"A tiny Windows tool in plain C, no libraries,", 28, FW_NORMAL, RGB(140, 160, 200));
    text(99, 374, L"no runtime, no telemetry.", 28, FW_NORMAL, RGB(140, 160, 200));
    gdi_end();

    static const wchar_t *chips[3] = { L"Blur and box", L"Arrows and text", L"Edit on screen" };
    int x = 99;
    for (int i = 0; i < 3; i++) {
        chip(x, 436, chips[i]);
        x += chip_w(chips[i]) + 14;
    }

    gdi_begin();
    text(99, 520, L"Windows 10 and 11  \x00B7  About 300 KB, one portable exe  \x00B7  GPL-3.0", 19, FW_NORMAL, RGB(112, 130, 168));
    gdi_end();
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { fwprintf(stderr, L"Usage: mkcover.exe out.png\n"); return 1; }
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token;
    if (GdiplusStartup(&token, &gsi, NULL) != GP_OK) return 1;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    g_dc = CreateCompatibleDC(NULL);
    SelectObject(g_dc, hbm);
    if (!hbm || !g_px) return 1;

    background();

    GpBitmap *bmp = NULL;
    GdipCreateBitmapFromScan0(W, H, W * 4, GP_PIXEL_32BPP_RGB, g_px, &bmp);
    GdipGetImageGraphicsContext(bmp, &g_g);
    GdipSetSmoothingMode(g_g, GP_SMOOTH_ANTIALIAS);
    GdipSetPixelOffsetMode(g_g, GP_PIXOFF_HALF);

    left_column();
    mockup();

    GdipFlush(g_g, 1);
    GdiFlush();
    GdipDeleteGraphics(g_g);
    for (int i = 0; i < W * H; i++) g_px[i * 4 + 3] = 255;
    GpStatus st = GdipSaveImageToFile(bmp, argv[1], &CLSID_EncPng, NULL);
    GdipDisposeImage(bmp);
    DeleteDC(g_dc);
    DeleteObject(hbm);
    GdiplusShutdown(token);
    if (st != GP_OK) { fwprintf(stderr, L"Could not write %s\n", argv[1]); return 1; }
    wprintf(L"Written: %s\n", argv[1]);
    return 0;
}

/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Draws the Dofram application icon in code and writes it as a .ico file.
 * Usage: makeicon.exe <output.ico> [preview.png]
 * Sizes 16..64 are stored as 32-bit BMP, 256 as PNG.
 * Design: blue rounded square, white selection corners, a mosaic square in the middle.
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gdip.h"

static const CLSID CLSID_EncPng = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

typedef struct { BYTE *data; DWORD size; } Blob;

static void rounded(GpPath *p, float x, float y, float w, float h, float rad)
{
    float d = rad * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
}

/* Draws one size into a straight-alpha BGRA buffer. */
static GpBitmap *draw(int s, BYTE *px)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GdipCreateBitmapFromScan0(s, s, s * 4, GP_PIXEL_32BPP_ARGB, px, &bmp);
    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
    GdipSetPixelOffsetMode(g, GP_PIXOFF_HIGHQ);
    GdipGraphicsClear(g, 0x00000000);

    float pad = s * 0.04f;
    GpRectF bg = { pad, pad, s - 2 * pad, s - 2 * pad };
    GpPath *path = NULL;
    GpBrush *grad = NULL;
    GdipCreatePath(GP_FILL_ALTERNATE, &path);
    rounded(path, bg.X, bg.Y, bg.Width, bg.Height, s * 0.22f);
    GdipCreateLineBrushFromRectWithAngle(&bg, 0xFF3478F6, 0xFF183A96, 45.0f, TRUE, GP_WRAP_TILE, &grad);
    GdipFillPath(g, grad, path);
    GdipDeleteBrush(grad);
    GdipDeletePath(path);

    /* Selection corners */
    float m = s * 0.2f, len = s * 0.2f;
    float pw = s * 0.075f;
    if (pw < 1.5f) pw = 1.5f;
    GpPen *pen = NULL;
    GdipCreatePen1(0xFFFFFFFF, pw, GP_UNIT_WORLD, &pen);
    GdipSetPenStartCap(pen, GP_CAP_ROUND);
    GdipSetPenEndCap(pen, GP_CAP_ROUND);
    GdipSetPenLineJoin(pen, GP_JOIN_ROUND);
    float a = m, b = s - m;
    GpPointF c1[3] = { { a, a + len }, { a, a }, { a + len, a } };
    GpPointF c2[3] = { { b - len, a }, { b, a }, { b, a + len } };
    GpPointF c3[3] = { { b, b - len }, { b, b }, { b - len, b } };
    GpPointF c4[3] = { { a + len, b }, { a, b }, { a, b - len } };
    GdipDrawLines(g, pen, c1, 3);
    GdipDrawLines(g, pen, c2, 3);
    GdipDrawLines(g, pen, c3, 3);
    GdipDrawLines(g, pen, c4, 3);
    GdipDeletePen(pen);

    /* Mosaic */
    int n = s >= 32 ? 3 : 2;
    float area = s * 0.36f;
    float x0 = (s - area) / 2.0f, y0 = (s - area) / 2.0f, cell = area / n;
    static const ARGB shades[3] = { 0xFFFFFFFF, 0xFFAACDFF, 0xFF78AAFA };
    GdipSetSmoothingMode(g, GP_SMOOTH_NONE);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            GpBrush *br = NULL;
            GdipCreateSolidFill(shades[(x + y * 2) % 3], &br);
            GdipFillRectangle(g, br, x0 + x * cell, y0 + y * cell, cell + 0.5f, cell + 0.5f);
            GdipDeleteBrush(br);
        }
    GdipDeleteGraphics(g);
    return bmp;
}

static Blob png_blob(GpBitmap *bmp)
{
    Blob out = { NULL, 0 };
    IStream *st = NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &st))) return out;
    if (GdipSaveImageToStream(bmp, st, &CLSID_EncPng, NULL) == GP_OK) {
        HGLOBAL hg = NULL;
        GetHGlobalFromStream(st, &hg);
        STATSTG stat;
        st->lpVtbl->Stat(st, &stat, STATFLAG_NONAME);
        out.size = (DWORD)stat.cbSize.QuadPart;
        out.data = (BYTE *)malloc(out.size);
        void *p = GlobalLock(hg);
        if (out.data && p) memcpy(out.data, p, out.size);
        GlobalUnlock(hg);
    }
    st->lpVtbl->Release(st);
    return out;
}

/* Icon BMP: BITMAPINFOHEADER, bottom-up BGRA rows, empty AND mask. */
static Blob dib_blob(const BYTE *px, int s)
{
    Blob out;
    int mask_row = ((s + 31) / 32) * 4;
    out.size = 40 + s * s * 4 + mask_row * s;
    out.data = (BYTE *)calloc(1, out.size);
    BITMAPINFOHEADER *h = (BITMAPINFOHEADER *)out.data;
    h->biSize = 40;
    h->biWidth = s;
    h->biHeight = s * 2;
    h->biPlanes = 1;
    h->biBitCount = 32;
    h->biSizeImage = s * s * 4 + mask_row * s;
    for (int y = 0; y < s; y++) memcpy(out.data + 40 + (size_t)y * s * 4, px + (size_t)(s - 1 - y) * s * 4, (size_t)s * 4);
    return out;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) { fwprintf(stderr, L"Usage: makeicon.exe output.ico [preview.png]\n"); return 1; }
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR token;
    if (GdiplusStartup(&token, &gsi, NULL) != GP_OK) return 1;

    static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    enum { N = sizeof(sizes) / sizeof(sizes[0]) };
    Blob blobs[N];
    for (int i = 0; i < N; i++) {
        int s = sizes[i];
        BYTE *px = (BYTE *)calloc((size_t)s * s, 4);
        GpBitmap *bmp = draw(s, px);
        if (s >= 256) {
            blobs[i] = png_blob(bmp);
            if (argc > 2) GdipSaveImageToFile(bmp, argv[2], &CLSID_EncPng, NULL);
        } else {
            blobs[i] = dib_blob(px, s);
        }
        GdipDisposeImage(bmp);
        free(px);
        if (!blobs[i].data) { fwprintf(stderr, L"Could not draw size %d\n", s); return 1; }
    }

    FILE *f = _wfopen(argv[1], L"wb");
    if (!f) { fwprintf(stderr, L"Cannot write %s\n", argv[1]); return 1; }
    WORD hdr[3] = { 0, 1, N };
    fwrite(hdr, 2, 3, f);
    DWORD offset = 6 + 16 * N;
    for (int i = 0; i < N; i++) {
        int s = sizes[i];
        BYTE e[16];
        memset(e, 0, sizeof(e));
        e[0] = (BYTE)(s >= 256 ? 0 : s);
        e[1] = (BYTE)(s >= 256 ? 0 : s);
        e[4] = 1;                      /* planes */
        e[6] = 32;                     /* bits per pixel */
        memcpy(e + 8, &blobs[i].size, 4);
        memcpy(e + 12, &offset, 4);
        fwrite(e, 1, 16, f);
        offset += blobs[i].size;
    }
    for (int i = 0; i < N; i++) { fwrite(blobs[i].data, 1, blobs[i].size, f); free(blobs[i].data); }
    fclose(f);
    GdiplusShutdown(token);
    wprintf(L"Written: %s\n", argv[1]);
    return 0;
}

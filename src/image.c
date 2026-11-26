/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Image buffers, screen capture, pixel effects, saving and clipboard.
 */
#include "dofram.h"
#include "gdip.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* Encoder class IDs of the built-in GDI+ codecs. */
static const CLSID CLSID_EncBmp  = { 0x557cf400, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const CLSID CLSID_EncJpeg = { 0x557cf401, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const CLSID CLSID_EncPng  = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
static const GUID GUID_EncQuality = { 0x1d5be4b5, 0xfa4a, 0x452d, { 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb } };

Img *img_new(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    Img *img = (Img *)calloc(1, sizeof(Img));
    if (!img) return NULL;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    img->hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!img->hbm || !bits) { free(img); return NULL; }
    img->px = (uint32_t *)bits;
    img->w = w;
    img->h = h;
    img->dc = CreateCompatibleDC(NULL);
    img->old = SelectObject(img->dc, img->hbm);
    return img;
}

void img_free(Img *img)
{
    if (!img) return;
    if (img->gp) GdipDisposeImage(img->gp);
    if (img->dc) {
        SelectObject(img->dc, img->old);
        DeleteDC(img->dc);
    }
    if (img->hbm) DeleteObject(img->hbm);
    free(img);
}

static Rc img_rect(const Img *img) { return rc_make(0, 0, img->w, img->h); }

void img_copy_rect(Img *dst, const Img *src, Rc r)
{
    r = rc_intersect(rc_intersect(r, img_rect(dst)), img_rect(src));
    if (rc_empty(r)) return;
    GdiFlush();
    for (int y = r.y; y < rc_bottom(r); y++)
        memcpy(dst->px + (size_t)y * dst->w + r.x, src->px + (size_t)y * src->w + r.x, (size_t)r.w * 4);
}

Img *img_crop(const Img *src, Rc r)
{
    r = rc_intersect(r, img_rect(src));
    if (rc_empty(r)) return NULL;
    Img *out = img_new(r.w, r.h);
    if (!out) return NULL;
    GdiFlush();
    for (int y = 0; y < r.h; y++)
        memcpy(out->px + (size_t)y * r.w, src->px + (size_t)(r.y + y) * src->w + r.x, (size_t)r.w * 4);
    return out;
}

Img *img_capture_screen(Rc s)
{
    Img *img = img_new(s.w, s.h);
    if (!img) return NULL;
    HDC sdc = GetDC(NULL);
    BitBlt(img->dc, 0, 0, s.w, s.h, sdc, s.x, s.y, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, sdc);
    GdiFlush();
    fx_opaque(img); /* BitBlt may leave the alpha byte at zero */
    return img;
}

static GpBitmap *img_gp(Img *img)
{
    if (!img->gp)
        GdipCreateBitmapFromScan0(img->w, img->h, img->w * 4, GP_PIXEL_32BPP_RGB, (BYTE *)img->px, &img->gp);
    return img->gp;
}

void *img_graphics(Img *img, Rc clip, bool antialias)
{
    GpGraphics *g = NULL;
    GdiFlush();
    if (!img_gp(img) || GdipGetImageGraphicsContext(img->gp, &g) != GP_OK) return NULL;
    clip = rc_intersect(clip, img_rect(img));
    GdipSetClipRectI(g, clip.x, clip.y, clip.w, clip.h, GP_COMBINE_REPLACE);
    GdipSetSmoothingMode(g, antialias ? GP_SMOOTH_ANTIALIAS : GP_SMOOTH_NONE);
    return g;
}

/* ------------------------------------------------------------------ */
/* Effects                                                              */
/* ------------------------------------------------------------------ */

void fx_opaque(Img *img)
{
    size_t n = (size_t)img->w * img->h;
    for (size_t i = 0; i < n; i++) img->px[i] |= 0xFF000000u;
}

void fx_fill(Img *img, Rc r, uint32_t rgb)
{
    r = rc_intersect(r, img_rect(img));
    if (rc_empty(r)) return;
    GdiFlush();
    uint32_t v = 0xFF000000u | (rgb & 0xFFFFFF);
    for (int y = r.y; y < rc_bottom(r); y++) {
        uint32_t *row = img->px + (size_t)y * img->w;
        for (int x = r.x; x < rc_right(r); x++) row[x] = v;
    }
}

void fx_darken(Img *img, int alpha)
{
    int keep = 255 - alpha;
    GdiFlush();
    size_t n = (size_t)img->w * img->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = img->px[i];
        uint32_t b = (c & 0xFF) * keep / 255;
        uint32_t g = ((c >> 8) & 0xFF) * keep / 255;
        uint32_t r = ((c >> 16) & 0xFF) * keep / 255;
        img->px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
}

/* Mosaic: every block becomes its average colour. */
void fx_pixelate(Img *img, Rc r, int block)
{
    r = rc_intersect(r, img_rect(img));
    if (rc_empty(r)) return;
    if (block < 2) block = 2;
    GdiFlush();
    for (int by = r.y; by < rc_bottom(r); by += block) {
        int bh = imin(block, rc_bottom(r) - by);
        for (int bx = r.x; bx < rc_right(r); bx += block) {
            int bw = imin(block, rc_right(r) - bx);
            uint64_t sb = 0, sg = 0, sr = 0;
            for (int y = by; y < by + bh; y++) {
                const uint32_t *row = img->px + (size_t)y * img->w;
                for (int x = bx; x < bx + bw; x++) {
                    uint32_t c = row[x];
                    sb += c & 0xFF; sg += (c >> 8) & 0xFF; sr += (c >> 16) & 0xFF;
                }
            }
            uint64_t n = (uint64_t)bw * bh;
            uint32_t v = 0xFF000000u | ((uint32_t)(sr / n) << 16) | ((uint32_t)(sg / n) << 8) | (uint32_t)(sb / n);
            for (int y = by; y < by + bh; y++) {
                uint32_t *row = img->px + (size_t)y * img->w;
                for (int x = bx; x < bx + bw; x++) row[x] = v;
            }
        }
    }
}

/* One horizontal box-average pass over a w*h BGRA buffer (3 colour channels). */
static void box_h(const uint8_t *src, uint8_t *dst, int w, int h, int rad)
{
    int win = rad * 2 + 1, stride = w * 4, last = (w - 1) * 4;
    for (int y = 0; y < h; y++) {
        int row = y * stride;
        int sb = 0, sg = 0, sr = 0;
        for (int k = -rad; k <= rad; k++) {
            int i = row + (k < 0 ? 0 : (k >= w ? last : k * 4));
            sb += src[i]; sg += src[i + 1]; sr += src[i + 2];
        }
        for (int x = 0; x < w; x++) {
            int o = row + x * 4;
            dst[o] = (uint8_t)(sb / win); dst[o + 1] = (uint8_t)(sg / win); dst[o + 2] = (uint8_t)(sr / win);
            int a = x + rad + 1, s = x - rad;
            int ai = row + (a >= w ? last : a * 4);
            int si = row + (s < 0 ? 0 : s * 4);
            sb += src[ai] - src[si]; sg += src[ai + 1] - src[si + 1]; sr += src[ai + 2] - src[si + 2];
        }
    }
}

static void box_v(const uint8_t *src, uint8_t *dst, int w, int h, int rad)
{
    int win = rad * 2 + 1, stride = w * 4, last_row = (h - 1) * stride;
    for (int x = 0; x < w; x++) {
        int col = x * 4;
        int sb = 0, sg = 0, sr = 0;
        for (int k = -rad; k <= rad; k++) {
            int i = col + (k < 0 ? 0 : (k >= h ? last_row : k * stride));
            sb += src[i]; sg += src[i + 1]; sr += src[i + 2];
        }
        for (int y = 0; y < h; y++) {
            int o = y * stride + col;
            dst[o] = (uint8_t)(sb / win); dst[o + 1] = (uint8_t)(sg / win); dst[o + 2] = (uint8_t)(sr / win);
            int a = y + rad + 1, s = y - rad;
            int ai = col + (a >= h ? last_row : a * stride);
            int si = col + (s < 0 ? 0 : s * stride);
            sb += src[ai] - src[si]; sg += src[ai + 1] - src[si + 1]; sr += src[ai + 2] - src[si + 2];
        }
    }
}

/* Three box-blur passes (close to a Gaussian). Only pixels inside r are read. */
void fx_blur(Img *img, Rc r, int radius)
{
    r = rc_intersect(r, img_rect(img));
    if (r.w < 2 || r.h < 2) return;
    if (radius < 1) radius = 1;
    size_t bytes = (size_t)r.w * r.h * 4;
    uint8_t *buf = (uint8_t *)malloc(bytes);
    uint8_t *tmp = (uint8_t *)malloc(bytes);
    if (!buf || !tmp) { free(buf); free(tmp); return; }
    GdiFlush();
    for (int y = 0; y < r.h; y++)
        memcpy(buf + (size_t)y * r.w * 4, img->px + (size_t)(r.y + y) * img->w + r.x, (size_t)r.w * 4);
    for (int pass = 0; pass < 3; pass++) {
        box_h(buf, tmp, r.w, r.h, radius);
        box_v(tmp, buf, r.w, r.h, radius);
    }
    for (int y = 0; y < r.h; y++) {
        uint32_t *dst = img->px + (size_t)(r.y + y) * img->w + r.x;
        const uint32_t *s = (const uint32_t *)(buf + (size_t)y * r.w * 4);
        for (int x = 0; x < r.w; x++) dst[x] = s[x] | 0xFF000000u;
    }
    free(buf);
    free(tmp);
}

/* Highlighter: multiply with the colour, so dark text stays readable. */
void fx_multiply(Img *img, Rc r, uint32_t rgb)
{
    r = rc_intersect(r, img_rect(img));
    if (rc_empty(r)) return;
    uint32_t mb = rgb & 0xFF, mg = (rgb >> 8) & 0xFF, mr = (rgb >> 16) & 0xFF;
    GdiFlush();
    for (int y = r.y; y < rc_bottom(r); y++) {
        uint32_t *row = img->px + (size_t)y * img->w;
        for (int x = r.x; x < rc_right(r); x++) {
            uint32_t c = row[x];
            uint32_t b = (c & 0xFF) * mb / 255;
            uint32_t g = ((c >> 8) & 0xFF) * mg / 255;
            uint32_t rr = ((c >> 16) & 0xFF) * mr / 255;
            row[x] = 0xFF000000u | (rr << 16) | (g << 8) | b;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Saving and clipboard                                                 */
/* ------------------------------------------------------------------ */

bool img_save(Img *img, const wchar_t *path, int jpeg_quality)
{
    const wchar_t *dot = wcsrchr(path, L'.');
    const CLSID *enc = &CLSID_EncPng;
    GpEncoderParameters params;
    GpEncoderParameters *pp = NULL;
    ULONG quality = (ULONG)iclamp(jpeg_quality, 30, 100);
    if (dot && (_wcsicmp(dot, L".jpg") == 0 || _wcsicmp(dot, L".jpeg") == 0)) {
        enc = &CLSID_EncJpeg;
        params.Count = 1;
        params.Parameter[0].Guid = GUID_EncQuality;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Type = GP_ENC_TYPE_LONG;
        params.Parameter[0].Value = &quality;
        pp = &params;
    } else if (dot && _wcsicmp(dot, L".bmp") == 0) {
        enc = &CLSID_EncBmp;
    }
    GdiFlush();
    if (!img_gp(img)) return false;
    return GdipSaveImageToFile(img->gp, path, enc, pp) == GP_OK;
}

static HGLOBAL make_dib(const Img *img)
{
    int row = ((img->w * 3) + 3) & ~3;
    size_t size = sizeof(BITMAPINFOHEADER) + (size_t)row * img->h;
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
    if (!hg) return NULL;
    BYTE *p = (BYTE *)GlobalLock(hg);
    if (!p) { GlobalFree(hg); return NULL; }
    BITMAPINFOHEADER *bh = (BITMAPINFOHEADER *)p;
    bh->biSize = sizeof(BITMAPINFOHEADER);
    bh->biWidth = img->w;
    bh->biHeight = img->h; /* bottom-up, the most widely understood layout */
    bh->biPlanes = 1;
    bh->biBitCount = 24;
    bh->biCompression = BI_RGB;
    bh->biSizeImage = (DWORD)((size_t)row * img->h);
    BYTE *bits = p + sizeof(BITMAPINFOHEADER);
    for (int y = 0; y < img->h; y++) {
        const uint32_t *src = img->px + (size_t)(img->h - 1 - y) * img->w;
        BYTE *dst = bits + (size_t)y * row;
        for (int x = 0; x < img->w; x++) {
            uint32_t c = src[x];
            dst[x * 3] = (BYTE)c;
            dst[x * 3 + 1] = (BYTE)(c >> 8);
            dst[x * 3 + 2] = (BYTE)(c >> 16);
        }
    }
    GlobalUnlock(hg);
    return hg;
}

static HGLOBAL make_png(Img *img)
{
    IStream *st = NULL;
    HGLOBAL hg = NULL;
    if (!img_gp(img)) return NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, FALSE, &st))) return NULL;
    if (GdipSaveImageToStream(img->gp, st, &CLSID_EncPng, NULL) == GP_OK) {
        if (FAILED(GetHGlobalFromStream(st, &hg))) hg = NULL;
    } else if (SUCCEEDED(GetHGlobalFromStream(st, &hg)) && hg) {
        GlobalFree(hg);
        hg = NULL;
    }
    st->lpVtbl->Release(st);
    return hg;
}

/* Puts the image on the clipboard as a classic DIB and as PNG. */
bool img_copy_to_clipboard(Img *img, HWND owner)
{
    GdiFlush();
    HGLOBAL dib = make_dib(img);
    HGLOBAL png = make_png(img);
    if (!dib) { if (png) GlobalFree(png); return false; }
    bool opened = false;
    for (int i = 0; i < 10 && !opened; i++) {
        opened = OpenClipboard(owner) != 0;
        if (!opened) Sleep(100);
    }
    if (!opened) {
        GlobalFree(dib);
        if (png) GlobalFree(png);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, dib) != NULL;
    if (!ok) GlobalFree(dib);
    if (png) {
        UINT fmt = RegisterClipboardFormatW(L"PNG");
        if (!fmt || !SetClipboardData(fmt, png)) GlobalFree(png);
    }
    CloseClipboard();
    return ok;
}

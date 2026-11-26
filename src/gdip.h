/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Minimal declarations for the GDI+ flat API (gdiplus.dll ships with every
 * supported Windows version). The SDK only provides C++ headers for GDI+,
 * so the few functions Dofram needs are declared here for plain C.
 */
#ifndef DOFRAM_GDIP_H
#define DOFRAM_GDIP_H

#include <windows.h>
#include <objidl.h>

typedef int GpStatus;
typedef float REAL;
typedef DWORD ARGB;

typedef void GpGraphics;
typedef void GpImage;
typedef void GpBitmap;
typedef void GpPen;
typedef void GpBrush;
typedef void GpPath;
typedef void GpFontFamily;
typedef void GpFontCollection;
typedef void GpStringFormat;
typedef void GpMatrix;

typedef struct { REAL X, Y; } GpPointF;
typedef struct { REAL X, Y, Width, Height; } GpRectF;

typedef struct {
    UINT32 GdiplusVersion;
    void *DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef struct {
    GUID Guid;
    ULONG NumberOfValues;
    ULONG Type;
    void *Value;
} GpEncoderParameter;

typedef struct {
    UINT Count;
    GpEncoderParameter Parameter[1];
} GpEncoderParameters;

#define GP_OK 0

#define GP_PIXEL_32BPP_RGB  0x00022009
#define GP_PIXEL_32BPP_ARGB 0x0026200A

#define GP_SMOOTH_NONE      3
#define GP_SMOOTH_ANTIALIAS 4
#define GP_PIXOFF_HIGHQ     2
#define GP_PIXOFF_HALF      4
#define GP_CAP_FLAT         0
#define GP_CAP_ROUND        2
#define GP_JOIN_MITER       0
#define GP_JOIN_ROUND       2
#define GP_UNIT_WORLD       0
#define GP_FILL_ALTERNATE   0
#define GP_FILL_WINDING     1
#define GP_FONT_BOLD        1
#define GP_DASH_DASH        1
#define GP_COMBINE_REPLACE  0
#define GP_WRAP_TILE        0
#define GP_ENC_TYPE_LONG    4

#define GPAPI __declspec(dllimport) GpStatus WINAPI

GPAPI GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *input, void *output);
__declspec(dllimport) void WINAPI GdiplusShutdown(ULONG_PTR token);

GPAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, GpBitmap **bmp);
GPAPI GdipDisposeImage(GpImage *img);
GPAPI GdipGetImageGraphicsContext(GpImage *img, GpGraphics **g);
GPAPI GdipDeleteGraphics(GpGraphics *g);
GPAPI GdipFlush(GpGraphics *g, INT intention);
GPAPI GdipGraphicsClear(GpGraphics *g, ARGB color);
GPAPI GdipSetSmoothingMode(GpGraphics *g, INT mode);
GPAPI GdipSetPixelOffsetMode(GpGraphics *g, INT mode);
GPAPI GdipSetClipRectI(GpGraphics *g, INT x, INT y, INT w, INT h, INT combine);

GPAPI GdipCreatePen1(ARGB color, REAL width, INT unit, GpPen **pen);
GPAPI GdipDeletePen(GpPen *pen);
GPAPI GdipSetPenStartCap(GpPen *pen, INT cap);
GPAPI GdipSetPenEndCap(GpPen *pen, INT cap);
GPAPI GdipSetPenLineJoin(GpPen *pen, INT join);
GPAPI GdipSetPenDashStyle(GpPen *pen, INT style);

GPAPI GdipCreateSolidFill(ARGB color, GpBrush **brush);
GPAPI GdipCreateLineBrushFromRectWithAngle(const GpRectF *rect, ARGB c1, ARGB c2, REAL angle,
                                           BOOL scalable, INT wrap, GpBrush **brush);
GPAPI GdipDeleteBrush(GpBrush *brush);

GPAPI GdipDrawLine(GpGraphics *g, GpPen *pen, REAL x1, REAL y1, REAL x2, REAL y2);
GPAPI GdipDrawLines(GpGraphics *g, GpPen *pen, const GpPointF *pts, INT count);
GPAPI GdipDrawRectangle(GpGraphics *g, GpPen *pen, REAL x, REAL y, REAL w, REAL h);
GPAPI GdipFillRectangle(GpGraphics *g, GpBrush *brush, REAL x, REAL y, REAL w, REAL h);
GPAPI GdipFillPolygon(GpGraphics *g, GpBrush *brush, const GpPointF *pts, INT count, INT fill);

GPAPI GdipCreatePath(INT fill, GpPath **path);
GPAPI GdipDeletePath(GpPath *path);
GPAPI GdipAddPathArc(GpPath *path, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
GPAPI GdipClosePathFigure(GpPath *path);
GPAPI GdipFillPath(GpGraphics *g, GpBrush *brush, GpPath *path);
GPAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *path);
GPAPI GdipGetPathWorldBounds(GpPath *path, GpRectF *bounds, const GpMatrix *m, const GpPen *pen);

GPAPI GdipCreateFontFamilyFromName(const WCHAR *name, GpFontCollection *coll, GpFontFamily **family);
GPAPI GdipGetGenericFontFamilySansSerif(GpFontFamily **family);
GPAPI GdipDeleteFontFamily(GpFontFamily *family);
GPAPI GdipStringFormatGetGenericTypographic(GpStringFormat **format);
GPAPI GdipAddPathString(GpPath *path, const WCHAR *text, INT length, const GpFontFamily *family,
                        INT style, REAL em, const GpRectF *layout, const GpStringFormat *format);

GPAPI GdipSaveImageToFile(GpImage *img, const WCHAR *file, const CLSID *encoder,
                          const GpEncoderParameters *params);
GPAPI GdipSaveImageToStream(GpImage *img, IStream *stream, const CLSID *encoder,
                            const GpEncoderParameters *params);

#undef GPAPI

static inline ARGB gp_argb(BYTE a, DWORD rgb) { return ((ARGB)a << 24) | (rgb & 0xFFFFFF); }

#endif

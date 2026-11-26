/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Edit objects (blur, mosaic, box, frame, arrow, pen, highlighter, text).
 * Every object stays editable: it can be selected, moved and resized later.
 */
#include "dofram.h"
#include "gdip.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t *const TOOL_KEYS[TOOL_COUNT] = {
    L"select", L"blur", L"pixelate", L"box", L"frame", L"arrow", L"pen", L"highlight", L"text"
};

const wchar_t *tool_key(Tool t) { return (t >= 0 && t < TOOL_COUNT) ? TOOL_KEYS[t] : L"blur"; }

Tool tool_from_key(const wchar_t *s, Tool fallback)
{
    for (int i = 0; i < TOOL_COUNT; i++)
        if (_wcsicmp(s, TOOL_KEYS[i]) == 0) return (Tool)i;
    return fallback;
}

/* Size levels 1..10 map to concrete values per tool. */
float op_line_width(int level) { return 1.5f + level * 1.2f; }
int op_blur_radius(int level) { return 2 + level * 3; }
int op_pixel_block(int level) { return 4 + level * 3; }
float op_font_px(int level) { return 12.0f + level * 4.0f; }
static float arrow_head(int level) { return op_line_width(level) * 3.2f + 8.0f; }

/* ------------------------------------------------------------------ */
/* Geometry                                                              */
/* ------------------------------------------------------------------ */

/* Handle order: 0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left. */
void geo_handles(Rc r, POINT out[8])
{
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    out[0] = pt(r.x, r.y);
    out[1] = pt(cx, r.y);
    out[2] = pt(rc_right(r), r.y);
    out[3] = pt(rc_right(r), cy);
    out[4] = pt(rc_right(r), rc_bottom(r));
    out[5] = pt(cx, rc_bottom(r));
    out[6] = pt(r.x, rc_bottom(r));
    out[7] = pt(r.x, cy);
}

Rc geo_resize(Rc o, int h, int dx, int dy)
{
    int l = o.x, t = o.y, r = rc_right(o), b = rc_bottom(o);
    if (h == 0 || h == 6 || h == 7) l += dx;
    if (h == 2 || h == 3 || h == 4) r += dx;
    if (h == 0 || h == 1 || h == 2) t += dy;
    if (h == 4 || h == 5 || h == 6) b += dy;
    int x1 = imin(l, r), x2 = imax(l, r), y1 = imin(t, b), y2 = imax(t, b);
    if (x2 == x1) x2 = x1 + 1;
    if (y2 == y1) y2 = y1 + 1;
    return rc_ltrb(x1, y1, x2, y2);
}

/* Rectangle spanned by two points, both points included. */
Rc geo_norm(POINT a, POINT b)
{
    int x1 = imin(a.x, b.x), y1 = imin(a.y, b.y);
    int x2 = imax(a.x, b.x), y2 = imax(a.y, b.y);
    return rc_make(x1, y1, x2 - x1 + 1, y2 - y1 + 1);
}

static double dist_seg(POINT p, POINT a, POINT b)
{
    double dx = b.x - a.x, dy = b.y - a.y;
    double len2 = dx * dx + dy * dy;
    double t = len2 == 0 ? 0 : ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    double px = a.x + t * dx - p.x, py = a.y + t * dy - p.y;
    return sqrt(px * px + py * py);
}

/* ------------------------------------------------------------------ */
/* Lifetime                                                              */
/* ------------------------------------------------------------------ */

Op *op_new(Tool kind, uint32_t color, int size)
{
    Op *o = (Op *)calloc(1, sizeof(Op));
    if (!o) return NULL;
    o->kind = kind;
    o->color = color & 0xFFFFFF;
    o->size = iclamp(size, 1, 10);
    return o;
}

Op *op_clone(const Op *src)
{
    Op *o = (Op *)malloc(sizeof(Op));
    if (!o) return NULL;
    *o = *src;
    o->pts = NULL;
    o->text = NULL;
    if (src->npts > 0) {
        o->pts = (POINT *)malloc(sizeof(POINT) * (size_t)src->npts);
        if (o->pts) memcpy(o->pts, src->pts, sizeof(POINT) * (size_t)src->npts);
        o->cap = o->pts ? src->npts : 0;
        o->npts = o->cap;
    }
    if (src->text) o->text = util_strdup(src->text);
    return o;
}

void op_free(Op *o)
{
    if (!o) return;
    free(o->pts);
    free(o->text);
    free(o);
}

bool op_is_region(const Op *o)
{
    return o->kind == TOOL_BLUR || o->kind == TOOL_PIXELATE || o->kind == TOOL_BOX ||
           o->kind == TOOL_FRAME || o->kind == TOOL_HIGHLIGHT;
}

void op_pen_add(Op *o, POINT p)
{
    if (o->npts == o->cap) {
        int nc = o->cap ? o->cap * 2 : 64;
        POINT *np = (POINT *)realloc(o->pts, sizeof(POINT) * (size_t)nc);
        if (!np) return;
        o->pts = np;
        o->cap = nc;
    }
    o->pts[o->npts++] = p;
}

/* ------------------------------------------------------------------ */
/* Text                                                                  */
/* ------------------------------------------------------------------ */

static GpPath *text_path(const Op *o, float x, float y)
{
    GpPath *path = NULL;
    GpFontFamily *ff = NULL;
    GpStringFormat *fmt = NULL;
    if (!o->text || !o->text[0]) return NULL;
    if (GdipCreatePath(GP_FILL_WINDING, &path) != GP_OK) return NULL;
    if (GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &ff) != GP_OK)
        GdipGetGenericFontFamilySansSerif(&ff);
    GdipStringFormatGetGenericTypographic(&fmt); /* shared object, not freed */
    GpRectF layout = { x, y, 0, 0 };
    GdipAddPathString(path, o->text, -1, ff, GP_FONT_BOLD, op_font_px(o->size), &layout, fmt);
    if (ff) GdipDeleteFontFamily(ff);
    return path;
}

static void text_measure(Op *o)
{
    o->text_ext = rc_make(0, 0, 0, 0);
    GpPath *path = text_path(o, 0, 0);
    if (!path) return;
    GpRectF b = { 0, 0, 0, 0 };
    if (GdipGetPathWorldBounds(path, &b, NULL, NULL) == GP_OK && b.Width > 0) {
        int x = (int)floorf(b.X), y = (int)floorf(b.Y);
        o->text_ext = rc_ltrb(x, y, (int)ceilf(b.X + b.Width), (int)ceilf(b.Y + b.Height));
    }
    GdipDeletePath(path);
}

void op_set_text(Op *o, const wchar_t *text)
{
    free(o->text);
    o->text = util_strdup(text ? text : L"");
    text_measure(o);
}

void op_set_size(Op *o, int size)
{
    o->size = iclamp(size, 1, 10);
    if (o->kind == TOOL_TEXT) text_measure(o);
}

/* ------------------------------------------------------------------ */
/* Queries                                                               */
/* ------------------------------------------------------------------ */

static Rc text_bounds(const Op *o)
{
    Rc r = o->text_ext;
    if (r.w <= 0) r = rc_make(0, 0, 1, 1);
    r = rc_offset(r, o->p.x, o->p.y);
    int pad = (int)op_font_px(o->size) / 4 + 4;
    return rc_inflate(r, pad, pad);
}

Rc op_bounds(const Op *o)
{
    switch (o->kind) {
    case TOOL_FRAME: {
        int w = (int)ceilf(op_line_width(o->size)) + 2;
        return rc_inflate(o->r, w, w);
    }
    case TOOL_ARROW: {
        int pad = (int)ceilf(arrow_head(o->size)) + 4;
        Rc r = rc_ltrb(imin(o->a.x, o->b.x), imin(o->a.y, o->b.y), imax(o->a.x, o->b.x), imax(o->a.y, o->b.y));
        return rc_inflate(r, pad, pad);
    }
    case TOOL_PEN: {
        if (o->npts == 0) return rc_make(0, 0, 0, 0);
        int x1 = INT_MAX, y1 = INT_MAX, x2 = INT_MIN, y2 = INT_MIN;
        for (int i = 0; i < o->npts; i++) {
            POINT p = o->pts[i];
            if (p.x < x1) x1 = p.x;
            if (p.y < y1) y1 = p.y;
            if (p.x > x2) x2 = p.x;
            if (p.y > y2) y2 = p.y;
        }
        int pad = (int)ceilf(op_line_width(o->size)) + 2;
        return rc_inflate(rc_ltrb(x1, y1, x2, y2), pad, pad);
    }
    case TOOL_TEXT:
        return text_bounds(o);
    default:
        return o->r;
    }
}

/* The rectangle the selection outline is drawn around. */
Rc op_frame(const Op *o) { return op_is_region(o) ? o->r : op_bounds(o); }

bool op_meaningful(const Op *o)
{
    switch (o->kind) {
    case TOOL_ARROW: return abs(o->a.x - o->b.x) + abs(o->a.y - o->b.y) >= 6;
    case TOOL_PEN: return o->npts >= 2;
    case TOOL_TEXT: {
        if (!o->text) return false;
        for (const wchar_t *s = o->text; *s; s++)
            if (!iswspace(*s)) return true;
        return false;
    }
    default: return o->r.w >= 2 && o->r.h >= 2;
    }
}

bool op_hit(const Op *o, POINT p, int tol)
{
    switch (o->kind) {
    case TOOL_ARROW:
        return dist_seg(p, o->a, o->b) <= op_line_width(o->size) / 2 + tol ||
               dist_seg(p, o->b, o->b) <= arrow_head(o->size) * 0.6 + tol;
    case TOOL_PEN: {
        double lim = op_line_width(o->size) / 2 + tol;
        for (int i = 1; i < o->npts; i++)
            if (dist_seg(p, o->pts[i - 1], o->pts[i]) <= lim) return true;
        return false;
    }
    case TOOL_TEXT:
        return rc_has(rc_inflate(text_bounds(o), tol, tol), p);
    default:
        return rc_has(rc_inflate(o->r, tol, tol), p);
    }
}

void op_move(Op *o, int dx, int dy)
{
    switch (o->kind) {
    case TOOL_ARROW:
        o->a.x += dx; o->a.y += dy;
        o->b.x += dx; o->b.y += dy;
        break;
    case TOOL_PEN:
        for (int i = 0; i < o->npts; i++) { o->pts[i].x += dx; o->pts[i].y += dy; }
        break;
    case TOOL_TEXT:
        o->p.x += dx; o->p.y += dy;
        break;
    default:
        o->r = rc_offset(o->r, dx, dy);
        break;
    }
}

int op_handles(const Op *o, POINT out[8])
{
    if (op_is_region(o)) { geo_handles(o->r, out); return 8; }
    if (o->kind == TOOL_ARROW) { out[0] = o->a; out[1] = o->b; return 2; }
    return 0;
}

void op_begin_handle_drag(Op *o)
{
    o->r_orig = o->r;
    o->a_orig = o->a;
    o->b_orig = o->b;
}

void op_drag_handle(Op *o, int index, int dx, int dy)
{
    if (op_is_region(o)) {
        o->r = geo_resize(o->r_orig, index, dx, dy);
    } else if (o->kind == TOOL_ARROW) {
        if (index == 0) o->a = pt(o->a_orig.x + dx, o->a_orig.y + dy);
        else o->b = pt(o->b_orig.x + dx, o->b_orig.y + dy);
    }
}

/* ------------------------------------------------------------------ */
/* Rendering                                                             */
/* ------------------------------------------------------------------ */

static void apply_arrow(const Op *o, GpGraphics *g)
{
    float w = op_line_width(o->size);
    float dx = (float)(o->b.x - o->a.x), dy = (float)(o->b.y - o->a.y);
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    float ux = dx / len, uy = dy / len;
    float head = arrow_head(o->size);
    if (head > len * 0.8f) head = len * 0.8f;
    float half = head * 0.55f;
    float bx = o->b.x - ux * head, by = o->b.y - uy * head;
    GpPointF tri[3] = {
        { (REAL)o->b.x, (REAL)o->b.y },
        { bx - uy * half, by + ux * half },
        { bx + uy * half, by - ux * half },
    };
    float ex = o->b.x - ux * head * 0.7f, ey = o->b.y - uy * head * 0.7f;
    GpPen *pen = NULL;
    GpBrush *br = NULL;
    GdipCreatePen1(gp_argb(255, o->color), w, GP_UNIT_WORLD, &pen);
    GdipCreateSolidFill(gp_argb(255, o->color), &br);
    if (pen && br) {
        GdipSetPenStartCap(pen, GP_CAP_ROUND);
        GdipSetPenEndCap(pen, GP_CAP_FLAT);
        GdipDrawLine(g, pen, (REAL)o->a.x, (REAL)o->a.y, ex, ey);
        GdipFillPolygon(g, br, tri, 3, GP_FILL_ALTERNATE);
    }
    if (pen) GdipDeletePen(pen);
    if (br) GdipDeleteBrush(br);
}

static void apply_pen(const Op *o, GpGraphics *g)
{
    if (o->npts < 2) return;
    GpPointF *pts = (GpPointF *)malloc(sizeof(GpPointF) * (size_t)o->npts);
    if (!pts) return;
    for (int i = 0; i < o->npts; i++) { pts[i].X = (REAL)o->pts[i].x; pts[i].Y = (REAL)o->pts[i].y; }
    GpPen *pen = NULL;
    if (GdipCreatePen1(gp_argb(255, o->color), op_line_width(o->size), GP_UNIT_WORLD, &pen) == GP_OK) {
        GdipSetPenStartCap(pen, GP_CAP_ROUND);
        GdipSetPenEndCap(pen, GP_CAP_ROUND);
        GdipSetPenLineJoin(pen, GP_JOIN_ROUND);
        GdipDrawLines(g, pen, pts, o->npts);
        GdipDeletePen(pen);
    }
    free(pts);
}

static bool is_dark(uint32_t rgb)
{
    int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    int mx = imax(r, imax(g, b)), mn = imin(r, imin(g, b));
    return (mx + mn) < 255; /* HSL lightness below 0.5 */
}

static void apply_text(const Op *o, GpGraphics *g)
{
    GpPath *path = text_path(o, (float)o->p.x, (float)o->p.y);
    if (!path) return;
    /* Outline in the opposite brightness keeps the text readable on any background. */
    float ow = op_font_px(o->size) / 7.0f;
    if (ow < 2.0f) ow = 2.0f;
    GpPen *pen = NULL;
    GpBrush *br = NULL;
    if (GdipCreatePen1(is_dark(o->color) ? 0xFFFFFFFF : 0xFF000000, ow, GP_UNIT_WORLD, &pen) == GP_OK) {
        GdipSetPenLineJoin(pen, GP_JOIN_ROUND);
        GdipDrawPath(g, pen, path);
        GdipDeletePen(pen);
    }
    if (GdipCreateSolidFill(gp_argb(255, o->color), &br) == GP_OK) {
        GdipFillPath(g, br, path);
        GdipDeleteBrush(br);
    }
    GdipDeletePath(path);
}

void op_apply(const Op *o, Img *img, Rc clip)
{
    Rc r = rc_intersect(o->r, clip);
    switch (o->kind) {
    case TOOL_BLUR: fx_blur(img, r, op_blur_radius(o->size)); return;
    case TOOL_PIXELATE: fx_pixelate(img, r, op_pixel_block(o->size)); return;
    case TOOL_HIGHLIGHT: fx_multiply(img, r, o->color); return;
    case TOOL_BOX: fx_fill(img, r, o->color); return;
    default: break;
    }
    GpGraphics *g = (GpGraphics *)img_graphics(img, clip, true);
    if (!g) return;
    if (o->kind == TOOL_FRAME) {
        GpPen *pen = NULL;
        if (GdipCreatePen1(gp_argb(255, o->color), op_line_width(o->size), GP_UNIT_WORLD, &pen) == GP_OK) {
            GdipSetPenLineJoin(pen, GP_JOIN_MITER);
            GdipDrawRectangle(g, pen, (REAL)o->r.x, (REAL)o->r.y, (REAL)o->r.w, (REAL)o->r.h);
            GdipDeletePen(pen);
        }
    } else if (o->kind == TOOL_ARROW) {
        apply_arrow(o, g);
    } else if (o->kind == TOOL_PEN) {
        apply_pen(o, g);
    } else if (o->kind == TOOL_TEXT) {
        apply_text(o, g);
    }
    GdipDeleteGraphics(g);
}

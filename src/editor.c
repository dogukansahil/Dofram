/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * The editor. One implementation serves two hosts:
 *  - the overlay: a frozen copy of every monitor where the area is selected,
 *    resized from its handles and edited in place;
 *  - the window: a normal resizable window with zoom, for an already cropped image.
 * Edits are a list of objects in image coordinates; the output is cropped last.
 */
#include "dofram.h"
#include "gdip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <windowsx.h>
#include <commctrl.h>

#define OVERLAY_CLASS L"DoframOverlay"
#define WINDOW_CLASS  L"DoframEditor"

enum { MODE_SELECTING, MODE_EDITING };
enum { DRAG_NONE, DRAG_NEW_CROP, DRAG_MOVE_CROP, DRAG_RESIZE_CROP, DRAG_DRAW, DRAG_MOVE_OBJ, DRAG_RESIZE_OBJ, DRAG_PAN };

enum {
    BTN_TOOL = 100,            /* BTN_TOOL + Tool */
    BTN_PAL = 200,             /* BTN_PAL + palette index */
    BTN_COLOR = 300, BTN_MINUS, BTN_SIZE, BTN_PLUS, BTN_UNDO, BTN_REDO, BTN_DEL,
    BTN_WINDOW, BTN_SAVEAS, BTN_SAVE, BTN_COPY, BTN_CLOSE, BTN_PALMORE
};

#define ACCENT_RGB 0x0096FF
#define MAX_BTNS   40
#define MAX_UNDO   200
#define PAL_COUNT  8

static const uint32_t PALETTE[PAL_COUNT] = {
    0x000000, 0xFFFFFF, 0xE61E28, 0xFF8C00, 0xFFEB3C, 0x28B446, 0x1E78FF, 0x9646DC
};

static const wchar_t *const TOOL_LABELS[TOOL_COUNT] = {
    L"Select", L"Blur", L"Mosaic", L"Box", L"Frame", L"Arrow", L"Pen", L"Marker", L"Text"
};

static const wchar_t *const TOOL_TIPS[TOOL_COUNT] = {
    L"Select / move (V): drag an object or the area, resize from the handles",
    L"Blur (1)", L"Mosaic (2)", L"Redaction box (3): the safest way to hide information", L"Frame (4)",
    L"Arrow (5)", L"Pen (6)", L"Highlighter (7)", L"Text (8): click and type, double-click to edit"
};

typedef struct {
    int id;
    const wchar_t *label;
    const wchar_t *tip;
    Rc r;
    bool swatch;
    uint32_t swatch_rgb;
    int group;
    int row;
} Btn;

typedef struct {
    Op **objs;
    int n;
    Rc crop;
} State;

typedef struct Editor {
    HWND hwnd;
    bool windowed;        /* false: overlay over the frozen screen */
    bool quick;           /* overlay only selects, no editing */
    Img *shot, *dim, *rendered, *back;
    Rc virt, full;
    int dpi;

    /* view: screen = image * zoom + origin (identity for the overlay) */
    double zoom;
    double ox, oy;
    int top;              /* window mode: height of the toolbar area */

    int mode, drag, handle;
    Rc crop, crop_orig;
    POINT down, last, cur;     /* image coordinates */
    POINT pan_from;            /* client coordinates */
    double pan_ox, pan_oy;
    bool has_mouse;

    Op **objs;
    int nobjs, capobjs;
    Op *sel, *drawing;
    State *undo[MAX_UNDO], *redo[MAX_UNDO];
    int nundo, nredo;
    State *before;
    bool dirty;

    Tool tool;
    int size;
    uint32_t colors[TOOL_COUNT];

    Btn btns[MAX_BTNS];
    int nbtns;
    int hover;               /* button id or 0 */
    Rc bar;
    bool palette_open;
    Rc last_decor;

    HWND edit;
    Op *text_target;
    int text_index;
    bool committing;
    uint32_t text_color;
    int text_size;
    State *text_before;
    HFONT edit_font;
    HBRUSH edit_brush;
    POINT text_anchor;       /* text origin in image coordinates */
    bool created;

    HFONT font, font_bold;
    HCURSOR cursor;
    wchar_t size_label[32];
} Editor;

static int S(const Editor *e, int px) { return ui_scale_dpi(px, e->dpi); }
static void place_edit(Editor *e);

/* ------------------------------------------------------------------ */
/* Coordinates                                                           */
/* ------------------------------------------------------------------ */

static POINT to_img(const Editor *e, int x, int y)
{
    if (!e->windowed) return pt(x, y);
    return pt((int)floor((x - e->ox) / e->zoom), (int)floor((y - e->oy) / e->zoom));
}

static POINT to_scr(const Editor *e, POINT p)
{
    if (!e->windowed) return p;
    return pt((int)floor(p.x * e->zoom + e->ox), (int)floor(p.y * e->zoom + e->oy));
}

static Rc rect_to_scr(const Editor *e, Rc r)
{
    if (!e->windowed) return r;
    POINT a = to_scr(e, pt(r.x, r.y)), b = to_scr(e, pt(rc_right(r), rc_bottom(r)));
    return rc_ltrb(a.x, a.y, b.x, b.y);
}

/* Screen-space tolerance in image pixels. */
static int img_tol(const Editor *e, int screen_px)
{
    if (!e->windowed) return screen_px;
    int t = (int)ceil(screen_px / e->zoom);
    return t < 1 ? 1 : t;
}

static Rc client_rect(const Editor *e)
{
    RECT r = { 0, 0, 0, 0 };
    if (e->hwnd) GetClientRect(e->hwnd, &r);
    return rc_from_rect(r);
}

/* Monitor that contains p (client coordinates of the overlay). */
static Rc monitor_of(const Editor *e, POINT p)
{
    if (e->windowed) return client_rect(e);
    POINT sp = pt(p.x + e->virt.x, p.y + e->virt.y);
    HMONITOR m = MonitorFromPoint(sp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    return rc_offset(rc_from_rect(mi.rcMonitor), -e->virt.x, -e->virt.y);
}

/* ------------------------------------------------------------------ */
/* Object list and rendering                                             */
/* ------------------------------------------------------------------ */

static void objs_insert(Editor *e, int index, Op *o)
{
    if (e->nobjs == e->capobjs) {
        int nc = e->capobjs ? e->capobjs * 2 : 32;
        Op **n = (Op **)realloc(e->objs, sizeof(Op *) * (size_t)nc);
        if (!n) return;
        e->objs = n;
        e->capobjs = nc;
    }
    if (index < 0 || index > e->nobjs) index = e->nobjs;
    memmove(e->objs + index + 1, e->objs + index, sizeof(Op *) * (size_t)(e->nobjs - index));
    e->objs[index] = o;
    e->nobjs++;
}

static int objs_index(const Editor *e, const Op *o)
{
    for (int i = 0; i < e->nobjs; i++)
        if (e->objs[i] == o) return i;
    return -1;
}

static void objs_remove(Editor *e, const Op *o)
{
    int i = objs_index(e, o);
    if (i < 0) return;
    memmove(e->objs + i, e->objs + i + 1, sizeof(Op *) * (size_t)(e->nobjs - i - 1));
    e->nobjs--;
}

static void objs_clear(Editor *e)
{
    for (int i = 0; i < e->nobjs; i++) op_free(e->objs[i]);
    e->nobjs = 0;
}

/* Rebuilds the dirty part of the rendered image. An object that touches the
 * region is included completely (blur reads its whole area), so the result is
 * always identical to drawing everything from scratch. */
static void render_region(Editor *e, Rc dirty)
{
    if (e->mode != MODE_EDITING) return;
    Rc R = rc_intersect(dirty, e->crop);
    if (rc_empty(R)) return;
    int total = e->nobjs + (e->drawing ? 1 : 0);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < total; i++) {
            Op *o = i < e->nobjs ? e->objs[i] : e->drawing;
            Rc eb = rc_intersect(op_bounds(o), e->crop);
            if (rc_empty(eb)) continue;
            if (rc_overlaps(eb, R) && !rc_contains(R, eb)) { R = rc_union(R, eb); changed = true; }
        }
    }
    img_copy_rect(e->rendered, e->shot, R);
    for (int i = 0; i < total; i++) {
        Op *o = i < e->nobjs ? e->objs[i] : e->drawing;
        Rc eb = rc_intersect(op_bounds(o), e->crop);
        if (!rc_empty(eb) && rc_overlaps(eb, R)) op_apply(o, e->rendered, e->crop);
    }
}

static void render_all(Editor *e) { render_region(e, e->crop); }

static void invalidate_img(Editor *e, Rc r)
{
    if (rc_empty(r)) return;
    Rc s = rc_inflate(rect_to_scr(e, r), 3, 3);
    RECT rr = rc_to_rect(s);
    InvalidateRect(e->hwnd, &rr, FALSE);
}

static void invalidate_scr(Editor *e, Rc r)
{
    if (rc_empty(r)) return;
    RECT rr = rc_to_rect(r);
    InvalidateRect(e->hwnd, &rr, FALSE);
}

/* ------------------------------------------------------------------ */
/* Undo                                                                  */
/* ------------------------------------------------------------------ */

static State *snapshot(Editor *e)
{
    State *s = (State *)calloc(1, sizeof(State));
    if (!s) return NULL;
    s->crop = e->crop;
    if (e->nobjs) {
        s->objs = (Op **)malloc(sizeof(Op *) * (size_t)e->nobjs);
        if (s->objs)
            for (int i = 0; i < e->nobjs; i++) s->objs[s->n++] = op_clone(e->objs[i]);
    }
    return s;
}

static void state_free(State *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) op_free(s->objs[i]);
    free(s->objs);
    free(s);
}

static void stack_push(State **stack, int *n, State *s)
{
    if (*n == MAX_UNDO) {
        state_free(stack[0]);
        memmove(stack, stack + 1, sizeof(State *) * (MAX_UNDO - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

static void clear_stack(State **stack, int *n)
{
    for (int i = 0; i < *n; i++) state_free(stack[i]);
    *n = 0;
}

static void push_undo(Editor *e, State *s)
{
    if (!s) return;
    stack_push(e->undo, &e->nundo, s);
    clear_stack(e->redo, &e->nredo);
    e->dirty = true;
}

static bool same_state(const State *a, const State *b)
{
    if (!rc_equal(a->crop, b->crop) || a->n != b->n) return false;
    for (int i = 0; i < a->n; i++) {
        const Op *x = a->objs[i], *y = b->objs[i];
        if (!rc_equal(op_bounds(x), op_bounds(y)) || x->color != y->color || x->size != y->size) return false;
        if (x->kind == TOOL_TEXT && wcscmp(x->text ? x->text : L"", y->text ? y->text : L"") != 0) return false;
    }
    return true;
}

static void layout_bar(Editor *e);
static void refresh_decor(Editor *e, bool now);

static void restore(Editor *e, const State *s)
{
    objs_clear(e);
    for (int i = 0; i < s->n; i++) objs_insert(e, -1, op_clone(s->objs[i]));
    e->crop = s->crop;
    e->sel = NULL;
    op_free(e->drawing);
    e->drawing = NULL;
    render_all(e);
    layout_bar(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
}

static void undo(Editor *e)
{
    if (!e->nundo || e->drag != DRAG_NONE) return;
    State *now = snapshot(e);
    if (!now) return;
    stack_push(e->redo, &e->nredo, now);
    State *s = e->undo[--e->nundo];
    restore(e, s);
    state_free(s);
    e->dirty = true;
}

static void redo(Editor *e)
{
    if (!e->nredo || e->drag != DRAG_NONE) return;
    State *now = snapshot(e);
    if (!now) return;
    stack_push(e->undo, &e->nundo, now);
    State *s = e->redo[--e->nredo];
    restore(e, s);
    state_free(s);
    e->dirty = true;
}

/* ------------------------------------------------------------------ */
/* Toolbar                                                               */
/* ------------------------------------------------------------------ */

static Tool color_target(const Editor *e) { return e->sel ? e->sel->kind : e->tool; }
static bool uses_color(Tool t) { return t != TOOL_BLUR && t != TOOL_PIXELATE && t != TOOL_SELECT; }

static int text_width(Editor *e, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(e->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(e->hwnd, dc);
    return sz.cx;
}

static Btn *add_btn(Editor *e, int id, const wchar_t *label, const wchar_t *tip, int group, int row)
{
    if (e->nbtns >= MAX_BTNS) return &e->btns[MAX_BTNS - 1];
    Btn *b = &e->btns[e->nbtns++];
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->label = label;
    b->tip = tip;
    b->group = group;
    b->row = row;
    int w = label ? text_width(e, (id == BTN_SAVE || id == BTN_COPY) ? e->font_bold : e->font, label) + S(e, 16) : S(e, 30);
    b->r = rc_make(0, 0, w, S(e, 30));
    return b;
}

/* Width of a row of buttons including the gaps between groups. */
static int row_width(Editor *e, int row, int gap, int group_gap)
{
    int w = 0, prev_group = -1;
    for (int i = 0; i < e->nbtns; i++) {
        Btn *b = &e->btns[i];
        if (b->row != row) continue;
        if (prev_group >= 0) w += b->group != prev_group ? group_gap : gap;
        w += b->r.w;
        prev_group = b->group;
    }
    return w;
}

static void layout_bar(Editor *e)
{
    e->nbtns = 0;
    e->bar = rc_make(0, 0, 0, 0);
    if (e->mode != MODE_EDITING) return;

    int h = S(e, 30), gap = S(e, 2), group_gap = S(e, 10), pad = S(e, 6), row_gap = S(e, 4);

    for (int i = 0; i < TOOL_COUNT; i++) add_btn(e, BTN_TOOL + i, TOOL_LABELS[i], TOOL_TIPS[i], 0, 0);

    Tool ct = color_target(e);
    Btn *c = add_btn(e, BTN_COLOR, NULL, L"Colour (C)", 1, 0);
    c->swatch = true;
    c->swatch_rgb = uses_color(ct) ? e->colors[ct] : 0x808080;
    add_btn(e, BTN_MINUS, L"\x2212", L"Smaller ([ or mouse wheel)", 1, 0);
    swprintf(e->size_label, 32, L"Size %d", e->size);
    add_btn(e, BTN_SIZE, e->size_label, L"Line width, blur strength, mosaic size and text size", 1, 0);
    add_btn(e, BTN_PLUS, L"+", L"Bigger (] or mouse wheel)", 1, 0);

    add_btn(e, BTN_UNDO, L"Undo", L"Undo (Ctrl+Z)", 2, 0);
    add_btn(e, BTN_REDO, L"Redo", L"Redo (Ctrl+Y)", 2, 0);
    add_btn(e, BTN_DEL, L"Delete", L"Delete the selected object (Delete)", 2, 0);

    if (e->windowed) {
        add_btn(e, BTN_SAVEAS, L"Save as\x2026", L"Save as (Ctrl+Shift+S): PNG, JPG or BMP", 3, 0);
        add_btn(e, BTN_SAVE, L"Save", L"Save to the screenshot folder (Ctrl+S)", 3, 0);
        add_btn(e, BTN_COPY, L"Copy", L"Copy to the clipboard (Ctrl+C)", 3, 0);
    } else {
        add_btn(e, BTN_WINDOW, L"Window", L"Open in a separate editor window", 3, 0);
        add_btn(e, BTN_SAVE, L"Save", L"Save to the screenshot folder (Ctrl+S)", 3, 0);
        add_btn(e, BTN_COPY, L"Copy", L"Copy to the clipboard (Ctrl+C or Enter)", 3, 0);
        add_btn(e, BTN_CLOSE, L"\x00D7", L"Close (Esc)", 3, 0);
    }

    Rc mon = e->windowed ? client_rect(e)
                         : monitor_of(e, pt(e->crop.x + e->crop.w / 2, e->crop.y + e->crop.h / 2));
    int rows = 1;
    if (row_width(e, 0, gap, group_gap) + pad * 2 > mon.w - S(e, 16)) {
        for (int i = 0; i < e->nbtns; i++)
            if (e->btns[i].group >= 2) e->btns[i].row = 1;
        rows = 2;
    }
    int base_rows = rows;
    if (e->palette_open) {
        for (int i = 0; i < PAL_COUNT; i++) {
            Btn *p = add_btn(e, BTN_PAL + i, NULL, L"Pick this colour", 4, rows);
            p->swatch = true;
            p->swatch_rgb = PALETTE[i];
        }
        add_btn(e, BTN_PALMORE, L"More\x2026", L"Custom colour", 4, rows);
        rows++;
    }

    int W = 0;
    for (int r = 0; r < rows; r++) W = imax(W, row_width(e, r, gap, group_gap));
    W += pad * 2;
    int H = rows * h + (rows - 1) * row_gap + pad * 2;
    int x, y;
    if (e->windowed) {
        x = S(e, 8);
        y = S(e, 6);
        e->top = base_rows * h + (base_rows - 1) * row_gap + pad * 2 + S(e, 12);
    } else {
        x = rc_right(e->crop) - W;
        x = imax(mon.x + S(e, 8), imin(rc_right(mon) - W - S(e, 8), x));
        y = rc_bottom(e->crop) + S(e, 10);
        if (y + H > rc_bottom(mon) - S(e, 8)) y = e->crop.y - H - S(e, 10);
        if (y < mon.y + S(e, 8)) y = imin(rc_bottom(e->crop), rc_bottom(mon)) - H - S(e, 10);
    }
    e->bar = rc_make(x, y, W, H);

    for (int r = 0; r < rows; r++) {
        int xx = x + pad, prev_group = -1;
        int yy = y + pad + r * (h + row_gap);
        for (int i = 0; i < e->nbtns; i++) {
            Btn *b = &e->btns[i];
            if (b->row != r) continue;
            if (prev_group >= 0) xx += b->group != prev_group ? group_gap : gap;
            b->r = rc_make(xx, yy, b->r.w, h);
            xx += b->r.w;
            prev_group = b->group;
        }
    }
}

static Btn *btn_by_id(Editor *e, int id)
{
    for (int i = 0; i < e->nbtns; i++)
        if (e->btns[i].id == id) return &e->btns[i];
    return NULL;
}

static Btn *btn_at(Editor *e, POINT client)
{
    for (int i = 0; i < e->nbtns; i++)
        if (rc_has(e->btns[i].r, client)) return &e->btns[i];
    return NULL;
}

static bool btn_enabled(const Editor *e, const Btn *b)
{
    switch (b->id) {
    case BTN_UNDO: return e->nundo > 0;
    case BTN_REDO: return e->nredo > 0;
    case BTN_DEL: return e->sel != NULL;
    case BTN_COLOR: return uses_color(color_target(e));
    case BTN_SIZE: return false;
    default: return true;
    }
}

static bool btn_active(const Editor *e, const Btn *b)
{
    if (b->id >= BTN_TOOL && b->id < BTN_TOOL + TOOL_COUNT) return b->id - BTN_TOOL == (int)e->tool;
    if (b->id == BTN_COLOR) return e->palette_open;
    return false;
}

/* ------------------------------------------------------------------ */
/* Painting                                                              */
/* ------------------------------------------------------------------ */

static void fill_round(GpGraphics *g, Rc r, int rad, ARGB color)
{
    GpPath *p = NULL;
    GpBrush *br = NULL;
    if (GdipCreatePath(GP_FILL_ALTERNATE, &p) != GP_OK) return;
    float d = (float)imax(1, rad * 2);
    GdipAddPathArc(p, (REAL)r.x, (REAL)r.y, d, d, 180, 90);
    GdipAddPathArc(p, (REAL)(rc_right(r) - d), (REAL)r.y, d, d, 270, 90);
    GdipAddPathArc(p, (REAL)(rc_right(r) - d), (REAL)(rc_bottom(r) - d), d, d, 0, 90);
    GdipAddPathArc(p, (REAL)r.x, (REAL)(rc_bottom(r) - d), d, d, 90, 90);
    GdipClosePathFigure(p);
    if (GdipCreateSolidFill(color, &br) == GP_OK) {
        GdipFillPath(g, br, p);
        GdipDeleteBrush(br);
    }
    GdipDeletePath(p);
}

static void fill_rect(GpGraphics *g, Rc r, ARGB color)
{
    GpBrush *br = NULL;
    if (GdipCreateSolidFill(color, &br) != GP_OK) return;
    GdipFillRectangle(g, br, (REAL)r.x, (REAL)r.y, (REAL)r.w, (REAL)r.h);
    GdipDeleteBrush(br);
}

/* 1-pixel outline whose pixels are exactly the border of r. */
static void stroke_rect(GpGraphics *g, Rc r, ARGB color, bool dashed)
{
    GpPen *pen = NULL;
    if (GdipCreatePen1(color, 1, GP_UNIT_WORLD, &pen) != GP_OK) return;
    if (dashed) GdipSetPenDashStyle(pen, GP_DASH_DASH);
    GdipDrawRectangle(g, pen, r.x + 0.5f, r.y + 0.5f, (REAL)(r.w - 1), (REAL)(r.h - 1));
    GdipDeletePen(pen);
}

static void draw_line(GpGraphics *g, float x1, float y1, float x2, float y2, ARGB color)
{
    GpPen *pen = NULL;
    if (GdipCreatePen1(color, 1, GP_UNIT_WORLD, &pen) != GP_OK) return;
    GdipDrawLine(g, pen, x1, y1, x2, y2);
    GdipDeletePen(pen);
}

/* GDI text on the back buffer. GDI+ must be flushed before. */
static void gdi_text(Editor *e, GpGraphics *g, const wchar_t *s, Rc r, HFONT f, COLORREF c, UINT flags)
{
    GdipFlush(g, 1);
    HDC dc = e->back->dc;
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    RECT rr = rc_to_rect(r);
    DrawTextW(dc, s, -1, &rr, flags | DT_NOPREFIX);
    SelectObject(dc, old);
    GdiFlush();
}

static SIZE measure(Editor *e, const wchar_t *s, HFONT f)
{
    RECT r = { 0, 0, 0, 0 };
    HDC dc = GetDC(e->hwnd);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(e->hwnd, dc);
    SIZE sz = { r.right - r.left, r.bottom - r.top };
    return sz;
}

static void draw_handles(Editor *e, GpGraphics *g, const POINT *hs, int n, ARGB border)
{
    int s = S(e, 4);
    for (int i = 0; i < n; i++) {
        POINT p = to_scr(e, hs[i]);
        Rc r = rc_make(p.x - s, p.y - s, s * 2 + 1, s * 2 + 1);
        fill_rect(g, r, 0xFFFFFFFF);
        stroke_rect(g, r, border, false);
    }
}

static Rc tip_rect(Editor *e)
{
    Btn *b = e->hover ? btn_by_id(e, e->hover) : NULL;
    if (!b || !b->tip || rc_empty(e->bar)) return rc_make(0, 0, 0, 0);
    SIZE sz = measure(e, b->tip, e->font);
    int w = sz.cx + S(e, 14), h = sz.cy + S(e, 8);
    int x = b->r.x + b->r.w / 2 - w / 2;
    int y = rc_bottom(e->bar) + S(e, 4);
    Rc mon = monitor_of(e, pt(e->bar.x, e->bar.y));
    if (y + h > rc_bottom(mon)) y = e->bar.y - h - S(e, 4);
    x = imax(mon.x + 2, imin(rc_right(mon) - w - 2, x));
    return rc_make(x, y, w, h);
}

static void draw_bar(Editor *e, GpGraphics *g)
{
    if (rc_empty(e->bar)) return;
    GdipSetSmoothingMode(g, GP_SMOOTH_ANTIALIAS);
    fill_round(g, e->bar, S(e, 8), gp_argb(240, 0x1E1E22));
    for (int i = 0; i < e->nbtns; i++) {
        Btn *b = &e->btns[i];
        bool en = btn_enabled(e, b);
        bool act = btn_active(e, b);
        bool hov = e->hover == b->id && en;
        if (act || hov) fill_round(g, b->r, S(e, 5), act ? gp_argb(255, ACCENT_RGB) : gp_argb(255, 0x46464E));
    }
    GdipSetSmoothingMode(g, GP_SMOOTH_NONE);
    for (int i = 0; i < e->nbtns; i++) {
        Btn *b = &e->btns[i];
        bool en = btn_enabled(e, b) || b->id == BTN_SIZE;
        if (b->swatch) {
            Rc r = rc_inflate(b->r, -S(e, 7), -S(e, 7));
            fill_rect(g, r, gp_argb(255, b->swatch_rgb));
            stroke_rect(g, r, 0xFFFFFFFF, false);
        } else {
            bool strong = b->id == BTN_COPY || b->id == BTN_SAVE;
            COLORREF tc = en ? RGB(255, 255, 255) : RGB(110, 110, 118);
            if (strong && en) tc = RGB(120, 200, 255);
            gdi_text(e, g, b->label, b->r, strong ? e->font_bold : e->font, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    Rc tr = tip_rect(e);
    if (!rc_empty(tr)) {
        Btn *b = btn_by_id(e, e->hover);
        fill_rect(g, tr, gp_argb(235, 0x0F0F12));
        gdi_text(e, g, b->tip, tr, e->font, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static Rc label_rect(Editor *e, const wchar_t *s, POINT p)
{
    SIZE sz = measure(e, s, e->font_bold);
    return rc_make(p.x, p.y, sz.cx + S(e, 10), sz.cy + S(e, 4));
}

static POINT size_label_pos(Editor *e)
{
    int y = e->crop.y - S(e, 26);
    if (y < monitor_of(e, pt(e->crop.x, e->crop.y)).y) y = e->crop.y + S(e, 6);
    return pt(e->crop.x, y);
}

typedef struct { Editor *e; GpGraphics *g; Rc clip; } HintCtx;

static const wchar_t *hint_text(const Editor *e)
{
    return e->quick
        ? L"Drag to select an area   \x2022   Click: this screen   \x2022   Enter: all screens   \x2022   Esc: cancel"
        : L"Drag to select an area, then resize it from the edges and edit it in place   \x2022   "
          L"Click: this screen   \x2022   Enter: all screens   \x2022   Esc: cancel";
}

static BOOL CALLBACK hint_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)m; (void)dc;
    HintCtx *h = (HintCtx *)lp;
    Editor *e = h->e;
    Rc b = rc_offset(rc_from_rect(*r), -e->virt.x, -e->virt.y);
    const wchar_t *hint = hint_text(e);
    SIZE sz = measure(e, hint, e->font_bold);
    Rc rect = rc_make(b.x + (b.w - sz.cx) / 2 - S(e, 14), b.y + S(e, 24), sz.cx + S(e, 28), sz.cy + S(e, 12));
    if (!rc_overlaps(rect, h->clip)) return TRUE;
    GdipSetSmoothingMode(h->g, GP_SMOOTH_ANTIALIAS);
    fill_round(h->g, rect, S(e, 8), gp_argb(200, 0x141418));
    GdipSetSmoothingMode(h->g, GP_SMOOTH_NONE);
    gdi_text(e, h->g, hint, rect, e->font_bold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return TRUE;
}

static bool show_lens(const Editor *e)
{
    return !e->windowed && e->has_mouse &&
           (e->mode == MODE_SELECTING || e->drag == DRAG_NEW_CROP || e->drag == DRAG_RESIZE_CROP);
}

#define LENS_ZOOM 8

static int lens_size(const Editor *e) { return ((ui_scale_dpi(120, e->dpi) / LENS_ZOOM) | 1) * LENS_ZOOM; }

static Rc lens_rect(Editor *e)
{
    int pad = S(e, 22), label_h = S(e, 22), ls = lens_size(e);
    Rc scr = monitor_of(e, e->cur);
    int x = e->cur.x + pad, y = e->cur.y + pad;
    if (x + ls > rc_right(scr)) x = e->cur.x - pad - ls;
    if (y + ls + label_h > rc_bottom(scr)) y = e->cur.y - pad - ls - label_h;
    return rc_make(x, y, ls, ls + label_h);
}

static void draw_lens(Editor *e, GpGraphics *g)
{
    Rc lr = lens_rect(e);
    int ls = lens_size(e), z = LENS_ZOOM, n = ls / z;
    Rc box = rc_make(lr.x, lr.y, ls, ls);
    Rc src = rc_make(e->cur.x - n / 2, e->cur.y - n / 2, n, n);
    fill_rect(g, box, 0xFF000000);
    GdipFlush(g, 1);
    Rc sc = rc_intersect(src, e->full);
    if (!rc_empty(sc)) {
        SetStretchBltMode(e->back->dc, COLORONCOLOR);
        StretchBlt(e->back->dc, box.x + (sc.x - src.x) * z, box.y + (sc.y - src.y) * z, sc.w * z, sc.h * z,
                   e->shot->dc, sc.x, sc.y, sc.w, sc.h, SRCCOPY);
        GdiFlush();
    }
    int c = (n / 2) * z;
    ARGB cross = gp_argb(160, ACCENT_RGB);
    draw_line(g, (REAL)box.x, box.y + c + z / 2 + 0.5f, (REAL)rc_right(box), box.y + c + z / 2 + 0.5f, cross);
    draw_line(g, box.x + c + z / 2 + 0.5f, (REAL)box.y, box.x + c + z / 2 + 0.5f, (REAL)rc_bottom(box), cross);
    stroke_rect(g, rc_make(box.x + c, box.y + c, z + 1, z + 1), 0xFFFFFFFF, false);
    stroke_rect(g, box, 0xFFFFFFFF, false);

    Rc scr = monitor_of(e, e->cur);
    wchar_t t[48];
    swprintf(t, 48, L"%d, %d", e->cur.x - scr.x, e->cur.y - scr.y);
    Rc lab = rc_make(box.x, rc_bottom(box), box.w, lr.h - ls);
    fill_rect(g, lab, gp_argb(220, 0x141418));
    gdi_text(e, g, t, lab, e->font_bold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void ensure_back(Editor *e)
{
    Rc c = client_rect(e);
    int w = imax(1, c.w), h = imax(1, c.h);
    if (e->back && e->back->w == w && e->back->h == h) return;
    img_free(e->back);
    e->back = img_new(w, h);
}

/* Draws the image part of the window-mode canvas. */
static void paint_canvas(Editor *e, Rc clip)
{
    HDC dc = e->back->dc;
    HBRUSH bg = CreateSolidBrush(RGB(38, 38, 42));
    RECT cr = rc_to_rect(clip);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    Rc dst = rect_to_scr(e, e->full);
    Rc vis = rc_intersect(rc_intersect(dst, clip), rc_make(0, e->top, e->back->w, e->back->h - e->top));
    if (rc_empty(vis)) return;
    if (fabs(e->zoom - 1.0) < 1e-6) {
        BitBlt(dc, vis.x, vis.y, vis.w, vis.h, e->rendered->dc, vis.x - dst.x, vis.y - dst.y, SRCCOPY);
        return;
    }
    /* Source pixels that cover the visible area, then their exact screen rectangle. */
    POINT a = to_img(e, vis.x, vis.y), b = to_img(e, rc_right(vis) - 1, rc_bottom(vis) - 1);
    Rc src = rc_intersect(rc_ltrb(a.x, a.y, b.x + 1, b.y + 1), e->full);
    if (rc_empty(src)) return;
    Rc d = rect_to_scr(e, src);
    IntersectClipRect(dc, vis.x, vis.y, rc_right(vis), rc_bottom(vis));
    bool smooth = e->zoom < 1.0 && e->drag == DRAG_NONE;
    SetStretchBltMode(dc, smooth ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, d.x, d.y, d.w, d.h, e->rendered->dc, src.x, src.y, src.w, src.h, SRCCOPY);
    SelectClipRgn(dc, NULL);
}

static void paint(Editor *e, Rc clip)
{
    ensure_back(e);
    if (!e->back) return;
    HDC dc = e->back->dc;

    if (e->windowed) {
        paint_canvas(e, clip);
    } else {
        Rc c = rc_intersect(clip, e->full);
        if (!rc_empty(c)) BitBlt(dc, c.x, c.y, c.w, c.h, e->dim->dc, c.x, c.y, SRCCOPY);
        if (!rc_empty(e->crop)) {
            Rc r = rc_intersect(e->crop, c);
            if (!rc_empty(r))
                BitBlt(dc, r.x, r.y, r.w, r.h, (e->mode == MODE_EDITING ? e->rendered : e->shot)->dc, r.x, r.y, SRCCOPY);
        }
    }
    GdiFlush();

    GpGraphics *g = (GpGraphics *)img_graphics(e->back, clip, false);
    if (!g) return;

    if (!e->windowed && !rc_empty(e->crop)) {
        stroke_rect(g, rc_inflate(e->crop, 1, 1), gp_argb(255, ACCENT_RGB), false);
        wchar_t t[48];
        swprintf(t, 48, L"%d \x00D7 %d", e->crop.w, e->crop.h);
        Rc lr = label_rect(e, t, size_label_pos(e));
        fill_rect(g, lr, gp_argb(220, 0x0078D7));
        gdi_text(e, g, t, lr, e->font_bold, RGB(255, 255, 255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (e->mode == MODE_EDITING) {
            POINT hs[8];
            geo_handles(e->crop, hs);
            draw_handles(e, g, hs, 8, gp_argb(255, ACCENT_RGB));
        }
    }

    if (!e->windowed && e->mode == MODE_SELECTING && e->drag == DRAG_NONE) {
        HintCtx h = { e, g, clip };
        EnumDisplayMonitors(NULL, NULL, hint_monitor, (LPARAM)&h);
    }

    if (e->sel && e->mode == MODE_EDITING) {
        Rc f = rc_inflate(rect_to_scr(e, op_frame(e->sel)), 2, 2);
        stroke_rect(g, f, 0xFF000000, false);
        stroke_rect(g, f, 0xFFFFFFFF, true);
        POINT hs[8];
        int n = op_handles(e->sel, hs);
        draw_handles(e, g, hs, n, gp_argb(255, ACCENT_RGB));
    }

    if (e->windowed) {
        Rc top = rc_make(0, 0, e->back->w, e->top);
        if (rc_overlaps(top, clip)) fill_rect(g, top, gp_argb(255, 0x26262A));
    }
    if (e->mode == MODE_EDITING) draw_bar(e, g);
    if (show_lens(e)) draw_lens(e, g);
    GdipDeleteGraphics(g);
}

/* Everything drawn over the image, in client coordinates, to repaint only that. */
static Rc decor(Editor *e)
{
    Rc d = rc_make(0, 0, 0, 0);
    if (!e->windowed && !rc_empty(e->crop)) {
        d = rc_union(d, rc_inflate(e->crop, S(e, 12), S(e, 12)));
        POINT lp = size_label_pos(e);
        d = rc_union(d, rc_make(lp.x - 2, lp.y - 2, S(e, 200), S(e, 34)));
    }
    if (e->sel) d = rc_union(d, rc_inflate(rect_to_scr(e, op_bounds(e->sel)), S(e, 12), S(e, 12)));
    if (!rc_empty(e->bar)) d = rc_union(d, rc_inflate(e->bar, 4, 4));
    d = rc_union(d, tip_rect(e));
    if (show_lens(e)) d = rc_union(d, rc_inflate(lens_rect(e), 2, 2));
    return d;
}

static void refresh_decor(Editor *e, bool now)
{
    Rc d = decor(e);
    invalidate_scr(e, rc_union(e->last_decor, d));
    e->last_decor = d;
    if (now) UpdateWindow(e->hwnd);
}

/* ------------------------------------------------------------------ */
/* Window-mode view                                                      */
/* ------------------------------------------------------------------ */

static void clamp_view(Editor *e)
{
    if (!e->windowed) return;
    Rc c = client_rect(e);
    double aw = c.w, ah = c.h - e->top;
    double iw = e->full.w * e->zoom, ih = e->full.h * e->zoom;
    if (iw <= aw) e->ox = floor((aw - iw) / 2);
    else e->ox = fmin(0, fmax(aw - iw, e->ox));
    if (ih <= ah) e->oy = e->top + floor((ah - ih) / 2);
    else e->oy = fmin(e->top, fmax(e->top + ah - ih, e->oy));
    e->ox = floor(e->ox);
    e->oy = floor(e->oy);
}

static void update_title(Editor *e)
{
    if (!e->windowed) return;
    wchar_t t[128];
    swprintf(t, 128, L"Dofram  \x2022  %d \x00D7 %d  \x2022  %d%%", e->full.w, e->full.h, (int)floor(e->zoom * 100 + 0.5));
    SetWindowTextW(e->hwnd, t);
}

static double fit_zoom(Editor *e)
{
    Rc c = client_rect(e);
    double z = fmin((c.w - 16.0) / e->full.w, (c.h - e->top - 16.0) / e->full.h);
    return fmin(1.0, fmax(0.05, z));
}

static void set_zoom(Editor *e, double z, POINT anchor_client, bool anchored)
{
    z = fmax(0.05, fmin(16.0, z));
    if (fabs(z - e->zoom) < 1e-6) return;
    Rc c = client_rect(e);
    if (!anchored) anchor_client = pt(c.w / 2, e->top + (c.h - e->top) / 2);
    double ix = (anchor_client.x - e->ox) / e->zoom, iy = (anchor_client.y - e->oy) / e->zoom;
    e->zoom = z;
    e->ox = anchor_client.x - ix * z;
    e->oy = anchor_client.y - iy * z;
    clamp_view(e);
    place_edit(e);
    update_title(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
}

static void scroll_by(Editor *e, int dx, int dy)
{
    e->ox += dx;
    e->oy += dy;
    clamp_view(e);
    place_edit(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Actions                                                               */
/* ------------------------------------------------------------------ */

static void commit_text(Editor *e);

static void editor_close(Editor *e, Outcome o)
{
    if (e->edit) commit_text(e);
    settings_save();
    Img *result = NULL;
    if (o != OUTCOME_NONE) {
        Rc c = rc_intersect(e->crop, e->full);
        if (rc_empty(c)) o = OUTCOME_NONE;
        else result = img_crop(e->mode == MODE_EDITING ? e->rendered : e->shot, c);
        if (!result) o = OUTCOME_NONE;
    }
    HWND h = e->hwnd;
    ShowWindow(h, SW_HIDE);
    DestroyWindow(h);
    app_overlay_done(o, result);
}

static void set_tool(Editor *e, Tool t)
{
    e->tool = t;
    g_cfg.default_tool = t;
    if (t != TOOL_SELECT) e->sel = NULL;
    layout_bar(e);
    refresh_decor(e, true);
}

static void set_color(Editor *e, uint32_t c)
{
    Tool ct = color_target(e);
    if (!uses_color(ct)) return;
    e->colors[ct] = c;
    if (e->sel && e->sel->color != c) {
        State *b = snapshot(e);
        e->sel->color = c;
        render_region(e, op_bounds(e->sel));
        invalidate_img(e, op_bounds(e->sel));
        push_undo(e, b);
    }
    layout_bar(e);
    refresh_decor(e, true);
}

static void size_edit_box(Editor *e);

static void set_size(Editor *e, int n)
{
    n = iclamp(n, 1, 10);
    e->size = n;
    g_cfg.tool_size = n;
    if (e->sel && e->sel->size != n) {
        State *b = snapshot(e);
        Rc old = op_bounds(e->sel);
        op_set_size(e->sel, n);
        Rc u = rc_union(old, op_bounds(e->sel));
        render_region(e, u);
        invalidate_img(e, u);
        push_undo(e, b);
    }
    if (e->edit) {
        e->text_size = n;
        size_edit_box(e);
    }
    layout_bar(e);
    refresh_decor(e, true);
}

static void delete_selected(Editor *e)
{
    if (!e->sel) return;
    State *b = snapshot(e);
    Rc bounds = op_bounds(e->sel);
    objs_remove(e, e->sel);
    op_free(e->sel);
    e->sel = NULL;
    render_region(e, bounds);
    invalidate_img(e, bounds);
    push_undo(e, b);
    layout_bar(e);
    refresh_decor(e, true);
}

static void toggle_palette(Editor *e, bool open)
{
    e->palette_open = open;
    layout_bar(e);
    if (e->windowed) InvalidateRect(e->hwnd, NULL, FALSE);
    refresh_decor(e, true);
}

static void pick_custom_color(Editor *e)
{
    Tool ct = color_target(e);
    if (!uses_color(ct)) return;
    uint32_t c = e->colors[ct];
    if (util_pick_color(e->hwnd, &c)) set_color(e, c);
    toggle_palette(e, false);
}

static Img *window_result(Editor *e) { return e->rendered; }

static void window_save(Editor *e, bool as)
{
    if (e->edit) commit_text(e);
    bool ok = as ? app_save_image_as(window_result(e), e->hwnd) : app_save_image(window_result(e), g_cfg.copy_on_save, e->hwnd);
    if (!ok) return;
    e->dirty = false;
    if (g_cfg.close_after_save) DestroyWindow(e->hwnd);
}

static void window_copy(Editor *e)
{
    if (e->edit) commit_text(e);
    if (!app_copy_image(window_result(e), e->hwnd)) return;
    e->dirty = false;
    if (g_cfg.close_after_copy) DestroyWindow(e->hwnd);
}

static void do_save(Editor *e) { if (e->windowed) window_save(e, false); else editor_close(e, OUTCOME_SAVE); }
static void do_copy(Editor *e) { if (e->windowed) window_copy(e); else editor_close(e, OUTCOME_COPY); }

static void do_cancel(Editor *e)
{
    if (e->windowed) PostMessageW(e->hwnd, WM_CLOSE, 0, 0);
    else editor_close(e, OUTCOME_NONE);
}

static void click_button(Editor *e, Btn *b)
{
    if (!btn_enabled(e, b)) return;
    int id = b->id;
    if (id >= BTN_TOOL && id < BTN_TOOL + TOOL_COUNT) { set_tool(e, (Tool)(id - BTN_TOOL)); return; }
    if (id >= BTN_PAL && id < BTN_PAL + PAL_COUNT) {
        set_color(e, PALETTE[id - BTN_PAL]);
        toggle_palette(e, false);
        return;
    }
    switch (id) {
    case BTN_COLOR: toggle_palette(e, !e->palette_open); break;
    case BTN_PALMORE: pick_custom_color(e); break;
    case BTN_MINUS: set_size(e, e->size - 1); break;
    case BTN_PLUS: set_size(e, e->size + 1); break;
    case BTN_UNDO: undo(e); break;
    case BTN_REDO: redo(e); break;
    case BTN_DEL: delete_selected(e); break;
    case BTN_WINDOW: editor_close(e, OUTCOME_WINDOW); break;
    case BTN_SAVE: do_save(e); break;
    case BTN_SAVEAS: window_save(e, true); break;
    case BTN_COPY: do_copy(e); break;
    case BTN_CLOSE: do_cancel(e); break;
    }
}

static void enter_editing(Editor *e)
{
    e->mode = MODE_EDITING;
    render_all(e);
    layout_bar(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------ */
/* Text editing (a child EDIT control placed over the image)             */
/* ------------------------------------------------------------------ */

static POINT text_offset(const Editor *e) { return pt(S(e, 4), S(e, 3)); }

static wchar_t *edit_get_text(HWND edit)
{
    int n = GetWindowTextLengthW(edit);
    wchar_t *raw = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n + 1));
    if (!raw) return NULL;
    GetWindowTextW(edit, raw, n + 1);
    /* CRLF to LF */
    wchar_t *w = raw;
    for (wchar_t *r = raw; *r; r++)
        if (*r != L'\r') *w++ = *r;
    *w = 0;
    return raw;
}

static void edit_set_text(HWND edit, const wchar_t *s)
{
    size_t n = wcslen(s), lf = 0;
    for (size_t i = 0; i < n; i++) if (s[i] == L'\n') lf++;
    wchar_t *buf = (wchar_t *)malloc(sizeof(wchar_t) * (n + lf + 1));
    if (!buf) return;
    wchar_t *w = buf;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == L'\n') *w++ = L'\r';
        *w++ = s[i];
    }
    *w = 0;
    SetWindowTextW(edit, buf);
    free(buf);
}

static void size_edit_box(Editor *e)
{
    if (!e->edit) return;
    int px = (int)floor(op_font_px(e->text_size) * (e->windowed ? e->zoom : 1.0) + 0.5);
    if (px < 6) px = 6;
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(e->edit, WM_SETFONT, (WPARAM)f, TRUE);
    if (e->edit_font) DeleteObject(e->edit_font);
    e->edit_font = f;

    wchar_t *t = edit_get_text(e->edit);
    wchar_t buf[4096];
    if (t && t[0]) swprintf(buf, 4096, L"%sW", t);
    else wcscpy_s(buf, 4096, L"Type text");
    free(t);
    RECT r = { 0, 0, 0, 0 };
    HDC dc = GetDC(e->edit);
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, buf, -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(e->edit, dc);
    int w = imax(S(e, 140), (r.right - r.left) + S(e, 14));
    int h = (r.bottom - r.top) + S(e, 10);
    SetWindowPos(e->edit, HWND_TOP, 0, 0, w, h, SWP_NOMOVE | SWP_NOACTIVATE);
}

/* Keeps the text box on its anchor when the window view moves or zooms. */
static void place_edit(Editor *e)
{
    if (!e->edit) return;
    POINT s = to_scr(e, e->text_anchor), off = text_offset(e);
    SetWindowPos(e->edit, HWND_TOP, s.x - off.x, s.y - off.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    size_edit_box(e);
}

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    HWND parent = (HWND)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN && GetKeyState(VK_SHIFT) >= 0) { PostMessageW(parent, WM_APP_TEXT_COMMIT, (WPARAM)h, 0); return 0; }
        if (wp == VK_ESCAPE) { PostMessageW(parent, WM_APP_TEXT_CANCEL, (WPARAM)h, 0); return 0; }
        break;
    case WM_CHAR:
        if ((wp == L'\r' && GetKeyState(VK_SHIFT) >= 0) || wp == 27) return 0;
        if (wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; } /* Ctrl+A */
        break;
    case WM_KILLFOCUS:
        PostMessageW(parent, WM_APP_TEXT_COMMIT, (WPARAM)h, 0);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, edit_proc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void start_text(Editor *e, POINT p, Op *existing)
{
    e->text_before = snapshot(e);
    e->text_target = existing;
    e->text_index = -1;
    e->sel = NULL;
    POINT off = text_offset(e);
    POINT scr;
    if (existing) {
        e->text_index = objs_index(e, existing);
        objs_remove(e, existing);
        render_region(e, op_bounds(existing));
        invalidate_img(e, op_bounds(existing));
        e->text_anchor = existing->p;
        scr = to_scr(e, existing->p);
        scr.x -= off.x;
        scr.y -= off.y;
        e->text_color = existing->color;
        e->text_size = existing->size;
    } else {
        scr = to_scr(e, p);
        e->text_anchor = to_img(e, scr.x + off.x, scr.y + off.y);
        e->text_color = e->colors[TOOL_TEXT];
        e->text_size = e->size;
    }
    uint32_t c = e->text_color;
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    int light = (imax(r, imax(g, b)) + imin(r, imin(g, b))) * 100 / 510; /* HSL lightness in % */
    if (e->edit_brush) DeleteObject(e->edit_brush);
    e->edit_brush = CreateSolidBrush(light < 35 ? RGB(235, 235, 235) : RGB(40, 40, 44));

    e->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOHSCROLL | ES_AUTOVSCROLL,
                              scr.x, scr.y, S(e, 140), S(e, 30), e->hwnd, (HMENU)1, g_inst, NULL);
    if (!e->edit) return;
    SetWindowSubclass(e->edit, edit_proc, 1, (DWORD_PTR)e->hwnd);
    if (existing && existing->text) edit_set_text(e->edit, existing->text);
    size_edit_box(e);
    SetFocus(e->edit);
    SendMessageW(e->edit, EM_SETSEL, 0, -1);
    layout_bar(e);
    refresh_decor(e, true);
}

static void remove_edit(Editor *e)
{
    HWND ed = e->edit;
    e->edit = NULL;
    if (ed) DestroyWindow(ed);
    SetFocus(e->hwnd);
}

static bool blank(const wchar_t *s)
{
    for (; s && *s; s++) if (!iswspace(*s)) return false;
    return true;
}

static void commit_text(Editor *e)
{
    if (e->committing || !e->edit) return;
    e->committing = true;
    wchar_t *t = edit_get_text(e->edit);
    POINT loc = e->text_anchor;
    remove_edit(e);

    Op *target = e->text_target;
    const wchar_t *orig = target ? target->text : NULL;
    bool changed = orig == NULL ? !blank(t) : (wcscmp(t ? t : L"", orig ? orig : L"") != 0 || blank(t));
    if (t && !blank(t)) {
        Op *op = target ? target : op_new(TOOL_TEXT, e->text_color, e->text_size);
        if (op) {
            op->color = e->text_color;
            op->size = e->text_size;
            op->p = loc;
            op_set_text(op, t);
            objs_insert(e, (e->text_index >= 0 && e->text_index <= e->nobjs) ? e->text_index : -1, op);
            e->sel = op;
            render_region(e, op_bounds(op));
        }
    } else if (target) {
        op_free(target);
    }
    if (changed) push_undo(e, e->text_before);
    else state_free(e->text_before);
    e->text_before = NULL;
    e->text_target = NULL;
    free(t);
    layout_bar(e);
    InvalidateRect(e->hwnd, NULL, FALSE);
    e->committing = false;
}

static void cancel_text(Editor *e)
{
    if (!e->edit) return;
    e->committing = true;
    remove_edit(e);
    if (e->text_target) {
        objs_insert(e, (e->text_index >= 0 && e->text_index <= e->nobjs) ? e->text_index : -1, e->text_target);
        render_region(e, op_bounds(e->text_target));
    }
    state_free(e->text_before);
    e->text_before = NULL;
    e->text_target = NULL;
    InvalidateRect(e->hwnd, NULL, FALSE);
    e->committing = false;
}

/* ------------------------------------------------------------------ */
/* Mouse                                                                 */
/* ------------------------------------------------------------------ */

static int hit_handle(const Editor *e, const POINT *hs, int n, POINT client)
{
    int t = S(e, 7);
    for (int i = 0; i < n; i++) {
        POINT s = to_scr(e, hs[i]);
        if (abs(client.x - s.x) <= t && abs(client.y - s.y) <= t) return i;
    }
    return -1;
}

static Op *hit_object(Editor *e, POINT p)
{
    if (!rc_has(e->crop, p)) return NULL;
    int tol = img_tol(e, S(e, 5));
    for (int i = e->nobjs - 1; i >= 0; i--)
        if (op_hit(e->objs[i], p, tol)) return e->objs[i];
    return NULL;
}

static LPCWSTR handle_cursor(int h)
{
    switch (h) {
    case 0: case 4: return IDC_SIZENWSE;
    case 2: case 6: return IDC_SIZENESW;
    case 1: case 5: return IDC_SIZENS;
    case 3: case 7: return IDC_SIZEWE;
    }
    return IDC_SIZEALL;
}

static LPCWSTR cursor_at(Editor *e, POINT client)
{
    if (e->mode == MODE_SELECTING) return IDC_CROSS;
    if (rc_has(e->bar, client)) return btn_at(e, client) ? IDC_HAND : IDC_ARROW;
    if (e->windowed && client.y < e->top) return IDC_ARROW;
    POINT p = to_img(e, client.x, client.y);
    if (e->sel) {
        POINT hs[8];
        int n = op_handles(e->sel, hs);
        int h = hit_handle(e, hs, n, client);
        if (h >= 0) return op_is_region(e->sel) ? handle_cursor(h) : IDC_SIZEALL;
    }
    if (!e->windowed) {
        POINT hs[8];
        geo_handles(e->crop, hs);
        int ch = hit_handle(e, hs, 8, client);
        if (ch >= 0) return handle_cursor(ch);
    }
    if (!rc_has(e->crop, p)) return e->windowed ? IDC_ARROW : IDC_CROSS;
    if (e->tool == TOOL_SELECT) {
        if (e->windowed && !hit_object(e, p)) return IDC_ARROW;
        return IDC_SIZEALL;
    }
    if (e->tool == TOOL_TEXT) {
        Op *o = hit_object(e, p);
        return (o && o->kind == TOOL_TEXT) ? IDC_HAND : IDC_IBEAM;
    }
    return IDC_CROSS;
}

static void update_cursor(Editor *e, POINT client)
{
    e->cursor = LoadCursorW(NULL, cursor_at(e, client));
    SetCursor(e->cursor);
}

static void begin_drag(Editor *e, int kind)
{
    e->drag = kind;
    SetCapture(e->hwnd);
}

static void cancel_drag(Editor *e)
{
    e->drag = DRAG_NONE;
    op_free(e->drawing);
    e->drawing = NULL;
    if (GetCapture() == e->hwnd) ReleaseCapture();
    if (e->mode == MODE_SELECTING) {
        e->crop = rc_make(0, 0, 0, 0);
        InvalidateRect(e->hwnd, NULL, FALSE);
        return;
    }
    if (e->before) {
        restore(e, e->before);
        state_free(e->before);
        e->before = NULL;
    }
}

static void on_mouse_down(Editor *e, POINT client, bool right, bool middle)
{
    POINT p = to_img(e, client.x, client.y);
    e->cur = p;
    if (e->edit) commit_text(e);

    if (right) {
        if (e->drag != DRAG_NONE) cancel_drag(e);
        else if (e->mode == MODE_SELECTING) do_cancel(e);
        return;
    }
    if (middle) {
        if (e->windowed && e->drag == DRAG_NONE) {
            e->pan_from = client;
            e->pan_ox = e->ox;
            e->pan_oy = e->oy;
            begin_drag(e, DRAG_PAN);
        }
        return;
    }

    if (e->mode == MODE_SELECTING) {
        e->down = p;
        e->crop = rc_make(p.x, p.y, 1, 1);
        begin_drag(e, DRAG_NEW_CROP);
        InvalidateRect(e->hwnd, NULL, FALSE);
        return;
    }

    Btn *b = btn_at(e, client);
    if (b) { click_button(e, b); return; }
    if (rc_has(e->bar, client)) return;
    if (e->windowed && client.y < e->top) return;
    if (e->palette_open) toggle_palette(e, false);

    e->before = snapshot(e);
    e->down = p;
    e->last = p;

    if (e->sel) {
        POINT hs[8];
        int n = op_handles(e->sel, hs);
        int h = hit_handle(e, hs, n, client);
        if (h >= 0) { e->handle = h; op_begin_handle_drag(e->sel); begin_drag(e, DRAG_RESIZE_OBJ); return; }
    }
    if (!e->windowed) {
        POINT hs[8];
        geo_handles(e->crop, hs);
        int ch = hit_handle(e, hs, 8, client);
        if (ch >= 0) { e->handle = ch; e->crop_orig = e->crop; begin_drag(e, DRAG_RESIZE_CROP); return; }
    }

    if (!rc_has(e->crop, p) || (e->windowed && e->tool == TOOL_SELECT && !hit_object(e, p))) {
        if (e->windowed) {
            /* Window mode: an empty click deselects and drags the view. */
            e->sel = NULL;
            state_free(e->before);
            e->before = NULL;
            e->pan_from = client;
            e->pan_ox = e->ox;
            e->pan_oy = e->oy;
            begin_drag(e, DRAG_PAN);
            layout_bar(e);
            refresh_decor(e, true);
            return;
        }
        e->sel = NULL;
        e->crop_orig = e->crop;
        e->crop = rc_make(p.x, p.y, 1, 1);
        begin_drag(e, DRAG_NEW_CROP);
        render_all(e);
        refresh_decor(e, true);
        return;
    }

    if (e->tool == TOOL_SELECT) {
        Op *o = hit_object(e, p);
        if (o) { e->sel = o; begin_drag(e, DRAG_MOVE_OBJ); }
        else { e->sel = NULL; e->crop_orig = e->crop; begin_drag(e, DRAG_MOVE_CROP); }
        layout_bar(e);
        refresh_decor(e, true);
        return;
    }

    if (e->tool == TOOL_TEXT) {
        state_free(e->before);
        e->before = NULL;
        Op *hit = hit_object(e, p);
        start_text(e, p, (hit && hit->kind == TOOL_TEXT) ? hit : NULL);
        return;
    }

    e->sel = NULL;
    uint32_t c = e->colors[e->tool];
    e->drawing = op_new(e->tool, c, e->size);
    if (!e->drawing) { state_free(e->before); e->before = NULL; return; }
    if (e->tool == TOOL_ARROW) { e->drawing->a = p; e->drawing->b = p; }
    else if (e->tool == TOOL_PEN) op_pen_add(e->drawing, p);
    else e->drawing->r = rc_make(p.x, p.y, 1, 1);
    begin_drag(e, DRAG_DRAW);
}

static void on_mouse_move(Editor *e, POINT client)
{
    POINT p = to_img(e, client.x, client.y);
    e->cur = p;
    e->has_mouse = true;
    int dx = p.x - e->down.x, dy = p.y - e->down.y;

    switch (e->drag) {
    case DRAG_NONE:
        if (e->mode == MODE_EDITING) {
            Btn *hb = btn_at(e, client);
            int id = hb ? hb->id : 0;
            if (id != e->hover) e->hover = id;
            update_cursor(e, client);
        }
        break;
    case DRAG_PAN:
        e->ox = e->pan_ox + (client.x - e->pan_from.x);
        e->oy = e->pan_oy + (client.y - e->pan_from.y);
        clamp_view(e);
        InvalidateRect(e->hwnd, NULL, FALSE);
        break;
    case DRAG_NEW_CROP: {
        Rc old = e->crop;
        e->crop = rc_intersect(geo_norm(e->down, p), e->full);
        render_all(e);
        invalidate_scr(e, rc_inflate(rc_union(old, e->crop), 2, 2));
        break;
    }
    case DRAG_RESIZE_CROP: {
        Rc old = e->crop;
        e->crop = rc_intersect(geo_resize(e->crop_orig, e->handle, dx, dy), e->full);
        if (rc_empty(e->crop))
            e->crop = rc_make(imin(p.x, rc_right(e->full) - 1), imin(p.y, rc_bottom(e->full) - 1), 1, 1);
        render_all(e);
        layout_bar(e);
        invalidate_scr(e, rc_inflate(rc_union(old, e->crop), 2, 2));
        break;
    }
    case DRAG_MOVE_CROP: {
        Rc old = e->crop;
        int nx = imax(0, imin(e->full.w - e->crop_orig.w, e->crop_orig.x + dx));
        int ny = imax(0, imin(e->full.h - e->crop_orig.h, e->crop_orig.y + dy));
        e->crop = rc_make(nx, ny, e->crop_orig.w, e->crop_orig.h);
        render_all(e);
        layout_bar(e);
        invalidate_scr(e, rc_inflate(rc_union(old, e->crop), 2, 2));
        break;
    }
    case DRAG_MOVE_OBJ: {
        Rc old = op_bounds(e->sel);
        op_move(e->sel, p.x - e->last.x, p.y - e->last.y);
        e->last = p;
        Rc u = rc_union(old, op_bounds(e->sel));
        render_region(e, u);
        invalidate_img(e, u);
        break;
    }
    case DRAG_RESIZE_OBJ: {
        Rc old = op_bounds(e->sel);
        op_drag_handle(e->sel, e->handle, dx, dy);
        Rc u = rc_union(old, op_bounds(e->sel));
        render_region(e, u);
        invalidate_img(e, u);
        break;
    }
    case DRAG_DRAW: {
        Op *d = e->drawing;
        Rc old = op_bounds(d);
        if (op_is_region(d)) {
            POINT q = p;
            if (GetKeyState(VK_SHIFT) < 0) {
                int m = imax(abs(dx), abs(dy));
                q = pt(e->down.x + (dx > 0 ? m : dx < 0 ? -m : 0), e->down.y + (dy > 0 ? m : dy < 0 ? -m : 0));
            }
            d->r = geo_norm(e->down, q);
        } else if (d->kind == TOOL_ARROW) {
            d->b = p;
        } else if (d->kind == TOOL_PEN) {
            if (d->npts == 0 || d->pts[d->npts - 1].x != p.x || d->pts[d->npts - 1].y != p.y) op_pen_add(d, p);
        }
        Rc u = rc_union(old, op_bounds(d));
        render_region(e, u);
        invalidate_img(e, u);
        break;
    }
    }
    refresh_decor(e, false);
}

static void on_mouse_up(Editor *e, POINT client, bool left)
{
    if (e->drag == DRAG_PAN) {
        e->drag = DRAG_NONE;
        if (GetCapture() == e->hwnd) ReleaseCapture();
        InvalidateRect(e->hwnd, NULL, FALSE);
        return;
    }
    if (!left || e->drag == DRAG_NONE) return;
    int kind = e->drag;
    e->drag = DRAG_NONE;
    if (GetCapture() == e->hwnd) ReleaseCapture();

    if (e->mode == MODE_SELECTING) {
        if (e->crop.w < 4 && e->crop.h < 4) e->crop = rc_intersect(monitor_of(e, client), e->full);
        if (e->quick) { editor_close(e, OUTCOME_QUICK); return; }
        enter_editing(e);
        update_cursor(e, client);
        return;
    }

    if (kind == DRAG_DRAW) {
        Op *d = e->drawing;
        e->drawing = NULL;
        if (d && op_meaningful(d)) {
            objs_insert(e, -1, d);
            e->sel = d;
        } else if (d) {
            render_region(e, op_bounds(d));
            invalidate_img(e, op_bounds(d));
            op_free(d);
        }
    } else if (kind == DRAG_NEW_CROP) {
        if (e->crop.w < 4 && e->crop.h < 4) { e->crop = e->crop_orig; render_all(e); }
    }

    if (e->before) {
        State *now = snapshot(e);
        if (!same_state(e->before, now)) push_undo(e, e->before);
        else state_free(e->before);
        state_free(now);
    }
    e->before = NULL;
    layout_bar(e);
    update_cursor(e, client);
    if (e->windowed && e->zoom < 1.0) InvalidateRect(e->hwnd, NULL, FALSE);
    invalidate_scr(e, e->last_decor);
    refresh_decor(e, true);
}

static void on_double_click(Editor *e, POINT client)
{
    if (e->mode != MODE_EDITING) { on_mouse_down(e, client, false, false); return; }
    POINT p = to_img(e, client.x, client.y);
    Op *t = hit_object(e, p);
    if (t && t->kind == TOOL_TEXT && !e->edit && !btn_at(e, client) &&
        (e->tool == TOOL_SELECT || e->tool == TOOL_TEXT)) {
        start_text(e, p, t);
        return;
    }
    on_mouse_down(e, client, false, false);
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                              */
/* ------------------------------------------------------------------ */

static bool on_key(Editor *e, UINT vk)
{
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    if (alt) return false;

    if (vk == VK_ESCAPE) {
        if (e->drag != DRAG_NONE) cancel_drag(e);
        else if (e->palette_open) toggle_palette(e, false);
        else if (e->sel) { e->sel = NULL; layout_bar(e); refresh_decor(e, true); }
        else do_cancel(e);
        return true;
    }
    if (e->mode == MODE_SELECTING) {
        if (vk == VK_RETURN && e->drag == DRAG_NONE) {
            e->crop = e->full;
            if (e->quick) editor_close(e, OUTCOME_QUICK);
            else enter_editing(e);
            return true;
        }
        return false;
    }
    if (e->drag != DRAG_NONE) return false;

    if (ctrl) {
        switch (vk) {
        case 'C': do_copy(e); return true;
        case 'S': if (e->windowed && shift) window_save(e, true); else do_save(e); return true;
        case 'Z': undo(e); return true;
        case 'Y': redo(e); return true;
        case 'A':
            if (!e->windowed) {
                State *b = snapshot(e);
                e->crop = e->full;
                render_all(e);
                layout_bar(e);
                InvalidateRect(e->hwnd, NULL, FALSE);
                push_undo(e, b);
            }
            return true;
        }
        if (e->windowed) {
            if (vk == '0' || vk == VK_NUMPAD0) { set_zoom(e, fit_zoom(e), pt(0, 0), false); return true; }
            if (vk == '1' || vk == VK_NUMPAD1) { set_zoom(e, 1.0, pt(0, 0), false); return true; }
            if (vk == VK_OEM_PLUS || vk == VK_ADD) { set_zoom(e, e->zoom * 1.25, pt(0, 0), false); return true; }
            if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { set_zoom(e, e->zoom / 1.25, pt(0, 0), false); return true; }
        }
        return false;
    }

    if (vk == VK_RETURN && !e->windowed) { do_copy(e); return true; }
    if (vk == VK_DELETE || vk == VK_BACK) { delete_selected(e); return true; }
    if (vk == 'V' || vk == '0' || vk == VK_NUMPAD0) { set_tool(e, TOOL_SELECT); return true; }
    if (vk >= '1' && vk <= '8') { set_tool(e, (Tool)(vk - '1' + 1)); return true; }
    if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD8) { set_tool(e, (Tool)(vk - VK_NUMPAD1 + 1)); return true; }
    if (vk == 'C') { toggle_palette(e, !e->palette_open); return true; }
    if (vk == VK_OEM_4 || vk == VK_SUBTRACT || vk == VK_OEM_MINUS) { set_size(e, e->size - 1); return true; }
    if (vk == VK_OEM_6 || vk == VK_ADD || vk == VK_OEM_PLUS) { set_size(e, e->size + 1); return true; }
    if (e->sel && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        int step = shift ? 10 : 1;
        int mx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
        int my = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
        State *b = snapshot(e);
        Rc old = op_bounds(e->sel);
        op_move(e->sel, mx, my);
        Rc u = rc_union(old, op_bounds(e->sel));
        render_region(e, u);
        invalidate_img(e, u);
        push_undo(e, b);
        refresh_decor(e, true);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Window procedure                                                      */
/* ------------------------------------------------------------------ */

static void editor_free(Editor *e)
{
    if (e->edit_font) DeleteObject(e->edit_font);
    if (e->edit_brush) DeleteObject(e->edit_brush);
    if (e->font) DeleteObject(e->font);
    if (e->font_bold) DeleteObject(e->font_bold);
    objs_clear(e);
    free(e->objs);
    op_free(e->drawing);
    if (e->text_target && objs_index(e, e->text_target) < 0) op_free(e->text_target);
    clear_stack(e->undo, &e->nundo);
    clear_stack(e->redo, &e->nredo);
    state_free(e->before);
    state_free(e->text_before);
    img_free(e->shot);
    img_free(e->dim);
    img_free(e->rendered);
    img_free(e->back);
    free(e);
}

static void make_fonts(Editor *e)
{
    if (e->font) DeleteObject(e->font);
    if (e->font_bold) DeleteObject(e->font_bold);
    e->font = ui_font(9, false, e->dpi);
    e->font_bold = ui_font(9, true, e->dpi);
}

static LRESULT CALLBACK editor_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Editor *e = (Editor *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        e = (Editor *)cs->lpCreateParams;
        e->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)e);
    }
    if (!e) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Rc clip = rc_from_rect(ps.rcPaint);
        if (!rc_empty(clip)) {
            paint(e, clip);
            if (e->back) BitBlt(dc, clip.x, clip.y, clip.w, clip.h, e->back->dc, clip.x, clip.y, SRCCOPY);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        if (e->windowed) {
            layout_bar(e);
            clamp_view(e);
            place_edit(e);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (e->windowed) {
            MINMAXINFO *mm = (MINMAXINFO *)lp;
            mm->ptMinTrackSize.x = S(e, 480);
            mm->ptMinTrackSize.y = S(e, 300);
        }
        return 0;
    case WM_DPICHANGED:
        if (!e->windowed) return 0; /* the overlay spans every monitor and keeps its size */
        e->dpi = HIWORD(wp);
        make_fonts(e);
        {
            RECT *r = (RECT *)lp;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout_bar(e);
        clamp_view(e);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(e->cursor ? e->cursor : LoadCursorW(NULL, IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!e->has_mouse || e->mode == MODE_SELECTING) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        on_mouse_move(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (e->hover) { e->hover = 0; refresh_decor(e, false); }
        return 0;
    case WM_LBUTTONDOWN:
        on_mouse_down(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), false, false);
        return 0;
    case WM_LBUTTONDBLCLK:
        on_double_click(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_RBUTTONDOWN:
        on_mouse_down(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), true, false);
        return 0;
    case WM_MBUTTONDOWN:
        on_mouse_down(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), false, true);
        return 0;
    case WM_LBUTTONUP:
        on_mouse_up(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), true);
        return 0;
    case WM_MBUTTONUP:
        if (e->drag == DRAG_PAN) on_mouse_up(e, pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)), false);
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != h && e->drag != DRAG_NONE) {
            if (e->drag == DRAG_PAN) e->drag = DRAG_NONE;
            else cancel_drag(e);
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (e->mode != MODE_EDITING) return 0;
        if (!e->windowed) { set_size(e, e->size + (delta > 0 ? 1 : -1)); return 0; }
        POINT sp = pt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        ScreenToClient(h, &sp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) set_zoom(e, delta > 0 ? e->zoom * 1.25 : e->zoom / 1.25, sp, true);
        else if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) scroll_by(e, delta * S(e, 120) / WHEEL_DELTA, 0);
        else scroll_by(e, 0, delta * S(e, 120) / WHEEL_DELTA);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        if (e->windowed) scroll_by(e, -GET_WHEEL_DELTA_WPARAM(wp) * S(e, 120) / WHEEL_DELTA, 0);
        return 0;
    case WM_KEYDOWN:
        if (on_key(e, (UINT)wp)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (wp == VK_F4 && !e->windowed) { do_cancel(e); return 0; }
        break;
    case WM_CHAR:
        return 0;
    case WM_CTLCOLOREDIT:
        if ((HWND)lp == e->edit) {
            HDC dc = (HDC)wp;
            uint32_t c = e->text_color;
            SetTextColor(dc, RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF));
            LOGBRUSH lb;
            GetObjectW(e->edit_brush, sizeof(lb), &lb);
            SetBkColor(dc, lb.lbColor);
            return (LRESULT)e->edit_brush;
        }
        break;
    case WM_COMMAND:
        if ((HWND)lp == e->edit && e->edit && HIWORD(wp) == EN_CHANGE) size_edit_box(e);
        return 0;
    case WM_APP_TEXT_COMMIT: /* ignore messages posted by an earlier, already closed box */
        if (e->edit && (HWND)wp == e->edit) commit_text(e);
        return 0;
    case WM_APP_TEXT_CANCEL:
        if (e->edit && (HWND)wp == e->edit) cancel_text(e);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !e->edit) SetFocus(h);
        return 0;
    case WM_CLOSE:
        if (e->windowed) {
            if (e->dirty &&
                MessageBoxW(h, L"Your edits have not been saved or copied. Close anyway?", APP_NAME,
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return 0;
            DestroyWindow(h);
        } else {
            editor_close(e, OUTCOME_NONE);
        }
        return 0;
    case WM_DESTROY:
        if (e->windowed) settings_save();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (e->created) editor_free(e); /* otherwise the failed creator frees it */
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool editor_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = editor_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_CROSS);
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hIcon = app_icon(true);
    wc.hIconSm = app_icon(false);
    if (!RegisterClassExW(&wc)) return false;
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    return RegisterClassExW(&wc) != 0;
}

static Editor *editor_new(Img *shot, bool windowed)
{
    Editor *e = (Editor *)calloc(1, sizeof(Editor));
    if (!e) return NULL;
    e->windowed = windowed;
    e->shot = shot;
    e->full = rc_make(0, 0, shot->w, shot->h);
    e->rendered = img_new(shot->w, shot->h);
    if (!e->rendered) { free(e); return NULL; }
    img_copy_rect(e->rendered, shot, e->full);
    e->zoom = 1.0;
    for (int i = 0; i < TOOL_COUNT; i++) e->colors[i] = 0xE61E28;
    e->colors[TOOL_BOX] = 0x000000;
    e->colors[TOOL_HIGHLIGHT] = 0xFFEB3C;
    e->tool = g_cfg.default_tool;
    if (e->tool < 0 || e->tool >= TOOL_COUNT) e->tool = TOOL_BLUR;
    e->size = iclamp(g_cfg.tool_size, 1, 10);
    e->hover = 0;
    return e;
}

HWND overlay_open(Img *shot, Rc virt, bool quick, const Rc *initial)
{
    Editor *e = editor_new(shot, false);
    if (!e) { img_free(shot); return NULL; }
    e->quick = quick;
    e->virt = virt;
    e->dpi = (int)GetDpiForSystem();
    e->dim = img_new(shot->w, shot->h);
    if (!e->dim) { editor_free(e); return NULL; }
    img_copy_rect(e->dim, shot, e->full);
    fx_darken(e->dim, 145);
    make_fonts(e);
    if (initial) {
        e->crop = rc_intersect(*initial, e->full);
        if (!rc_empty(e->crop)) { e->mode = MODE_EDITING; render_all(e); }
    }
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OVERLAY_CLASS, APP_NAME, WS_POPUP,
                             virt.x, virt.y, virt.w, virt.h, NULL, NULL, g_inst, e);
    if (!h) { editor_free(e); return NULL; }
    e->created = true;
    if (e->mode == MODE_EDITING) layout_bar(e);
    e->cursor = LoadCursorW(NULL, IDC_CROSS);
    ShowWindow(h, SW_SHOW);
    SetWindowPos(h, HWND_TOPMOST, virt.x, virt.y, virt.w, virt.h, SWP_SHOWWINDOW);
    SetForegroundWindow(h);
    SetFocus(h);
    POINT cp;
    GetCursorPos(&cp);
    ScreenToClient(h, &cp);
    e->cur = cp;
    e->has_mouse = true;
    InvalidateRect(h, NULL, FALSE);
    UpdateWindow(h);
    return h;
}

HWND editor_window_open(Img *img)
{
    Editor *e = editor_new(img, true);
    if (!e) { img_free(img); return NULL; }
    e->crop = e->full;
    e->mode = MODE_EDITING;
    POINT cp;
    GetCursorPos(&cp);
    HMONITOR mon = MonitorFromPoint(cp, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    GetDpiForMonitorFn fn = shcore ? (GetDpiForMonitorFn)(void *)GetProcAddress(shcore, "GetDpiForMonitor") : NULL;
    if (!fn || FAILED(fn(mon, 0, &dx, &dy))) dx = GetDpiForSystem();
    if (shcore) FreeLibrary(shcore);
    e->dpi = (int)dx;
    make_fonts(e);
    layout_bar(e);

    Rc wa = rc_from_rect(mi.rcWork);
    int chrome_w = S(e, 40), chrome_h = S(e, 60) + e->top;
    int max_w = (int)(wa.w * 0.92) - chrome_w, max_h = (int)(wa.h * 0.92) - chrome_h;
    e->zoom = 1.0;
    if (img->w > max_w || img->h > max_h) e->zoom = fmin((double)max_w / img->w, (double)max_h / img->h);
    if (e->zoom < 0.05) e->zoom = 0.05;
    int w = imax(S(e, 820), (int)(img->w * e->zoom) + chrome_w);
    int hh = imax(S(e, 420), (int)(img->h * e->zoom) + chrome_h);
    w = imin(w, wa.w);
    hh = imin(hh, wa.h);
    HWND h = CreateWindowExW(0, WINDOW_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW,
                             wa.x + (wa.w - w) / 2, wa.y + (wa.h - hh) / 2, w, hh, NULL, NULL, g_inst, e);
    if (!h) { editor_free(e); return NULL; }
    e->created = true;
    layout_bar(e);
    clamp_view(e);
    update_title(e);
    e->cursor = LoadCursorW(NULL, IDC_ARROW);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    return h;
}

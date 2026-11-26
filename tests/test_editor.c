/* Dofram - screenshot tool for Windows
 * Copyright (C) 2026 Dogukan Sahil and Dofram contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Off-screen tests. The editor is driven with synthetic mouse and keyboard
 * calls on hidden windows (no real input is sent) and every rendered frame is
 * written as PNG to build\test\ for inspection. The pure logic of the update
 * check, the command line and the self-installer is tested too; those tests
 * only read the system, they never install or change anything.
 * Build and run with: build.bat test
 */
#include "../src/editor.c"
#include "../src/settingsdlg.c"

#include <stdarg.h>

HINSTANCE g_inst;
static int g_failures, g_checks;
static Img *g_last_result;
static Outcome g_last_outcome;
static wchar_t g_out[MAX_PATH];

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; wprintf(L"FAIL %hs:%d  %hs\n", __FILE__, __LINE__, #cond); } } while (0)

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
void app_notify(const wchar_t *t, const wchar_t *x, const wchar_t *p, bool w) { (void)p; (void)w; wprintf(L"notify: %s | %s\n", t, x); }
void app_open_folder(const wchar_t *f, const wchar_t *s) { (void)f; (void)s; }
bool app_save_image(Img *img, bool c, HWND o) { (void)img; (void)c; (void)o; return true; }
bool app_save_image_as(Img *img, HWND o) { (void)img; (void)o; return true; }
bool app_copy_image(Img *img, HWND o) { (void)img; (void)o; return true; }
void app_overlay_done(Outcome o, Img *r)
{
    g_last_outcome = o;
    if (g_last_result) img_free(g_last_result);
    g_last_result = r;
}

/* ---- helpers ---- */
static void save(Img *img, const wchar_t *name)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%s\\%s.png", g_out, name);
    CHECK(img_save(img, p, 90));
}

static void frame(Editor *e, const wchar_t *name)
{
    Rc c = client_rect(e);
    paint(e, c);
    save(e->back, name);
}

static void down(Editor *e, int x, int y) { on_mouse_down(e, pt(x, y), false, false); }
static void move(Editor *e, int x, int y) { on_mouse_move(e, pt(x, y)); }
static void up(Editor *e, int x, int y) { on_mouse_up(e, pt(x, y), true); }
static void drag(Editor *e, int x1, int y1, int x2, int y2)
{
    down(e, x1, y1);
    for (int i = 1; i <= 8; i++) move(e, x1 + (x2 - x1) * i / 8, y1 + (y2 - y1) * i / 8);
    up(e, x2, y2);
}

static Img *fake_screen(int w, int h)
{
    Img *img = img_new(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            img->px[y * w + x] = 0xFF000000u | ((uint32_t)(x * 255 / w) << 16) | ((uint32_t)(y * 255 / h) << 8) | 0x80;
    HFONT f = ui_font(14, false, 96);
    HGDIOBJ old = SelectObject(img->dc, f);
    SetBkMode(img->dc, TRANSPARENT);
    SetTextColor(img->dc, RGB(0, 0, 0));
    for (int y = 40; y < h; y += 60)
        TextOutW(img->dc, 40, y, L"The quick brown fox jumps over the lazy dog. 0123456789", 55);
    SelectObject(img->dc, old);
    DeleteObject(f);
    GdiFlush();
    fx_opaque(img);
    return img;
}

/* Creates an overlay editor on a hidden window of the given size. */
static Editor *hidden_overlay(int w, int h, bool quick)
{
    Img *shot = fake_screen(w, h);
    Editor *e = editor_new(shot, false);
    e->quick = quick;
    e->virt = rc_make(0, 0, w, h);
    e->dpi = 96;
    e->dim = img_new(w, h);
    img_copy_rect(e->dim, shot, e->full);
    fx_darken(e->dim, 145);
    make_fonts(e);
    CreateWindowExW(WS_EX_TOOLWINDOW, OVERLAY_CLASS, L"test", WS_POPUP, -20000, -20000, w, h, NULL, NULL, g_inst, e);
    e->cur = pt(300, 200);
    e->has_mouse = true;
    return e;
}

static Editor *hidden_window(int iw, int ih, int cw, int ch)
{
    Img *img = fake_screen(iw, ih);
    Editor *e = editor_new(img, true);
    e->crop = e->full;
    e->mode = MODE_EDITING;
    e->dpi = 96;
    make_fonts(e);
    CreateWindowExW(0, WINDOW_CLASS, L"test", WS_POPUP, -20000, -20000, cw, ch, NULL, NULL, g_inst, e);
    layout_bar(e);
    e->zoom = 0.5;
    clamp_view(e);
    return e;
}

/* ---- tests ---- */

static void test_hotkeys(void)
{
    Hotkey h = hotkey_parse(L"Ctrl+Shift+X");
    CHECK(h.ctrl && h.shift && !h.alt && !h.win && h.vk == 'X');
    wchar_t t[64];
    hotkey_format(&h, t, 64);
    CHECK(wcscmp(t, L"Ctrl+Shift+X") == 0);
    h = hotkey_parse(L"PrintScreen");
    CHECK(h.vk == VK_SNAPSHOT && !h.ctrl);
    h = hotkey_parse(L"win + alt + F12");
    CHECK(h.win && h.alt && h.vk == VK_F12);
    hotkey_format(&h, t, 64);
    CHECK(wcscmp(t, L"Alt+Win+F12") == 0);
    h = hotkey_parse(L"Ctrl+Num5");
    CHECK(h.vk == VK_NUMPAD5);
    h = hotkey_parse(L"Ctrl+");
    CHECK(!hotkey_valid(&h));
}

static void test_settings(void)
{
    settings_load();
    CHECK(wcscmp(g_cfg.hotkey, L"Ctrl+Shift+X") == 0 || g_cfg.hotkey[0]);
    Settings keep = g_cfg;
    wcscpy_s(g_cfg.hotkey, 64, L"Alt+F9");
    g_cfg.after = AFTER_BOTH;
    g_cfg.jpg = true;
    g_cfg.default_tool = TOOL_ARROW;
    g_cfg.tool_size = 7;
    wcscpy_s(g_cfg.pattern, 128, L"shot_{date}");
    CHECK(settings_save());
    memset(&g_cfg, 0, sizeof(g_cfg));
    settings_load();
    CHECK(wcscmp(g_cfg.hotkey, L"Alt+F9") == 0);
    CHECK(g_cfg.after == AFTER_BOTH);
    CHECK(g_cfg.jpg);
    CHECK(g_cfg.default_tool == TOOL_ARROW);
    CHECK(g_cfg.tool_size == 7);
    CHECK(wcscmp(g_cfg.pattern, L"shot_{date}") == 0);
    g_cfg = keep;
    settings_save();
}

static void test_effects(void)
{
    Img *a = fake_screen(200, 120);
    uint32_t before = a->px[60 * 200 + 100];
    fx_blur(a, rc_make(50, 30, 100, 60), 8);
    fx_pixelate(a, rc_make(0, 0, 40, 40), 10);
    CHECK(a->px[0] == a->px[9] && a->px[0] == a->px[9 * 200 + 9]);
    fx_fill(a, rc_make(150, 0, 50, 50), 0x123456);
    CHECK((a->px[10 * 200 + 160] & 0xFFFFFF) == 0x123456);
    fx_multiply(a, rc_make(0, 100, 200, 20), 0xFFFF00);
    CHECK((a->px[110 * 200 + 50] & 0xFF) == 0);
    (void)before;
    Img *c = img_crop(a, rc_make(10, 10, 30, 20));
    CHECK(c && c->w == 30 && c->h == 20 && c->px[0] == a->px[10 * 200 + 10]);
    save(a, L"effects");
    img_free(c);
    img_free(a);
}

static void test_overlay(void)
{
    Editor *e = hidden_overlay(1280, 760, false);
    frame(e, L"01_selecting");
    drag(e, 120, 100, 900, 520);
    CHECK(e->mode == MODE_EDITING);
    CHECK(e->crop.x == 120 && e->crop.y == 100 && e->crop.w == 781 && e->crop.h == 421);
    frame(e, L"02_editing");

    set_tool(e, TOOL_BLUR);
    drag(e, 150, 130, 400, 220);
    CHECK(e->nobjs == 1 && e->sel == e->objs[0]);
    set_tool(e, TOOL_PIXELATE);
    drag(e, 420, 130, 600, 220);
    set_tool(e, TOOL_BOX);
    drag(e, 620, 130, 760, 200);
    set_tool(e, TOOL_FRAME);
    drag(e, 150, 250, 380, 380);
    set_tool(e, TOOL_ARROW);
    drag(e, 420, 450, 640, 300);
    set_tool(e, TOOL_PEN);
    drag(e, 680, 300, 860, 480);
    set_tool(e, TOOL_HIGHLIGHT);
    drag(e, 140, 400, 500, 430);
    CHECK(e->nobjs == 7);

    set_tool(e, TOOL_TEXT);
    down(e, 700, 230);
    CHECK(e->edit != NULL);
    SetWindowTextW(e->edit, L"Hello, Dofram!");
    commit_text(e);
    CHECK(e->nobjs == 8 && e->objs[7]->kind == TOOL_TEXT && wcscmp(e->objs[7]->text, L"Hello, Dofram!") == 0);
    frame(e, L"03_all_tools");

    /* select and move the box, then undo / redo */
    set_tool(e, TOOL_SELECT);
    Op *box = e->objs[2];
    Rc r0 = box->r;
    drag(e, 690, 160, 740, 190);
    CHECK(e->sel == box && box->r.x == r0.x + 50 && box->r.y == r0.y + 30);
    frame(e, L"04_moved_box");
    undo(e);
    CHECK(e->objs[2]->r.x == r0.x && e->objs[2]->r.y == r0.y);
    redo(e);
    CHECK(e->objs[2]->r.x == r0.x + 50);

    /* resize the crop from its bottom-right handle */
    drag(e, 901, 521, 1000, 600);
    CHECK(rc_right(e->crop) == 1000 && rc_bottom(e->crop) == 600);
    /* size change on the selected object */
    e->sel = e->objs[3];
    set_size(e, 9);
    CHECK(e->objs[3]->size == 9);
    /* palette open */
    toggle_palette(e, true);
    e->hover = BTN_PAL + 2;
    frame(e, L"05_palette");
    set_color(e, PALETTE[6]);
    CHECK(e->objs[3]->color == PALETTE[6]);
    toggle_palette(e, false);

    /* rendered result equals a full re-render */
    Img *inc = img_crop(e->rendered, e->crop);
    render_all(e);
    Img *fullr = img_crop(e->rendered, e->crop);
    CHECK(inc && fullr && memcmp(inc->px, fullr->px, (size_t)inc->w * inc->h * 4) == 0);
    img_free(inc);
    img_free(fullr);

    /* delete selected */
    int n = e->nobjs;
    e->sel = e->objs[0];
    delete_selected(e);
    CHECK(e->nobjs == n - 1);

    Rc crop = e->crop;
    editor_close(e, OUTCOME_COPY);
    CHECK(g_last_outcome == OUTCOME_COPY && g_last_result && g_last_result->w == crop.w && g_last_result->h == crop.h);
    if (g_last_result) save(g_last_result, L"06_result");
}

static void test_quick_and_escape(void)
{
    Editor *e = hidden_overlay(800, 500, true);
    drag(e, 100, 100, 300, 200);
    CHECK(g_last_outcome == OUTCOME_QUICK && g_last_result && g_last_result->w == 201);

    e = hidden_overlay(800, 500, false);
    on_key(e, VK_ESCAPE);
    CHECK(g_last_outcome == OUTCOME_NONE && g_last_result == NULL);

    /* the same through real window messages */
    e = hidden_overlay(800, 500, false);
    HWND h = e->hwnd;
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(50, 60));
    SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(250, 160));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(250, 160));
    CHECK(e->mode == MODE_EDITING && e->crop.w == 201 && e->crop.h == 101);
    SendMessageW(h, WM_KEYDOWN, '3', 0);
    CHECK(e->tool == TOOL_BOX);
    SendMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(60, 70));
    SendMessageW(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(120, 100));
    SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(120, 100));
    CHECK(e->nobjs == 1);
    SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    CHECK(e->objs[0]->size == imin(10, g_cfg.tool_size) && e->size == g_cfg.tool_size);
    SendMessageW(h, WM_KEYDOWN, VK_DELETE, 0);
    CHECK(e->nobjs == 0);
    SendMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
    CHECK(g_last_outcome == OUTCOME_NONE && !IsWindow(h));

    e = hidden_overlay(800, 500, false);
    on_key(e, VK_RETURN);
    CHECK(e->mode == MODE_EDITING && e->crop.w == 800 && e->crop.h == 500);
    frame(e, L"07_whole_screen");
    on_key(e, VK_RETURN);
    CHECK(g_last_outcome == OUTCOME_COPY && g_last_result && g_last_result->w == 800);
}

static void test_window(void)
{
    Editor *e = hidden_window(1600, 900, 1000, 640);
    frame(e, L"08_window_empty");
    set_tool(e, TOOL_BOX);
    /* client (100, 200) maps to image coordinates through the zoom */
    POINT a = to_img(e, 200, 250), b = to_img(e, 400, 350);
    drag(e, 200, 250, 400, 350);
    CHECK(e->nobjs == 1);
    CHECK(abs(e->objs[0]->r.x - a.x) <= 1 && abs(rc_right(e->objs[0]->r) - 1 - b.x) <= 1);
    set_tool(e, TOOL_ARROW);
    drag(e, 500, 500, 800, 300);
    set_tool(e, TOOL_TEXT);
    down(e, 450, 220);
    SetWindowTextW(e->edit, L"Zoomed text");
    commit_text(e);
    CHECK(e->nobjs == 3);
    set_tool(e, TOOL_SELECT);
    frame(e, L"09_window_edits");
    set_zoom(e, 1.0, pt(0, 0), false);
    frame(e, L"10_window_100");
    CHECK(e->dirty);
    e->dirty = false;
    DestroyWindow(e->hwnd);
}

/* Renders the settings dialog off-screen with PrintWindow. */
static void test_settings_dialog(void)
{
    HWND dlg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, dialog_proc, 0);
    CHECK(dlg != NULL);
    if (!dlg) return;
    RECT r;
    GetWindowRect(dlg, &r);
    SetWindowPos(dlg, NULL, -20000, -20000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(dlg, SW_SHOWNOACTIVATE);
    Img *img = img_new(r.right - r.left, r.bottom - r.top);
    PrintWindow(dlg, img->dc, 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    fx_opaque(img);
    save(img, L"11_settings");
    img_free(img);
    wchar_t t[64];
    GetDlgItemTextW(dlg, IDC_HOTKEY, t, 64);
    CHECK(wcscmp(t, g_cfg.hotkey) == 0);
    DestroyWindow(dlg);
}

/* ---- pure logic of the update check, command line and installer (nothing on the system changes) ---- */

static void test_versions(void)
{
    int v[3];
    CHECK(upd_parse_version("v1.2.3", v) && v[0] == 1 && v[1] == 2 && v[2] == 3);
    CHECK(upd_parse_version("1.10", v) && v[0] == 1 && v[1] == 10 && v[2] == 0);
    CHECK(upd_parse_version("V2.0.1-beta.4", v) && v[0] == 2 && v[1] == 0 && v[2] == 1);
    CHECK(upd_parse_version("3", v) && v[0] == 3 && v[1] == 0);
    CHECK(!upd_parse_version("latest", v));
    CHECK(!upd_parse_version("", v));
    CHECK(!upd_parse_version(NULL, v));
    CHECK(!upd_parse_version("v99999999.0.0", v));

    int a[3] = { 1, 0, 0 }, b[3] = { 1, 0, 1 }, c[3] = { 1, 1, 0 }, d[3] = { 2, 0, 0 }, e[3] = { 1, 0, 0 };
    CHECK(upd_compare(a, b) < 0 && upd_compare(b, a) > 0);
    CHECK(upd_compare(b, c) < 0 && upd_compare(c, d) < 0);
    CHECK(upd_compare(a, e) == 0);
    int nine[3] = { 1, 9, 0 }, ten[3] = { 1, 10, 0 };
    CHECK(upd_compare(nine, ten) < 0); /* numeric, not text order */

    int have[3] = { DOFRAM_VERSION_MAJOR, DOFRAM_VERSION_MINOR, DOFRAM_VERSION_PATCH };
    CHECK(upd_parse_version(DOFRAM_VERSION_STR, v) && upd_compare(v, have) == 0);
}

static void test_release_json(void)
{
    static const char json[] =
        "{\n  \"url\": \"https://api.github.com/repos/dogukansahil/Dofram/releases/1\",\n"
        "  \"html_url\": \"https://github.com/dogukansahil/Dofram/releases/tag/v1.2.0\",\n"
        "  \"author\": { \"login\": \"x\", \"html_url\": \"https://github.com/x\" },\n"
        "  \"name\": \"mentions \\\"tag_name\\\" in text\",\n"
        "  \"tag_name\" :\t\"v1.2.0\",\n  \"body\": \"notes\"\n}";
    char out[64], url[512];
    CHECK(upd_json_string(json, "tag_name", out, sizeof(out)) && strcmp(out, "v1.2.0") == 0);
    CHECK(upd_json_string(json, "html_url", url, sizeof(url)) &&
          strcmp(url, "https://github.com/dogukansahil/Dofram/releases/tag/v1.2.0") == 0);
    CHECK(!upd_json_string(json, "missing", out, sizeof(out)) && out[0] == 0);
    CHECK(!upd_json_string(json, "name", out, sizeof(out)));          /* escapes are refused */
    CHECK(!upd_json_string("{\"tag_name\": \"v1.2.0\"}", "tag_name", out, 4)); /* does not fit */
    CHECK(!upd_json_string("{\"tag_name\": 5}", "tag_name", out, sizeof(out)));
    CHECK(!upd_json_string("{\"tag_name\": \"v1.2", "tag_name", out, sizeof(out)));
    /* a 404 answer has no tag */
    CHECK(!upd_json_string("{\"message\":\"Not Found\",\"documentation_url\":\"https://docs.github.com\"}", "tag_name",
                           out, sizeof(out)));

    CHECK(upd_release_url_ok("https://github.com/dogukansahil/Dofram/releases/tag/v1.2.0"));
    CHECK(!upd_release_url_ok("https://github.com/dogukansahil/DoframX/releases"));
    CHECK(!upd_release_url_ok("https://evil.example/dogukansahil/Dofram/"));
    CHECK(!upd_release_url_ok("https://github.com/dogukansahil/Dofram/a b"));
    CHECK(!upd_release_url_ok(NULL));
}

static void test_args(void)
{
    AppArgs a;
    wchar_t *v1[] = { L"Dofram.exe" };
    args_parse(1, v1, &a);
    CHECK(!a.tray && !a.capture && !a.settings && !a.install && !a.uninstall && !a.quiet && !a.portable && !a.elevated_child);
    wchar_t *v2[] = { L"Dofram.exe", L"--TRAY", L"--capture", L"--settings", L"--portable", L"unknown" };
    args_parse(6, v2, &a);
    CHECK(a.tray && a.capture && a.settings && a.portable && !a.install);
    wchar_t *v3[] = { L"Dofram.exe", L"--install", L"--quiet" };
    args_parse(3, v3, &a);
    CHECK(a.install && a.quiet && !a.uninstall);
    wchar_t *v4[] = { L"Dofram.exe", L"--uninstall", L"--quiet", L"--elevated" };
    args_parse(4, v4, &a);
    CHECK(a.uninstall && a.quiet && a.elevated_child);
    args_parse(0, NULL, &a);
    CHECK(!a.tray && !a.install);
}

static void test_install_logic(void)
{
    AppArgs plain, tray, capture, portable, install;
    memset(&plain, 0, sizeof(plain));
    tray = capture = portable = install = plain;
    tray.tray = true;
    capture.capture = true;
    portable.portable = true;
    install.install = true;

    InstallState fresh = { false, false, true, false, NULL };
    CHECK(inst_offer_kind(&plain, &fresh) == OFFER_INSTALL);
    CHECK(inst_offer_kind(&tray, &fresh) == OFFER_NONE);      /* start-up entry: never interrupt */
    CHECK(inst_offer_kind(&capture, &fresh) == OFFER_NONE);   /* capture shortcut */
    CHECK(inst_offer_kind(&portable, &fresh) == OFFER_NONE);  /* --portable */
    CHECK(inst_offer_kind(&install, &fresh) == OFFER_NONE);

    InstallState st = fresh;
    st.portable_ini = true;
    CHECK(inst_offer_kind(&plain, &st) == OFFER_NONE);        /* dofram.ini next to the exe */
    st = fresh;
    st.running_installed = true;
    CHECK(inst_offer_kind(&plain, &st) == OFFER_NONE);
    st = fresh;
    st.ask_enabled = false;
    CHECK(inst_offer_kind(&plain, &st) == OFFER_NONE);        /* "Don't ask me again" */
    st = fresh;
    st.installed = true;
    st.installed_version = DOFRAM_VERSION_WSTR;
    CHECK(inst_offer_kind(&plain, &st) == OFFER_NONE);        /* same version already installed */
    st.installed_version = L"0.9.0";
    CHECK(inst_offer_kind(&plain, &st) == OFFER_UPDATE);
    st.installed_version = NULL;
    CHECK(inst_offer_kind(&plain, &st) == OFFER_UPDATE);

    CHECK(inst_path_in_dir(L"C:\\Program Files\\Dofram\\Dofram.exe", L"C:\\Program Files\\Dofram"));
    CHECK(inst_path_in_dir(L"c:\\program files\\dofram\\DOFRAM.EXE", L"C:\\Program Files\\Dofram\\"));
    CHECK(!inst_path_in_dir(L"C:\\Program Files\\Dofram2\\Dofram.exe", L"C:\\Program Files\\Dofram"));
    CHECK(!inst_path_in_dir(L"C:\\Program Files\\Dofram\\sub\\Dofram.exe", L"C:\\Program Files\\Dofram"));
    CHECK(!inst_path_in_dir(L"C:\\Users\\me\\Downloads\\Dofram.exe", L"C:\\Program Files\\Dofram"));
    CHECK(!inst_path_in_dir(L"", L"C:\\Program Files\\Dofram"));

    const wchar_t *exe = L"C:\\Program Files\\Dofram\\Dofram.exe";
    CHECK(inst_command_targets(L"\"C:\\Program Files\\Dofram\\Dofram.exe\" --tray", exe));
    CHECK(inst_command_targets(L"  \"c:\\program files\\dofram\\dofram.exe\"", exe));
    CHECK(inst_command_targets(L"C:\\Tools\\Dofram.exe --tray", L"C:\\Tools\\Dofram.exe"));
    CHECK(!inst_command_targets(L"\"D:\\Portable\\Dofram.exe\" --tray", exe));
    CHECK(!inst_command_targets(L"\"C:\\Program Files\\Dofram\\Dofram.exe", exe));
    CHECK(!inst_command_targets(L"", exe));

    /* where an installation would live, read only */
    wchar_t dir[MAX_PATH], target[MAX_PATH];
    inst_install_dir(dir, MAX_PATH);
    inst_installed_exe(target, MAX_PATH);
    CHECK(inst_path_in_dir(target, dir));
    size_t n = wcslen(target);
    CHECK(n > 18 && _wcsicmp(target + n - 18, L"\\Dofram\\Dofram.exe") == 0);
    CHECK(!inst_running_installed());   /* the test program runs from build\test */
    CHECK(settings_is_portable());      /* build.bat puts a dofram.ini next to it */
}

static void test_settings_install_flag(void)
{
    settings_load();
    Settings keep = g_cfg;
    CHECK(g_cfg.ask_to_install);        /* default: ask */
    g_cfg.ask_to_install = false;
    CHECK(settings_save());
    g_cfg.ask_to_install = true;
    settings_load();
    CHECK(!g_cfg.ask_to_install);
    g_cfg = keep;
    settings_save();
}

int wmain(int argc, wchar_t **argv)
{
    (void)argc; (void)argv;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = GetModuleHandleW(NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, NULL);
    editor_register(g_inst);

    util_exe_path(g_out, MAX_PATH);
    wchar_t *slash = wcsrchr(g_out, L'\\');
    if (slash) *slash = 0;
    wcscat_s(g_out, MAX_PATH, L"\\frames");
    util_ensure_dir(g_out);

    test_hotkeys();
    test_settings();
    test_effects();
    test_overlay();
    test_quick_and_escape();
    test_window();
    test_settings_dialog();
    test_versions();
    test_release_json();
    test_args();
    test_install_logic();
    test_settings_install_flag();

    if (g_last_result) img_free(g_last_result);
    GdiplusShutdown(tok);
    wprintf(L"%d checks, %d failed. Frames in %s\n", g_checks, g_failures, g_out);
    return g_failures ? 1 : 0;
}

/* The overlay: a minimal search bar (think Spotlight / PowerToys Run) with a
 * small mic dot, one text box and a plain results list. It follows the
 * Windows app theme (light/dark) and the system accent colour.
 *
 * Background mode: the bar stays hidden; JARVIS just listens, acts and talks
 * back. While listening a tiny pill near the bottom of the screen shows the
 * mic dot and what was heard. The pill never takes focus, so an Upload/Open
 * dialog in front keeps it. The bar appears only when it is needed (a list
 * to pick from, voice unavailable, or "show window").
 *
 * Plain GDI, double buffered. The mic dot is drawn at 3x into a small DIB
 * and scaled down with HALFTONE for cheap anti-aliasing. A timer runs only
 * while listening, and the process exits as soon as the work is done. */
#include "jarvis.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

/* DWM attributes (defined here: older MinGW headers lack some of them) */
#define JV_DWMWA_DARK_MODE   20
#define JV_DWMWA_CORNER      33
#define JV_DWMWA_BORDER      34
#define JV_DWMWCP_ROUND      2

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#define PILL_CLASS  L"JarvisPillWnd"

/* layout, logical px (96 dpi) */
#define L_W          600
#define L_INPUT_H    56
#define L_STATUS_H   24
#define L_DOT_X      24        /* mic dot centre */
#define L_DOT_BOX    22        /* dot drawing box (room for the halo) */
#define L_EDIT_X     46
#define L_PAD_R      20
#define L_ROWS_PAD   6
#define L_ROW_H      40
#define L_HINT_H     30
#define L_PILL_H     32
#define L_PILL_MIN   140
#define L_PILL_MAX   460
#define L_PILL_SHADE 10        /* room around the pill for its soft shadow */
#define L_PILL_LIFT  64        /* distance from the bottom of the work area */

/* timers */
#define T_ANIM     1
#define T_PREVIEW  2
#define T_CLOSE    3
#define T_CLOSECAP 4

#define ANIM_MS        33
#define PREVIEW_MS     120
#define GRACE_MS       400
#define CLOSE_CAP_MS   4000
#define SS             3          /* dot supersampling factor */

#define EDIT_ID        100
#define WM_APP_CLOSE   (WM_APP + 20)
#define WM_APP_NORMAL  (WM_APP + 21)   /* pill clicked: show the bar */

enum { ST_IDLE, ST_HINT, ST_REPLY };
enum { MP_NONE, MP_HIDE, MP_SHOW };

typedef struct {
    COLORREF bg, text, dim, sel, hover, line, accent;
    bool light;
} Theme;

static Theme T;

static struct {
    HINSTANCE inst;
    HWND  hwnd, edit, dialog, pill;
    UINT  dpi;
    RECT  work;                 /* work area of our monitor */

    HFONT fEdit, fName, fSmall, fStatus, fIcon;
    HBRUSH brBg;

    /* mic dot offscreen (3x) */
    HDC     ddc;
    HBITMAP dbmp, dold;
    int     dsize;

    /* state */
    bool  bg;                   /* background (hidden) mode */
    bool  listening, listen_requested, speaking, busy, closing, anim_on;
    int   voice_state;          /* 0 unknown, 1 ok, -1 unavailable */
    double level, level_target; /* mic level 0..1 */
    DWORD shown_at;

    wchar_t status[320];
    int     status_kind;
    wchar_t hyp[320];           /* what was heard (live) */
    wchar_t cue[64];

    Row   rows[MAX_HITS];
    int   nrows, sel, hover;
    bool  user_sel, rows_preview;
    PickMode pick_mode;

    bool  suppress_change, ctrl_space, ctrl_h, edit_from_cmd;

    bool  close_pending, close_min_done, close_spoken;
    bool  tracking;
} g;

static CmdResult g_res;   /* big: keep it off the stack */

static void pill_refresh(void);
static void go_normal(void);

/* ------------------------------------------------------------- helpers */
static int S(int v) { return MulDiv(v, (int)g.dpi, 96); }

static COLORREF mix(COLORREF a, COLORREF b, double t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

static bool reg_dword(const wchar_t *key, const wchar_t *name, DWORD *out)
{
    DWORD sz = sizeof *out;
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, out, &sz) == ERROR_SUCCESS;
}

static bool colourful(COLORREF c)
{
    int r = GetRValue(c), gg = GetGValue(c), b = GetBValue(c);
    int mx = max(r, max(gg, b)), mn = min(r, min(gg, b));
    return mx - mn >= 48 && mx >= 70;
}

static void load_theme(void)
{
    DWORD v = 0;
    T.light = reg_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"AppsUseLightTheme", &v) && v;
    if (T.light) {
        T.bg = RGB(0xFA, 0xFA, 0xFA); T.text = RGB(0x1A, 0x1A, 0x1A);
        T.dim = RGB(0x6B, 0x6B, 0x6B); T.sel = RGB(0xEB, 0xEB, 0xEB);
    } else {
        T.bg = RGB(0x1F, 0x1F, 0x1F); T.text = RGB(0xF2, 0xF2, 0xF2);
        T.dim = RGB(0x9A, 0x9A, 0x9A); T.sel = RGB(0x2D, 0x2D, 0x2D);
    }
    T.hover = mix(T.bg, T.sel, 0.55);
    T.line  = mix(T.bg, T.text, T.light ? 0.10 : 0.12);

    /* accent: the real accent colour (ABGR), else DWM colorization (ARGB), else calm blue */
    T.accent = RGB(0x4C, 0x8B, 0xF5);
    COLORREF c;
    BOOL opaque;
    if (reg_dword(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", &v) &&
        colourful(c = (COLORREF)(v & 0xFFFFFF)))
        T.accent = c;
    else if (SUCCEEDED(DwmGetColorizationColor(&v, &opaque)) &&
             colourful(c = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255)))
        T.accent = c;

    if (g.brBg) DeleteObject(g.brBg);
    g.brBg = CreateSolidBrush(T.bg);

    if (g.hwnd) {
        BOOL dark = !T.light;
        DWORD corner = JV_DWMWCP_ROUND;
        COLORREF border = T.line;
        DwmSetWindowAttribute(g.hwnd, JV_DWMWA_DARK_MODE, &dark, sizeof dark);
        DwmSetWindowAttribute(g.hwnd, JV_DWMWA_CORNER, &corner, sizeof corner);
        DwmSetWindowAttribute(g.hwnd, JV_DWMWA_BORDER, &border, sizeof border);
    }
}

static int header_h(void) { return L_INPUT_H + (g.status[0] ? L_STATUS_H : 0); }

static int client_h(void)
{
    return header_h() + (g.nrows ? 1 + L_ROWS_PAD + g.nrows * L_ROW_H + L_HINT_H : 0);
}

static RECT dot_rect(void)
{
    int l = S(L_DOT_X) - S(L_DOT_BOX) / 2, t = S(L_INPUT_H) / 2 - S(L_DOT_BOX) / 2;
    RECT r = { l, t, l + S(L_DOT_BOX), t + S(L_DOT_BOX) };
    return r;
}

static RECT status_rect(void)
{
    RECT r = { S(L_EDIT_X), S(L_INPUT_H - 8), S(L_W - L_PAD_R), S(L_INPUT_H - 8 + L_STATUS_H) };
    return r;
}

static RECT row_rect(int i)
{
    int y = S(header_h() + 1 + L_ROWS_PAD + i * L_ROW_H);
    RECT r = { S(8), y, S(L_W - 8), y + S(L_ROW_H) };
    return r;
}

static bool font_is(HFONT f, const wchar_t *face)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE] = L"";
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return _wcsicmp(got, face) == 0;
}

static HFONT mkfont(const wchar_t *face, int px, int weight, const wchar_t *fallback)
{
    HFONT f = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    if (fallback && !font_is(f, face)) {
        DeleteObject(f);
        f = CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, fallback);
    }
    return f;
}

static void free_fonts(void)
{
    HFONT *fs[] = { &g.fEdit, &g.fName, &g.fSmall, &g.fStatus, &g.fIcon };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++)
        if (*fs[i]) { DeleteObject(*fs[i]); *fs[i] = NULL; }
}

static void free_dot(void)
{
    if (g.ddc) {
        SelectObject(g.ddc, g.dold);
        DeleteObject(g.dbmp);
        DeleteDC(g.ddc);
        g.ddc = NULL;
    }
}

#define UI_FONT  L"Segoe UI Variable Text"

static void make_resources(void)
{
    free_fonts();
    g.fEdit   = mkfont(UI_FONT, 16, FW_NORMAL, L"Segoe UI");
    g.fName   = mkfont(UI_FONT, 14, FW_NORMAL, L"Segoe UI");
    g.fStatus = mkfont(UI_FONT, 13, FW_NORMAL, L"Segoe UI");
    g.fSmall  = mkfont(UI_FONT, 12, FW_NORMAL, L"Segoe UI");
    g.fIcon   = mkfont(L"Segoe Fluent Icons", 14, FW_NORMAL, L"Segoe MDL2 Assets");
    free_dot();
}

static int text_w(HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = { 0, 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return sz.cx;
}

#define UPLOAD_LABEL L"\x2192 Upload window"

/* place the edit control in the input row */
static void layout_edit(void)
{
    if (!g.edit) return;
    SendMessageW(g.edit, WM_SETFONT, (WPARAM)g.fEdit, FALSE);
    HDC dc = GetDC(g.edit);
    HGDIOBJ o = SelectObject(dc, g.fEdit);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, o);
    ReleaseDC(g.edit, dc);
    int h = tm.tmHeight + 2;
    int x = S(L_EDIT_X);
    int right = S(L_W - L_PAD_R);
    if (g.dialog) right -= text_w(g.fSmall, UPLOAD_LABEL) + S(16);
    SetWindowPos(g.edit, NULL, x, (S(L_INPUT_H) - h) / 2, right - x, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void resize_window(bool center)
{
    int w = S(L_W), h = S(client_h());
    if (center) {
        int x = g.work.left + (g.work.right - g.work.left - w) / 2;
        int y = g.work.top + (g.work.bottom - g.work.top) * 20 / 100;
        if (y + h > g.work.bottom) y = g.work.bottom - h;
        if (y < g.work.top) y = g.work.top;
        SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    } else {
        RECT wr;
        GetWindowRect(g.hwnd, &wr);
        int y = wr.top;
        if (y + h > g.work.bottom) y = g.work.bottom - h;
        if (y < g.work.top) y = g.work.top;
        SetWindowPos(g.hwnd, NULL, wr.left, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(g.hwnd, NULL, FALSE);
}

static void set_status(const wchar_t *s, int kind)
{
    bool had = g.status[0] != 0;
    str_copy(g.status, 320, s ? s : L"");
    g.status_kind = g.status[0] ? kind : ST_IDLE;
    if (had != (g.status[0] != 0)) resize_window(false);
    else {
        RECT r = status_rect();
        InvalidateRect(g.hwnd, &r, FALSE);
    }
    pill_refresh();
}

static void invalidate_rows(void)
{
    RECT r = { 0, S(header_h()), S(L_W), S(client_h()) };
    InvalidateRect(g.hwnd, &r, FALSE);
}

static void invalidate_dot(void)
{
    RECT r = dot_rect();
    InvalidateRect(g.hwnd, &r, FALSE);
}

static void update_cue(void)
{
    const wchar_t *c = g.voice_state < 0 ? L"Search or type a command"
                     : (g.listening || g.listen_requested) ? L"Listening\x2026"
                     : L"Search or say a command";
    if (!g.edit || str_eq(c, g.cue)) return;
    str_copy(g.cue, 64, c);
    SendMessageW(g.edit, EM_SETCUEBANNER, TRUE, (LPARAM)g.cue);
}

/* ------------------------------------------------------------- mic dot */
static void disc(HDC dc, double cx, double cy, double r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r + 1), (int)(cy + r + 1));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

/* draws the dot (box of L_DOT_BOX logical px) at x,y of dst over `bg` */
static void render_dot(HDC dst, int x, int y, COLORREF bg)
{
    int D = S(L_DOT_BOX), big = D * SS;
    if (!g.ddc || g.dsize != big) {
        free_dot();
        BITMAPINFO bi = { .bmiHeader = { sizeof(BITMAPINFOHEADER), big, -big, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
        void *bits;
        HDC sdc = GetDC(NULL);
        g.ddc  = CreateCompatibleDC(sdc);
        g.dbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        ReleaseDC(NULL, sdc);
        g.dold = SelectObject(g.ddc, g.dbmp);
        g.dsize = big;
    }
    HDC dc = g.ddc;
    double u = big / (double)L_DOT_BOX, c = big / 2.0;
    RECT all = { 0, 0, big, big };
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &all, b);
    DeleteObject(b);

    const double R = 5.0;   /* 10 px dot */
    if (g.voice_state < 0) {
        /* voice unavailable: hollow ring */
        disc(dc, c, c, R * u, mix(bg, T.dim, 0.7));
        disc(dc, c, c, (R - 1.4) * u, bg);
    } else if (g.listening) {
        double t = GetTickCount() / 1000.0;
        double breath = 0.5 + 0.5 * sin(t * 3.6);
        double lv = g.level;
        double halo = R + 1.2 + 2.6 * lv + 1.0 * breath;
        disc(dc, c, c, halo * u, mix(bg, T.accent, 0.16 + 0.14 * lv));
        disc(dc, c, c, R * u, T.accent);
    } else if (g.listen_requested || g.speaking || g.busy) {
        disc(dc, c, c, R * u, mix(bg, T.accent, 0.65));
    } else {
        disc(dc, c, c, R * u, mix(bg, T.dim, 0.75));
    }
    SetStretchBltMode(dst, HALFTONE);
    SetBrushOrgEx(dst, 0, 0, NULL);
    StretchBlt(dst, x, y, D, D, dc, 0, 0, big, big, SRCCOPY);
}

/* --------------------------------------------------------------- paint */
static void fill_round(HDC dc, const RECT *r, int rad, COLORREF fill)
{
    HBRUSH b = CreateSolidBrush(fill);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r->left, r->top, r->right + 1, r->bottom + 1, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

static void fill_rect(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void text_at(HDC dc, HFONT f, COLORREF col, const wchar_t *s, RECT *r, UINT fmt)
{
    SelectObject(dc, f);
    SetTextColor(dc, col);
    DrawTextW(dc, s, -1, r, fmt | DT_NOPREFIX);
}

static const wchar_t *row_glyph(const Row *row)
{
    if (row->kind != ROW_HIT) return L"\xE946";            /* info */
    if (row->hit.is_dir) return L"\xE8B7";                 /* folder */
    const wchar_t *dot = wcsrchr(path_name(row->hit.path), L'.');
    if (dot && (!_wcsicmp(dot, L".lnk") || !_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".url")))
        return L"\xE7AC";                                  /* app */
    switch (ext_class_of(row->hit.path)) {
    case EXT_IMAGE: return L"\xEB9F";
    case EXT_VIDEO: return L"\xE714";
    case EXT_AUDIO: return L"\xE8D6";
    default:        return L"\xE8A5";                      /* document */
    }
}

static void paint_row(HDC dc, int i)
{
    const Row *row = &g.rows[i];
    RECT r = row_rect(i);
    bool sel = (i == g.sel && g.user_sel);
    bool hov = (i == g.hover);
    if (sel || hov) fill_round(dc, &r, S(8), sel ? T.sel : T.hover);

    UINT one = DT_SINGLELINE | DT_VCENTER;
    wchar_t num[4] = { (wchar_t)(L'1' + i), 0 };
    RECT nr = { r.left + S(6), r.top, r.left + S(24), r.bottom };
    text_at(dc, g.fSmall, sel ? T.text : T.dim, num, &nr, one | DT_CENTER);

    RECT ic = { r.left + S(28), r.top, r.left + S(50), r.bottom };
    text_at(dc, g.fIcon, T.dim, row_glyph(row), &ic, one | DT_CENTER);

    const wchar_t *name;
    wchar_t sub[MAX_PATH];
    if (row->kind == ROW_HIT) {
        name = path_name(row->hit.path);
        if (!name[0]) name = row->hit.path;
        str_copy(sub, MAX_PATH, row->hit.path);
        size_t cut = (size_t)(name - row->hit.path);
        if (cut > 0 && cut < MAX_PATH) {
            sub[cut] = 0;
            size_t n = wcslen(sub);
            if (n > 3 && sub[n - 1] == L'\\') sub[n - 1] = 0;  /* keep "C:\" */
        } else sub[0] = 0;
    } else {
        name = row->title;
        str_copy(sub, MAX_PATH, row->sub);
    }
    int x = r.left + S(58), right = r.right - S(12);
    int nw = text_w(g.fName, name) + 1;
    int maxn = sub[0] ? (right - x) * 60 / 100 : right - x;
    if (nw > maxn) nw = maxn;
    RECT tr = { x, r.top, x + nw, r.bottom };
    text_at(dc, g.fName, T.text, name, &tr, one | DT_END_ELLIPSIS);
    if (sub[0]) {
        RECT pr = { x + nw + S(10), r.top, right, r.bottom };
        text_at(dc, g.fSmall, T.dim, sub, &pr, one | DT_PATH_ELLIPSIS);
    }
}

static void paint_all(HDC dc, int w, int h)
{
    fill_rect(dc, 0, 0, w, h, T.bg);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g.fSmall);

    RECT d = dot_rect();
    render_dot(dc, d.left, d.top, T.bg);

    if (g.dialog) {
        RECT ur = { S(L_EDIT_X), 0, S(L_W - L_PAD_R), S(L_INPUT_H) };
        text_at(dc, g.fSmall, T.dim, UPLOAD_LABEL, &ur, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    if (g.status[0]) {
        RECT sr = status_rect();
        text_at(dc, g.fStatus, g.status_kind == ST_REPLY ? T.text : T.dim, g.status, &sr,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    }

    if (g.nrows) {
        int ly = S(header_h());
        fill_rect(dc, 0, ly, w, ly + 1, T.line);
        for (int i = 0; i < g.nrows; i++) paint_row(dc, i);

        RECT hr = { S(L_EDIT_X - 26), h - S(L_HINT_H), S(L_W - L_PAD_R), h - S(4) };
        text_at(dc, g.fSmall, T.dim, L"enter  open        shift+enter  show in folder        esc  close",
                &hr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        if (search_busy())
            text_at(dc, g.fSmall, T.dim, L"indexing\x2026", &hr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, of);
}

static void on_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    RECT dr = dot_rect(), inter;
    bool only_dot = IntersectRect(&inter, &ps.rcPaint, &dr) && EqualRect(&inter, &ps.rcPaint);
    if (only_dot) {
        render_dot(dc, dr.left, dr.top, T.bg);   /* single blit: no flicker */
    } else {
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        HGDIOBJ ob = SelectObject(mdc, bmp);
        paint_all(mdc, cr.right, cr.bottom);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
    }
    EndPaint(h, &ps);
}

/* ---------------------------------------------------------------- pill */
static void pill_hide(void)
{
    if (g.pill && IsWindowVisible(g.pill)) ShowWindow(g.pill, SW_HIDE);
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* signed distance from (px,py) to a rounded rect (centre cx,cy, half size hw,hh, radius r) */
static double sd_round(double px, double py, double cx, double cy, double hw, double hh, double r)
{
    double qx = fabs(px - cx) - (hw - r), qy = fabs(py - cy) - (hh - r);
    double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    double in = qx > qy ? qx : qy;
    return sqrt(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

/* Renders the pill into a per-pixel-alpha bitmap: GDI draws dot + text on an
 * opaque background, then each pixel gets its alpha from the (anti-aliased)
 * capsule shape, a 1px border and a soft shadow. */
static void pill_render(const wchar_t *text, bool dim)
{
    if (!g.pill) {
        g.pill = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                                 PILL_CLASS, L"JARVIS", WS_POPUP, 0, 0, 1, 1, NULL, NULL, g.inst, NULL);
        if (!g.pill) return;
    }
    int M = S(L_PILL_SHADE), ph = S(L_PILL_H);
    int pw = S(10) + S(L_DOT_BOX) + S(6) + text_w(g.fStatus, text) + S(16);
    if (pw < S(L_PILL_MIN)) pw = S(L_PILL_MIN);
    if (pw > S(L_PILL_MAX)) pw = S(L_PILL_MAX);
    int W = pw + 2 * M, H = ph + 2 * M;

    HDC sdc = GetDC(NULL);
    HDC mdc = CreateCompatibleDC(sdc);
    BITMAPINFO bi = { .bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    DWORD *px = NULL;
    HBITMAP bmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    if (!bmp || !px) { DeleteDC(mdc); ReleaseDC(NULL, sdc); return; }
    HGDIOBJ ob = SelectObject(mdc, bmp);

    COLORREF bgc = T.light ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x26, 0x26, 0x26);
    fill_rect(mdc, 0, 0, W, H, bgc);
    SetBkMode(mdc, TRANSPARENT);
    render_dot(mdc, M + S(10), M + (ph - S(L_DOT_BOX)) / 2, bgc);
    RECT tr = { M + S(10) + S(L_DOT_BOX) + S(6), M, M + pw - S(16), M + ph };
    HGDIOBJ of = SelectObject(mdc, g.fStatus);
    text_at(mdc, g.fStatus, dim ? T.dim : T.text, text, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(mdc, of);
    GdiFlush();

    COLORREF bc = T.light ? RGB(0xDA, 0xDA, 0xDA) : RGB(0x3A, 0x3A, 0x3A);
    double cx = M + pw / 2.0, cy = M + ph / 2.0, hw = pw / 2.0, hh = ph / 2.0, rad = hh;
    double sig = S(4), lift = S(2), shade = T.light ? 0.16 : 0.32;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            double fx = x + 0.5, fy = y + 0.5;
            double d = sd_round(fx, fy, cx, cy, hw, hh, rad);
            double a = clamp01(0.5 - d);
            double inner = clamp01(0.5 - (d + 1.0));
            double ds = sd_round(fx, fy - lift, cx, cy, hw, hh, rad);
            double sh = ds <= 0 ? shade : shade * exp(-(ds * ds) / (2 * sig * sig));
            DWORD p = px[y * W + x];
            double r = (p >> 16) & 255, gg = (p >> 8) & 255, b = p & 255;
            if (a > 0) {
                double t = (a - inner) / a;   /* border share */
                r = r + (GetRValue(bc) - r) * t;
                gg = gg + (GetGValue(bc) - gg) * t;
                b = b + (GetBValue(bc) - b) * t;
            }
            double A = a + (1 - a) * sh;
            px[y * W + x] = ((DWORD)(A * 255 + 0.5) << 24) | ((DWORD)(r * a + 0.5) << 16) |
                            ((DWORD)(gg * a + 0.5) << 8) | (DWORD)(b * a + 0.5);
        }
    }

    POINT pos = { g.work.left + (g.work.right - g.work.left - W) / 2,
                  g.work.bottom - S(L_PILL_LIFT) - ph - M };
    SIZE size = { W, H };
    POINT src = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g.pill, sdc, &pos, &size, mdc, &src, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(g.pill)) {
        SetWindowPos(g.pill, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    SelectObject(mdc, ob);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
}

/* shows / updates / hides the pill to match the state (background mode only) */
static void pill_refresh(void)
{
    const wchar_t *txt = NULL;
    bool dim = false;
    if (g.bg && !g.closing && g.hwnd) {
        if (g.listening || g.listen_requested) {
            if (g.hyp[0]) txt = g.hyp;
            else { txt = L"Listening\x2026"; dim = true; }
        } else if (g.status[0] && (g.speaking || g.close_pending)) {
            txt = g.status;   /* the reply, until we exit */
        }
    }
    if (txt) pill_render(txt, dim);
    else pill_hide();
}

static LRESULT CALLBACK pill_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if (g.hwnd) PostMessageW(g.hwnd, WM_APP_NORMAL, 0, 0);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ----------------------------------------------------------- animation */
static void anim_update(void)
{
    bool want = !g.closing && g.listening;
    if (want && !g.anim_on) SetTimer(g.hwnd, T_ANIM, ANIM_MS, NULL);
    else if (!want && g.anim_on) KillTimer(g.hwnd, T_ANIM);
    g.anim_on = want;
}

static void state_changed(void)
{
    update_cue();
    anim_update();
    invalidate_dot();
    pill_refresh();
}

/* ------------------------------------------------------------- closing */
static void begin_close(void)
{
    if (g.closing) return;
    g.closing = true;
    KillTimer(g.hwnd, T_ANIM);
    KillTimer(g.hwnd, T_PREVIEW);
    KillTimer(g.hwnd, T_CLOSE);
    KillTimer(g.hwnd, T_CLOSECAP);
    g.anim_on = false;
    pill_hide();
    ShowWindow(g.hwnd, SW_HIDE);
    PostMessageW(g.hwnd, WM_APP_CLOSE, 0, 0);
}

static void finish_close(void)
{
    voice_stop(3000);                       /* let the reply finish speaking */
    if (search_busy()) search_wait(60000);  /* let a background re-index save */
    DestroyWindow(g.hwnd);
}

static void cancel_close(void)
{
    g.close_pending = false;
    KillTimer(g.hwnd, T_CLOSE);
    KillTimer(g.hwnd, T_CLOSECAP);
}

static void schedule_close(bool wait_speech)
{
    g.close_pending = true;
    g.close_min_done = false;
    g.close_spoken = !wait_speech;
    int d = g_cfg.close_delay_ms > 0 ? g_cfg.close_delay_ms : 1;
    if (d > CLOSE_CAP_MS) d = CLOSE_CAP_MS;
    SetTimer(g.hwnd, T_CLOSE, (UINT)d, NULL);
    SetTimer(g.hwnd, T_CLOSECAP, CLOSE_CAP_MS, NULL);
}

/* ------------------------------------------------------------ commands */
static void set_rows(const Row *rows, int n, bool preview)
{
    if (n < 0) n = 0;
    if (n > MAX_HITS) n = MAX_HITS;
    int old = g.nrows;
    if (n) memcpy(g.rows, rows, sizeof(Row) * (size_t)n);
    g.nrows = n;
    g.sel = -1;
    g.hover = -1;
    g.user_sel = false;
    g.rows_preview = preview;
    if (old != n) resize_window(false);
    else invalidate_rows();
}

static void set_edit_text(const wchar_t *s)
{
    g.suppress_change = true;
    SetWindowTextW(g.edit, s);
    g.suppress_change = false;
    int n = GetWindowTextLengthW(g.edit);
    SendMessageW(g.edit, EM_SETSEL, n, n);
}

static void start_listen(void)
{
    if (g.voice_state < 0) return;
    cancel_close();
    g.hyp[0] = 0;
    voice_listen(true);
    g.listen_requested = true;
    state_changed();
}

static void force_foreground(void)
{
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId();
    DWORD other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    bool attach = other && other != me && AttachThreadInput(me, other, TRUE);
    SetForegroundWindow(g.hwnd);
    BringWindowToTop(g.hwnd);
    SetFocus(g.edit);
    if (attach) AttachThreadInput(me, other, FALSE);
}

/* normal -> background: hide the bar, keep listening */
static void go_background(void)
{
    if (g.bg || g.closing) return;
    if (g.voice_state < 0) {
        set_status(L"Voice unavailable \x2014 can't run in the background", ST_HINT);
        return;
    }
    log_line(L"mode: background");
    g.bg = true;                       /* first: hiding deactivates the window */
    KillTimer(g.hwnd, T_PREVIEW);
    cancel_close();
    ShowWindow(g.hwnd, SW_HIDE);       /* focus goes back to the previous app */
    set_edit_text(L"");
    g.edit_from_cmd = false;
    set_rows(NULL, 0, true);
    set_status(L"", ST_IDLE);
    if (!g.listening) start_listen();
    else { g.hyp[0] = 0; state_changed(); }
}

/* background -> normal: show the bar for the rest of this run */
static void go_normal(void)
{
    if (!g.bg || g.closing) return;
    log_line(L"mode: normal");
    g.bg = false;
    pill_hide();
    if (g.hyp[0] && !g.listening) {
        set_edit_text(g.hyp);
        SendMessageW(g.edit, EM_SETSEL, 0, -1);
        g.edit_from_cmd = true;
    }
    layout_edit();
    resize_window(true);
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    force_foreground();
    g.shown_at = GetTickCount();
    state_changed();
}

/* "hide window", "jarvis, background mode please" ... -> MP_HIDE / MP_SHOW */
static int mode_phrase(const wchar_t *text)
{
    wchar_t b[LINE_MAX_CH], out[LINE_MAX_CH] = L"";
    str_copy(b, LINE_MAX_CH, text);
    str_lower(b);
    for (wchar_t *p = b; *p; p++)
        if (!iswalnum(*p)) *p = L' ';
    wchar_t *ctx = NULL;
    for (wchar_t *t = wcstok(b, L" ", &ctx); t; t = wcstok(NULL, L" ", &ctx)) {
        if (str_eq(t, L"jarvis") || str_eq(t, L"please") || str_eq(t, L"the") || str_eq(t, L"hey"))
            continue;
        if (out[0]) str_cat(out, LINE_MAX_CH, L" ");
        str_cat(out, LINE_MAX_CH, t);
    }
    static const wchar_t *hide[] = { L"hide", L"hide window", L"hide yourself", L"background",
                                     L"background mode", L"go to background", L"go background",
                                     L"run in background", NULL };
    static const wchar_t *show[] = { L"show", L"show window", L"show yourself", L"normal mode",
                                     L"go to normal mode", L"show search bar", NULL };
    for (int i = 0; hide[i]; i++) if (str_eq(out, hide[i])) return MP_HIDE;
    for (int i = 0; show[i]; i++) if (str_eq(out, show[i])) return MP_SHOW;
    return MP_NONE;
}

static void apply_result(CmdResult *r)
{
    if (r->rows_replace) {
        set_rows(r->rows, r->nrows, false);
        g.pick_mode = r->pick_mode;
    }
    set_status(r->reply, ST_REPLY);
    /* a list to pick from needs the bar */
    if (g.bg && r->rows_replace && r->nrows > 0) go_normal();

    bool spoke = false;
    bool speak = r->speak || (g.bg && r->reply[0]);   /* hidden: the voice is the only output */
    if (speak) {
        bool again = r->listen_again && voice_available();
        voice_say(r->reply, again);
        if (again) g.listen_requested = true;
        spoke = g.voice_state >= 0;   /* a voice thread exists to report VE_SPOKEN */
        g.speaking = spoke;
    } else if (r->listen_again && voice_available()) {
        voice_listen(true);
        g.listen_requested = true;
    }
    /* hidden and nothing more to do: exit after replying */
    if (r->close || (g.bg && !r->listen_again && !docs_busy())) schedule_close(spoke);
    else if (g.close_pending) cancel_close();
    state_changed();
}

static void run_text(const wchar_t *text)
{
    if (g.busy || g.closing) return;
    KillTimer(g.hwnd, T_PREVIEW);
    wchar_t buf[LINE_MAX_CH];
    str_copy(buf, LINE_MAX_CH, text);
    str_trim(buf);
    if (!buf[0]) return;

    switch (mode_phrase(buf)) {
    case MP_HIDE:
        if (g.bg) start_listen();
        else go_background();
        return;
    case MP_SHOW:
        g.hyp[0] = 0;
        if (g.bg) go_normal();
        set_edit_text(L"");
        return;
    }

    if (g.bg) str_copy(g.hyp, 320, buf);   /* shown in the box if the bar appears */
    g.busy = true;
    memset(&g_res, 0, sizeof g_res);
    cmd_run(buf, &g_res);
    g.busy = false;
    if (g.closing) return;
    apply_result(&g_res);
    SendMessageW(g.edit, EM_SETSEL, 0, -1);   /* typing replaces it */
    g.edit_from_cmd = true;
}

static void pick_row(int i, bool reveal)
{
    if (g.busy || g.closing || i < 0 || i >= g.nrows) return;
    PickMode mode;
    if (reveal) mode = PICK_REVEAL;
    else if (g.rows_preview) {
        HWND d = cmd_dialog();
        mode = (d && dialog_alive(d)) ? PICK_DIALOG : PICK_OPEN;
    } else mode = g.pick_mode;
    Row row = g.rows[i];
    g.busy = true;
    memset(&g_res, 0, sizeof g_res);
    cmd_pick(&row, mode, &g_res);
    g.busy = false;
    if (g.closing) return;
    apply_result(&g_res);
}

static void do_preview(void)
{
    wchar_t text[LINE_MAX_CH];
    GetWindowTextW(g.edit, text, LINE_MAX_CH);
    str_trim(text);
    if (!text[0]) {
        if (g.rows_preview) set_rows(NULL, 0, true);
        return;
    }
    static Row rows[MAX_HITS];
    int n = cmd_preview(text, rows, MAX_HITS);
    set_rows(rows, n, true);
}

static void on_enter(bool shift)
{
    if (g.user_sel && g.sel >= 0 && g.sel < g.nrows) { pick_row(g.sel, shift); return; }
    wchar_t text[LINE_MAX_CH];
    GetWindowTextW(g.edit, text, LINE_MAX_CH);
    str_trim(text);
    if (!text[0]) {
        if (g.nrows) pick_row(g.sel >= 0 ? g.sel : 0, shift);
        return;
    }
    if (shift && g.nrows) { pick_row(g.sel >= 0 ? g.sel : 0, true); return; }
    run_text(text);
}

static void move_sel(int d)
{
    if (!g.nrows) return;
    int s = g.sel;
    if (!g.user_sel || s < 0) s = d > 0 ? 0 : g.nrows - 1;
    else s = (s + d + g.nrows) % g.nrows;
    g.sel = s;
    g.user_sel = true;
    invalidate_rows();
}

static void stop_listening_for_typing(void)
{
    if (g.listening || g.listen_requested) {
        voice_listen(false);
        g.listen_requested = false;
        state_changed();
    }
}

/* -------------------------------------------------------------- public */
void ui_toggle_listen(void)
{
    if (!g.hwnd || g.closing) return;
    if (g.voice_state < 0) {
        set_status(L"Voice unavailable \x2014 type a command", ST_HINT);
        return;
    }
    if (g.listening || g.listen_requested) {
        voice_listen(false);
        g.listen_requested = false;
        state_changed();
    } else {
        if (g.status_kind == ST_HINT) set_status(L"", ST_IDLE);
        start_listen();
    }
}

/* --------------------------------------------------------------- input */
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    switch (m) {
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (w) {
        case VK_RETURN: on_enter(shift); return 0;
        case VK_ESCAPE: begin_close(); return 0;
        case VK_UP:     move_sel(-1); return 0;
        case VK_DOWN:   move_sel(1); return 0;
        case VK_F2:     ui_toggle_listen(); return 0;
        case VK_SPACE:
            if (ctrl) { g.ctrl_space = true; ui_toggle_listen(); return 0; }
            break;
        case 'H':
            if (ctrl) { g.ctrl_h = true; go_background(); return 0; }
            break;
        default:
            if (ctrl && w >= '1' && w <= '8') { pick_row((int)(w - '1'), shift); return 0; }
            break;
        }
        break;
    }
    case WM_CHAR:
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        if (w == L' ' && g.ctrl_space) { g.ctrl_space = false; return 0; }
        if (w == 8 && g.ctrl_h) { g.ctrl_h = false; return 0; }   /* Ctrl+H = backspace char */
        if (w >= L' ' && w != 127) stop_listening_for_typing();
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, edit_proc, id);
        break;
    }
    return DefSubclassProc(h, m, w, l);
}

static int row_at(int x, int y)
{
    POINT pt = { x, y };
    for (int i = 0; i < g.nrows; i++) {
        RECT r = row_rect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static void on_voice(WPARAM ev, LPARAM lp)
{
    switch ((VoiceEvent)ev) {
    case VE_READY:
        g.voice_state = lp ? 1 : -1;
        if (!lp) {
            g.listen_requested = false;
            if (g.bg) {
                go_normal();
                set_status(L"Voice unavailable \x2014 type a command", ST_HINT);
            }
        }
        state_changed();
        break;
    case VE_LISTENING:
        g.listening = lp != 0;
        g.listen_requested = false;
        if (g.listening) {
            g.speaking = false;
            g.hyp[0] = 0;
            cancel_close();
            if (!g.bg && g.edit_from_cmd) { set_edit_text(L""); g.edit_from_cmd = false; }
            if (g.status_kind == ST_HINT) set_status(L"", ST_IDLE);
        } else {
            g.level = g.level_target = 0;
        }
        state_changed();
        break;
    case VE_HYPOTHESIS:
        if (lp) {
            str_copy(g.hyp, 320, (wchar_t *)lp);
            free((void *)lp);
            if (!g.bg) { set_edit_text(g.hyp); g.edit_from_cmd = true; }
            pill_refresh();
        }
        break;
    case VE_RESULT:
        if (lp) {
            wchar_t *t = (wchar_t *)lp;
            str_copy(g.hyp, 320, t);
            if (!g.bg) set_edit_text(t);
            run_text(t);
            free(t);
        }
        break;
    case VE_NOMATCH:
        g.hyp[0] = 0;
        if (!g.bg) {
            if (g.edit_from_cmd) { set_edit_text(L""); g.edit_from_cmd = false; }
            set_status(L"Didn't catch that \x2014 try again", ST_HINT);
        }
        if (!g.listening && !g.listen_requested && !g.closing) {
            voice_listen(true);
            g.listen_requested = true;
        }
        state_changed();
        break;
    case VE_LEVEL: {
        double v = (double)lp / 100.0;
        if (v > 1) v = 1;
        if (v < 0) v = 0;
        if (v > g.level_target) g.level_target = v;
        break;
    }
    case VE_TIMEOUT:
        if (g.bg) {                        /* nothing said: just go away quietly */
            log_line(L"close: no speech (background)");
            begin_close();
            break;
        }
        if (g.edit_from_cmd && g.hyp[0] == 0) { set_edit_text(L""); g.edit_from_cmd = false; }
        set_status(L"Didn't hear anything \x2014 press F2 to talk", ST_HINT);
        break;
    case VE_SPOKEN:
        g.speaking = false;
        if (g.close_pending) {
            g.close_spoken = true;
            if (g.close_min_done) begin_close();
        }
        state_changed();
        break;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        on_paint(h);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)w;
        SetTextColor(dc, T.text);
        SetBkColor(dc, T.bg);
        return (LRESULT)g.brBg;
    }
    case WM_COMMAND:
        if (LOWORD(w) == EDIT_ID && HIWORD(w) == EN_CHANGE && !g.suppress_change) {
            g.edit_from_cmd = false;
            SetTimer(h, T_PREVIEW, PREVIEW_MS, NULL);
            if (g.user_sel) { g.user_sel = false; invalidate_rows(); }
            if (g.status[0]) set_status(L"", ST_IDLE);
        }
        return 0;
    case WM_TIMER:
        switch (w) {
        case T_ANIM:
            g.level += (g.level_target - g.level) * 0.35;
            g.level_target *= 0.82;
            if (g.bg) pill_refresh();
            else invalidate_dot();
            break;
        case T_PREVIEW:
            KillTimer(h, T_PREVIEW);
            do_preview();
            break;
        case T_CLOSE:
            KillTimer(h, T_CLOSE);
            g.close_min_done = true;
            if (g.close_pending && g.close_spoken) begin_close();
            break;
        case T_CLOSECAP:
            KillTimer(h, T_CLOSECAP);
            if (g.close_pending) begin_close();
            break;
        }
        return 0;
    case WM_APP_VOICE:
        on_voice(w, l);
        return 0;
    case WM_APP_TOGGLE:
        if (g.closing) return 0;
        if (g.bg) {
            /* hidden: Copilot key again = stop listening; with nothing on
             * screen there is nothing left to do, so exit quietly */
            if (g.listening || g.listen_requested) {
                voice_listen(false);
                g.listen_requested = false;
                log_line(L"close: toggled off (background)");
                begin_close();
            } else {
                start_listen();
            }
            return 0;
        }
        force_foreground();
        g.shown_at = GetTickCount();
        ui_toggle_listen();
        return 0;
    case WM_APP_NORMAL:
        go_normal();
        return 0;
    case WM_APP_TASK:   /* a document task (convert, split...) finished */
        if (l) {
            static CmdResult tr;
            cmd_task_done((const DocResult *)l, &tr);
            free((void *)l);
            if (!g.closing && !g.busy) apply_result(&tr);
        }
        return 0;
    case WM_APP_RUN:
        if (l) {
            wchar_t *t = (wchar_t *)l;
            if (!g.bg) set_edit_text(t);
            run_text(t);
            free(t);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            if (!g.bg && !g.closing && GetTickCount() - g.shown_at > GRACE_MS) {
                wchar_t cls[64] = L"";
                if (l) GetClassNameW((HWND)l, cls, 64);
                log_line(L"close: lost focus to %ls", cls[0] ? cls : L"(other app)");
                begin_close();
            }
        } else if (g.edit) {
            SetFocus(g.edit);
        }
        return 0;
    case WM_SETFOCUS:
        if (g.edit) SetFocus(g.edit);
        return 0;
    case WM_KEYDOWN:   /* focus can land on the window itself: forward */
        if (g.edit) return SendMessageW(g.edit, m, w, l);
        return 0;
    case WM_MOUSEMOVE: {
        if (!g.tracking) {
            TRACKMOUSEEVENT t = { sizeof t, TME_LEAVE, h, 0 };
            g.tracking = TrackMouseEvent(&t);
        }
        int r = row_at((short)LOWORD(l), (short)HIWORD(l));
        if (r != g.hover) { g.hover = r; invalidate_rows(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g.tracking = false;
        if (g.hover >= 0) { g.hover = -1; invalidate_rows(); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            RECT dr = dot_rect();
            bool hand = PtInRect(&dr, pt) || row_at(pt.x, pt.y) >= 0;
            SetCursor(LoadCursorW(NULL, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        RECT dr = dot_rect();
        if (PtInRect(&dr, pt)) { ui_toggle_listen(); return 0; }
        int r = row_at(pt.x, pt.y);
        if (r >= 0) {
            g.sel = r;
            g.user_sel = true;
            invalidate_rows();
            pick_row(r, GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        /* anywhere else: drag the window */
        ReleaseCapture();
        SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && str_eq((const wchar_t *)l, L"ImmersiveColorSet")) {
            load_theme();
            InvalidateRect(h, NULL, FALSE);
            if (g.edit) InvalidateRect(g.edit, NULL, TRUE);
            pill_refresh();
        }
        break;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(w);
        RECT *s = (RECT *)l;
        make_resources();
        layout_edit();
        HMONITOR mon = MonitorFromRect(s, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { .cbSize = sizeof mi };
        if (GetMonitorInfoW(mon, &mi)) g.work = mi.rcWork;
        SetWindowPos(h, NULL, s->left, s->top, S(L_W), S(client_h()), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        begin_close();
        return 0;
    case WM_APP_CLOSE:
        finish_close();
        return 0;
    case WM_DESTROY:
        KillTimer(h, T_ANIM);
        if (g.pill) { DestroyWindow(g.pill); g.pill = NULL; }
        free_fonts();
        free_dot();
        if (g.brBg) { DeleteObject(g.brBg); g.brBg = NULL; }
        g.hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool ui_create(HINSTANCE inst, HWND dialog, bool listen, const wchar_t *initial_cmd, bool background)
{
    g.inst = inst;
    g.dialog = dialog;
    g.sel = g.hover = -1;
    g.pick_mode = PICK_OPEN;
    g.bg = background;

    WNDCLASSEXW wc = { .cbSize = sizeof wc };
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) return false;

    WNDCLASSEXW pc = { .cbSize = sizeof pc };
    pc.lpfnWndProc = pill_proc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(NULL, IDC_HAND);
    pc.lpszClassName = PILL_CLASS;
    RegisterClassExW(&pc);

    /* the monitor with the mouse on it */
    POINT cur;
    GetCursorPos(&cur);
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi);
    g.work = mi.rcWork;

    g.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, APP_CLASS, APP_NAME,
                             WS_POPUP | WS_CLIPCHILDREN,
                             g.work.left + 10, g.work.top + 10, 100, 100,
                             NULL, NULL, inst, NULL);
    if (!g.hwnd) return false;
    cmd_set_notify(g.hwnd);   /* document tasks report back with WM_APP_TASK */
    g.dpi = GetDpiForWindow(g.hwnd);
    if (!g.dpi) g.dpi = 96;

    load_theme();
    make_resources();

    g.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)EDIT_ID, inst, NULL);
    if (!g.edit) { DestroyWindow(g.hwnd); return false; }
    SetWindowSubclass(g.edit, edit_proc, 1, 0);
    SendMessageW(g.edit, EM_SETLIMITTEXT, LINE_MAX_CH - 1, 0);
    SendMessageW(g.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    layout_edit();
    resize_window(true);

    if (!voice_start(g.hwnd)) g.voice_state = -1;
    if (g.bg && !(initial_cmd && initial_cmd[0])) listen = true;   /* hidden: always listen */
    if (g.bg && g.voice_state < 0) {
        go_normal();
        set_status(L"Voice unavailable \x2014 type a command", ST_HINT);
    }
    update_cue();

    if (!g.bg) {
        ShowWindow(g.hwnd, SW_SHOW);
        UpdateWindow(g.hwnd);
        force_foreground();
        g.shown_at = GetTickCount();
    }

    if (listen && g.voice_state >= 0) {
        voice_listen(true);
        g.listen_requested = true;
    }
    state_changed();

    if (initial_cmd && initial_cmd[0]) {
        if (!g.bg) set_edit_text(initial_cmd);
        run_text(initial_cmd);
    }
    return true;
}

int ui_loop(void)
{
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

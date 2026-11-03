/* The overlay: a small borderless, always-on-top window with an animated
 * "arc reactor", a command box and a results list.
 *
 * Plain GDI, double buffered. The reactor is drawn at 3x into an offscreen
 * DIB and scaled down with HALFTONE, which gives cheap anti-aliasing. The
 * animation timer only runs while the window is visible, and the process
 * exits as soon as the window closes. */
#include "jarvis.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* DWM attributes (defined here: older MinGW headers lack some of them) */
#define JV_DWMWA_DARK_MODE   20
#define JV_DWMWA_CORNER      33
#define JV_DWMWA_BORDER      34
#define JV_DWMWCP_ROUND      2

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

/* colours */
#define C_BG      RGB(0x0B, 0x12, 0x20)
#define C_PANEL   RGB(0x11, 0x1A, 0x2E)
#define C_ACCENT  RGB(0x22, 0xD3, 0xEE)
#define C_ACCENT2 RGB(0x38, 0xBD, 0xF8)
#define C_TEXT    RGB(0xE6, 0xF1, 0xFF)
#define C_DIM     RGB(0x7C, 0x8B, 0xA5)
#define C_SEL     RGB(0x16, 0x23, 0x3D)
#define C_LINE    RGB(0x1A, 0x26, 0x40)
#define C_HOVER   RGB(0x0F, 0x18, 0x2A)

/* layout, logical px (96 dpi) */
#define L_W         680
#define L_PAD       18
#define L_REACTOR   84
#define L_HEADER    118
#define L_COL       (L_PAD + L_REACTOR + 20)
#define L_PANEL_Y   68
#define L_PANEL_H   38
#define L_ROW_H     44
#define L_ROWS_PAD  10
#define L_FOOTER    30
#define L_MIC_W     34

/* timers */
#define T_ANIM     1
#define T_PREVIEW  2
#define T_CLOSE    3
#define T_CLOSECAP 4

#define ANIM_MS        30
#define PREVIEW_MS     120
#define GRACE_MS       400
#define CLOSE_CAP_MS   4000
#define SS             3          /* reactor supersampling factor */

#define EDIT_ID        100
#define WM_APP_CLOSE   (WM_APP + 20)

enum { ST_IDLE, ST_LISTEN, ST_HEARD, ST_REPLY, ST_HINT };

static struct {
    HINSTANCE inst;
    HWND  hwnd, edit, dialog;
    UINT  dpi;
    RECT  work;                 /* work area of our monitor */

    HFONT fTitle, fChip, fStatus, fEdit, fName, fPath, fIcon, fBadge, fHint, fMic;
    HBRUSH brPanel;

    /* reactor offscreen (3x) */
    HDC     rdc;
    HBITMAP rbmp, rold;
    int     rsize;              /* big size in px */

    /* state */
    bool  listening, listen_requested, speaking, busy, closing;
    int   voice_state;          /* 0 unknown, 1 ok, -1 unavailable */
    double phase;               /* rotation, degrees */
    double level, level_target; /* mic level 0..1 */
    ULONGLONG last_tick;
    DWORD shown_at;

    wchar_t status[320];
    int     status_kind;

    Row   rows[MAX_HITS];
    int   nrows, sel, hover;
    bool  user_sel, rows_preview;
    PickMode pick_mode;

    bool  suppress_change, ctrl_space;

    bool  close_pending, close_min_done, close_spoken;
    bool  tracking;
} g;

static CmdResult g_res;   /* big: keep it off the stack */

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

static int client_h(void)
{
    return L_HEADER + (g.nrows ? g.nrows * L_ROW_H + L_ROWS_PAD : 0) + L_FOOTER;
}

static RECT reactor_rect(void)
{
    RECT r = { S(L_PAD), S(16), S(L_PAD + L_REACTOR), S(16 + L_REACTOR) };
    return r;
}

static RECT panel_rect(void)
{
    RECT r = { S(L_COL), S(L_PANEL_Y), S(L_W - L_PAD), S(L_PANEL_Y + L_PANEL_H) };
    return r;
}

static RECT mic_rect(void)
{
    RECT p = panel_rect();
    RECT r = { p.right - S(L_MIC_W), p.top, p.right, p.bottom };
    return r;
}

static RECT row_rect(int i)
{
    int y = S(L_HEADER + 4 + i * L_ROW_H);
    RECT r = { S(L_PAD), y, S(L_W - L_PAD), y + S(L_ROW_H - 4) };
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
    HFONT *fs[] = { &g.fTitle, &g.fChip, &g.fStatus, &g.fEdit, &g.fName, &g.fPath,
                    &g.fIcon, &g.fBadge, &g.fHint, &g.fMic };
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++)
        if (*fs[i]) { DeleteObject(*fs[i]); *fs[i] = NULL; }
}

static void free_reactor(void)
{
    if (g.rdc) {
        SelectObject(g.rdc, g.rold);
        DeleteObject(g.rbmp);
        DeleteDC(g.rdc);
        g.rdc = NULL;
    }
}

static void make_resources(void)
{
    free_fonts();
    g.fTitle  = mkfont(L"Bahnschrift SemiBold", 13, FW_NORMAL, L"Segoe UI Semibold");
    g.fChip   = mkfont(L"Bahnschrift SemiBold", 10, FW_NORMAL, L"Segoe UI Semibold");
    g.fStatus = mkfont(L"Segoe UI", 14, FW_NORMAL, NULL);
    g.fEdit   = mkfont(L"Segoe UI", 17, FW_NORMAL, NULL);
    g.fName   = mkfont(L"Segoe UI Semibold", 14, FW_NORMAL, L"Segoe UI");
    g.fPath   = mkfont(L"Segoe UI", 12, FW_NORMAL, NULL);
    g.fIcon   = mkfont(L"Segoe Fluent Icons", 17, FW_NORMAL, L"Segoe MDL2 Assets");
    g.fMic    = mkfont(L"Segoe Fluent Icons", 15, FW_NORMAL, L"Segoe MDL2 Assets");
    g.fBadge  = mkfont(L"Segoe UI Semibold", 11, FW_NORMAL, L"Segoe UI");
    g.fHint   = mkfont(L"Segoe UI", 12, FW_NORMAL, NULL);
    if (!g.brPanel) g.brPanel = CreateSolidBrush(C_PANEL);

    free_reactor();
    g.rsize = S(L_REACTOR) * SS;
    BITMAPINFO bi = { .bmiHeader = { sizeof(BITMAPINFOHEADER), g.rsize, -g.rsize, 1, 32, BI_RGB } };
    void *bits;
    HDC sdc = GetDC(NULL);
    g.rdc  = CreateCompatibleDC(sdc);
    g.rbmp = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, sdc);
    g.rold = SelectObject(g.rdc, g.rbmp);
}

/* place the edit control inside the panel */
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
    RECT p = panel_rect();
    int h = tm.tmHeight + 2;
    int x = p.left + S(14);
    SetWindowPos(g.edit, NULL, x, p.top + (p.bottom - p.top - h) / 2,
                 p.right - S(L_MIC_W) - x, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void resize_window(bool center)
{
    int w = S(L_W), h = S(client_h());
    if (center) {
        int x = g.work.left + (g.work.right - g.work.left - w) / 2;
        int y = g.work.top + (g.work.bottom - g.work.top) * 22 / 100;
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
    str_copy(g.status, 320, s ? s : L"");
    g.status_kind = kind;
    RECT r = { S(L_COL), S(38), S(L_W - L_PAD), S(L_PANEL_Y - 4) };
    InvalidateRect(g.hwnd, &r, FALSE);
}

static void invalidate_rows(void)
{
    RECT r = { 0, S(L_HEADER), S(L_W), S(client_h()) };
    InvalidateRect(g.hwnd, &r, FALSE);
}

/* ------------------------------------------------------------- reactor */
static void ring(HDC dc, double cx, double cy, double r, double w, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, (int)(w + 0.5), c);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r), (int)(cy + r));
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(p);
}

static void disc(HDC dc, double cx, double cy, double r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, (int)(cx - r), (int)(cy - r), (int)(cx + r + 1), (int)(cy + r + 1));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

/* n arc segments of `len` degrees spread evenly, starting at `start` */
static void arcs(HDC dc, double cx, double cy, double r, double w, COLORREF c,
                 int n, double start, double len)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    HPEN p = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT, (DWORD)(w + 0.5), &lb, 0, NULL);
    HGDIOBJ op = SelectObject(dc, p);
    for (int i = 0; i < n; i++) {
        double a = start + i * 360.0 / n;
        double rad = a * 3.14159265358979 / 180.0;
        MoveToEx(dc, (int)(cx + r * cos(rad)), (int)(cy - r * sin(rad)), NULL);
        AngleArc(dc, (int)cx, (int)cy, (DWORD)r, (FLOAT)a, (FLOAT)len);
    }
    SelectObject(dc, op);
    DeleteObject(p);
}

static void ticks(HDC dc, double cx, double cy, double r0, double r1, double w, COLORREF c, int n, double start)
{
    HPEN p = CreatePen(PS_SOLID, (int)(w + 0.5), c);
    HGDIOBJ op = SelectObject(dc, p);
    for (int i = 0; i < n; i++) {
        double rad = (start + i * 360.0 / n) * 3.14159265358979 / 180.0;
        double cs = cos(rad), sn = sin(rad);
        MoveToEx(dc, (int)(cx + r0 * cs), (int)(cy - r0 * sn), NULL);
        LineTo(dc, (int)(cx + r1 * cs), (int)(cy - r1 * sn));
    }
    SelectObject(dc, op);
    DeleteObject(p);
}

static void render_reactor(void)
{
    HDC dc = g.rdc;
    double u = g.rsize / (double)L_REACTOR;     /* one logical unit at 3x */
    double c = g.rsize / 2.0;
    RECT all = { 0, 0, g.rsize, g.rsize };
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    double t = GetTickCount() / 1000.0;
    double lv = g.level;
    double glow;                                 /* 0..1 overall brightness */
    double pulse;                                /* core size pulse 0..1 */
    if (g.listening) {
        glow = 0.78 + 0.22 * lv;
        pulse = 0.25 + 0.75 * lv;
    } else if (g.speaking || g.busy) {
        glow = 0.8;
        pulse = 0.35 + 0.25 * (0.5 + 0.5 * sin(t * 9.0));
    } else {
        glow = 0.55;
        pulse = 0.2 + 0.15 * (0.5 + 0.5 * sin(t * 2.2));
    }
    COLORREF acc  = mix(C_BG, C_ACCENT, glow);
    COLORREF acc2 = mix(C_BG, C_ACCENT2, glow * 0.9);
    double a = g.phase;

    /* soft halo when listening */
    if (g.listening) {
        for (int i = 6; i >= 1; i--)
            ring(dc, c, c, (39.5 + lv * 2.5) * u, 1.0 * u, mix(C_BG, C_ACCENT, lv * 0.10 * i));
    }
    ring(dc, c, c, 40.5 * u, 1.0 * u, mix(C_BG, C_ACCENT, 0.25 + 0.25 * glow));
    ticks(dc, c, c, 36.0 * u, 38.5 * u, 1.0 * u, mix(C_BG, C_ACCENT2, 0.35 * glow + 0.1), 48, -a * 0.35);
    arcs(dc, c, c, 33.0 * u, 3.2 * u, acc, 3, a, 72);
    arcs(dc, c, c, 33.0 * u, 1.0 * u, mix(C_BG, C_ACCENT, 0.3), 3, a + 80, 32);
    arcs(dc, c, c, 27.5 * u, 2.2 * u, acc2, 4, -a * 1.6, 44);
    ring(dc, c, c, 23.5 * u, 1.2 * u, mix(C_BG, C_ACCENT, 0.45 * glow + 0.1));
    /* reactor "coils" */
    arcs(dc, c, c, 20.0 * u, 4.0 * u, mix(C_BG, C_ACCENT, 0.30 + 0.35 * glow), 10, a * 0.25 + 6, 24);

    /* glowing core: discs from the outside in, bg -> accent -> white */
    double cr = (11.0 + 4.0 * pulse) * u;
    const int steps = 14;
    for (int i = 0; i < steps; i++) {
        double k = i / (double)(steps - 1);          /* 0 outside .. 1 centre */
        double r = cr * (1.0 - 0.85 * k);
        COLORREF col = k < 0.55 ? mix(C_BG, C_ACCENT, (k / 0.55) * (0.55 + 0.45 * glow))
                                : mix(C_ACCENT, RGB(0xF2, 0xFD, 0xFF), (k - 0.55) / 0.45 * (0.6 + 0.4 * glow));
        disc(dc, c, c, r, col);
    }
}

static void blit_reactor(HDC dst)
{
    RECT r = reactor_rect();
    render_reactor();
    SetStretchBltMode(dst, HALFTONE);
    SetBrushOrgEx(dst, 0, 0, NULL);
    StretchBlt(dst, r.left, r.top, r.right - r.left, r.bottom - r.top,
               g.rdc, 0, 0, g.rsize, g.rsize, SRCCOPY);
}

/* --------------------------------------------------------------- paint */
static void fill_round(HDC dc, const RECT *r, int rad, COLORREF fill, COLORREF border, bool has_border)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = has_border ? CreatePen(PS_SOLID, 1, border) : NULL;
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p ? (HGDIOBJ)p : GetStockObject(NULL_PEN));
    RoundRect(dc, r->left, r->top, r->right + (p ? 0 : 1), r->bottom + (p ? 0 : 1), rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    if (p) DeleteObject(p);
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
    if (row->kind != ROW_HIT) return L"\xE946";
    if (row->hit.is_dir) return L"\xE8B7";
    const wchar_t *dot = wcsrchr(path_name(row->hit.path), L'.');
    if (dot && (!_wcsicmp(dot, L".lnk") || !_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".url")))
        return L"\xE946";
    switch (ext_class_of(row->hit.path)) {
    case EXT_IMAGE: return L"\xEB9F";
    case EXT_VIDEO: return L"\xE714";
    case EXT_AUDIO: return L"\xE8D6";
    default:        return L"\xE8A5";
    }
}

static void paint_row(HDC dc, int i)
{
    const Row *row = &g.rows[i];
    RECT r = row_rect(i);
    bool sel = (i == g.sel && g.user_sel);
    bool hov = (i == g.hover);
    if (sel) {
        fill_round(dc, &r, S(10), C_SEL, 0, false);
        RECT bar = { r.left, r.top + S(8), r.left + S(3), r.bottom - S(8) };
        fill_round(dc, &bar, S(3), C_ACCENT, 0, false);
    } else if (hov) {
        fill_round(dc, &r, S(10), C_HOVER, 0, false);
    }

    /* number badge */
    int cy = (r.top + r.bottom) / 2;
    RECT b = { r.left + S(12), cy - S(10), r.left + S(32), cy + S(10) };
    if (sel) fill_round(dc, &b, S(6), C_ACCENT, 0, false);
    else     fill_round(dc, &b, S(6), C_PANEL, C_LINE, true);
    wchar_t num[4] = { (wchar_t)(L'1' + i), 0 };
    text_at(dc, g.fBadge, sel ? C_BG : C_DIM, num, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* icon */
    RECT ic = { r.left + S(40), r.top, r.left + S(68), r.bottom };
    text_at(dc, g.fIcon, sel ? C_ACCENT : C_ACCENT2, row_glyph(row), &ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* name + sub */
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
    int x = r.left + S(76), rr = r.right - S(12);
    if (sub[0]) {
        RECT nr = { x, r.top + S(4), rr, r.top + S(22) };
        text_at(dc, g.fName, C_TEXT, name, &nr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
        RECT pr = { x, r.top + S(21), rr, r.bottom - S(3) };
        text_at(dc, g.fPath, C_DIM, sub, &pr, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_VCENTER);
    } else {
        RECT nr = { x, r.top, rr, r.bottom };
        text_at(dc, g.fName, C_TEXT, name, &nr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    }
}

static bool voice_ok(void) { return g.voice_state >= 0; }

static void paint_all(HDC dc, int w, int h)
{
    fill_rect(dc, 0, 0, w, h, C_BG);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g.fStatus);

    blit_reactor(dc);

    /* title + chip */
    int x = S(L_COL);
    SelectObject(dc, g.fTitle);
    SetTextCharacterExtra(dc, S(3));
    SetTextColor(dc, C_ACCENT);
    const wchar_t *title = L"J.A.R.V.I.S";
    SIZE ts;
    GetTextExtentPoint32W(dc, title, (int)wcslen(title), &ts);
    TextOutW(dc, x, S(16), title, (int)wcslen(title));
    SetTextCharacterExtra(dc, 0);

    if (g.dialog) {
        SelectObject(dc, g.fChip);
        SetTextCharacterExtra(dc, S(1));
        const wchar_t *chip = L"UPLOAD MODE";
        SIZE cs;
        GetTextExtentPoint32W(dc, chip, (int)wcslen(chip), &cs);
        RECT cr = { x + ts.cx + S(12), S(15), x + ts.cx + S(12) + cs.cx + S(14), S(15) + S(18) };
        fill_round(dc, &cr, S(9), mix(C_BG, C_ACCENT, 0.12), mix(C_BG, C_ACCENT, 0.6), true);
        text_at(dc, g.fChip, C_ACCENT, chip, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextCharacterExtra(dc, 0);
    }

    /* status line */
    {
        COLORREF col = C_DIM;
        wchar_t buf[340];
        const wchar_t *s = g.status;
        switch (g.status_kind) {
        case ST_LISTEN: col = C_ACCENT; break;
        case ST_HEARD:  col = C_TEXT; swprintf(buf, 340, L"\x201C%ls\x201D", g.status); s = buf; break;
        case ST_REPLY:  col = C_TEXT; break;
        default: break;
        }
        RECT sr = { x, S(38), S(L_W - L_PAD), S(L_PANEL_Y - 4) };
        text_at(dc, g.fStatus, col, s, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER);
    }

    /* input panel */
    RECT p = panel_rect();
    bool focused = GetFocus() == g.edit;
    COLORREF border = g.listening ? mix(C_PANEL, C_ACCENT, 0.85)
                    : focused ? mix(C_PANEL, C_ACCENT, 0.35) : C_LINE;
    fill_round(dc, &p, S(12), C_PANEL, border, true);
    RECT m = mic_rect();
    text_at(dc, g.fMic, g.listening ? C_ACCENT : voice_ok() ? C_DIM : mix(C_PANEL, C_DIM, 0.4),
            L"\xE720", &m, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    /* results */
    if (g.nrows) {
        int ly = S(L_HEADER);
        fill_rect(dc, S(L_PAD), ly - 1, S(L_W - L_PAD), ly, C_LINE);
        for (int i = 0; i < g.nrows; i++) paint_row(dc, i);
    }

    /* footer */
    int fy = h - S(L_FOOTER);
    fill_rect(dc, S(L_PAD), fy, S(L_W - L_PAD), fy + 1, C_LINE);
    RECT fr = { S(L_PAD), fy + 1, S(L_W - L_PAD), h };
    const wchar_t *hint = voice_ok()
        ? L"Enter open  \x00B7  Shift+Enter show in folder  \x00B7  F2 talk  \x00B7  Esc close"
        : L"Enter open  \x00B7  Shift+Enter show in folder  \x00B7  voice unavailable  \x00B7  Esc close";
    text_at(dc, g.fHint, C_DIM, hint, &fr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (search_busy()) {
        text_at(dc, g.fHint, mix(C_BG, C_DIM, 0.7), L"indexing\x2026", &fr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
    }

    SelectObject(dc, of);
}

static void on_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT cr;
    GetClientRect(h, &cr);
    RECT rr = reactor_rect();
    RECT inter;
    bool only_reactor = EqualRect(&ps.rcPaint, &rr) ||
        (IntersectRect(&inter, &ps.rcPaint, &rr) && EqualRect(&inter, &ps.rcPaint));
    if (only_reactor) {
        blit_reactor(dc);   /* single blit: no flicker */
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

/* ------------------------------------------------------------- closing */
static void begin_close(void)
{
    if (g.closing) return;
    g.closing = true;
    KillTimer(g.hwnd, T_ANIM);
    KillTimer(g.hwnd, T_PREVIEW);
    KillTimer(g.hwnd, T_CLOSE);
    KillTimer(g.hwnd, T_CLOSECAP);
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

static void apply_result(CmdResult *r)
{
    set_status(r->reply, ST_REPLY);
    if (r->rows_replace) {
        set_rows(r->rows, r->nrows, false);
        g.pick_mode = r->pick_mode;
    }
    bool spoke = false;
    if (r->speak) {
        bool again = r->listen_again && voice_available();
        voice_say(r->reply, again);
        if (again) g.listen_requested = true;
        spoke = g.voice_state >= 0;   /* a voice thread exists to report VE_SPOKEN */
        g.speaking = spoke;
    } else if (r->listen_again && voice_available()) {
        voice_listen(true);
        g.listen_requested = true;
    }
    if (r->close) schedule_close(spoke);
    else if (g.close_pending) cancel_close();
}

static void run_text(const wchar_t *text)
{
    if (g.busy || g.closing) return;
    KillTimer(g.hwnd, T_PREVIEW);
    wchar_t buf[LINE_MAX_CH];
    str_copy(buf, LINE_MAX_CH, text);
    str_trim(buf);
    if (!buf[0]) return;
    g.busy = true;
    memset(&g_res, 0, sizeof g_res);
    cmd_run(buf, &g_res);
    g.busy = false;
    if (g.closing) return;
    apply_result(&g_res);
    SendMessageW(g.edit, EM_SETSEL, 0, -1);   /* typing replaces it */
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

static void set_edit_text(const wchar_t *s)
{
    g.suppress_change = true;
    SetWindowTextW(g.edit, s);
    g.suppress_change = false;
    int n = GetWindowTextLengthW(g.edit);
    SendMessageW(g.edit, EM_SETSEL, n, n);
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
        if (g.status_kind == ST_LISTEN || g.status_kind == ST_HEARD) set_status(L"", ST_IDLE);
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
        if (g.status_kind == ST_LISTEN || g.status_kind == ST_HEARD)
            set_status(L"Mic off \x2014 type a command", ST_HINT);
    } else {
        cancel_close();
        voice_listen(true);
        g.listen_requested = true;
        set_status(g.voice_state > 0 ? L"Listening\x2026" : L"Starting the microphone\x2026", ST_LISTEN);
    }
    InvalidateRect(g.hwnd, NULL, FALSE);
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
        default:
            if (ctrl && w >= '1' && w <= '8') { pick_row((int)(w - '1'), shift); return 0; }
            break;
        }
        break;
    }
    case WM_CHAR:
        if (w == L'\r' || w == L'\n' || w == 27) return 0;
        if (w == L' ' && g.ctrl_space) { g.ctrl_space = false; return 0; }
        if (w >= L' ' && w != 127) stop_listening_for_typing();
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        RECT p = panel_rect();
        InflateRect(&p, 1, 1);
        InvalidateRect(GetParent(h), &p, FALSE);
        break;
    }
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
            set_status(L"Voice unavailable \x2014 type a command", ST_HINT);
        } else if (g.listen_requested && g.status_kind == ST_LISTEN) {
            set_status(L"Listening\x2026", ST_LISTEN);
        }
        InvalidateRect(g.hwnd, NULL, FALSE);
        break;
    case VE_LISTENING:
        g.listening = lp != 0;
        g.listen_requested = false;
        if (g.listening) {
            g.speaking = false;
            cancel_close();
            set_status(L"Listening\x2026", ST_LISTEN);
        } else {
            g.level = g.level_target = 0;
            if (g.status_kind == ST_LISTEN) set_status(L"", ST_IDLE);
        }
        InvalidateRect(g.hwnd, NULL, FALSE);
        break;
    case VE_HYPOTHESIS:
        if (lp) {
            set_status((wchar_t *)lp, ST_HEARD);
            free((void *)lp);
        }
        break;
    case VE_RESULT:
        if (lp) {
            wchar_t *t = (wchar_t *)lp;
            set_edit_text(t);
            set_status(t, ST_HEARD);
            run_text(t);
            free(t);
        }
        break;
    case VE_NOMATCH:
        set_status(L"Didn't catch that \x2014 try again", ST_LISTEN);
        if (!g.listening && !g.listen_requested && !g.closing) {
            voice_listen(true);
            g.listen_requested = true;
        }
        break;
    case VE_LEVEL: {
        double v = (double)lp / 100.0;
        if (v > 1) v = 1;
        if (v < 0) v = 0;
        if (v > g.level_target) g.level_target = v;
        break;
    }
    case VE_TIMEOUT:
        set_status(voice_ok() ? L"Type a command, or press F2 to talk" : L"Type a command", ST_HINT);
        break;
    case VE_SPOKEN:
        g.speaking = false;
        if (g.close_pending) {
            g.close_spoken = true;
            if (g.close_min_done) begin_close();
        }
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
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_PANEL);
        return (LRESULT)g.brPanel;
    }
    case WM_COMMAND:
        if (LOWORD(w) == EDIT_ID && HIWORD(w) == EN_CHANGE && !g.suppress_change) {
            SetTimer(h, T_PREVIEW, PREVIEW_MS, NULL);
            if (g.user_sel) { g.user_sel = false; invalidate_rows(); }
        }
        return 0;
    case WM_TIMER:
        switch (w) {
        case T_ANIM: {
            ULONGLONG now = GetTickCount64();
            double dt = (now - g.last_tick) / 1000.0;
            if (dt > 0.2) dt = 0.2;
            g.last_tick = now;
            double speed = g.listening ? 70 : (g.speaking || g.busy) ? 200 : 28;   /* deg/s */
            g.phase = fmod(g.phase + speed * dt, 3600.0);
            g.level += (g.level_target - g.level) * 0.35;
            g.level_target *= 0.82;
            RECT rr = reactor_rect();
            InvalidateRect(h, &rr, FALSE);
            break;
        }
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
        if (!g.closing) {
            force_foreground();
            g.shown_at = GetTickCount();
            ui_toggle_listen();
        }
        return 0;
    case WM_APP_RUN:
        if (l) {
            wchar_t *t = (wchar_t *)l;
            set_edit_text(t);
            run_text(t);
            free(t);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            if (!g.closing && GetTickCount() - g.shown_at > GRACE_MS) {
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
            RECT rr = reactor_rect(), mr = mic_rect();
            bool hand = PtInRect(&rr, pt) || PtInRect(&mr, pt) || row_at(pt.x, pt.y) >= 0;
            SetCursor(LoadCursorW(NULL, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { (short)LOWORD(l), (short)HIWORD(l) };
        RECT rr = reactor_rect(), mr = mic_rect();
        if (PtInRect(&rr, pt) || PtInRect(&mr, pt)) { ui_toggle_listen(); return 0; }
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
        free_fonts();
        free_reactor();
        if (g.brPanel) { DeleteObject(g.brPanel); g.brPanel = NULL; }
        g.hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool ui_create(HINSTANCE inst, HWND dialog, bool listen, const wchar_t *initial_cmd)
{
    g.inst = inst;
    g.dialog = dialog;
    g.sel = g.hover = -1;
    g.pick_mode = PICK_OPEN;

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
    g.dpi = GetDpiForWindow(g.hwnd);
    if (!g.dpi) g.dpi = 96;

    BOOL dark = TRUE;
    DWORD corner = JV_DWMWCP_ROUND;
    COLORREF border = RGB(0x1E, 0x5A, 0x6E);   /* subtle cyan */
    DwmSetWindowAttribute(g.hwnd, JV_DWMWA_DARK_MODE, &dark, sizeof dark);
    DwmSetWindowAttribute(g.hwnd, JV_DWMWA_CORNER, &corner, sizeof corner);
    DwmSetWindowAttribute(g.hwnd, JV_DWMWA_BORDER, &border, sizeof border);

    make_resources();

    g.edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                             0, 0, 10, 10, g.hwnd, (HMENU)(INT_PTR)EDIT_ID, inst, NULL);
    if (!g.edit) { DestroyWindow(g.hwnd); return false; }
    SetWindowSubclass(g.edit, edit_proc, 1, 0);
    SendMessageW(g.edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Say or type a command\x2026");
    SendMessageW(g.edit, EM_SETLIMITTEXT, LINE_MAX_CH - 1, 0);
    SendMessageW(g.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    layout_edit();

    set_status(L"At your service.", ST_IDLE);
    resize_window(true);

    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    force_foreground();
    g.shown_at = GetTickCount();
    g.last_tick = GetTickCount64();
    SetTimer(g.hwnd, T_ANIM, ANIM_MS, NULL);

    if (!voice_start(g.hwnd)) g.voice_state = -1;
    if (listen && g.voice_state >= 0) {
        voice_listen(true);
        g.listen_requested = true;
        set_status(L"Starting the microphone\x2026", ST_LISTEN);
    }
    if (initial_cmd && initial_cmd[0]) {
        set_edit_text(initial_cmd);
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

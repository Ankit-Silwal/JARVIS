/* Command interpreter: turns a spoken or typed sentence into an action
 * (open a file/folder/app/site, web search, system action, file dialog fill).
 * Speech input is messy, so everything goes through normalize() first. */
#include "jarvis.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wctype.h>
#include <ctype.h>

#define S_CAP   512
#define ACT_CAP 2400

/* ------------------------------------------------------------ module state */
static HWND     s_dialog;               /* Upload/Open dialog we were started over */
static bool     s_dry;                  /* record actions instead of doing them */
static wchar_t  s_action[ACT_CAP];      /* last action (dry-run format) */
static Row      s_rows[MAX_HITS];       /* rows of the last list, for "number two" */
static int      s_nrows;
static PickMode s_mode;

typedef const wchar_t *const WList[];   /* NULL-terminated word/phrase list */

/* ----------------------------------------------------------------- helpers */
static bool in_list(const wchar_t *s, const wchar_t *const *l)
{
    for (; *l; l++) if (str_eq(s, *l)) return true;
    return false;
}

static bool contains_word(const wchar_t *s, const wchar_t *w)
{
    size_t n = wcslen(w);
    for (const wchar_t *p = wcsstr(s, w); p; p = wcsstr(p + 1, w))
        if ((p == s || p[-1] == L' ') && (p[n] == 0 || p[n] == L' ')) return true;
    return false;
}

/* If s ends with " suffix" (or equals suffix) cuts it off in place. */
static bool cut_suffix(wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    if (a == b && str_eq(s, suf)) { s[0] = 0; return true; }
    if (a > b && s[a - b - 1] == L' ' && str_eq(s + a - b, suf)) { s[a - b - 1] = 0; return true; }
    return false;
}

/* Removes leading phrases from the list (repeatedly) in place. */
static bool cut_prefixes(wchar_t *s, const wchar_t *const *l)
{
    bool any = false, again = true;
    while (again) {
        again = false;
        for (const wchar_t *const *p = l; *p; p++) {
            const wchar_t *rest = str_after_word(s, *p);
            if (rest) { memmove(s, rest, (wcslen(rest) + 1) * sizeof(wchar_t)); again = any = true; break; }
        }
    }
    return any;
}

static int split_words(const wchar_t *s, wchar_t w[][64], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == L' ') s++;
        if (!*s) break;
        int len = 0;
        while (*s && *s != L' ') { if (len < 63) w[n][len++] = *s; s++; }
        w[n++][len] = 0;
    }
    return n;
}

static void join_words(wchar_t w[][64], int from, int to, wchar_t *out, size_t cap)
{
    out[0] = 0;
    for (int i = from; i < to; i++) {
        if (w[i][0] == 0) continue;
        if (out[0]) str_cat(out, cap, L" ");
        str_cat(out, cap, w[i]);
    }
}

/* "google drive" -> "Google Drive" (for replies) */
static void title_case(const wchar_t *in, wchar_t *out, size_t cap)
{
    str_copy(out, cap, in);
    if (wcslen(out) > 100) out[100] = 0;
    bool start = true;
    for (wchar_t *p = out; *p; p++) {
        if (start && iswalpha(*p)) *p = (wchar_t)towupper(*p);
        start = (*p == L' ');
    }
}

static void reply(CmdResult *r, const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vswprintf(r->reply, 300, fmt, ap);
    va_end(ap);
    r->reply[299] = 0;
    r->speak = true;
}

static void record(const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vswprintf(s_action, ACT_CAP, fmt, ap);
    va_end(ap);
    s_action[ACT_CAP - 1] = 0;
}

/* an action completed: close the overlay, forget any list */
static void done(CmdResult *r) { r->close = true; r->speak = true; s_nrows = 0; }

static bool dialog_active(void) { return s_dialog && dialog_alive(s_dialog); }
static bool is_shell_path(const wchar_t *p) { return _wcsnicmp(p, L"shell:", 6) == 0; }

/* Short display name of a path: file name without directories */
static void short_name(const wchar_t *path, wchar_t *out, size_t cap)
{
    str_copy(out, cap, path_name(path));
    size_t n = wcslen(out);
    if (n > 3 && out[n - 1] == L'\\') out[n - 1] = 0;   /* "C:\" stays */
    if (wcslen(out) > 100) out[100] = 0;
}

/* -------------------------------------------------------- normalization */
static WList TLDS = { L"com", L"org", L"net", L"io", L"in", L"edu", L"gov", L"co", L"ai", L"dev",
                      L"app", L"me", L"uk", L"us", L"info", L"xyz", L"tv", L"gg", L"ly", L"np",
                      L"so", L"site", L"tech", L"online", NULL };

static WList LEAD_FILLERS = {
    L"go ahead and", L"can you please", L"could you please", L"would you please", L"will you please",
    L"can you", L"could you", L"would you", L"will you", L"i want to", L"i wanna", L"i need to",
    L"i'd like to", L"i would like to", L"let's", L"lets", L"hey", L"hi", L"hello", L"ok", L"okay",
    L"please", L"jarvis", L"um", L"uh", L"so", L"just", L"yo", NULL };
static WList TRAIL_FILLERS = { L"please", L"for me", L"right now", L"now", L"thanks",
                               L"thank you", NULL };

/* Lowercase, drop punctuation (keeping it inside tokens like youtube.com, c:\x,
 * what's), collapse spaces, join "x dot com", strip filler words. */
static void normalize(const wchar_t *in, wchar_t *out, size_t cap)
{
    wchar_t tmp[S_CAP];
    size_t n = 0;
    for (size_t i = 0; in[i] && n + 1 < S_CAP; i++) {
        wchar_t c = (wchar_t)towlower(in[i]);
        if (iswalnum(c)) tmp[n++] = c;
        else if (wcschr(L".:/-_'\\+#&", c) && n && tmp[n - 1] != L' ' &&
                 (iswalnum(in[i + 1]) || in[i + 1] == L'/' || in[i + 1] == L'\\'))
            tmp[n++] = c;
        else tmp[n++] = L' ';
    }
    tmp[n] = 0;
    str_collapse_spaces(tmp);

    /* "docs dot google dot com" -> "docs.google.com" (right to left) */
    static wchar_t w[48][64], o[48][64];
    int nw = split_words(tmp, w, 48), m = 0;
    for (int i = nw - 1; i >= 0; i--) {
        if (str_eq(w[i], L"dot") && i > 0 && m > 0 && (in_list(o[m - 1], TLDS) || wcschr(o[m - 1], L'.'))) {
            wchar_t j[64];
            swprintf(j, 64, L"%ls.%ls", w[i - 1], o[m - 1]);
            str_copy(o[m - 1], 64, j);
            i--;
            continue;
        }
        str_copy(o[m++], 64, w[i]);
    }
    out[0] = 0;
    for (int i = m - 1; i >= 0; i--) {
        if (out[0]) str_cat(out, cap, L" ");
        str_cat(out, cap, o[i]);
    }

    for (bool again = true; again;) {
        again = cut_prefixes(out, LEAD_FILLERS);
        for (const wchar_t *const *p = TRAIL_FILLERS; *p; p++)
            if (cut_suffix(out, *p)) again = true;
        /* "open downloads, jarvis" - but keep it in "search github for jarvis" */
        static WList KEEP_BEFORE = { L"for", L"about", L"called", L"named", L"search", L"find", L"google",
                                     L"youtube", L"github", L"wikipedia", L"open", NULL };
        size_t L = wcslen(out);
        if (L > 7 && str_ends(out, L" jarvis")) {
            wchar_t prev[64] = L"";
            const wchar_t *end = out + L - 7, *b = end;
            while (b > out && b[-1] != L' ') b--;
            if (end - b < 64) { memcpy(prev, b, (size_t)(end - b) * sizeof(wchar_t)); prev[end - b] = 0; }
            if (!in_list(prev, KEEP_BEFORE)) { out[L - 7] = 0; again = true; }
        }
    }
    str_trim(out);
}

static WList ARTICLES = { L"my", L"the", L"a", L"an", L"this", L"that", L"our", L"your", L"some", NULL };

static void strip_articles(const wchar_t *in, wchar_t *out, size_t cap)
{
    str_copy(out, cap, in);
    cut_prefixes(out, ARTICLES);
}

/* ----------------------------------------------------------- real actions */
static bool shell_open(const wchar_t *file, const wchar_t *params)
{
    return (INT_PTR)ShellExecuteW(NULL, L"open", file, params, NULL, SW_SHOWNORMAL) > 32;
}

static bool act_open(const wchar_t *path)
{
    record(L"OPEN %ls", path);
    if (s_dry) return true;
    if (is_shell_path(path)) return shell_open(L"explorer.exe", path);
    return shell_open(path, NULL);
}

static bool act_reveal(const wchar_t *path)
{
    record(L"REVEAL %ls", path);
    if (s_dry) return true;
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
        if (SUCCEEDED(hr)) return true;
    }
    wchar_t args[MAX_PATH + 16];
    swprintf(args, MAX_PATH + 16, L"/select,\"%ls\"", path);
    return shell_open(L"explorer.exe", args);
}

static bool act_dialog(const wchar_t *path)
{
    record(L"DIALOG %ls", path);
    if (s_dry) return true;
    return dialog_fill(s_dialog, path);
}

static bool act_url(const wchar_t *url)
{
    record(L"URL %ls", url);
    if (s_dry) return true;
    wchar_t q[ACT_CAP];
    swprintf(q, ACT_CAP, L"\"%ls\"", url);
    if (g_cfg.browser[0] && shell_open(g_cfg.browser, q)) return true;
    return shell_open(url, NULL);
}

static void press_key(WORD vk, int times)
{
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    for (int i = 0; i < times; i++) SendInput(2, in, sizeof(INPUT));
}

static void act_sys(const wchar_t *name)
{
    record(L"SYS %ls", name);
    if (s_dry) return;
    if (str_eq(name, L"lock")) LockWorkStation();
    else if (str_eq(name, L"screenshot")) shell_open(L"ms-screenclip:", NULL);
    else if (str_eq(name, L"volume_up")) press_key(VK_VOLUME_UP, 5);
    else if (str_eq(name, L"volume_down")) press_key(VK_VOLUME_DOWN, 5);
    else if (str_eq(name, L"mute")) press_key(VK_VOLUME_MUTE, 1);
    else if (str_eq(name, L"reindex")) search_rebuild_async();
}

/* Splits a configured app value into program and arguments. Values can be
 * `chrome.exe --incognito`, `C:\Program Files\X\y.exe -flag`, `"C:\a b\c.exe" x`
 * or a URI like `ms-settings:display`. */
static void split_target(const wchar_t *val, wchar_t *exe, wchar_t *args)
{
    args[0] = 0;
    if (val[0] == L'"') {
        const wchar_t *e = wcschr(val + 1, L'"');
        size_t n = e ? (size_t)(e - val - 1) : wcslen(val + 1);
        if (n >= MAX_PATH) n = MAX_PATH - 1;
        memcpy(exe, val + 1, n * sizeof(wchar_t)); exe[n] = 0;
        if (e) { str_copy(args, MAX_PATH, e + 1); str_trim(args); }
        return;
    }
    /* program extension followed by a space or the end marks the split */
    static WList EXTS = { L".exe", L".lnk", L".bat", L".cmd", L".com", L".msc", L".cpl", NULL };
    for (const wchar_t *p = val; *p; p++) {
        for (const wchar_t *const *x = EXTS; *x; x++) {
            size_t xl = wcslen(*x);
            if (_wcsnicmp(p, *x, xl) == 0 && (p[xl] == 0 || p[xl] == L' ')) {
                size_t n = (size_t)(p - val) + xl;
                if (n >= MAX_PATH) n = MAX_PATH - 1;
                memcpy(exe, val, n * sizeof(wchar_t)); exe[n] = 0;
                str_copy(args, MAX_PATH, val + n); str_trim(args);
                return;
            }
        }
    }
    /* no extension: split before the first " -" / " /" option */
    str_copy(exe, MAX_PATH, val);
    wchar_t *opt = wcsstr(exe, L" -");
    if (!opt) opt = wcsstr(exe, L" /");
    if (opt) { str_copy(args, MAX_PATH, opt + 1); *opt = 0; }
}

/* Can ShellExecute plausibly start this program? */
static bool target_exists(const wchar_t *exe)
{
    if (!exe[0]) return false;
    if (wcschr(exe, L'\\') || (exe[1] == L':' && exe[2])) return path_exists(exe);
    if (wcschr(exe, L':')) return true;                 /* URI: ms-settings:, spotify: ... */
    wchar_t buf[MAX_PATH];
    if (SearchPathW(NULL, exe, L".exe", MAX_PATH, buf, NULL)) return true;
    /* "App Paths" registrations (winword.exe, chrome.exe, ...) */
    wchar_t key[MAX_PATH + 80];
    swprintf(key, MAX_PATH + 80, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls%ls",
             exe, wcschr(exe, L'.') ? L"" : L".exe");
    DWORD cb = sizeof buf;
    if (SHGetValueW(HKEY_CURRENT_USER, key, NULL, NULL, buf, &cb) == ERROR_SUCCESS) return true;
    cb = sizeof buf;
    return SHGetValueW(HKEY_LOCAL_MACHINE, key, NULL, NULL, buf, &cb) == ERROR_SUCCESS;
}

static bool act_app(const wchar_t *val)
{
    wchar_t exe[MAX_PATH], args[MAX_PATH];
    split_target(val, exe, args);
    if (!target_exists(exe)) return false;
    record(L"APP %ls", val);
    if (s_dry) return true;
    return shell_open(exe, args[0] ? args : NULL);
}

/* ------------------------------------------------------------ url helpers */
static void url_encode(const wchar_t *text, wchar_t *out, size_t cap)
{
    char *u = utf8_from_wide(text);
    out[0] = 0;
    if (!u) return;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)u; *p && n + 4 < cap; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') out[n++] = *p;
        else if (*p == ' ') out[n++] = L'+';
        else n += (size_t)swprintf(out + n, 4, L"%%%02X", *p);
    }
    out[n] = 0;
    free(u);
}

/* "youtube.com", "docs.google.com/x", "https://..." */
static bool looks_like_url(const wchar_t *s)
{
    if (wcschr(s, L' ') || !wcschr(s, L'.')) return false;
    if (wcsstr(s, L"://")) return true;
    wchar_t host[256];
    str_copy(host, 256, s);
    wchar_t *slash = wcschr(host, L'/');
    if (slash) *slash = 0;
    const wchar_t *dot = wcsrchr(host, L'.');
    return dot && in_list(dot + 1, TLDS);
}

static void make_url(const wchar_t *s, wchar_t *out, size_t cap)
{
    if (wcsstr(s, L"://")) str_copy(out, cap, s);
    else swprintf(out, cap, L"https://%ls", s);
}

/* --------------------------------------------------------- config lookups */
static const wchar_t *kv_try(const KVList *l, const wchar_t *name, const wchar_t *const *suffixes)
{
    const wchar_t *v = kv_get(l, name);
    if (v || !suffixes) return v;
    for (; *suffixes; suffixes++) {
        wchar_t t[256];
        str_copy(t, 256, name);
        if (cut_suffix(t, *suffixes) && t[0] && (v = kv_get(l, t))) return v;
    }
    return NULL;
}

static WList SITE_SUFFIX   = { L"website", L"web site", L"site", L"page", L"homepage", L"home page", NULL };
static WList APP_SUFFIX    = { L"app", L"application", L"program", NULL };
static WList FOLDER_SUFFIX = { L"folder", L"directory", NULL };

static const wchar_t *site_url(const wchar_t *n)    { return kv_try(&g_cfg.sites, n, SITE_SUFFIX); }
static const wchar_t *app_target(const wchar_t *n)  { return kv_try(&g_cfg.apps, n, APP_SUFFIX); }
static const wchar_t *folder_path(const wchar_t *n) { return kv_try(&g_cfg.folders, n, FOLDER_SUFFIX); }

/* folder usable as a search filter (real directory, not shell:) */
static const wchar_t *filter_folder(const wchar_t *n)
{
    const wchar_t *f = folder_path(n);
    return f && !is_shell_path(f) ? f : NULL;
}

/* "c drive", "see drive", "drive d", "c colon", "c:" -> "C:\" (may not exist) */
static bool parse_drive(const wchar_t *s, wchar_t *out)
{
    static const struct { const wchar_t *w; wchar_t l; } SPOKEN[] = {
        {L"see", L'c'}, {L"sea", L'c'}, {L"cee", L'c'}, {L"si", L'c'}, {L"dee", L'd'}, {L"de", L'd'},
        {L"ee", L'e'}, {L"ef", L'f'}, {L"eff", L'f'}, {L"gee", L'g'}, {L"ji", L'g'},
    };
    wchar_t w[6][64];
    wchar_t t[128];
    strip_articles(s, t, 128);
    cut_prefixes(t, (WList){ L"local disk", L"disk", NULL });
    int n = split_words(t, w, 6);
    const wchar_t *letter = NULL;
    if (n == 1 && wcslen(w[0]) == 2 && w[0][1] == L':') letter = w[0];
    else if (n == 2 && (str_eq(w[1], L"drive") || str_eq(w[1], L"colon"))) letter = w[0];
    else if (n == 2 && str_eq(w[0], L"drive")) letter = w[1];
    if (!letter) return false;
    wchar_t c = 0;
    if (iswalpha(letter[0]) && (letter[1] == 0 || letter[1] == L':')) c = letter[0];
    for (size_t i = 0; !c && i < sizeof SPOKEN / sizeof SPOKEN[0]; i++)
        if (str_eq(letter, SPOKEN[i].w)) c = SPOKEN[i].l;
    if (!c) return false;
    swprintf(out, 8, L"%lc:\\", (wint_t)towupper(c));
    return true;
}

/* ------------------------------------------------------------ file queries */
typedef struct {
    wchar_t  text[256];
    wchar_t  folder[MAX_PATH];
    ExtClass ext;
    unsigned flags;
    bool     typed;     /* had a type / recency / folder filter: it's a file query */
} FileQ;

static ExtClass type_word(const wchar_t *t)
{
    static const struct { const wchar_t *w; ExtClass e; } T[] = {
        {L"photo",EXT_IMAGE},{L"photos",EXT_IMAGE},{L"image",EXT_IMAGE},{L"images",EXT_IMAGE},
        {L"picture",EXT_IMAGE},{L"pictures",EXT_IMAGE},{L"pic",EXT_IMAGE},{L"pics",EXT_IMAGE},
        {L"screenshot",EXT_IMAGE},{L"screenshots",EXT_IMAGE},{L"png",EXT_IMAGE},{L"jpg",EXT_IMAGE},{L"jpeg",EXT_IMAGE},
        {L"pdf",EXT_PDF},{L"pdfs",EXT_PDF},
        {L"ppt",EXT_PPT},{L"pptx",EXT_PPT},{L"powerpoint",EXT_PPT},{L"powerpoints",EXT_PPT},
        {L"presentation",EXT_PPT},{L"presentations",EXT_PPT},{L"slides",EXT_PPT},{L"slide",EXT_PPT},{L"deck",EXT_PPT},
        {L"doc",EXT_DOC},{L"docx",EXT_DOC},{L"document",EXT_DOC},{L"documents",EXT_DOC},
        {L"excel",EXT_SHEET},{L"spreadsheet",EXT_SHEET},{L"spreadsheets",EXT_SHEET},{L"sheet",EXT_SHEET},
        {L"sheets",EXT_SHEET},{L"xlsx",EXT_SHEET},{L"xls",EXT_SHEET},{L"csv",EXT_SHEET},
        {L"video",EXT_VIDEO},{L"videos",EXT_VIDEO},{L"movie",EXT_VIDEO},{L"movies",EXT_VIDEO},{L"mp4",EXT_VIDEO},
        {L"song",EXT_AUDIO},{L"songs",EXT_AUDIO},{L"audio",EXT_AUDIO},{L"mp3",EXT_AUDIO},
        {L"zip",EXT_ARCHIVE},{L"zips",EXT_ARCHIVE},{L"archive",EXT_ARCHIVE},{L"archives",EXT_ARCHIVE},{L"rar",EXT_ARCHIVE},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (str_eq(t, T[i].w)) return T[i].e;
    return EXT_ANY;
}

static void build_query(const wchar_t *src, FileQ *q, unsigned flags)
{
    static WList PREPS = { L"in", L"from", L"on", L"inside", L"under", L"at", NULL };
    static WList RECENT = { L"latest", L"newest", L"last", L"recent", L"recently", NULL };
    static WList FILEWORDS = { L"file", L"files", L"document", L"documents", L"doc", L"docs", NULL };
    static WList SHEETWORDS = { L"file", L"files", L"sheet", L"sheets", L"spreadsheet", L"workbook", NULL };
    memset(q, 0, sizeof *q);
    wchar_t w[32][64];
    int n = split_words(src, w, 32);

    /* "... in downloads" / "... on my desktop" */
    for (int i = n - 2; i >= 0; i--) {
        if (!in_list(w[i], PREPS)) continue;
        wchar_t ph[256], ph2[256];
        join_words(w, i + 1, n, ph, 256);
        strip_articles(ph, ph2, 256);
        const wchar_t *f = filter_folder(ph2);
        if (f) { str_copy(q->folder, MAX_PATH, f); n = i; break; }
    }

    bool recent = false;
    for (int i = 0; i < n; i++) {
        const wchar_t *t = w[i], *nx = i + 1 < n ? w[i + 1] : L"";
        if (in_list(t, ARTICLES) || str_eq(t, L"called") || str_eq(t, L"named")) continue;
        if (str_eq(t, L"most") && str_eq(nx, L"recent")) { recent = true; i++; continue; }
        if (in_list(t, RECENT)) { recent = true; continue; }
        if (str_eq(t, L"word") && in_list(nx, FILEWORDS)) { q->ext = EXT_DOC; i++; continue; }
        if (str_eq(t, L"excel") && in_list(nx, SHEETWORDS)) { q->ext = EXT_SHEET; i++; continue; }
        if (str_eq(t, L"power") && (str_eq(nx, L"point") || str_eq(nx, L"points"))) { q->ext = EXT_PPT; i++; continue; }
        ExtClass e = type_word(t);
        if (e) {
            q->ext = e;
            if (str_starts(t, L"screenshot")) { if (q->text[0]) str_cat(q->text, 256, L" "); str_cat(q->text, 256, L"screenshot"); }
            continue;
        }
        if (str_eq(t, L"file") || str_eq(t, L"files")) { flags |= SF_FILES_ONLY; continue; }
        if (str_eq(t, L"folder") || str_eq(t, L"folders") || str_eq(t, L"directory")) { flags |= SF_DIRS_ONLY; continue; }
        if (q->text[0]) str_cat(q->text, 256, L" ");
        str_cat(q->text, 256, t);
    }

    /* "latest download" / "downloads pdf": a folder name becomes the folder filter */
    if (!q->folder[0] && (recent || q->ext)) {
        int m = split_words(q->text, w, 32);
        for (int i = 0; i < m; i++) {
            const wchar_t *f = filter_folder(str_eq(w[i], L"downloaded") ? L"downloads" : w[i]);
            if (f) { str_copy(q->folder, MAX_PATH, f); w[i][0] = 0; join_words(w, 0, m, q->text, 256); break; }
        }
    }
    if ((flags & SF_DIRS_ONLY) && (flags & SF_FILES_ONLY)) flags &= ~(unsigned)SF_FILES_ONLY;
    if (recent) flags |= SF_RECENT;
    q->flags = flags;
    q->typed = recent || q->ext || q->folder[0] || (flags & (SF_FILES_ONLY | SF_DIRS_ONLY));
}

static int run_query(const FileQ *q, Hit *hits, int max)
{
    SearchQuery sq = { q->text, q->folder[0] ? q->folder : NULL, q->ext, q->flags };
    return search_files(&sq, hits, max);
}

/* The index may still be building on first run: wait up to ~8 s. */
static bool wait_index(void)
{
    for (int i = 0; i < 160 && !search_ready(); i++) Sleep(50);
    return search_ready();
}

/* ---------------------------------------------------------------- results */
static void show_list(const Hit *hits, int n, PickMode mode, CmdResult *r)
{
    r->nrows = n;
    for (int i = 0; i < n; i++) {
        memset(&r->rows[i], 0, sizeof(Row));
        r->rows[i].kind = ROW_HIT;
        r->rows[i].hit = hits[i];
        s_rows[i] = r->rows[i];
    }
    s_nrows = n;
    s_mode = mode;
    r->pick_mode = mode;
    r->rows_replace = true;
    r->listen_again = true;
    r->speak = true;
    record(L"LIST %d", n);
}

/* Open a resolved file or folder (into the dialog when one is active). */
static void deliver(const wchar_t *path, CmdResult *r)
{
    wchar_t nm[128];
    short_name(path, nm, 128);
    if (dialog_active() && act_dialog(path)) reply(r, L"Selected %ls.", nm);
    else { act_open(path); reply(r, L"Opening %ls.", nm); }
    done(r);
}

typedef enum { FM_OPEN, FM_FIND, FM_REVEAL, FM_UPLOAD } FileMode;

static void web_search(int engine, const wchar_t *q, CmdResult *r);

static void do_files(const wchar_t *display, const FileQ *q, FileMode mode, bool play, CmdResult *r)
{
    if (!q->text[0] && !q->typed) {
        reply(r, L"What should I look for?");
        record(L"SAY");
        r->listen_again = true;
        return;
    }
    if (!wait_index()) {
        reply(r, L"Still indexing your files, try again in a moment.");
        record(L"SAY");
        return;
    }
    Hit hits[MAX_HITS];
    int n = run_query(q, hits, MAX_HITS);
    if (n == 0) {
        if (play) { web_search(1, display, r); return; }   /* "play X": nothing local -> YouTube */
        wchar_t d[128];
        str_copy(d, 128, display);
        reply(r, L"I couldn't find %ls.", d);
        record(L"SAY");
        r->listen_again = true;
        return;
    }
    bool dlg = dialog_active();
    bool confident = (q->flags & SF_RECENT) ||
                     (hits[0].score >= 60 && (n == 1 || hits[0].score - hits[1].score >= 12));
    switch (mode) {
    case FM_FIND:
        show_list(hits, n, dlg ? PICK_DIALOG : PICK_OPEN, r);
        reply(r, L"Here's what I found.");
        return;
    case FM_REVEAL: {
        wchar_t nm[128];
        short_name(hits[0].path, nm, 128);
        act_reveal(hits[0].path);
        reply(r, L"Here's %ls.", nm);
        done(r);
        return;
    }
    case FM_UPLOAD:
        if (!dlg) {
            show_list(hits, n, PICK_REVEAL, r);
            reply(r, L"No upload window is open, here's where it is.");
            return;
        }
        break;
    case FM_OPEN:
        break;
    }
    if (confident) { deliver(hits[0].path, r); return; }
    show_list(hits, n, dlg ? PICK_DIALOG : PICK_OPEN, r);
    if (n == 1) reply(r, L"I found one possible match. Say one to open it.");
    else reply(r, L"I found %d matches. Which one?", n);
}

/* ------------------------------------------------------------- web search */
enum { W_GOOGLE, W_YOUTUBE, W_WIKI, W_GITHUB, W_MAPS, W_DIR };

static const struct { const wchar_t *pre, *suf; int engine; } WEB[] = {
    /* youtube */
    {L"search youtube for", L"", W_YOUTUBE}, {L"search on youtube for", L"", W_YOUTUBE},
    {L"youtube search for", L"", W_YOUTUBE}, {L"youtube search", L"", W_YOUTUBE}, {L"search youtube", L"", W_YOUTUBE},
    {L"play", L"on youtube", W_YOUTUBE}, {L"watch", L"on youtube", W_YOUTUBE}, {L"search for", L"on youtube", W_YOUTUBE},
    {L"search", L"on youtube", W_YOUTUBE}, {L"find", L"on youtube", W_YOUTUBE}, {L"look up", L"on youtube", W_YOUTUBE},
    {L"open", L"on youtube", W_YOUTUBE}, {L"", L"on youtube", W_YOUTUBE}, {L"youtube", L"", W_YOUTUBE},
    /* wikipedia */
    {L"search wikipedia for", L"", W_WIKI}, {L"wikipedia search for", L"", W_WIKI}, {L"search wikipedia", L"", W_WIKI},
    {L"look up", L"on wikipedia", W_WIKI}, {L"search for", L"on wikipedia", W_WIKI}, {L"search", L"on wikipedia", W_WIKI},
    {L"", L"on wikipedia", W_WIKI}, {L"wikipedia", L"", W_WIKI}, {L"wiki", L"", W_WIKI},
    /* github */
    {L"search github for", L"", W_GITHUB}, {L"github search for", L"", W_GITHUB}, {L"search github", L"", W_GITHUB},
    {L"search for", L"on github", W_GITHUB}, {L"search", L"on github", W_GITHUB}, {L"find", L"on github", W_GITHUB},
    {L"look up", L"on github", W_GITHUB}, {L"", L"on github", W_GITHUB}, {L"github", L"", W_GITHUB},
    /* maps */
    {L"get directions to", L"", W_DIR}, {L"give me directions to", L"", W_DIR}, {L"directions to", L"", W_DIR},
    {L"direction to", L"", W_DIR}, {L"how do i get to", L"", W_DIR},
    {L"where is", L"on the map", W_MAPS}, {L"where is", L"on map", W_MAPS}, {L"where is", L"on maps", W_MAPS},
    {L"show", L"on the map", W_MAPS}, {L"show", L"on maps", W_MAPS}, {L"find", L"on the map", W_MAPS},
    {L"find", L"on maps", W_MAPS}, {L"", L"on the map", W_MAPS}, {L"", L"on maps", W_MAPS},
    {L"", L"on google maps", W_MAPS}, {L"map of", L"", W_MAPS}, {L"a map of", L"", W_MAPS},
    {L"google maps", L"", W_MAPS}, {L"maps", L"", W_MAPS},
    /* google */
    {L"search google for", L"", W_GOOGLE}, {L"google search for", L"", W_GOOGLE}, {L"google search", L"", W_GOOGLE},
    {L"search google", L"", W_GOOGLE}, {L"google for", L"", W_GOOGLE}, {L"search the web for", L"", W_GOOGLE},
    {L"search the internet for", L"", W_GOOGLE}, {L"search web for", L"", W_GOOGLE},
    {L"search internet for", L"", W_GOOGLE}, {L"search online for", L"", W_GOOGLE},
    {L"search the web", L"", W_GOOGLE}, {L"search online", L"", W_GOOGLE},
    {L"look up", L"on google", W_GOOGLE}, {L"search for", L"on google", W_GOOGLE}, {L"search", L"on google", W_GOOGLE},
    {L"", L"on google", W_GOOGLE}, {L"look up", L"", W_GOOGLE}, {L"lookup", L"", W_GOOGLE}, {L"google", L"", W_GOOGLE},
};

/* Does s ask for a web search? Fills the query text. */
static bool match_web(const wchar_t *s, int *engine, wchar_t *query, size_t cap)
{
    for (size_t i = 0; i < sizeof WEB / sizeof WEB[0]; i++) {
        const wchar_t *p = WEB[i].pre[0] ? str_after_word(s, WEB[i].pre) : s;
        if (!p) continue;
        str_copy(query, cap, p);
        if (WEB[i].suf[0] && !cut_suffix(query, WEB[i].suf)) continue;
        str_trim(query);
        if (!query[0] || str_eq(query, L"for")) continue;
        *engine = WEB[i].engine;
        return true;
    }
    return false;
}

static void web_search(int engine, const wchar_t *q, CmdResult *r)
{
    static const wchar_t *BASE[] = {
        L"https://www.google.com/search?q=", L"https://www.youtube.com/results?search_query=",
        L"https://en.wikipedia.org/w/index.php?search=", L"https://github.com/search?q=",
        L"https://www.google.com/maps/search/", L"https://www.google.com/maps/dir/?api=1&destination=",
    };
    static const wchar_t *SAYS[] = {
        L"Searching Google for %ls.", L"Searching YouTube for %ls.", L"Searching Wikipedia for %ls.",
        L"Searching GitHub for %ls.", L"Showing %ls on the map.", L"Getting directions to %ls.",
    };
    wchar_t enc[1800], url[ACT_CAP], d[128];
    url_encode(q, enc, 1800);
    swprintf(url, ACT_CAP, L"%ls%ls", BASE[engine], enc);
    act_url(url);
    str_copy(d, 128, q);
    reply(r, SAYS[engine], d);
    done(r);
}

/* --------------------------------------------------------- name resolution */
/* Configured site / app / folder with exactly this name. */
static bool try_config(const wchar_t *name, CmdResult *r)
{
    wchar_t disp[128];
    title_case(name, disp, 128);
    const wchar_t *v;
    if ((v = site_url(name))) {
        act_url(v);
        reply(r, L"Opening %ls.", disp);
        done(r);
        return true;
    }
    if ((v = app_target(name))) {
        wchar_t lnk[MAX_PATH], an[128];
        if (act_app(v)) reply(r, L"Opening %ls.", disp);
        else if (search_app(name, lnk, an) >= 65) { act_app(lnk); reply(r, L"Opening %ls.", an); }
        else {
            reply(r, L"I couldn't find %ls on this PC.", disp);
            record(L"SAY");
            return true;
        }
        done(r);
        return true;
    }
    if ((v = folder_path(name)) && (is_shell_path(v) || path_is_dir(v))) {
        if (dialog_active() && act_dialog(v)) reply(r, L"Going to %ls.", disp);
        else { act_open(v); reply(r, L"Opening %ls.", disp); }
        done(r);
        return true;
    }
    return false;
}

static bool try_drive_or_url(const wchar_t *name, CmdResult *r)
{
    wchar_t drv[8], url[ACT_CAP];
    if (parse_drive(name, drv)) {
        if (!path_is_dir(drv)) {
            reply(r, L"There's no drive %lc on this PC.", drv[0]);
            record(L"SAY");
            return true;
        }
        if (dialog_active() && act_dialog(drv)) reply(r, L"Going to drive %lc.", drv[0]);
        else { act_open(drv); reply(r, L"Opening drive %lc.", drv[0]); }
        done(r);
        return true;
    }
    if (looks_like_url(name)) {
        make_url(name, url, ACT_CAP);
        act_url(url);
        reply(r, L"Opening %ls.", name);
        done(r);
        return true;
    }
    return false;
}

/* "open X" / bare "X" */
static void resolve_open(const wchar_t *rest, bool play, CmdResult *r)
{
    wchar_t name[256];
    strip_articles(rest, name, 256);
    if (!name[0]) { reply(r, L"What should I open?"); record(L"SAY"); r->listen_again = true; return; }
    if (try_config(name, r)) return;
    int engine; wchar_t q[256];
    if (match_web(name, &engine, q, 256)) { web_search(engine, q, r); return; }
    if (try_drive_or_url(name, r)) return;

    FileQ fq;
    build_query(name, &fq, 0);
    if (!fq.typed) {
        wchar_t lnk[MAX_PATH], an[128];
        if (search_app(name, lnk, an) >= 80) {
            act_app(lnk);
            reply(r, L"Opening %ls.", an);
            done(r);
            return;
        }
    }
    do_files(name, &fq, FM_OPEN, play, r);
}

/* "go to X": folders first */
static void resolve_goto(const wchar_t *rest, CmdResult *r)
{
    wchar_t name[256];
    strip_articles(rest, name, 256);
    const wchar_t *v = folder_path(name);
    if (v && (is_shell_path(v) || path_is_dir(v))) { try_config(name, r); return; }
    if (site_url(name)) { try_config(name, r); return; }
    if (try_drive_or_url(name, r)) return;
    FileQ fq;
    build_query(name, &fq, SF_DIRS_ONLY);
    do_files(name, &fq, FM_OPEN, false, r);
}

static void resolve_upload(const wchar_t *rest, CmdResult *r)
{
    wchar_t name[256];
    strip_articles(rest, name, 256);
    const wchar_t *v = folder_path(name);
    if (dialog_active() && v) { try_config(name, r); return; }
    FileQ fq;
    build_query(name, &fq, 0);
    do_files(name, &fq, FM_UPLOAD, false, r);
}

/* ------------------------------------------------------------ fixed phrases */
static WList CLOSE_W = { L"cancel", L"never mind", L"nevermind", L"close", L"close it", L"exit", L"stop", L"quit",
    L"bye", L"goodbye", L"good bye", L"nothing", L"no", L"nope", L"dismiss", L"forget it", L"that's all",
    L"thats all", L"hide", L"go away", L"cancel that", L"close jarvis", NULL };
static WList HELP_W = { L"help", L"help me", L"what can you do", L"what can i say", L"what do you do", L"commands",
    L"show commands", L"list commands", L"what are the commands", L"options", NULL };
static WList TIME_W = { L"time", L"what time is it", L"what's the time", L"whats the time", L"what is the time",
    L"tell me the time", L"what time", L"current time", L"the time", L"time now", L"what's the time now", NULL };
static WList DATE_W = { L"date", L"what's the date", L"whats the date", L"what is the date", L"what day is it",
    L"what's today", L"whats today", L"what is today", L"today's date", L"todays date", L"what's today's date",
    L"what is today's date", L"the date", L"what day is today", L"today", L"what's the date today",
    L"what is the date today", L"tell me the date", NULL };
static WList LOCK_W = { L"lock", L"lock it", L"lock pc", L"lock the pc", L"lock my pc", L"lock computer",
    L"lock the computer", L"lock my computer", L"lock screen", L"lock the screen", L"lock laptop",
    L"lock my laptop", L"lock the laptop", L"lock windows", NULL };
static WList SHOT_W = { L"screenshot", L"take a screenshot", L"take screenshot", L"screen shot",
    L"take a screen shot", L"capture screen", L"capture the screen", L"snip", L"screen capture", L"take a snip", NULL };
static WList VUP_W = { L"volume up", L"turn up the volume", L"turn the volume up", L"turn volume up",
    L"increase volume", L"increase the volume", L"raise volume", L"raise the volume", L"louder",
    L"make it louder", L"sound up", NULL };
static WList VDN_W = { L"volume down", L"turn down the volume", L"turn the volume down", L"turn volume down",
    L"decrease volume", L"decrease the volume", L"lower volume", L"lower the volume", L"quieter",
    L"make it quieter", L"softer", L"sound down", NULL };
static WList MUTE_W = { L"mute", L"unmute", L"un mute", L"mute volume", L"mute the volume", L"mute sound",
    L"mute the sound", L"mute audio", L"toggle mute", L"silence", L"mute it", NULL };
static WList REINDEX_W = { L"reindex", L"re index", L"refresh index", L"refresh the index", L"rebuild index",
    L"rebuild the index", L"update index", L"update the index", L"rescan", L"re scan", L"rescan files",
    L"refresh files", L"index files", L"reindex files", NULL };

static bool is_battery(const wchar_t *s)
{
    return (contains_word(s, L"battery") || contains_word(s, L"charge")) && wcslen(s) < 48;
}

static bool is_fixed(const wchar_t *s)
{
    const wchar_t *const *lists[] = { CLOSE_W, HELP_W, TIME_W, DATE_W, LOCK_W, SHOT_W, VUP_W, VDN_W, MUTE_W, REINDEX_W };
    for (size_t i = 0; i < sizeof lists / sizeof lists[0]; i++) if (in_list(s, lists[i])) return true;
    return is_battery(s);
}

static void show_help(CmdResult *r)
{
    static const wchar_t *H[][2] = {
        {L"open downloads", L"Open a folder, app or website"},
        {L"open latest photo", L"Open your newest file of a kind"},
        {L"find resume", L"Search your files"},
        {L"upload resume pdf", L"Fill an Upload / Open window"},
        {L"google black holes", L"Search the web, YouTube, Wikipedia, maps"},
        {L"show essay in folder", L"Show a file in Explorer"},
        {L"what time is it", L"Time, date and battery"},
        {L"volume up, mute, lock", L"Control your PC"},
    };
    int n = (int)(sizeof H / sizeof H[0]);
    if (n > MAX_HITS) n = MAX_HITS;
    for (int i = 0; i < n; i++) {
        memset(&r->rows[i], 0, sizeof(Row));
        r->rows[i].kind = ROW_INFO;
        str_copy(r->rows[i].title, MAX_PATH, H[i][0]);
        str_copy(r->rows[i].sub, MAX_PATH, H[i][1]);
    }
    r->nrows = n;
    r->rows_replace = true;
    r->listen_again = true;
    s_nrows = 0;
    reply(r, L"Here are a few things you can say.");
    record(L"SAY");
}

static bool run_fixed(const wchar_t *s, CmdResult *r)
{
    wchar_t buf[128];
    if (in_list(s, CLOSE_W)) { record(L"CLOSE"); reply(r, L"Okay."); done(r); return true; }
    if (in_list(s, HELP_W)) { show_help(r); return true; }
    if (in_list(s, TIME_W)) {
        if (!GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, NULL, NULL, buf, 128)) buf[0] = 0;
        reply(r, L"It's %ls.", buf);
        record(L"SAY"); done(r); return true;
    }
    if (in_list(s, DATE_W)) {
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, buf, 128, NULL)) buf[0] = 0;
        reply(r, L"Today is %ls.", buf);
        record(L"SAY"); done(r); return true;
    }
    if (is_battery(s)) {
        SYSTEM_POWER_STATUS ps;
        if (!GetSystemPowerStatus(&ps) || ps.BatteryFlag == 128 || ps.BatteryLifePercent == 255)
            reply(r, L"This PC doesn't report a battery.");
        else
            reply(r, L"Battery is at %d percent%ls.", ps.BatteryLifePercent,
                  (ps.BatteryFlag & 8) ? L", charging" : ps.ACLineStatus == 1 ? L", plugged in" : L"");
        record(L"SAY"); done(r); return true;
    }
    if (in_list(s, LOCK_W))    { act_sys(L"lock"); reply(r, L"Locking."); done(r); return true; }
    if (in_list(s, SHOT_W))    { act_sys(L"screenshot"); reply(r, L"Select an area."); done(r); return true; }
    if (in_list(s, VUP_W))     { act_sys(L"volume_up"); reply(r, L"Volume up."); done(r); return true; }
    if (in_list(s, VDN_W))     { act_sys(L"volume_down"); reply(r, L"Volume down."); done(r); return true; }
    if (in_list(s, MUTE_W))    { act_sys(L"mute"); reply(r, L"Toggled mute."); done(r); return true; }
    if (in_list(s, REINDEX_W)) {
        record(L"SYS reindex");
        search_rebuild_async();     /* safe in dry-run too: it only refreshes our own index */
        reply(r, L"Re-indexing your files.");
        done(r);
        return true;
    }
    return false;
}

/* ----------------------------------------------------- picking from a list */
/* "two", "number 3", "the second one", "open 2", "show the first one in folder".
 * Returns row number (1-based) or 0; *reveal set for "in folder" style picks. */
static int parse_pick(const wchar_t *s, bool *reveal)
{
    static WList REVEAL_ONLY = { L"open folder", L"open the folder", L"show in folder", L"show it in folder",
        L"show in explorer", L"show folder", L"reveal", L"reveal it", L"open file location",
        L"open location", L"open containing folder", L"open the containing folder", L"show me where it is", NULL };
    static WList REVEAL_SUF = { L"in folder", L"in the folder", L"in explorer", L"in file explorer", NULL };
    static WList LEAD = { L"open", L"pick", L"choose", L"select", L"upload", L"attach", L"play", L"show",
        L"reveal", L"use", L"go with", L"number", L"no", L"num", L"option", L"the", L"it", L"me", L"launch", NULL };
    static WList YES = { L"yes", L"yeah", L"yep", L"open it", L"that one", L"this one", L"do it", L"sure", NULL };
    *reveal = false;
    if (in_list(s, REVEAL_ONLY)) { *reveal = true; return 1; }
    if (s_nrows == 1 && in_list(s, YES)) return 1;
    wchar_t t[256];
    str_copy(t, 256, s);
    for (const wchar_t *const *p = REVEAL_SUF; *p; p++) if (cut_suffix(t, *p)) *reveal = true;
    if (str_after_word(t, L"reveal")) *reveal = true;
    cut_prefixes(t, LEAD);
    if (!str_eq(t, L"one")) cut_suffix(t, L"one");     /* "second one" -> "second" */
    cut_suffix(t, L"file");
    cut_suffix(t, L"result");
    if (!t[0] || wcschr(t, L' ')) return 0;
    int n = number_word(t);
    return n >= 1 && n <= s_nrows ? n : 0;
}

/* ---------------------------------------------------------------- public */
void cmd_init(HWND dialog)
{
    s_dialog = dialog;
    s_nrows = 0;
    s_action[0] = 0;
}

HWND cmd_dialog(void) { return s_dialog; }
void cmd_set_dry_run(bool on) { s_dry = on; }
const wchar_t *cmd_last_action(void) { return s_action; }

void cmd_pick(const Row *row, PickMode mode, CmdResult *r)
{
    memset(r, 0, sizeof *r);
    if (!row || row->kind != ROW_HIT) { reply(r, L"That's not something I can open."); record(L"SAY"); return; }
    Row rw = *row;                          /* row may point into s_rows */
    wchar_t nm[128];
    short_name(rw.hit.path, nm, 128);
    if (mode == PICK_REVEAL) {
        act_reveal(rw.hit.path);
        reply(r, L"Here's %ls.", nm);
    } else if (mode == PICK_DIALOG && dialog_active() && act_dialog(rw.hit.path)) {
        reply(r, L"Selected %ls.", nm);
    } else {
        act_open(rw.hit.path);
        reply(r, L"Opening %ls.", nm);
    }
    done(r);
}

static const struct { const wchar_t *pre, *suf; } REVEAL[] = {
    {L"show me", L"in folder"}, {L"show me", L"in the folder"}, {L"show me", L"in explorer"},
    {L"show", L"in folder"}, {L"show", L"in the folder"}, {L"show", L"in explorer"}, {L"show", L"in file explorer"},
    {L"open", L"in folder"}, {L"open", L"in explorer"}, {L"locate", L"in explorer"},
    {L"reveal", L""}, {L"open the folder containing", L""}, {L"open folder containing", L""},
    {L"open the folder of", L""}, {L"open folder of", L""}, {L"open the containing folder of", L""},
    {L"open containing folder of", L""}, {L"open the file location of", L""}, {L"open file location of", L""},
    {L"open location of", L""},
};
static WList FIND_V = { L"search my computer for", L"search my files for", L"search files for", L"search for",
    L"search", L"find me", L"find", L"where is", L"where's", L"wheres", L"where are", L"locate",
    L"look for", NULL };
static WList GOTO_V = { L"go to", L"goto", L"take me to", L"navigate to", L"switch to", L"jump to",
    L"open the folder", L"open folder", NULL };
static WList UPLOAD_V = { L"upload", L"attach", L"select", L"pick", L"choose", NULL };
static WList OPEN_V = { L"open up", L"open", L"launch", L"start", L"run", L"show me", L"show", L"play",
    L"bring up", L"pull up", L"load", L"view", L"display", NULL };
static WList BROWSER_SUF = { L"in chrome", L"in google chrome", L"in the browser", L"in browser",
    L"on chrome", L"in my browser", NULL };

static bool match_reveal(const wchar_t *s, wchar_t *out, size_t cap)
{
    for (size_t i = 0; i < sizeof REVEAL / sizeof REVEAL[0]; i++) {
        const wchar_t *p = str_after_word(s, REVEAL[i].pre);
        if (!p) continue;
        str_copy(out, cap, p);
        if (REVEAL[i].suf[0] && !cut_suffix(out, REVEAL[i].suf)) continue;
        if (out[0]) return true;
    }
    return false;
}

/* "open X in chrome": X is a site name or a web address */
static bool try_browser(const wchar_t *s, CmdResult *r)
{
    wchar_t t[256], name[256], url[ACT_CAP];
    str_copy(t, 256, s);
    bool hit = false;
    for (const wchar_t *const *p = BROWSER_SUF; *p && !hit; p++) hit = cut_suffix(t, *p);
    if (!hit) return false;
    cut_prefixes(t, OPEN_V);
    cut_prefixes(t, GOTO_V);
    strip_articles(t, name, 256);
    if (!name[0]) return false;
    const wchar_t *v = site_url(name);
    if (v) str_copy(url, ACT_CAP, v);
    else if (wcschr(name, L'.')) make_url(name, url, ACT_CAP);
    else if (!wcschr(name, L' ')) swprintf(url, ACT_CAP, L"https://www.%ls.com", name);
    else { web_search(W_GOOGLE, name, r); return true; }
    wchar_t disp[128];
    title_case(name, disp, 128);
    act_url(url);
    reply(r, L"Opening %ls.", disp);
    done(r);
    return true;
}

void cmd_run(const wchar_t *text, CmdResult *r)
{
    memset(r, 0, sizeof *r);
    r->speak = true;
    s_action[0] = 0;
    wchar_t s[S_CAP];
    normalize(text ? text : L"", s, S_CAP);
    if (!s[0]) {
        reply(r, L"Yes?");
        record(L"SAY");
        r->listen_again = true;
        return;
    }
    log_line(L"cmd: %ls", s);

    /* follow-up to a list: "two", "the second one", "open folder" */
    if (s_nrows > 0 && s_rows[0].kind == ROW_HIT) {
        bool reveal;
        int n = parse_pick(s, &reveal);
        if (n) { cmd_pick(&s_rows[n - 1], reveal ? PICK_REVEAL : s_mode, r); return; }
    }
    if (run_fixed(s, r)) return;

    /* an exact configured name wins over everything ("google drive", "youtube") */
    {
        wchar_t name[256];
        strip_articles(s, name, 256);
        if (name[0] && try_config(name, r)) return;
    }
    if (try_browser(s, r)) return;

    wchar_t q[S_CAP];
    int engine;
    if (match_web(s, &engine, q, S_CAP)) { web_search(engine, q, r); return; }

    FileQ fq;
    if (match_reveal(s, q, S_CAP)) {
        wchar_t name[256];
        strip_articles(q, name, 256);
        build_query(name, &fq, 0);
        do_files(name, &fq, FM_REVEAL, false, r);
        return;
    }
    const wchar_t *rest;
    for (const wchar_t *const *v = FIND_V; *v; v++)
        if ((rest = str_after_word(s, *v)) && *rest) {
            wchar_t name[256];
            strip_articles(rest, name, 256);
            build_query(name, &fq, 0);
            do_files(name, &fq, FM_FIND, false, r);
            return;
        }
    for (const wchar_t *const *v = GOTO_V; *v; v++)
        if ((rest = str_after_word(s, *v)) && *rest) { resolve_goto(rest, r); return; }
    for (const wchar_t *const *v = UPLOAD_V; *v; v++)
        if ((rest = str_after_word(s, *v)) && *rest) { resolve_upload(rest, r); return; }
    for (const wchar_t *const *v = OPEN_V; *v; v++)
        if ((rest = str_after_word(s, *v))) { resolve_open(rest, str_eq(*v, L"play"), r); return; }
    resolve_open(s, false, r);
}

int cmd_preview(const wchar_t *text, Row *rows, int max)
{
    wchar_t s[S_CAP], q[S_CAP], name[256];
    int engine;
    normalize(text ? text : L"", s, S_CAP);
    if (!s[0] || is_fixed(s) || match_web(s, &engine, q, S_CAP)) return 0;
    if (match_reveal(s, q, S_CAP)) str_copy(s, S_CAP, q);
    else {
        const wchar_t *const *lists[] = { FIND_V, GOTO_V, UPLOAD_V, OPEN_V };
        for (size_t i = 0; i < 4; i++) if (cut_prefixes(s, lists[i])) break;
    }
    strip_articles(s, name, 256);
    if (wcslen(name) < 2 || !search_ready()) return 0;
    FileQ fq;
    build_query(name, &fq, 0);
    if (!fq.text[0] && !fq.typed) return 0;
    Hit hits[MAX_HITS];
    if (max > MAX_HITS) max = MAX_HITS;
    int n = run_query(&fq, hits, max);
    for (int i = 0; i < n; i++) {
        memset(&rows[i], 0, sizeof(Row));
        rows[i].kind = ROW_HIT;
        rows[i].hit = hits[i];
    }
    return n;
}

/* names the recognizer can handle: letters, digits, space, apostrophe, hyphen */
static bool speakable(const wchar_t *s)
{
    if (!s[0]) return false;
    for (; *s; s++) if (!iswalnum(*s) && *s != L' ' && *s != L'\'' && *s != L'-') return false;
    return true;
}

void cmd_grammar(PhraseFn fn, void *ctx)
{
    static WList VERBS = { L"open", L"go to", L"launch", L"start", L"show", L"upload", L"find", NULL };
    static WList PHRASES = { L"cancel", L"never mind", L"close", L"help", L"what time is it", L"what's the date",
        L"battery", L"lock", L"screenshot", L"volume up", L"volume down", L"mute", L"number one", L"number two",
        L"number three", L"number four", L"number five", L"number six", L"number seven", L"number eight",
        L"first", L"second", L"third", L"fourth", L"fifth", L"sixth", L"seventh", L"eighth",
        L"show in folder", L"what can you do", L"what day is it", NULL };
    for (const wchar_t *const *v = VERBS; *v; v++) fn(*v, NULL, ctx);
    const KVList *lists[] = { &g_cfg.sites, &g_cfg.apps, &g_cfg.folders };
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < lists[l]->count; i++)
            if (speakable(lists[l]->items[i].key)) fn(NULL, lists[l]->items[i].key, ctx);
    /* complete phrases: split into first word + rest (rest "" for one-word phrases) */
    for (const wchar_t *const *p = PHRASES; *p; p++) {
        wchar_t verb[64];
        const wchar_t *sp = wcschr(*p, L' ');
        size_t n = sp ? (size_t)(sp - *p) : wcslen(*p);
        if (n > 63) n = 63;
        memcpy(verb, *p, n * sizeof(wchar_t)); verb[n] = 0;
        fn(verb, sp ? sp + 1 : L"", ctx);
    }
}

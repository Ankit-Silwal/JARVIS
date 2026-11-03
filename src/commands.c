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
static wchar_t  s_last[MAX_PATH];       /* last file opened / listed / produced ("it") */
static wchar_t  s_sel[DOC_MAX_SRC][MAX_PATH];   /* Explorer selection at start ("this") */
static int      s_nsel;
static void set_last(const wchar_t *p);

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

/* "google drive" -> "Google Drive", "vs code" -> "VS Code" (for replies) */
static void title_case(const wchar_t *in, wchar_t *out, size_t cap)
{
    static const wchar_t *const SPECIAL[][2] = {
        {L"vs", L"VS"}, {L"pdf", L"PDF"}, {L"ppt", L"PPT"}, {L"pc", L"PC"}, {L"usb", L"USB"},
        {L"ai", L"AI"}, {L"cmd", L"CMD"}, {L"wifi", L"Wi-Fi"}, {L"wi-fi", L"Wi-Fi"}, {L"vscode", L"VS Code"},
        {L"youtube", L"YouTube"}, {L"github", L"GitHub"}, {L"chatgpt", L"ChatGPT"}, {L"linkedin", L"LinkedIn"},
        {L"whatsapp", L"WhatsApp"}, {L"onedrive", L"OneDrive"}, {L"powerpoint", L"PowerPoint"},
        {L"powershell", L"PowerShell"},
    };
    wchar_t w[24][64];
    int n = split_words(in, w, 24);
    out[0] = 0;
    for (int i = 0; i < n; i++) {
        const wchar_t *word = w[i];
        for (size_t k = 0; k < sizeof SPECIAL / sizeof SPECIAL[0]; k++)
            if (str_eq(w[i], SPECIAL[k][0])) { word = SPECIAL[k][1]; break; }
        if (word == w[i] && iswalpha(w[i][0])) w[i][0] = (wchar_t)towupper(w[i][0]);
        if (i) str_cat(out, cap, L" ");
        str_cat(out, cap, word);
    }
    if (wcslen(out) > 100) out[100] = 0;
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
    wchar_t tmp[S_CAP], low[S_CAP];
    size_t n = 0;
    /* towlower() only folds ASCII; ini keys are lowered with CharLowerBuff too */
    str_copy(low, S_CAP, in);
    str_lower(low);
    for (size_t i = 0; low[i] && n + 1 < S_CAP; i++) {
        wchar_t c = low[i];
        if (iswalnum(c)) tmp[n++] = c;
        else if (wcschr(L".:/-_'\\+#&", c) && n && tmp[n - 1] != L' ' &&
                 (iswalnum(low[i + 1]) || low[i + 1] == L'/' || low[i + 1] == L'\\'))
            tmp[n++] = c;
        else if (c == L':' && n && iswalpha(tmp[n - 1]) && (n == 1 || tmp[n - 2] == L' '))
            tmp[n++] = c;                 /* typed drive: "open c:" */
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

/* Browsers that can be named in a command ("open youtube in brave").
 * Longer names first so "google chrome" wins over "chrome". */
#define CHROME_PATHS {L"%ProgramFiles%\\Google\\Chrome\\Application\\chrome.exe", \
    L"%ProgramFiles(x86)%\\Google\\Chrome\\Application\\chrome.exe", \
    L"%LOCALAPPDATA%\\Google\\Chrome\\Application\\chrome.exe"}
#define EDGE_PATHS {L"%ProgramFiles(x86)%\\Microsoft\\Edge\\Application\\msedge.exe", \
    L"%ProgramFiles%\\Microsoft\\Edge\\Application\\msedge.exe", NULL}
#define FIREFOX_PATHS {L"%ProgramFiles%\\Mozilla Firefox\\firefox.exe", \
    L"%ProgramFiles(x86)%\\Mozilla Firefox\\firefox.exe", NULL}
#define OPERA_PATHS {L"%LOCALAPPDATA%\\Programs\\Opera\\opera.exe", \
    L"%LOCALAPPDATA%\\Programs\\Opera GX\\opera.exe", NULL}
typedef struct { const wchar_t *name, *exe, *disp; const wchar_t *paths[3]; } Browser;
static const Browser BROWSERS[] = {
    {L"google chrome", L"chrome.exe", L"Chrome", CHROME_PATHS},
    {L"chrome", L"chrome.exe", L"Chrome", CHROME_PATHS},
    {L"brave", L"brave.exe", L"Brave", {L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
        L"%ProgramFiles%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe",
        L"%ProgramFiles(x86)%\\BraveSoftware\\Brave-Browser\\Application\\brave.exe"}},
    {L"microsoft edge", L"msedge.exe", L"Edge", EDGE_PATHS},
    {L"edge", L"msedge.exe", L"Edge", EDGE_PATHS},
    {L"mozilla firefox", L"firefox.exe", L"Firefox", FIREFOX_PATHS},
    {L"firefox", L"firefox.exe", L"Firefox", FIREFOX_PATHS},
    {L"opera gx", L"opera.exe", L"Opera GX", OPERA_PATHS},
    {L"opera", L"opera.exe", L"Opera", OPERA_PATHS},
};
static const Browser *s_browser;        /* browser named in this command (NULL = configured) */
static bool s_browser_missing;          /* ...and it isn't installed */

static bool target_exists(const wchar_t *exe);

static bool open_in_browser(const Browser *b, const wchar_t *qurl)
{
    if (target_exists(b->exe) && shell_open(b->exe, qurl)) return true;
    for (int i = 0; i < 3 && b->paths[i]; i++) {
        wchar_t p[MAX_PATH];
        expand_env(b->paths[i], p, MAX_PATH);
        if (!wcschr(p, L'%') && path_exists(p) && shell_open(p, qurl)) return true;
    }
    return false;
}

static bool act_url(const wchar_t *url)
{
    if (s_browser) record(L"URL %ls [%ls]", url, s_browser->exe);
    else record(L"URL %ls", url);
    if (s_dry) return true;
    wchar_t q[ACT_CAP];
    swprintf(q, ACT_CAP, L"\"%ls\"", url);
    if (s_browser) {
        if (open_in_browser(s_browser, q)) return true;
        s_browser_missing = true;
    }
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

/* Start Menu shortcut (.lnk/.url/.appref-ms): a full path, never split into
 * program + arguments ("Game - Deluxe.url" would be cut at " -"). */
static bool act_shortcut(const wchar_t *path)
{
    if (!path_exists(path)) return false;
    record(L"APP %ls", path);
    if (s_dry) return true;
    return shell_open(path, NULL);
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
    if (n == 1) set_last(hits[0].path);
    r->pick_mode = mode;
    r->rows_replace = true;
    r->listen_again = true;
    r->speak = true;
    record(L"LIST %d", n);
}

/* The index can be up to index_max_age_min old: a hit may have been moved or
 * deleted since. Says so (and refreshes the index) instead of failing silently. */
static bool hit_missing(const wchar_t *path, const wchar_t *nm, CmdResult *r)
{
    if (is_shell_path(path) || path_exists(path)) return false;
    reply(r, L"%ls seems to have been moved or deleted. Refreshing my index.", nm);
    record(L"SAY");
    search_rebuild_async();
    s_nrows = 0;
    return true;
}

/* Open a resolved file or folder (into the dialog when one is active). */
static void deliver(const wchar_t *path, CmdResult *r)
{
    wchar_t nm[128];
    short_name(path, nm, 128);
    if (hit_missing(path, nm, r)) return;
    if (dialog_active() && act_dialog(path)) reply(r, L"Selected %ls.", nm);
    else if (act_open(path)) reply(r, L"Opening %ls.", nm);
    else { reply(r, L"I couldn't open %ls.", nm); return; }
    set_last(path);
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
        if (hit_missing(hits[0].path, nm, r)) return;
        act_reveal(hits[0].path);
        set_last(hits[0].path);
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
/* Display name for a config hit: the key that matched ("calculator app" -> "Calculator") */
static void config_disp(const wchar_t *name, const KVList *l, const wchar_t *const *suffixes, wchar_t *disp)
{
    wchar_t t[256];
    str_copy(t, 256, name);
    if (!kv_get(l, t))
        for (; *suffixes; suffixes++) {
            str_copy(t, 256, name);
            if (cut_suffix(t, *suffixes) && t[0] && kv_get(l, t)) break;
            str_copy(t, 256, name);
        }
    title_case(t, disp, 128);
}

/* Configured site / app / folder with exactly this name. */
static bool try_config(const wchar_t *name, CmdResult *r)
{
    wchar_t disp[128];
    const wchar_t *v;
    if ((v = site_url(name))) {
        config_disp(name, &g_cfg.sites, SITE_SUFFIX, disp);
        act_url(v);
        reply(r, L"Opening %ls.", disp);
        done(r);
        return true;
    }
    if ((v = app_target(name))) {
        wchar_t lnk[MAX_PATH], an[128];
        config_disp(name, &g_cfg.apps, APP_SUFFIX, disp);
        if (act_app(v)) reply(r, L"Opening %ls.", disp);
        else if (search_app(name, lnk, an) >= 65 && act_shortcut(lnk)) reply(r, L"Opening %ls.", an);
        else {
            reply(r, L"I couldn't find %ls on this PC.", disp);
            record(L"SAY");
            return true;
        }
        done(r);
        return true;
    }
    if ((v = folder_path(name)) && (is_shell_path(v) || path_is_dir(v))) {
        config_disp(name, &g_cfg.folders, FOLDER_SUFFIX, disp);
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
static bool open_ref(const wchar_t *phrase, bool reveal, CmdResult *r);

static void resolve_open(const wchar_t *rest, bool play, CmdResult *r)
{
    wchar_t name[256];
    if (open_ref(rest, false, r)) return;   /* "open this" = the selected / last file */
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
        if (search_app(name, lnk, an) >= 80 && act_shortcut(lnk)) {
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
    /* (a configured folder that doesn't exist falls through to a file search
     * instead of returning with no reply at all) */
    if (dialog_active() && folder_path(name) && try_config(name, r)) return;
    FileQ fq;
    build_query(name, &fq, 0);
    do_files(name, &fq, FM_UPLOAD, false, r);
}

/* ------------------------------------------------------------ fixed phrases */
static WList CLOSE_W = { L"cancel", L"never mind", L"nevermind", L"close", L"close it", L"exit", L"stop", L"quit",
    L"bye", L"goodbye", L"good bye", L"nothing", L"no", L"nope", L"dismiss", L"forget it", L"that's all",
    L"thats all", L"hide", L"go away", L"cancel that", L"close jarvis", L"quit jarvis", L"exit jarvis", NULL };
/* the subset that means "I'm done with JARVIS" (the others only cancel) */
static WList QUIT_W = { L"close", L"close it", L"exit", L"quit", L"cancel", L"cancel that", L"bye", L"goodbye",
    L"good bye", L"that's all", L"thats all", L"go away", L"dismiss", L"close jarvis", L"quit jarvis",
    L"exit jarvis", NULL };
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
        {L"find resume, open latest photo", L"Search your files"},
        {L"upload resume pdf", L"Fill an Upload / Open window"},
        {L"google black holes", L"Search the web, YouTube, Wikipedia, maps"},
        {L"convert resume to pdf", L"PDF \x2194 Word, slides, sheets, images"},
        {L"split physics notes at page 5 and save it in downloads", L"Split, extract pages, merge PDFs"},
        {L"rename this to final report", L"Rename, move, copy, delete selected files"},
        {L"what time is it, volume up, lock", L"Time, date, battery and your PC"},
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
    if (in_list(s, CLOSE_W)) {
        record(L"CLOSE");
        reply(r, L"Okay.");
        done(r);
        r->quit = in_list(s, QUIT_W);   /* "close" / "exit": end JARVIS even with stay_open */
        return true;
    }
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

/* ---------------------------------------------------------- document tasks
 * "convert resume to pdf", "split notes at page 5 and save it in downloads",
 * "merge these", "rename this to final report", "delete it" ...
 * A Plan holds the spoken file names; they are resolved one by one (a list
 * is shown when unsure and the plan waits for "two"), then the task runs. */
typedef struct {
    DocOp   op;                          /* DOC_NONE = convert, direction from the file type */
    int     nfile;
    wchar_t q[DOC_MAX_SRC][160];         /* spoken name ("" once resolved) */
    wchar_t path[DOC_MAX_SRC][MAX_PATH];
    wchar_t name[160];                   /* "name it X" / rename target */
    wchar_t folder[MAX_PATH];            /* "save it in X" (resolved) */
    int     a, b;
} Plan;

/* is dir an ancestor of (or the same as) p? */
static bool is_parent_of_path(const wchar_t *dir, const wchar_t *p)
{
    size_t n = wcslen(dir);
    while (n > 3 && dir[n - 1] == L'\\') n--;
    return _wcsnicmp(dir, p, n) == 0 && (p[n] == 0 || p[n] == L'\\');
}

static Plan    s_plan;                   /* waiting for a pick from the list */
static bool    s_plan_wait;
static bool    s_del_wait;               /* asked "move X to the Recycle Bin?" */
static wchar_t s_del[MAX_PATH];
static HWND    s_notify;
static wchar_t s_orig[S_CAP];            /* the command as typed (for name casing) */

static void set_last(const wchar_t *p)
{
    if (!p || !p[0] || is_shell_path(p)) return;
    str_copy(s_last, MAX_PATH, p);
    s_nsel = 0;     /* "it" now means what we just did, not the old selection */
}

static WList REF_W = { L"this", L"that", L"it", L"these", L"those", L"them", L"selected", L"current", NULL };
static WList REF_FILL = { L"file", L"files", L"doc", L"docs", L"document", L"documents", L"one", L"ones",
    L"pdf", L"pdfs", L"thing", L"item", L"items", L"the", L"selection", L"word", L"presentation", L"slides",
    L"sheet", L"spreadsheet", L"image", L"photo", L"picture", L"folder", L"two", L"all", L"of", L"both", NULL };

/* "this", "this pdf", "these files", "it" -> true (plural for these/those/them/files) */
static bool is_ref(const wchar_t *phrase, ExtClass *hint)
{
    wchar_t w[12][64];
    int n = split_words(phrase, w, 12);
    bool ref = false;
    if (hint) *hint = EXT_ANY;
    if (!n) return false;
    for (int i = 0; i < n; i++) {
        if (in_list(w[i], REF_W)) { ref = true; continue; }
        if (!in_list(w[i], REF_FILL)) return false;
        ExtClass e = type_word(w[i]);
        if (e && hint) *hint = e;
    }
    return ref;
}

/* can this op work on that file? */
static bool op_accepts(DocOp op, const wchar_t *p)
{
    ExtClass e = ext_class_of(p);
    bool dir = path_is_dir(p);
    switch (op) {
    case DOC_TO_PDF:
        return !dir && (e == EXT_DOC || e == EXT_PPT || e == EXT_SHEET || e == EXT_IMAGE ||
                        _wcsicmp(PathFindExtensionW(p), L".txt") == 0);
    case DOC_NONE:
        return op_accepts(DOC_TO_PDF, p) || (!dir && e == EXT_PDF);
    case DOC_TO_DOCX: case DOC_SPLIT: case DOC_EXTRACT: case DOC_MERGE: case DOC_PAGES:
        return !dir && e == EXT_PDF;
    default:
        return true;
    }
}

static ExtClass op_ext(DocOp op)
{
    return (op == DOC_TO_DOCX || op == DOC_SPLIT || op == DOC_EXTRACT || op == DOC_MERGE || op == DOC_PAGES)
           ? EXT_PDF : EXT_ANY;
}

/* "this" -> selected files (that fit), else the last file; returns count */
static int resolve_ref(DocOp op, ExtClass hint, wchar_t (*out)[MAX_PATH], int max)
{
    int n = 0;
    for (int i = 0; i < s_nsel && n < max; i++)
        if (path_exists(s_sel[i]) && op_accepts(op, s_sel[i]) && (!hint || ext_class_of(s_sel[i]) == hint))
            str_copy(out[n++], MAX_PATH, s_sel[i]);
    if (!n && s_last[0] && path_exists(s_last) && op_accepts(op, s_last))
        str_copy(out[n++], MAX_PATH, s_last);
    if (!n && hint) return resolve_ref(op, EXT_ANY, out, max);   /* "this doc" on a .txt */
    return n;
}

/* lower-case name -> the casing the user typed, when it is in the original */
static void orig_case(wchar_t *s)
{
    size_t n = wcslen(s);
    if (!n) return;
    for (const wchar_t *p = s_orig; *p; p++)
        if (_wcsnicmp(p, s, n) == 0) { memcpy(s, p, n * sizeof(wchar_t)); return; }
}

/* file name the user said: no path characters, no format word at the end */
static void clean_name(const wchar_t *in, wchar_t *out, size_t cap)
{
    size_t n = 0;
    for (const wchar_t *p = in; *p && n + 1 < cap; p++)
        if (!wcschr(L"<>:\"/\\|?*", *p)) out[n++] = *p;
    out[n] = 0;
    str_collapse_spaces(out);
    cut_prefixes(out, (WList){ L"it", L"them", L"as", L"to", L"the", NULL });
    static WList FMT = { L"pdf", L"pdf file", L"docx", L"word document", L"word file", L"file", NULL };
    for (const wchar_t *const *f = FMT; *f; f++) if (wcslen(out) > wcslen(*f) + 1) cut_suffix(out, *f);
    orig_case(out);
}

/* split "C:\a\b c.pdf" -> dir "C:\a", base "b c", ext ".pdf" */
static void path_parts(const wchar_t *p, wchar_t *dir, wchar_t *base, wchar_t *ext)
{
    str_copy(dir, MAX_PATH, p);
    wchar_t *s = wcsrchr(dir, L'\\');
    const wchar_t *name = s ? s + 1 : dir;
    str_copy(base, MAX_PATH, name);
    if (s) *s = 0;
    if (wcslen(dir) == 2 && dir[1] == L':') str_cat(dir, MAX_PATH, L"\\");
    ext[0] = 0;
    wchar_t *d = wcsrchr(base, L'.');
    if (d && d != base && !path_is_dir(p)) { str_copy(ext, 32, d); *d = 0; }
}

static void join_path(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    size_t n = wcslen(dir);
    swprintf(out, MAX_PATH, n && dir[n - 1] == L'\\' ? L"%ls%ls" : L"%ls\\%ls", dir, name);
}

/* "downloads", "my desktop folder", "d drive", "c:\x", or a folder search */
static bool resolve_folder(const wchar_t *text, wchar_t *out)
{
    wchar_t t[256];
    strip_articles(text, t, 256);
    cut_suffix(t, L"folder");
    cut_suffix(t, L"directory");
    if (!t[0]) return false;
    const wchar_t *f = filter_folder(t);
    if (!f) { wchar_t t2[256]; swprintf(t2, 256, L"%ls folder", t); f = filter_folder(t2); }
    if (f && path_is_dir(f)) { str_copy(out, MAX_PATH, f); return true; }
    if (parse_drive(t, out) && path_is_dir(out)) return true;
    if (wcschr(t, L'\\') && path_is_dir(t)) { str_copy(out, MAX_PATH, t); return true; }
    if (!wait_index()) return false;
    Hit hits[MAX_HITS];
    SearchQuery sq = { t, NULL, EXT_ANY, SF_DIRS_ONLY };
    int n = search_files(&sq, hits, MAX_HITS);
    if (n && hits[0].score >= 60 && path_is_dir(hits[0].path)) { str_copy(out, MAX_PATH, hits[0].path); return true; }
    return false;
}

/* ---- parsing helpers on word arrays */
static bool words_at(wchar_t w[][64], int n, int i, const wchar_t *phrase, int *len)
{
    wchar_t p[8][64];
    int m = split_words(phrase, p, 8);
    if (i < 0 || i + m > n) return false;
    for (int k = 0; k < m; k++) if (!str_eq(w[i + k], p[k])) return false;
    if (len) *len = m;
    return true;
}

/* 1..99 from "5", "five", "twenty five", "5th"; returns words used (0 = none).
 * "to"/"for" are NOT numbers here: "pages three to seven". */
static int page_num(wchar_t w[][64], int n, int i, int *val)
{
    static const wchar_t *U[] = { L"zero", L"one", L"two", L"three", L"four", L"five", L"six", L"seven", L"eight",
        L"nine", L"ten", L"eleven", L"twelve", L"thirteen", L"fourteen", L"fifteen", L"sixteen", L"seventeen",
        L"eighteen", L"nineteen" };
    static const wchar_t *T[] = { L"twenty", L"thirty", L"forty", L"fifty", L"sixty", L"seventy", L"eighty", L"ninety" };
    if (i >= n) return 0;
    if (iswdigit(w[i][0])) {
        const wchar_t *p = w[i];
        while (iswdigit(*p)) p++;
        if (*p && !str_eq(p, L"st") && !str_eq(p, L"nd") && !str_eq(p, L"rd") && !str_eq(p, L"th")) return 0;
        *val = _wtoi(w[i]);
        return 1;
    }
    for (int k = 1; k < 20; k++) if (str_eq(w[i], U[k])) { *val = k; return 1; }
    for (int k = 0; k < 8; k++)
        if (str_eq(w[i], T[k])) {
            *val = 20 + 10 * k;
            for (int u = 1; u < 10 && i + 1 < n; u++) if (str_eq(w[i + 1], U[u])) { *val += u; return 2; }
            return 1;
        }
    int v = number_word(w[i]);     /* "fifth", "third" ... */
    if (v > 0 && !in_list(w[i], (WList){ L"to", L"too", L"for", L"won", L"ate", L"tree", NULL })) { *val = v; return 1; }
    return 0;
}

static void trim_and(wchar_t w[][64], int *n)
{
    while (*n > 0 && in_list(w[*n - 1], (WList){ L"and", L"then", L"also", L"please", NULL })) (*n)--;
}

/* Pulls "and name it X" / "save it in Y" off the end (any order). Returns
 * false (with a reply) when the folder can't be found. */
static bool take_modifiers(wchar_t w[][64], int *n, Plan *p, CmdResult *r)
{
    static WList NAME_M = { L"and name it", L"name it", L"and call it", L"call it", L"and name them", L"name them",
        L"and call them", L"call them", L"as named", L"named", L"and rename it", L"and title it", L"with the name",
        L"and save it as", L"save it as", L"and save as", L"save as", NULL };
    static WList DIR_M = { L"and put it in", L"put it in", L"and put them in", L"put them in", L"and put it on",
        L"put it on", L"and save it in", L"save it in", L"and save it to", L"save it to", L"and save them in",
        L"save them in", L"and save them to", L"save them to", L"and save to", L"save to", L"and save in",
        L"save in", L"and save it on", L"save it on", L"and store it in", L"store it in", L"and place it in",
        L"place it in", L"and keep it in", L"keep it in", L"and put the result in", L"put the result in", NULL };
    for (;;) {
        int best = -1, blen = 0;
        bool bdir = false;
        for (int pass = 0; pass < 2; pass++)
            for (const wchar_t *const *m = pass ? DIR_M : NAME_M; *m; m++)
                for (int i = *n - 2; i >= 1; i--) {
                    int len;
                    if (!words_at(w, *n, i, *m, &len) || i + len >= *n) continue;
                    if (i < best || (i == best && len <= blen)) break;
                    /* "save as pdf" is a format, not a name */
                    if (str_starts(*m, L"save") && !pass) {
                        wchar_t v[128];
                        join_words(w, i + len, *n, v, 128);
                        if (in_list(v, (WList){ L"pdf", L"a pdf", L"word", L"a word document", L"docx", L"word document", NULL })) continue;
                    }
                    best = i; blen = len; bdir = pass == 1;
                    break;
                }
        if (best < 0) return true;
        wchar_t v[256];
        join_words(w, best + blen, *n, v, 256);
        *n = best;
        trim_and(w, n);
        if (bdir) {
            if (!resolve_folder(v, p->folder)) {
                wchar_t d[128];
                str_copy(d, 128, v);
                reply(r, L"I couldn't find a folder called %ls.", d);
                record(L"SAY");
                return false;
            }
        } else clean_name(v, p->name, 160);
    }
}

/* "this" alone when no file phrase was said */
static void add_file(Plan *p, const wchar_t *q)
{
    if (p->nfile >= DOC_MAX_SRC) return;
    wchar_t t[160];
    str_copy(t, 160, q);
    str_trim(t);
    if (!t[0]) str_copy(t, 160, L"this");
    str_copy(p->q[p->nfile++], 160, t);
}

static const wchar_t *const PDF_T[] = { L"pdf", L"pdf file", L"pdf document", L"pdfs", NULL };
static const wchar_t *const WORD_T[] = { L"editable word document", L"editable word", L"word document",
    L"word file", L"word doc", L"word", L"docx", L"doc", L"docs", L"document", NULL };

/* target format at word i ("pdf", "a word document"): DOC_TO_PDF / DOC_TO_DOCX, words used */
static DocOp target_at(wchar_t w[][64], int n, int i, int *used)
{
    int skip = (i < n && (str_eq(w[i], L"a") || str_eq(w[i], L"an"))) ? 1 : 0;
    int len;
    for (const wchar_t *const *t = PDF_T; *t; t++)
        if (words_at(w, n, i + skip, *t, &len)) { *used = skip + len; return DOC_TO_PDF; }
    for (const wchar_t *const *t = WORD_T; *t; t++)
        if (words_at(w, n, i + skip, *t, &len)) { *used = skip + len; return DOC_TO_DOCX; }
    return DOC_NONE;
}

/* Returns 0: not a document command, 1: plan ready, 2: replied (error / question) */
static int parse_docs(const wchar_t *s, Plan *p, CmdResult *r)
{
    static WList CONVERT_V = { L"convert", L"change", L"turn", L"export", L"save", L"make", L"transform", NULL };
    static WList SPLIT_V = { L"split", L"divide", L"cut", L"break", L"separate", NULL };
    static WList EXTRACT_V = { L"extract", L"take", L"pull", L"get", L"save", L"copy", L"take out", L"pull out", NULL };
    static WList MERGE_V = { L"merge", L"combine", L"join", L"stitch", NULL };
    static WList DELETE_V = { L"delete", L"remove", L"trash", L"erase", L"recycle", L"bin", NULL };
    memset(p, 0, sizeof *p);
    static wchar_t w[48][64];
    int n = split_words(s, w, 48);
    if (n == 0) return 0;
    const wchar_t *v0 = w[0];
    wchar_t q[256];

    /* ---- page count: "how many pages in X", "page count of X", "count pages in X" */
    {
        int st = -1, len;
        if (words_at(w, n, 0, L"how many pages", &len)) st = len;
        else if (words_at(w, n, 0, L"count the pages", &len) || words_at(w, n, 0, L"count pages", &len) ||
                 words_at(w, n, 0, L"page count", &len) || words_at(w, n, 0, L"number of pages", &len)) st = len;
        if (st >= 0) {
            while (st < n && in_list(w[st], (WList){ L"are", L"in", L"of", L"does", L"is", L"there", L"has", L"for", NULL })) st++;
            while (n > st && in_list(w[n - 1], (WList){ L"have", L"has", L"got", L"contain", NULL })) n--;
            join_words(w, st, n, q, 256);
            p->op = DOC_PAGES;
            add_file(p, q);
            return 1;
        }
    }

    bool conv_v = in_list(v0, CONVERT_V), split_v = in_list(v0, SPLIT_V), merge_v = in_list(v0, MERGE_V);
    bool ren_v = str_eq(v0, L"rename"), move_v = str_eq(v0, L"move"), copy_v = str_eq(v0, L"copy");
    bool del_v = in_list(v0, DELETE_V);
    /* "extract pages 2 to 4 from X", "take page 3 of X" */
    int pg = -1;
    if (in_list(v0, EXTRACT_V)) {
        int i = 1;
        if (i < n && (str_eq(w[i], L"out") || str_eq(w[i], L"the"))) i++;
        if (i < n && (str_eq(w[i], L"page") || str_eq(w[i], L"pages"))) pg = i;
    }
    bool bare_pdf = false;   /* "<file> to pdf", "pdf to word <file>" */
    int len;
    if (!conv_v && !split_v && !merge_v && !ren_v && !move_v && !copy_v && !del_v && pg < 0) {
        if (n >= 3 && (words_at(w, n, 0, L"pdf to", &len) || words_at(w, n, 0, L"pdf into", &len) ||
                       words_at(w, n, 0, L"word to", &len) || words_at(w, n, 0, L"docx to", &len) ||
                       words_at(w, n, 0, L"doc to", &len))) bare_pdf = true;
        else {
            for (int i = n - 1; i >= 1; i--)
                if (in_list(w[i], (WList){ L"to", L"into", NULL })) {
                    int used;
                    if (target_at(w, n, i + 1, &used) == DOC_TO_PDF && i + 1 + used == n) bare_pdf = true;
                    break;
                }
        }
        if (!bare_pdf) return 0;
    }
    /* cheap exclusions: these verbs start other commands too */
    if (conv_v || in_list(v0, EXTRACT_V)) {
        bool any = pg >= 0;
        for (int i = 1; i < n && !any; i++) { int used; any = target_at(w, n, i, &used) != DOC_NONE && i > 1; }
        if (!any && !str_eq(v0, L"convert") && !copy_v) return 0;
    }
    if (str_eq(v0, L"cut") || str_eq(v0, L"break") || str_eq(v0, L"separate") || str_eq(v0, L"divide")) {
        bool page = false;
        for (int i = 1; i < n; i++) if (str_eq(w[i], L"page") || str_eq(w[i], L"pages")) page = true;
        if (!page) return 0;
    }

    if (!take_modifiers(w, &n, p, r)) return 2;

    /* ---- extract */
    if (pg >= 0) {
        int a = 0, b = 0, i = pg + 1, u;
        if (i < n && wcschr(w[i], L'-')) {               /* "2-4" */
            wchar_t *dash = wcschr(w[i], L'-');
            a = _wtoi(w[i]); b = _wtoi(dash + 1);
            i++;
        } else if ((u = page_num(w, n, i, &a))) {
            i += u;
            if (i < n && in_list(w[i], (WList){ L"to", L"through", L"thru", L"till", L"until", L"and", L"-", NULL }) &&
                (u = page_num(w, n, i + 1, &b))) i += 1 + u;
            else b = a;
        }
        if (a < 1 || b < a) {
            reply(r, L"Which pages? Say for example: extract pages 2 to 5 from notes.");
            record(L"SAY");
            r->listen_again = true;
            return 2;
        }
        if (i < n && in_list(w[i], (WList){ L"from", L"of", L"in", L"out", NULL })) i++;
        if (i < n && str_eq(w[i], L"of")) i++;
        join_words(w, i, n, q, 256);
        p->op = DOC_EXTRACT; p->a = a; p->b = b;
        add_file(p, q);
        return 1;
    }

    /* ---- split */
    if (split_v) {
        int at = -1, N = 0, u = 0, adj = 0;
        for (int i = 1; i < n && at < 0; i++)
            if ((str_eq(w[i], L"page") || str_eq(w[i], L"pages")) && (u = page_num(w, n, i + 1, &N))) at = i;
        if (at < 0)   /* "split notes at 5" */
            for (int i = 1; i + 1 < n && at < 0; i++)
                if (in_list(w[i], (WList){ L"at", L"after", L"until", L"till", NULL }) && (u = page_num(w, n, i + 1, &N))) at = i + 1;
        if (at < 0 || N < 1) {
            reply(r, L"After which page should I split it? Say for example: split notes at page 5.");
            record(L"SAY");
            r->listen_again = true;
            return 2;
        }
        int k = at;   /* walk back over "at", "after", "up to", "until" ... and "into two parts" */
        while (k > 1 && in_list(w[k - 1], (WList){ L"at", L"after", L"up", L"to", L"until", L"till", L"from",
                                                   L"on", L"by", L"before", L"with", L"the", L"upto", NULL })) {
            if (str_eq(w[k - 1], L"from") || str_eq(w[k - 1], L"before")) adj = -1;
            k--;
        }
        if (k > 2 && in_list(w[k - 1], (WList){ L"parts", L"part", L"halves", L"pieces", L"pdfs", L"files", NULL })) k--;
        if (k > 2 && in_list(w[k - 1], (WList){ L"two", L"2", L"half", L"halves", NULL }) &&
            in_list(w[k - 2], (WList){ L"into", L"in", NULL })) k -= 2;
        join_words(w, 1, k, q, 256);
        p->op = DOC_SPLIT;
        p->a = N + adj;
        if (p->a < 1) p->a = 1;
        add_file(p, q);
        return 1;
    }

    /* ---- merge */
    if (merge_v) {
        while (n > 1 && in_list(w[n - 1], (WList){ L"together", L"one", L"pdf", L"file", L"single", L"a", L"into", NULL })) n--;
        p->op = DOC_MERGE;
        int st = 1;
        for (int i = 1; i <= n; i++)
            if (i == n || in_list(w[i], (WList){ L"and", L"with", L"plus", L"&", NULL })) {
                join_words(w, st, i, q, 256);
                if (q[0]) add_file(p, q);
                st = i + 1;
            }
        if (p->nfile == 0) add_file(p, L"these");
        return 1;
    }

    /* ---- delete */
    if (del_v) {
        while (n > 1 && in_list(w[n - 1], (WList){ L"permanently", L"forever", NULL })) n--;
        int cut;
        for (int i = 1; i < n; i++)
            if ((words_at(w, n, i, L"to the recycle bin", &cut) || words_at(w, n, i, L"to recycle bin", &cut) ||
                 words_at(w, n, i, L"to the trash", &cut) || words_at(w, n, i, L"to trash", &cut) ||
                 words_at(w, n, i, L"to the bin", &cut)) && i + cut == n) { n = i; break; }
        join_words(w, 1, n, q, 256);
        p->op = DOC_DELETE;
        add_file(p, q);
        return 1;
    }

    /* ---- rename / move / copy */
    if (ren_v || move_v || copy_v) {
        int at = -1;
        for (int i = n - 2; i >= 1 && at < 0; i--)
            if (in_list(w[i], ren_v ? (WList){ L"to", L"as", L"into", NULL } : (WList){ L"to", L"into", L"in", L"onto", L"inside", NULL }))
                at = i;
        wchar_t dest[256] = L"";
        if (at >= 0) { join_words(w, at + 1, n, dest, 256); n = at; }
        join_words(w, 1, n, q, 256);
        add_file(p, q);
        if (ren_v) {
            p->op = DOC_RENAME;
            if (dest[0]) clean_name(dest, p->name, 160);
            if (!p->name[0]) { reply(r, L"What should I call it? Say for example: rename it to final report."); record(L"SAY"); r->listen_again = true; return 2; }
            return 1;
        }
        p->op = move_v ? DOC_MOVE : DOC_COPY;
        if (dest[0]) {
            wchar_t d2[256];
            strip_articles(dest, d2, 256);
            if (move_v && in_list(d2, (WList){ L"trash", L"recycle bin", L"bin", L"the recycle bin", NULL })) { p->op = DOC_DELETE; return 1; }
            if (!resolve_folder(dest, p->folder)) {
                wchar_t d[128];
                str_copy(d, 128, dest);
                reply(r, L"I couldn't find a folder called %ls.", d);
                record(L"SAY");
                return 2;
            }
        }
        if (!p->folder[0]) { reply(r, L"Where to? Say for example: move it to downloads."); record(L"SAY"); r->listen_again = true; return 2; }
        return 1;
    }

    /* ---- convert */
    {
        int st = 1, end = n, tail = n;
        DocOp op = DOC_NONE;
        if (bare_pdf && !in_list(v0, CONVERT_V)) {
            if (words_at(w, n, 0, L"pdf to", &len) || words_at(w, n, 0, L"pdf into", &len)) {
                int used;
                if ((op = target_at(w, n, 2, &used)) == DOC_TO_DOCX) st = 2 + used;
                else return 0;
            } else if (words_at(w, n, 0, L"word to", &len) || words_at(w, n, 0, L"docx to", &len) || words_at(w, n, 0, L"doc to", &len)) {
                int used;
                if ((op = target_at(w, n, 2, &used)) == DOC_TO_PDF) st = 2 + used;
                else return 0;
            } else st = 0;
        }
        if (op == DOC_NONE) {
            for (int i = n - 1; i >= st + 1; i--) {
                int used;
                bool prep = in_list(w[i], (WList){ L"to", L"into", L"as", NULL });
                DocOp t = prep ? target_at(w, n, i + 1, &used) : DOC_NONE;
                if (!t && str_eq(v0, L"make") && (str_eq(w[i], L"a") || str_eq(w[i], L"an"))) { t = target_at(w, n, i, &used); prep = false; }
                if (t) { op = t; end = i; tail = i + (prep ? 1 : 0) + used; break; }
            }
            if (op == DOC_NONE && !str_eq(v0, L"convert")) return 0;
            /* "to pdf as final report", "to word named x" */
            if (tail < n) {
                int k = tail;
                while (k < n && in_list(w[k], (WList){ L"and", L"as", L"named", L"called", L"with", L"name", L"it", NULL })) k++;
                join_words(w, k, n, q, 256);
                if (q[0] && !p->name[0]) clean_name(q, p->name, 160);
            }
        } else end = n;
        join_words(w, st, end, q, 256);
        p->op = op;
        add_file(p, q);
        return 1;
    }
}

static const wchar_t *op_verb(DocOp op)
{
    switch (op) {
    case DOC_TO_PDF: case DOC_TO_DOCX: case DOC_NONE: return L"convert";
    case DOC_SPLIT: return L"split";
    case DOC_EXTRACT: return L"take pages from";
    case DOC_MERGE: return L"merge";
    case DOC_PAGES: return L"count";
    case DOC_RENAME: return L"rename";
    case DOC_MOVE: return L"move";
    case DOC_COPY: return L"copy";
    case DOC_DELETE: return L"delete";
    }
    return L"use";
}

static void plan_run(Plan *p, CmdResult *r);

/* Resolves file i of the plan; false when it replied (list / not found) */
static bool plan_resolve(Plan *p, int i, CmdResult *r)
{
    ExtClass hint;
    wchar_t refs[DOC_MAX_SRC][MAX_PATH];
    if (is_ref(p->q[i], &hint)) {
        int max = p->op == DOC_MERGE ? DOC_MAX_SRC - i : 1;
        int n = resolve_ref(p->op, hint, refs, max);
        if (!n) {
            reply(r, s_nsel ? L"The selected file isn't something I can %ls. Which file?" : L"Which file?", op_verb(p->op));
            record(L"SAY");
            r->listen_again = true;
            return false;
        }
        str_copy(p->path[i], MAX_PATH, refs[0]);
        p->q[i][0] = 0;
        /* "merge these": every selected PDF */
        for (int k = 1; k < n && p->nfile < DOC_MAX_SRC; k++) {
            str_copy(p->path[p->nfile], MAX_PATH, refs[k]);
            p->q[p->nfile++][0] = 0;
        }
        return true;
    }
    wchar_t name[256];
    strip_articles(p->q[i], name, 256);
    if (wcschr(name, L'\\') && path_exists(name)) { str_copy(p->path[i], MAX_PATH, name); p->q[i][0] = 0; return true; }
    if (!wait_index()) { reply(r, L"Still indexing your files, try again in a moment."); record(L"SAY"); return false; }
    FileQ fq;
    build_query(name, &fq, (p->op == DOC_RENAME || p->op == DOC_MOVE || p->op == DOC_COPY || p->op == DOC_DELETE) ? 0 : SF_FILES_ONLY);
    if (!fq.ext) fq.ext = op_ext(p->op);
    Hit all[MAX_HITS], hits[MAX_HITS];
    int na = run_query(&fq, all, MAX_HITS), n = 0;
    for (int k = 0; k < na; k++) if (op_accepts(p->op, all[k].path)) hits[n++] = all[k];
    wchar_t d[128];
    str_copy(d, 128, name);
    if (n == 0) {
        reply(r, na ? L"I found %ls, but it isn't something I can %ls." : L"I couldn't find %ls.", d, op_verb(p->op));
        record(L"SAY");
        r->listen_again = true;
        return false;
    }
    bool confident = (fq.flags & SF_RECENT) || (hits[0].score >= 60 && (n == 1 || hits[0].score - hits[1].score >= 12));
    if (confident) { str_copy(p->path[i], MAX_PATH, hits[0].path); p->q[i][0] = 0; return true; }
    show_list(hits, n, PICK_OPEN, r);
    s_plan = *p;
    s_plan_wait = true;
    if (n == 1) reply(r, L"Is it this one? Say one to %ls it.", op_verb(p->op));
    else reply(r, L"Which %ls should I %ls? Say a number.", d, op_verb(p->op));
    return false;
}

static void plan_continue(Plan *p, CmdResult *r)
{
    for (int i = 0; i < p->nfile; i++)
        if (p->q[i][0] && !plan_resolve(p, i, r)) return;
    plan_run(p, r);
}

/* all files known: work out the output names and run (or record) the task */
static void plan_run(Plan *p, CmdResult *r)
{
    DocTask t;
    memset(&t, 0, sizeof t);
    t.op = p->op;
    t.a = p->a; t.b = p->b;
    t.nsrc = p->nfile;
    for (int i = 0; i < p->nfile; i++) str_copy(t.src[i], MAX_PATH, p->path[i]);
    const wchar_t *src = t.src[0];
    if (t.op == DOC_NONE) t.op = ext_class_of(src) == EXT_PDF ? DOC_TO_DOCX : DOC_TO_PDF;
    wchar_t n0[128], dir[MAX_PATH], base[MAX_PATH], ext[32], fname[MAX_PATH];
    short_name(src, n0, 128);
    path_parts(src, dir, base, ext);
    const wchar_t *out_dir = p->folder[0] ? p->folder : dir;
    const wchar_t *nmb = p->name[0] ? p->name : base;

    if (t.op == DOC_TO_PDF && ext_class_of(src) == EXT_PDF) { reply(r, L"%ls is already a PDF.", n0); record(L"SAY"); return; }
    if (t.op == DOC_MERGE && t.nsrc < 2) {
        reply(r, L"I need at least two PDFs to merge. Say for example: merge notes and homework.");
        record(L"SAY");
        r->listen_again = true;
        return;
    }
    if (t.op == DOC_DELETE || t.op == DOC_MOVE || t.op == DOC_RENAME) {
        const wchar_t *why = docs_protected(src);
        if (why) { reply(r, L"I won't %ls %ls, %ls.", op_verb(t.op), n0, why); record(L"SAY"); return; }
    }

    switch (t.op) {
    case DOC_TO_PDF:  swprintf(fname, MAX_PATH, L"%ls.pdf", nmb);  join_path(t.dst[0], out_dir, fname); break;
    case DOC_TO_DOCX: swprintf(fname, MAX_PATH, L"%ls.docx", nmb); join_path(t.dst[0], out_dir, fname); break;
    case DOC_SPLIT:
        swprintf(fname, MAX_PATH, L"%ls part 1.pdf", nmb); join_path(t.dst[0], out_dir, fname);
        swprintf(fname, MAX_PATH, L"%ls part 2.pdf", nmb); join_path(t.dst[1], out_dir, fname);
        break;
    case DOC_EXTRACT:
        if (p->name[0]) swprintf(fname, MAX_PATH, L"%ls.pdf", p->name);
        else if (t.a == t.b) swprintf(fname, MAX_PATH, L"%ls page %d.pdf", base, t.a);
        else swprintf(fname, MAX_PATH, L"%ls pages %d-%d.pdf", base, t.a, t.b);
        join_path(t.dst[0], out_dir, fname);
        break;
    case DOC_MERGE:
        if (p->name[0]) swprintf(fname, MAX_PATH, L"%ls.pdf", p->name);
        else swprintf(fname, MAX_PATH, L"%ls merged.pdf", base);
        join_path(t.dst[0], out_dir, fname);
        break;
    case DOC_RENAME: case DOC_MOVE: case DOC_COPY: {
        const wchar_t *e = p->name[0] ? PathFindExtensionW(p->name) : L"";
        bool has_ext = e[0] && (ext_class_of(p->name) != EXT_ANY || _wcsicmp(e, ext) == 0);
        if (p->name[0]) swprintf(fname, MAX_PATH, L"%ls%ls", p->name, has_ext ? L"" : ext);
        else str_copy(fname, MAX_PATH, path_name(src));
        join_path(t.dst[0], out_dir, fname);
        if (t.op != DOC_RENAME && path_is_dir(src) && is_parent_of_path(src, t.dst[0])) {
            reply(r, L"I can't put a folder inside itself.");
            record(L"SAY");
            return;
        }
        break;
    }
    default: break;
    }
    for (int k = 0; k < 2; k++) if (t.dst[k][0]) { wchar_t u[MAX_PATH]; docs_unique(t.dst[k], u); str_copy(t.dst[k], MAX_PATH, u); }

    /* the dry-run action line */
    static const wchar_t *const FILEV[] = { [DOC_RENAME] = L"rename", [DOC_MOVE] = L"move", [DOC_COPY] = L"copy" };
    switch (t.op) {
    case DOC_TO_PDF:  record(L"DOC convert-pdf %ls -> %ls", src, t.dst[0]); break;
    case DOC_TO_DOCX: record(L"DOC convert-docx %ls -> %ls", src, t.dst[0]); break;
    case DOC_SPLIT:   record(L"DOC split %ls @%d -> %ls | %ls", src, t.a, t.dst[0], t.dst[1]); break;
    case DOC_EXTRACT: record(L"DOC extract %ls %d-%d -> %ls", src, t.a, t.b, t.dst[0]); break;
    case DOC_PAGES:   record(L"DOC pages %ls", src); break;
    case DOC_MERGE: {
        wchar_t list[ACT_CAP] = L"";
        for (int i = 0; i < t.nsrc; i++) { if (i) str_cat(list, ACT_CAP, L" + "); str_cat(list, ACT_CAP, t.src[i]); }
        record(L"DOC merge %ls -> %ls", list, t.dst[0]);
        break;
    }
    case DOC_RENAME: case DOC_MOVE: case DOC_COPY:
        record(L"FILE %ls %ls -> %ls", FILEV[t.op], src, t.dst[0]);
        break;
    case DOC_DELETE:
        record(L"ASK delete %ls", src);
        break;
    default: break;
    }
    s_nrows = 0;

    if (t.op == DOC_DELETE) {
        str_copy(s_del, MAX_PATH, src);
        s_del_wait = true;
        reply(r, L"Move %ls to the Recycle Bin? Say yes or no.", n0);
        r->listen_again = true;
        return;
    }
    wchar_t o1[128];
    short_name(t.dst[0], o1, 128);
    if (t.op == DOC_RENAME || t.op == DOC_MOVE || t.op == DOC_COPY) {
        if (s_dry) {
            if (t.op == DOC_RENAME) reply(r, L"Renamed it to %ls.", o1);
            else reply(r, L"%ls %ls.", t.op == DOC_MOVE ? L"Moved" : L"Copied", o1);
            done(r);
            return;
        }
        DocResult res;
        docs_run(&t, &res);
        str_copy(r->reply, 300, res.reply);
        r->speak = true;
        if (res.ok) { set_last(res.out[0]); done(r); }
        return;
    }
    switch (t.op) {
    case DOC_TO_PDF:  reply(r, L"Converting %ls to PDF\x2026", n0); break;
    case DOC_TO_DOCX: reply(r, L"Converting %ls to Word\x2026", n0); break;
    case DOC_SPLIT:   reply(r, L"Splitting %ls after page %d\x2026", n0, t.a); break;
    case DOC_EXTRACT:
        if (t.a == t.b) reply(r, L"Taking page %d from %ls\x2026", t.a, n0);
        else reply(r, L"Taking pages %d to %d from %ls\x2026", t.a, t.b, n0);
        break;
    case DOC_MERGE:   reply(r, L"Merging %d PDFs\x2026", t.nsrc); break;
    case DOC_PAGES:   reply(r, L"Counting the pages of %ls\x2026", n0); break;
    default: break;
    }
    if (s_dry) { done(r); return; }
    if (docs_busy()) { reply(r, L"I'm still working on the last one, give me a moment."); record(L"SAY"); return; }
    if (!docs_run_async(&t, s_notify)) { reply(r, L"I couldn't start that."); record(L"SAY"); return; }
    r->close = false;          /* the result arrives later as WM_APP_TASK */
    r->listen_again = false;
}

/* follow-up to "Move X to the Recycle Bin?" */
static bool run_delete_answer(const wchar_t *s, CmdResult *r)
{
    static WList YES_W = { L"yes", L"yeah", L"yep", L"yup", L"sure", L"do it", L"confirm", L"yes delete it",
        L"yes please", L"delete it", L"go ahead", L"ok", L"okay", L"yes do it", NULL };
    static WList NO_W = { L"no", L"nope", L"don't", L"dont", L"cancel", L"stop", L"never mind", L"nevermind",
        L"keep it", L"no thanks", L"don't delete it", L"dont delete it", NULL };
    s_del_wait = false;
    wchar_t n0[128];
    short_name(s_del, n0, 128);
    if (in_list(s, YES_W)) {
        record(L"FILE delete %ls", s_del);
        if (s_dry) { reply(r, L"Moved %ls to the Recycle Bin.", n0); done(r); return true; }
        DocTask t;
        memset(&t, 0, sizeof t);
        t.op = DOC_DELETE; t.nsrc = 1;
        str_copy(t.src[0], MAX_PATH, s_del);
        DocResult res;
        docs_run(&t, &res);
        str_copy(r->reply, 300, res.reply);
        r->speak = true;
        if (res.ok && _wcsicmp(s_last, s_del) == 0) s_last[0] = 0;
        for (int i = 0; i < s_nsel; i++) if (_wcsicmp(s_sel[i], s_del) == 0) s_nsel = 0;
        done(r);
        return true;
    }
    if (in_list(s, NO_W)) { record(L"SAY"); reply(r, L"Okay, I kept %ls.", n0); done(r); return true; }
    return false;   /* something else: forget the question, run it as a new command */
}

/* "open this", "show it in folder": the Explorer selection or the last file */
static bool open_ref(const wchar_t *phrase, bool reveal, CmdResult *r)
{
    ExtClass hint;
    if (!is_ref(phrase, &hint)) return false;
    wchar_t p[1][MAX_PATH];
    if (!resolve_ref(DOC_RENAME, hint, p, 1)) {
        reply(r, L"Which file?");
        record(L"SAY");
        r->listen_again = true;
        return true;
    }
    if (reveal) {
        wchar_t n0[128];
        short_name(p[0], n0, 128);
        if (hit_missing(p[0], n0, r)) return true;
        act_reveal(p[0]);
        reply(r, L"Here's %ls.", n0);
        set_last(p[0]);
        done(r);
    } else deliver(p[0], r);
    return true;
}

static bool try_docs(const wchar_t *s, CmdResult *r)
{
    Plan p;
    int k = parse_docs(s, &p, r);
    if (k == 0) return false;
    if (k == 1) plan_continue(&p, r);
    return true;
}

void cmd_set_notify(HWND hwnd) { s_notify = hwnd; }

void cmd_set_selection(const wchar_t (*paths)[MAX_PATH], int n)
{
    s_nsel = 0;
    for (int i = 0; i < n && s_nsel < DOC_MAX_SRC; i++) str_copy(s_sel[s_nsel++], MAX_PATH, paths[i]);
}

void cmd_task_done(const DocResult *d, CmdResult *r)
{
    memset(r, 0, sizeof *r);
    if (!d) return;
    str_copy(r->reply, 300, d->reply);
    r->speak = true;
    if (!d->ok || d->nout == 0) { record(L"SAY"); return; }
    for (int i = 0; i < d->nout && i < MAX_HITS; i++) {
        memset(&r->rows[i], 0, sizeof(Row));
        r->rows[i].kind = ROW_HIT;
        str_copy(r->rows[i].hit.path, MAX_PATH, d->out[i]);
        r->rows[i].hit.score = 100;
        s_rows[i] = r->rows[i];
    }
    r->nrows = d->nout;
    s_nrows = d->nout;
    s_mode = PICK_OPEN;
    r->pick_mode = PICK_OPEN;
    r->rows_replace = true;
    r->listen_again = true;    /* "open it" / "one" */
    set_last(d->out[0]);
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
    if (hit_missing(rw.hit.path, nm, r)) return;
    if (s_plan_wait && mode != PICK_REVEAL) {   /* "convert resume to pdf" ... "two" */
        s_plan_wait = false;
        Plan p = s_plan;
        for (int i = 0; i < p.nfile; i++)
            if (p.q[i][0]) { str_copy(p.path[i], MAX_PATH, rw.hit.path); p.q[i][0] = 0; break; }
        s_nrows = 0;
        plan_continue(&p, r);
        return;
    }
    s_plan_wait = false;
    set_last(rw.hit.path);
    if (mode == PICK_REVEAL) {
        act_reveal(rw.hit.path);
        reply(r, L"Here's %ls.", nm);
    } else if (mode == PICK_DIALOG && dialog_active() && act_dialog(rw.hit.path)) {
        reply(r, L"Selected %ls.", nm);
    } else if (act_open(rw.hit.path)) {
        reply(r, L"Opening %ls.", nm);
    } else {
        reply(r, L"I couldn't open %ls.", nm);
        return;
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
static WList BROWSER_SUF = { L"in the browser", L"in browser", L"in my browser", NULL };

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

/* Cuts " in|on|with|using <browser> [browser]" off t; returns the browser. */
static const Browser *cut_browser(wchar_t *t)
{
    static WList PREP = { L"in", L"on", L"with", L"using", NULL };
    for (size_t i = 0; i < sizeof BROWSERS / sizeof BROWSERS[0]; i++)
        for (const wchar_t *const *p = PREP; *p; p++) {
            wchar_t suf[64];
            swprintf(suf, 64, L"%ls %ls browser", *p, BROWSERS[i].name);
            if (cut_suffix(t, suf)) return &BROWSERS[i];
            swprintf(suf, 64, L"%ls %ls", *p, BROWSERS[i].name);
            if (cut_suffix(t, suf)) return &BROWSERS[i];
        }
    return NULL;
}

/* "Chrome" for browser=chrome.exe (or a full path to it), for replies */
static const wchar_t *configured_browser_name(void)
{
    const wchar_t *exe = path_name(g_cfg.browser);
    for (size_t i = 0; i < sizeof BROWSERS / sizeof BROWSERS[0]; i++)
        if (!_wcsicmp(exe, BROWSERS[i].exe)) return BROWSERS[i].disp;
    return NULL;
}

/* X is a site name, a web address or a web search ("google black holes") */
static bool open_web_target(wchar_t *t, CmdResult *r)
{
    wchar_t full[256], name[256], url[ACT_CAP], q[256];
    str_copy(full, 256, t);
    cut_prefixes(t, OPEN_V);
    cut_prefixes(t, GOTO_V);
    strip_articles(t, name, 256);
    if (!name[0]) return false;
    int engine;
    const wchar_t *v = site_url(name);
    if (v) str_copy(url, ACT_CAP, v);
    else if (match_web(full, &engine, q, 256)) { web_search(engine, q, r); return true; }
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

/* "open X in brave", "youtube lofi in edge", "open X in the browser" */
static bool try_browser(const wchar_t *s, CmdResult *r)
{
    wchar_t t[256];
    str_copy(t, 256, s);
    const Browser *b = cut_browser(t);
    bool hit = b != NULL;
    for (const wchar_t *const *p = BROWSER_SUF; *p && !hit; p++) hit = cut_suffix(t, *p);
    if (!hit) return false;
    s_browser = b;
    s_browser_missing = false;
    bool ok = open_web_target(t, r);
    s_browser = NULL;
    if (!ok || !b) return ok;
    if (s_browser_missing) {
        const wchar_t *cfg = configured_browser_name();
        if (cfg && _wcsicmp(path_name(g_cfg.browser), b->exe))
            reply(r, L"%ls isn't installed, opening in %ls.", b->disp, cfg);
        else
            reply(r, L"%ls isn't installed, opening in your default browser.", b->disp);
    } else {
        size_t n = wcslen(r->reply);
        if (n && r->reply[n - 1] == L'.') r->reply[--n] = 0;
        wchar_t tail[64];
        swprintf(tail, 64, L" in %ls.", b->disp);
        str_cat(r->reply, 300, tail);
    }
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
    str_copy(s_orig, S_CAP, text);

    /* answer to "Move X to the Recycle Bin? Say yes or no." */
    if (s_del_wait && run_delete_answer(s, r)) return;
    s_del_wait = false;

    /* follow-up to a list: "two", "the second one", "open folder" */
    if (s_nrows > 0 && s_rows[0].kind == ROW_HIT) {
        bool reveal;
        int n = parse_pick(s, &reveal);
        if (n) { cmd_pick(&s_rows[n - 1], reveal ? PICK_REVEAL : s_mode, r); return; }
    }
    s_plan_wait = false;    /* anything else: forget a pending "which file?" */
    /* a bare "two" / "number 3" with no list up: don't search files named "2" */
    {
        wchar_t t[S_CAP];
        str_copy(t, S_CAP, s);
        cut_prefixes(t, (WList){ L"number", L"no", L"num", L"option", L"the", NULL });
        if (!str_eq(t, L"one")) cut_suffix(t, L"one");
        if (t[0] && !wcschr(t, L' ') && number_word(t) > 0) {
            reply(r, s_nrows ? L"There's no number %ls in the list." : L"There's no list to pick from yet.", t);
            record(L"SAY");
            r->listen_again = true;
            return;
        }
    }
    if (run_fixed(s, r)) return;

    /* an exact configured name wins over everything ("google drive", "youtube") */
    {
        wchar_t name[256];
        strip_articles(s, name, 256);
        if (name[0] && try_config(name, r)) return;
    }
    if (try_docs(s, r)) return;
    if (try_browser(s, r)) return;

    wchar_t q[S_CAP];
    int engine;
    if (match_web(s, &engine, q, S_CAP)) { web_search(engine, q, r); return; }

    FileQ fq;
    if (match_reveal(s, q, S_CAP)) {
        wchar_t name[256];
        if (open_ref(q, true, r)) return;
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
    {   /* document commands: no live file list (a click would run "open") */
        wchar_t w1[1][64];
        static WList DOCV = { L"convert", L"split", L"merge", L"combine", L"extract", L"rename", L"move",
                              L"copy", L"delete", L"remove", L"trash", L"erase", NULL };
        if (split_words(s, w1, 1) && in_list(w1[0], DOCV)) return 0;
    }
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
    static WList VERBS = { L"open", L"go to", L"launch", L"start", L"show", L"upload", L"find", L"convert",
        L"split", L"merge", L"extract", L"rename", L"move", L"copy", L"delete", NULL };
    static WList PHRASES = { L"cancel", L"never mind", L"close", L"help", L"what time is it", L"what's the date",
        L"battery", L"lock", L"screenshot", L"volume up", L"volume down", L"mute", L"number one", L"number two",
        L"number three", L"number four", L"number five", L"number six", L"number seven", L"number eight",
        L"first", L"second", L"third", L"fourth", L"fifth", L"sixth", L"seventh", L"eighth",
        L"show in folder", L"what can you do", L"what day is it", L"yes", L"no",
        L"convert this to pdf", L"convert this to word", L"merge these", L"delete this", NULL };
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

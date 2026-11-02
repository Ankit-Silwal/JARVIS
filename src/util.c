/* Small string / path helpers. */
#include "jarvis.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wctype.h>

void str_lower(wchar_t *s) { for (; *s; s++) *s = (wchar_t)towlower(*s); }

void str_trim(wchar_t *s)
{
    size_t n = wcslen(s), i = 0;
    while (n && iswspace(s[n - 1])) s[--n] = 0;
    while (s[i] && iswspace(s[i])) i++;
    if (i) memmove(s, s + i, (n - i + 1) * sizeof(wchar_t));
}

void str_collapse_spaces(wchar_t *s)
{
    wchar_t *w = s; bool sp = false;
    for (wchar_t *r = s; *r; r++) {
        if (iswspace(*r)) { if (!sp && w != s) *w++ = L' '; sp = true; }
        else { *w++ = *r; sp = false; }
    }
    *w = 0;
    str_trim(s);
}

bool str_starts(const wchar_t *s, const wchar_t *p) { return wcsncmp(s, p, wcslen(p)) == 0; }

bool str_ends(const wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && wcscmp(s + a - b, suf) == 0;
}

const wchar_t *str_after_word(const wchar_t *s, const wchar_t *p)
{
    size_t n = wcslen(p);
    if (wcsncmp(s, p, n) != 0) return NULL;
    if (s[n] == 0) return s + n;
    if (s[n] == L' ') return s + n + 1;
    return NULL;
}

bool str_eq(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b) == 0; }

void str_copy(wchar_t *dst, size_t cap, const wchar_t *src)
{
    if (!cap) return;
    size_t n = src ? wcslen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n) memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = 0;
}

void str_cat(wchar_t *dst, size_t cap, const wchar_t *src)
{
    size_t n = wcslen(dst);
    if (n < cap) str_copy(dst + n, cap - n, src);
}

wchar_t *wcs_dup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = malloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

static const struct { const wchar_t *w; int n; } NUMS[] = {
    {L"one",1},{L"won",1},{L"first",1},{L"1st",1},
    {L"two",2},{L"to",2},{L"too",2},{L"second",2},{L"2nd",2},
    {L"three",3},{L"tree",3},{L"third",3},{L"3rd",3},
    {L"four",4},{L"for",4},{L"fourth",4},{L"4th",4},
    {L"five",5},{L"fifth",5},{L"5th",5},
    {L"six",6},{L"sixth",6},{L"6th",6},
    {L"seven",7},{L"seventh",7},{L"7th",7},
    {L"eight",8},{L"ate",8},{L"eighth",8},{L"8th",8},
    {L"nine",9},{L"ninth",9},{L"ten",10},{L"tenth",10},
};

int number_word(const wchar_t *w)
{
    if (iswdigit(w[0])) {
        int v = _wtoi(w);
        const wchar_t *p = w; while (iswdigit(*p)) p++;
        if (*p == 0 || str_eq(p, L"st") || str_eq(p, L"nd") || str_eq(p, L"rd") || str_eq(p, L"th"))
            return v;
        return 0;
    }
    for (size_t i = 0; i < sizeof NUMS / sizeof NUMS[0]; i++)
        if (str_eq(w, NUMS[i].w)) return NUMS[i].n;
    return 0;
}

/* Splits on anything that is not a letter or digit. Lowercases. */
int tokenize(const wchar_t *s, Token *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && !iswalnum(*s)) s++;
        if (!*s) break;
        int len = 0;
        Token *t = &out[n];
        while (*s && iswalnum(*s)) {
            if (len < 63) t->w[len++] = (wchar_t)towlower(*s);
            s++;
        }
        t->w[len] = 0;
        t->alt[0] = 0;
        int num = number_word(t->w);
        /* only "real" number words get a digit alternative (not "to"/"for") */
        if (num && !iswdigit(t->w[0]) && !str_eq(t->w, L"to") && !str_eq(t->w, L"too") &&
            !str_eq(t->w, L"for") && !str_eq(t->w, L"won") && !str_eq(t->w, L"ate") &&
            !str_eq(t->w, L"tree"))
            swprintf(t->alt, 16, L"%d", num);
        n++;
    }
    return n;
}

/* Levenshtein with early exit; returns limit+1 when over the limit. */
int edit_distance(const wchar_t *a, int la, const wchar_t *b, int lb, int limit)
{
    if (abs(la - lb) > limit) return limit + 1;
    if (la > 63 || lb > 63) return limit + 1;
    int prev[64], cur[64];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        int rowmin = cur[0];
        for (int j = 1; j <= lb; j++) {
            int c = prev[j - 1] + (a[i - 1] != b[j - 1]);
            if (prev[j] + 1 < c) c = prev[j] + 1;
            if (cur[j - 1] + 1 < c) c = cur[j - 1] + 1;
            cur[j] = c;
            if (c < rowmin) rowmin = c;
        }
        if (rowmin > limit) return limit + 1;
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

char *utf8_from_wide(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0;
    return s;
}

wchar_t *wide_from_utf8(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    wchar_t *w = malloc((n + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n] = 0;
    return w;
}

void expand_env(const wchar_t *in, wchar_t *out, size_t cap)
{
    DWORD n = ExpandEnvironmentStringsW(in, out, (DWORD)cap);
    if (n == 0 || n > cap) str_copy(out, cap, in);
}

bool path_exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

bool path_is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

const wchar_t *path_name(const wchar_t *p)
{
    const wchar_t *n = p;
    for (const wchar_t *c = p; *c; c++)
        if ((*c == L'\\' || *c == L'/') && c[1]) n = c + 1;
    return n;
}

/* Appends to %LOCALAPPDATA%\JARVIS\jarvis.log (kept small: truncated past 256 KB). */
void log_line(const wchar_t *fmt, ...)
{
    if (!g_cfg.data_dir[0]) return;
    wchar_t path[MAX_PATH], msg[1024];
    swprintf(path, MAX_PATH, L"%ls\\jarvis.log", g_cfg.data_dir);
    va_list ap; va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);

    WIN32_FILE_ATTRIBUTE_DATA fa;
    bool big = GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024;
    HANDLE h = CreateFileW(path, big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[1100];
    swprintf(line, 1100, L"%04d-%02d-%02d %02d:%02d:%02d  %ls\r\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
    char *u = utf8_from_wide(line);
    if (u) { DWORD wr; WriteFile(h, u, (DWORD)strlen(u), &wr, NULL); free(u); }
    CloseHandle(h);
}

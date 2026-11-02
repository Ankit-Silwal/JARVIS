/* File index + fuzzy search.
 *
 * The index is a flat list of every file/folder under the configured roots,
 * cached in %LOCALAPPDATA%\JARVIS\index.txt. It loads in a few milliseconds
 * and is refreshed in a low-priority background thread when it gets old,
 * so JARVIS never has to run in the background between uses. */
#include "jarvis.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#define MAX_ENTRIES   400000
#define MAX_DEPTH     14
#define BLOCK_CHARS   (1 << 20)

typedef struct {
    wchar_t *path;
    wchar_t *name;      /* points into path */
    wchar_t *lc;        /* lowercase copy of name */
    FILETIME mt;
    unsigned short depth;
    unsigned char is_dir;
    unsigned char ext;
} Entry;

typedef struct Block { struct Block *next; size_t used; wchar_t data[BLOCK_CHARS]; } Block;

typedef struct {
    Entry *e;
    int n, cap;
    Block *blocks;
} Index;

static Index  *g_idx;                 /* current published index */
static SRWLOCK g_lock = SRWLOCK_INIT;
static HANDLE  g_thread;
static HANDLE  g_ready;               /* signalled once an index is available */
static volatile LONG g_busy;

/* ------------------------------------------------------------ index memory */
static wchar_t *arena_put(Index *ix, const wchar_t *s, size_t len)
{
    if (!ix->blocks || ix->blocks->used + len + 1 > BLOCK_CHARS) {
        Block *b = malloc(sizeof(Block));
        if (!b) return NULL;
        b->next = ix->blocks; b->used = 0;
        ix->blocks = b;
    }
    wchar_t *d = ix->blocks->data + ix->blocks->used;
    memcpy(d, s, len * sizeof(wchar_t));
    d[len] = 0;
    ix->blocks->used += len + 1;
    return d;
}

static void index_free(Index *ix)
{
    if (!ix) return;
    for (Block *b = ix->blocks; b;) { Block *n = b->next; free(b); b = n; }
    free(ix->e);
    free(ix);
}

static const struct { const wchar_t *ext; ExtClass c; } EXTS[] = {
    {L"jpg",EXT_IMAGE},{L"jpeg",EXT_IMAGE},{L"png",EXT_IMAGE},{L"gif",EXT_IMAGE},{L"bmp",EXT_IMAGE},
    {L"webp",EXT_IMAGE},{L"heic",EXT_IMAGE},{L"svg",EXT_IMAGE},{L"tif",EXT_IMAGE},{L"tiff",EXT_IMAGE},{L"ico",EXT_IMAGE},
    {L"pdf",EXT_PDF},
    {L"ppt",EXT_PPT},{L"pptx",EXT_PPT},{L"odp",EXT_PPT},{L"key",EXT_PPT},
    {L"doc",EXT_DOC},{L"docx",EXT_DOC},{L"odt",EXT_DOC},{L"rtf",EXT_DOC},
    {L"xls",EXT_SHEET},{L"xlsx",EXT_SHEET},{L"csv",EXT_SHEET},{L"ods",EXT_SHEET},
    {L"mp4",EXT_VIDEO},{L"mkv",EXT_VIDEO},{L"mov",EXT_VIDEO},{L"avi",EXT_VIDEO},{L"webm",EXT_VIDEO},{L"wmv",EXT_VIDEO},
    {L"mp3",EXT_AUDIO},{L"wav",EXT_AUDIO},{L"m4a",EXT_AUDIO},{L"flac",EXT_AUDIO},{L"aac",EXT_AUDIO},{L"ogg",EXT_AUDIO},
    {L"zip",EXT_ARCHIVE},{L"rar",EXT_ARCHIVE},{L"7z",EXT_ARCHIVE},{L"tar",EXT_ARCHIVE},{L"gz",EXT_ARCHIVE},
    {L"c",EXT_CODE},{L"h",EXT_CODE},{L"cpp",EXT_CODE},{L"py",EXT_CODE},{L"js",EXT_CODE},{L"ts",EXT_CODE},
    {L"jsx",EXT_CODE},{L"tsx",EXT_CODE},{L"java",EXT_CODE},{L"html",EXT_CODE},{L"css",EXT_CODE},{L"ipynb",EXT_CODE},
    {L"txt",EXT_TEXT},{L"md",EXT_TEXT},
};

ExtClass ext_class_of(const wchar_t *path)
{
    const wchar_t *dot = wcsrchr(path_name(path), L'.');
    if (!dot || !dot[1]) return EXT_ANY;
    for (size_t i = 0; i < sizeof EXTS / sizeof EXTS[0]; i++)
        if (_wcsicmp(dot + 1, EXTS[i].ext) == 0) return EXTS[i].c;
    return EXT_ANY;
}

static void index_add(Index *ix, const wchar_t *path, size_t len, bool is_dir, FILETIME mt, int depth)
{
    if (ix->n >= MAX_ENTRIES) return;
    if (ix->n == ix->cap) {
        int nc = ix->cap ? ix->cap * 2 : 16384;
        Entry *ne = realloc(ix->e, nc * sizeof(Entry));
        if (!ne) return;
        ix->e = ne; ix->cap = nc;
    }
    wchar_t *p = arena_put(ix, path, len);
    if (!p) return;
    Entry *e = &ix->e[ix->n];
    e->path = p;
    e->name = (wchar_t *)path_name(p);
    size_t nl = wcslen(e->name);
    e->lc = arena_put(ix, e->name, nl);
    if (!e->lc) return;
    str_lower(e->lc);
    e->mt = mt;
    e->is_dir = is_dir;
    e->depth = (unsigned short)depth;
    e->ext = is_dir ? EXT_ANY : (unsigned char)ext_class_of(e->name);
    ix->n++;
}

/* ------------------------------------------------------------- scanning */
static bool skip_dir_name(const wchar_t *n)
{
    if (n[0] == L'.') return true;
    for (int i = 0; i < g_cfg.nskip; i++)
        if (_wcsicmp(n, g_cfg.skip[i]) == 0) return true;
    return false;
}

static bool is_other_root(const wchar_t *path, int self)
{
    for (int i = 0; i < g_cfg.nroots; i++)
        if (i != self && _wcsicmp(path, g_cfg.roots[i]) == 0) return true;
    return false;
}

static bool root_inside_other(int r)
{
    size_t L1 = wcslen(g_cfg.roots[r]);
    for (int i = 0; i < g_cfg.nroots; i++) {
        if (i == r) continue;
        size_t L2 = wcslen(g_cfg.roots[i]);
        if (L1 > L2 && _wcsnicmp(g_cfg.roots[r], g_cfg.roots[i], L2) == 0 && g_cfg.roots[r][L2] == L'\\')
            return true;
    }
    return false;
}

typedef struct { wchar_t *path; int depth; } DirItem;

static void scan_root(Index *ix, int r)
{
    const wchar_t *root = g_cfg.roots[r];
    if (!path_is_dir(root) || root_inside_other(r)) return;

    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME zero = {0};
    index_add(ix, root, wcslen(root), true,
              GetFileAttributesExW(root, GetFileExInfoStandard, &fa) ? fa.ftLastWriteTime : zero, 0);

    int cap = 256, top = 0;
    DirItem *st = malloc(cap * sizeof(DirItem));
    if (!st) return;
    st[top].path = wcs_dup(root); st[top].depth = 0; top++;

    wchar_t pat[MAX_PATH + 4], full[MAX_PATH * 2];
    while (top > 0 && ix->n < MAX_ENTRIES) {
        DirItem d = st[--top];
        swprintf(pat, MAX_PATH + 4, L"%ls\\*", d.path);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const wchar_t *n = fd.cFileName;
                if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
                DWORD a = fd.dwFileAttributes;
                if (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                int len = swprintf(full, MAX_PATH * 2, L"%ls\\%ls", d.path, n);
                if (len <= 0 || len >= MAX_PATH) continue;
                if (a & FILE_ATTRIBUTE_DIRECTORY) {
                    if (skip_dir_name(n)) continue;
                    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK))
                        continue;
                    if (is_other_root(full, r)) continue;
                    index_add(ix, full, len, true, fd.ftLastWriteTime, d.depth + 1);
                    if (d.depth + 1 < MAX_DEPTH) {
                        if (top == cap) {
                            DirItem *ns = realloc(st, cap * 2 * sizeof(DirItem));
                            if (!ns) continue;
                            st = ns; cap *= 2;
                        }
                        st[top].path = wcs_dup(full); st[top].depth = d.depth + 1; top++;
                    }
                } else {
                    if (n[0] == L'.' || (n[0] == L'~' && n[1] == L'$')) continue;  /* dotfiles, Office temp */
                    const wchar_t *dot = wcsrchr(n, L'.');
                    if (dot && (!_wcsicmp(dot, L".tmp") || !_wcsicmp(dot, L".lnk") || !_wcsicmp(dot, L".ini") ||
                                !_wcsicmp(dot, L".crdownload") || !_wcsicmp(dot, L".part")))
                        continue;
                    index_add(ix, full, len, false, fd.ftLastWriteTime, d.depth + 1);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(d.path);
    }
    while (top > 0) free(st[--top].path);
    free(st);
}

static Index *index_build(void)
{
    Index *ix = calloc(1, sizeof(Index));
    if (!ix) return NULL;
    for (int r = 0; r < g_cfg.nroots; r++) scan_root(ix, r);
    return ix;
}

/* --------------------------------------------------------- disk cache */
static void index_path(wchar_t *out) { swprintf(out, MAX_PATH, L"%ls\\index.txt", g_cfg.data_dir); }

static void index_save(Index *ix)
{
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    index_path(path);
    swprintf(tmp, MAX_PATH, L"%ls.tmp", path);
    FILE *f = _wfopen(tmp, L"wb");
    if (!f) return;
    fputs("JARVIS-INDEX 1\n", f);
    char buf[MAX_PATH * 4];
    for (int i = 0; i < ix->n; i++) {
        Entry *e = &ix->e[i];
        int u = WideCharToMultiByte(CP_UTF8, 0, e->path, -1, buf, sizeof buf, NULL, NULL);
        if (u <= 0) continue;
        unsigned long long t = ((unsigned long long)e->mt.dwHighDateTime << 32) | e->mt.dwLowDateTime;
        fprintf(f, "%c\t%u\t%llx\t%s\n", e->is_dir ? 'D' : 'F', e->depth, t, buf);
    }
    fclose(f);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static Index *index_load(void)
{
    wchar_t path[MAX_PATH];
    index_path(path);
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 200LL * 1024 * 1024) { CloseHandle(h); return NULL; }
    char *data = malloc((size_t)sz.QuadPart + 1);
    DWORD rd = 0;
    if (!data || !ReadFile(h, data, (DWORD)sz.QuadPart, &rd, NULL)) { free(data); CloseHandle(h); return NULL; }
    CloseHandle(h);
    data[rd] = 0;
    if (strncmp(data, "JARVIS-INDEX 1\n", 15) != 0) { free(data); return NULL; }

    Index *ix = calloc(1, sizeof(Index));
    if (!ix) { free(data); return NULL; }
    wchar_t wpath[MAX_PATH];
    char *p = data + 15;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0;
        /* kind \t depth \t time \t path */
        char kind = p[0];
        char *t1 = strchr(p, '\t');
        char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
        if (t3) {
            int depth = atoi(t1 + 1);
            unsigned long long t = strtoull(t2 + 1, NULL, 16);
            int n = MultiByteToWideChar(CP_UTF8, 0, t3 + 1, -1, wpath, MAX_PATH);
            if (n > 1) {
                FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
                index_add(ix, wpath, n - 1, kind == 'D', ft, depth);
            }
        }
        p = nl + 1;
    }
    free(data);
    return ix;
}

static void publish(Index *ix)
{
    AcquireSRWLockExclusive(&g_lock);
    Index *old = g_idx;
    g_idx = ix;
    ReleaseSRWLockExclusive(&g_lock);
    index_free(old);
    SetEvent(g_ready);
}

static bool index_stale(void)
{
    wchar_t path[MAX_PATH];
    index_path(path);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return true;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a = { .LowPart = fa.ftLastWriteTime.dwLowDateTime, .HighPart = fa.ftLastWriteTime.dwHighDateTime };
    ULARGE_INTEGER b = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime };
    unsigned long long age_min = (b.QuadPart - a.QuadPart) / (10000000ULL * 60);
    return age_min >= (unsigned long long)(g_cfg.index_max_age_min > 0 ? g_cfg.index_max_age_min : 30);
}

static DWORD WINAPI index_thread(LPVOID force)
{
    bool have = false;
    if (!force) {
        Index *ix = index_load();
        if (ix) { publish(ix); have = true; }
    }
    if (force || !have || index_stale()) {
        /* be gentle while the user is working, but hurry if they're waiting on the first index */
        if (have) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        DWORD t0 = GetTickCount();
        Index *ix = index_build();
        if (have) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        if (ix) {
            index_save(ix);
            log_line(L"index rebuilt: %d entries in %lu ms", ix->n, GetTickCount() - t0);
            publish(ix);
        }
    }
    InterlockedExchange(&g_busy, 0);
    return 0;
}

static void start_thread(bool force)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return;
    if (g_thread) { CloseHandle(g_thread); g_thread = NULL; }
    g_thread = CreateThread(NULL, 0, index_thread, force ? (LPVOID)1 : NULL, 0, NULL);
    if (!g_thread) InterlockedExchange(&g_busy, 0);
}

void search_init(void)
{
    if (!g_ready) g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    start_thread(false);
}

void search_rebuild_async(void) { start_thread(true); }
bool search_busy(void) { return g_busy != 0; }
bool search_ready(void) { return WaitForSingleObject(g_ready, 0) == WAIT_OBJECT_0; }

void search_wait(DWORD ms)
{
    if (g_thread) WaitForSingleObject(g_thread, ms);
}

int search_count(void)
{
    AcquireSRWLockShared(&g_lock);
    int n = g_idx ? g_idx->n : 0;
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* ------------------------------------------------------------- scoring */
static bool word_start(const wchar_t *name, size_t i)
{
    if (i == 0) return true;
    wchar_t a = name[i - 1], b = name[i];
    if (!iswalnum(a)) return true;
    if (iswlower(a) && iswupper(b)) return true;           /* camelCase */
    if (iswdigit(a) != iswdigit(b)) return true;            /* chapter2 */
    return false;
}

static bool find_token(const wchar_t *hay, const wchar_t *orig, const wchar_t *tok, bool *at_word)
{
    const wchar_t *p = hay;
    bool found = false;
    *at_word = false;
    while ((p = wcsstr(p, tok)) != NULL) {
        found = true;
        if (word_start(orig, (size_t)(p - hay))) { *at_word = true; return true; }
        p++;
    }
    return found;
}

static bool fuzzy_word(const wchar_t *lc, const wchar_t *tok)
{
    int tl = (int)wcslen(tok);
    if (tl < 4) return false;
    int limit = tl >= 7 ? 2 : 1;
    const wchar_t *p = lc;
    while (*p) {
        while (*p && !iswalnum(*p)) p++;
        const wchar_t *s = p;
        while (*p && iswalnum(*p)) p++;
        int wl = (int)(p - s);
        if (wl >= 3 && edit_distance(tok, tl, s, wl, limit) <= limit) return true;
        /* also allow the token to fuzzily match the start of a longer word */
        if (wl > tl && edit_distance(tok, tl, s, tl, limit) <= (limit > 1 ? 1 : limit) && tl >= 5) return true;
    }
    return false;
}

static void normalize_stem(const wchar_t *lc, bool is_dir, wchar_t *out, size_t cap, bool nospace)
{
    size_t n = 0;
    const wchar_t *end = lc + wcslen(lc);
    if (!is_dir) { const wchar_t *dot = wcsrchr(lc, L'.'); if (dot && dot != lc) end = dot; }
    bool sp = false;
    for (const wchar_t *p = lc; p < end && n + 1 < cap; p++) {
        if (iswalnum(*p)) { out[n++] = *p; sp = false; }
        else if (!nospace && !sp && n) { out[n++] = L' '; sp = true; }
    }
    while (n && out[n - 1] == L' ') n--;
    out[n] = 0;
}

static int days_old(FILETIME ft, ULONGLONG now)
{
    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    if (t > now) return 0;
    return (int)((now - t) / (10000000ULL * 86400));
}

static int score_entry(const Entry *e, const Token *tk, int nt, const wchar_t *joined,
                       const wchar_t *joined_ns, int qlen, ULONGLONG now)
{
    int s = 0;
    if (nt == 0) {
        s = 50;
    } else {
        int matched = 0, miss = 0, in_name = 0;
        for (int i = 0; i < nt; i++) {
            bool at_word = false;
            const wchar_t *t = tk[i].w;
            bool hit = find_token(e->lc, e->name, t, &at_word);
            if (!hit && tk[i].alt[0]) { t = tk[i].alt; hit = find_token(e->lc, e->name, t, &at_word); }
            if (hit) {
                matched++; in_name++;
                s += at_word ? 24 : (wcslen(t) >= 3 ? 12 : 4);
                if (e->lc == wcsstr(e->lc, t)) s += 6;
                continue;
            }
            if (fuzzy_word(e->lc, tk[i].w)) { matched++; in_name++; s += 12; continue; }
            /* token appears in a parent folder name? */
            size_t dl = (size_t)(e->name - e->path);
            if (dl > 0 && dl < MAX_PATH && wcslen(tk[i].w) >= 3) {
                wchar_t dir[MAX_PATH];
                memcpy(dir, e->path, dl * sizeof(wchar_t)); dir[dl] = 0;
                str_lower(dir);
                if (wcsstr(dir, tk[i].w)) { matched++; s += 7; continue; }
            }
            miss++;
        }
        if (in_name == 0) return 0;               /* at least one word must be in the name */
        if (miss) {
            /* tolerate one misheard word in longer queries */
            if (nt < 3 || miss > 1) return 0;
            s -= 20;
        }
        if (matched == nt && in_name == nt) s += 15;

        wchar_t stem[MAX_PATH];
        normalize_stem(e->lc, e->is_dir, stem, MAX_PATH, false);
        if (str_eq(stem, joined)) s += 60;
        else {
            normalize_stem(e->lc, e->is_dir, stem, MAX_PATH, true);
            if (str_eq(stem, joined_ns)) s += 55;
            else if (str_starts(stem, joined_ns)) s += 20;
        }
        /* prefer names close in length to the query */
        int extra = (int)wcslen(stem) - qlen;
        if (extra > 0) s -= extra / 6;
    }
    int d = days_old(e->mt, now);
    if (d < 2) s += 8; else if (d < 14) s += 5; else if (d < 60) s += 2;
    s -= e->depth > 2 ? (e->depth - 2) : 0;
    return s > 0 ? s : 1;
}

static bool better(const Hit *a, const Hit *b, bool recent)
{
    LONG c = CompareFileTime(&a->mtime, &b->mtime);
    if (recent) {
        if (c != 0) return c > 0;
        return a->score > b->score;
    }
    if (a->score != b->score) return a->score > b->score;
    return c > 0;
}

int search_files(const SearchQuery *q, Hit *out, int max)
{
    Token tk[12];
    int nt = q->text ? tokenize(q->text, tk, 12) : 0;
    wchar_t joined[512] = L"", joined_ns[512] = L"";
    for (int i = 0; i < nt; i++) {
        if (i) str_cat(joined, 512, L" ");
        str_cat(joined, 512, tk[i].w);
        str_cat(joined_ns, 512, tk[i].w);
    }
    int qlen = (int)wcslen(joined_ns);
    size_t flen = q->in_folder ? wcslen(q->in_folder) : 0;
    bool recent = (q->flags & SF_RECENT) != 0;

    FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
    ULONGLONG now = ((ULONGLONG)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;

    int n = 0;
    AcquireSRWLockShared(&g_lock);
    Index *ix = g_idx;
    for (int i = 0; ix && i < ix->n; i++) {
        const Entry *e = &ix->e[i];
        if ((q->flags & SF_FILES_ONLY) && e->is_dir) continue;
        if ((q->flags & SF_DIRS_ONLY) && !e->is_dir) continue;
        if (q->ext != EXT_ANY && e->ext != q->ext) continue;
        if (flen) {
            if (_wcsnicmp(e->path, q->in_folder, flen) != 0 || e->path[flen] != L'\\') continue;
        }
        /* a bare "recent files" style query shouldn't list folders */
        if (nt == 0 && e->is_dir) continue;
        int s = score_entry(e, tk, nt, joined, joined_ns, qlen, now);
        if (s <= 0) continue;

        Hit h; h.score = s; h.mtime = e->mt; h.is_dir = e->is_dir;
        /* insertion into the top-N list */
        int pos = n;
        while (pos > 0 && better(&h, &out[pos - 1], recent)) pos--;
        if (pos >= max) continue;
        int last = n < max ? n : max - 1;
        for (int j = last; j > pos; j--) out[j] = out[j - 1];
        out[pos] = h;
        str_copy(out[pos].path, MAX_PATH, e->path);
        if (n < max) n++;
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* ------------------------------------------------------- Start Menu apps */
typedef struct { wchar_t path[MAX_PATH]; wchar_t name[128]; wchar_t lc[128]; } App;
static App *g_apps;
static int  g_napps, g_capps;

static void scan_apps(const wchar_t *dir, int depth)
{
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t *n = fd.cFileName;
        if (n[0] == L'.') continue;
        wchar_t full[MAX_PATH];
        if (swprintf(full, MAX_PATH, L"%ls\\%ls", dir, n) < 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 3) scan_apps(full, depth + 1);
            continue;
        }
        const wchar_t *dot = wcsrchr(n, L'.');
        if (!dot || (_wcsicmp(dot, L".lnk") && _wcsicmp(dot, L".url") && _wcsicmp(dot, L".appref-ms"))) continue;
        if (g_napps == g_capps) {
            int nc = g_capps ? g_capps * 2 : 256;
            App *na = realloc(g_apps, nc * sizeof(App));
            if (!na) break;
            g_apps = na; g_capps = nc;
        }
        App *a = &g_apps[g_napps];
        str_copy(a->path, MAX_PATH, full);
        size_t L = (size_t)(dot - n);
        if (L > 127) L = 127;
        memcpy(a->name, n, L * sizeof(wchar_t)); a->name[L] = 0;
        str_copy(a->lc, 128, a->name);
        str_lower(a->lc);
        if (wcsstr(a->lc, L"uninstall") || wcsstr(a->lc, L"readme") || wcsstr(a->lc, L"documentation") ||
            wcsstr(a->lc, L"release notes") || wcsstr(a->lc, L"website") || wcsstr(a->lc, L"help"))
            continue;
        g_napps++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int search_app(const wchar_t *query, wchar_t *out_path, wchar_t *out_name)
{
    if (!g_apps) {
        PWSTR p = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Programs, 0, NULL, &p))) scan_apps(p, 0);
        CoTaskMemFree(p); p = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &p))) scan_apps(p, 0);
        CoTaskMemFree(p);
        if (!g_apps) { g_apps = malloc(sizeof(App)); g_capps = 1; }
    }
    Token qt[8];
    int nq = tokenize(query, qt, 8);
    if (!nq) return 0;
    wchar_t qj[256] = L"";
    for (int i = 0; i < nq; i++) { if (i) str_cat(qj, 256, L" "); str_cat(qj, 256, qt[i].w); }

    int best = 0;
    for (int i = 0; i < g_napps; i++) {
        Token at[16];
        int na = tokenize(g_apps[i].lc, at, 16);
        wchar_t aj[256] = L"";
        for (int k = 0; k < na; k++) { if (k) str_cat(aj, 256, L" "); str_cat(aj, 256, at[k].w); }
        int s = 0;
        if (str_eq(aj, qj)) s = 100;
        else {
            /* every spoken word must be a whole word of the app name */
            int ok = 0;
            for (int j = 0; j < nq; j++)
                for (int k = 0; k < na; k++)
                    if (str_eq(qt[j].w, at[k].w)) { ok++; break; }
            if (ok == nq && na <= nq + 2) s = 80 - (na - nq) * 5;
            else if (ok == nq && na <= nq + 4 && nq >= 2) s = 65;
        }
        if (s > best) {
            best = s;
            str_copy(out_path, MAX_PATH, g_apps[i].path);
            if (out_name) str_copy(out_name, 128, g_apps[i].name);
        }
    }
    return best;
}

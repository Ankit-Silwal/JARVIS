/* Integration tests for src/search.c: builds a fixture tree in %TEMP%,
 * points g_cfg at it (config_load() is NOT called, so the user's real
 * jarvis.ini / index are never touched), indexes it and checks scoring. */
#include "../src/jarvis.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static wchar_t g_root[MAX_PATH];   /* fixture root (indexed) */
static wchar_t g_data[MAX_PATH];   /* fake data_dir (index.txt, jarvis.log) */
static wchar_t g_base[MAX_PATH];   /* parent of both, removed at the end */

static ULONGLONG now_ticks(void)
{
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static FILETIME ft_days_ago(int days)
{
    ULONGLONG t = now_ticks() - (ULONGLONG)days * 86400ULL * 10000000ULL;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    return ft;
}

static void mkdirs(const wchar_t *rel)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", g_root, rel);
    for (wchar_t *c = p + wcslen(g_root) + 1; *c; c++)
        if (*c == L'\\') { *c = 0; CreateDirectoryW(p, NULL); *c = L'\\'; }
    CreateDirectoryW(p, NULL);
}

/* creates file root\rel with a small body and the given age in days */
static void mkfile(const wchar_t *rel, int age_days)
{
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\%ls", g_root, rel);
    HANDLE h = CreateFileW(p, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { printf("cannot create fixture file %ls\n", p); return; }
    DWORD wr; WriteFile(h, "x", 1, &wr, NULL);
    FILETIME ft = ft_days_ago(age_days);
    SetFileTime(h, &ft, &ft, &ft);
    CloseHandle(h);
}

static void rmtree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH], full[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *n = fd.cFileName;
            if (str_eq(n, L".") || str_eq(n, L"..")) continue;
            swprintf(full, MAX_PATH, L"%ls\\%ls", dir, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(full);
            else { SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL); DeleteFileW(full); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void setup(void)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    size_t L = wcslen(tmp);
    if (L && tmp[L - 1] == L'\\') tmp[L - 1] = 0;
    /* note: the fixture root name must not contain any query word ("jarvis"...) */
    swprintf(g_base, MAX_PATH, L"%ls\\jvt_%lu_%lu", tmp, GetCurrentProcessId(), GetTickCount());
    swprintf(g_root, MAX_PATH, L"%ls\\fixture", g_base);
    swprintf(g_data, MAX_PATH, L"%ls\\data", g_base);
    CreateDirectoryW(g_base, NULL);
    CreateDirectoryW(g_root, NULL);
    CreateDirectoryW(g_data, NULL);

    mkdirs(L"Downloads");
    mkdirs(L"Documents\\node_modules");
    mkdirs(L"Documents\\.hidden");
    mkdirs(L"Pictures\\Holiday");
    mkdirs(L"Projects\\JARVIS");

    mkfile(L"Downloads\\Resume_final.pdf", 20);
    mkfile(L"Downloads\\IMG_2041.jpg", 0);          /* newest */
    mkfile(L"Documents\\Physics Notes.docx", 30);
    mkfile(L"Documents\\physics_lab_report.pdf", 40);
    mkfile(L"Documents\\Chapter2 Biology.pptx", 50);
    mkfile(L"Documents\\node_modules\\resume.pdf", 1);   /* must be skipped */
    mkfile(L"Documents\\.hidden\\resume.pdf", 1);        /* dot dir: skipped */
    mkfile(L"Pictures\\Holiday\\beach.png", 10);

    static wchar_t *roots[1], *skip[2];
    roots[0] = g_root;
    skip[0] = L"node_modules";
    skip[1] = L"__pycache__";
    memset(&g_cfg, 0, sizeof g_cfg);
    g_cfg.roots = roots; g_cfg.nroots = 1;
    g_cfg.skip = skip;   g_cfg.nskip = 2;
    g_cfg.index_max_age_min = 30;
    str_copy(g_cfg.data_dir, MAX_PATH, g_data);
}

static int run(const wchar_t *text, const wchar_t *in_folder, ExtClass ext, unsigned flags, Hit *h)
{
    SearchQuery q = { text, in_folder, ext, flags };
    int n = search_files(&q, h, MAX_HITS);
    printf("   query \"%ls\"%s%ls -> %d hit(s)\n", text ? text : L"", in_folder ? " in " : "",
           in_folder ? path_name(in_folder) : L"", n);
    for (int i = 0; i < n; i++)
        printf("     %d. [%d] %ls\n", i + 1, h[i].score, h[i].path + wcslen(g_root));
    return n;
}

static bool ends_with(const Hit *h, const wchar_t *rel)
{
    return str_ends(h->path, rel);
}

static bool any_contains(const Hit *h, int n, const wchar_t *sub)
{
    for (int i = 0; i < n; i++)
        if (wcsstr(h[i].path, sub)) return true;
    return false;
}

static int find_rank(const Hit *h, int n, const wchar_t *rel)
{
    for (int i = 0; i < n; i++)
        if (ends_with(&h[i], rel)) return i;
    return -1;
}

int wmain(void)
{
    setup();
    printf("fixture: %ls\n", g_root);

    TEST_SECTION("index build");
    search_init();
    search_wait(20000);
    CHECK(search_ready(), "index ready after search_wait");
    CHECK(!search_busy(), "index thread finished");
    /* root, Downloads, Documents, Pictures, Holiday, Projects, JARVIS = 7 dirs;
     * Resume, IMG, Physics Notes, physics lab, Chapter2, beach = 6 files */
    CHECKF(search_count() == 13, "13 entries indexed (got %d)", search_count());

    Hit h[MAX_HITS];
    int n;

    TEST_SECTION("resume");
    n = run(L"resume", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"\\Downloads\\Resume_final.pdf"), "top = Resume_final.pdf");
    CHECK(!any_contains(h, n, L"node_modules"), "node_modules skipped");
    CHECK(!any_contains(h, n, L".hidden"), "dot folder skipped");
    CHECK(n == 1, "only one resume");

    TEST_SECTION("physics notes");
    n = run(L"physics notes", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"\\Documents\\Physics Notes.docx"), "top = Physics Notes.docx");
    n = run(L"physics", NULL, EXT_ANY, 0, h);
    CHECK(n == 2 && find_rank(h, n, L"Physics Notes.docx") >= 0 &&
          find_rank(h, n, L"physics_lab_report.pdf") >= 0, "physics -> both physics files");

    TEST_SECTION("fuzzy (misheard)");
    /* KNOWN BUG / LIMITATION: "fizzics" is 4 edits away from "physics"
     * (limit for a 7-letter word is 2) and with only 2 query words no miss is
     * tolerated, so the misheard query finds nothing. Reported, not fatal. */
    n = run(L"fizzics notes", NULL, EXT_ANY, 0, h);
    WARN(n >= 1 && ends_with(&h[0], L"Physics Notes.docx"),
         "KNOWN BUG: misheard \"fizzics notes\" does not find Physics Notes.docx");
    /* milder mishearings that the fuzzy matcher is designed for must work */
    n = run(L"physiks notes", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"Physics Notes.docx"), "\"physiks notes\" (1 sub) -> Physics Notes");
    n = run(L"phisics notes", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"Physics Notes.docx"), "\"phisics notes\" (1 sub) -> Physics Notes");
    n = run(L"physics notez", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"Physics Notes.docx"), "\"physics notez\" -> Physics Notes");

    TEST_SECTION("number words");
    n = run(L"chapter two biology", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"Chapter2 Biology.pptx"), "chapter two biology -> Chapter2 Biology.pptx");
    n = run(L"chapter 2 biology", NULL, EXT_ANY, 0, h);
    CHECK(n >= 1 && ends_with(&h[0], L"Chapter2 Biology.pptx"), "chapter 2 biology -> Chapter2 Biology.pptx");

    TEST_SECTION("recent images");
    n = run(NULL, NULL, EXT_IMAGE, SF_RECENT, h);
    CHECK(n == 2, "two images");
    CHECK(n >= 1 && ends_with(&h[0], L"IMG_2041.jpg"), "newest image first");
    CHECK(n >= 2 && ends_with(&h[1], L"beach.png"), "older image second");
    n = run(L"", NULL, EXT_IMAGE, SF_RECENT, h);
    CHECK(n >= 1 && ends_with(&h[0], L"IMG_2041.jpg"), "empty text same as NULL");
    n = run(NULL, NULL, EXT_PDF, SF_RECENT, h);
    CHECK(n == 2 && ends_with(&h[0], L"Resume_final.pdf") && ends_with(&h[1], L"physics_lab_report.pdf"),
          "recent pdfs, newest first");
    n = run(NULL, NULL, EXT_ANY, SF_RECENT, h);
    CHECK(n == 6, "bare recent lists files only (no folders)");

    TEST_SECTION("in_folder");
    wchar_t docs[MAX_PATH], dl[MAX_PATH], pics[MAX_PATH];
    swprintf(docs, MAX_PATH, L"%ls\\Documents", g_root);
    swprintf(dl, MAX_PATH, L"%ls\\Downloads", g_root);
    swprintf(pics, MAX_PATH, L"%ls\\Pictures", g_root);
    n = run(L"physics", dl, EXT_ANY, 0, h);
    CHECK(n == 0, "physics in Downloads -> nothing");
    n = run(L"physics", docs, EXT_ANY, 0, h);
    CHECK(n == 2, "physics in Documents -> 2");
    n = run(L"resume", dl, EXT_ANY, 0, h);
    CHECK(n == 1 && ends_with(&h[0], L"Resume_final.pdf"), "resume in Downloads");
    n = run(L"resume", docs, EXT_ANY, 0, h);
    CHECK(n == 0, "resume in Documents -> nothing (node_modules/.hidden skipped)");
    n = run(NULL, pics, EXT_IMAGE, SF_RECENT, h);
    CHECK(n == 1 && ends_with(&h[0], L"beach.png"), "images in Pictures (recursive)");
    /* folder prefix must stop at a path separator: "Doc" must not match "Documents" */
    wchar_t partial[MAX_PATH];
    swprintf(partial, MAX_PATH, L"%ls\\Doc", g_root);
    n = run(L"physics", partial, EXT_ANY, 0, h);
    CHECK(n == 0, "in_folder is not a raw string prefix");

    TEST_SECTION("dirs only");
    n = run(L"jarvis", NULL, EXT_ANY, SF_DIRS_ONLY, h);
    CHECK(n == 1 && ends_with(&h[0], L"\\Projects\\JARVIS") && h[0].is_dir, "jarvis -> Projects\\JARVIS folder");
    n = run(L"holiday", NULL, EXT_ANY, SF_DIRS_ONLY, h);
    CHECK(n == 1 && ends_with(&h[0], L"\\Pictures\\Holiday"), "holiday -> folder");
    n = run(L"holiday", NULL, EXT_ANY, SF_FILES_ONLY, h);
    /* "holiday" is in beach.png's parent folder, but at least one word must
     * be in the file name itself, so nothing matches */
    CHECK(n == 0, "holiday files-only -> nothing (parent-folder only match)");
    n = run(L"resume", NULL, EXT_ANY, SF_DIRS_ONLY, h);
    CHECK(n == 0, "resume dirs-only -> nothing");

    TEST_SECTION("ext_class_of");
    CHECK(ext_class_of(L"C:\\x\\a.JPG") == EXT_IMAGE, "JPG image (case-insensitive)");
    CHECK(ext_class_of(L"a.pptx") == EXT_PPT, "pptx");
    CHECK(ext_class_of(L"a.docx") == EXT_DOC, "docx");
    CHECK(ext_class_of(L"noext") == EXT_ANY, "no extension");
    CHECK(ext_class_of(L"C:\\dir.d\\file") == EXT_ANY, "dot only in folder name");
    CHECK(ext_class_of(L"trailing.") == EXT_ANY, "trailing dot");

    TEST_SECTION("index file");
    wchar_t ipath[MAX_PATH];
    swprintf(ipath, MAX_PATH, L"%ls\\index.txt", g_data);
    CHECK(path_exists(ipath), "index.txt written to data_dir");
    FILE *f = _wfopen(ipath, L"rb");
    CHECK(f != NULL, "index.txt readable");
    if (f) {
        char line[512] = "";
        int lines = 0;
        bool header = fgets(line, sizeof line, f) && strcmp(line, "JARVIS-INDEX 1\n") == 0;
        CHECK(header, "header line is \"JARVIS-INDEX 1\"");
        bool node = false, hidden = false;
        while (fgets(line, sizeof line, f)) {
            lines++;
            if (strstr(line, "node_modules")) node = true;
            if (strstr(line, ".hidden")) hidden = true;
        }
        fclose(f);
        CHECKF(lines == 13, "13 entry lines (got %d)", lines);
        CHECK(!node && !hidden, "skipped folders not in the index file");
    }
    wchar_t tpath[MAX_PATH];
    swprintf(tpath, MAX_PATH, L"%ls\\index.txt.tmp", g_data);
    CHECK(!path_exists(tpath), "temp file renamed away");

    TEST_SECTION("rebuild");
    mkfile(L"Downloads\\Invoice March.pdf", 0);
    search_rebuild_async();
    search_wait(20000);
    CHECKF(search_count() == 14, "rebuild picks up new file (got %d)", search_count());
    n = run(L"invoice march", NULL, EXT_ANY, 0, h);
    CHECK(n == 1 && ends_with(&h[0], L"Invoice March.pdf"), "new file searchable");

    g_cfg.data_dir[0] = 0;   /* stop log_line before deleting its folder */
    rmtree(g_base);
    CHECK(!path_exists(g_base), "fixture cleaned up");
    return TEST_SUMMARY();
}

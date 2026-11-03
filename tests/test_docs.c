/* Real document jobs (Word, Excel, Python + PyMuPDF) on throwaway files in
 * %TEMP%. Slow and needs Office + PyMuPDF, so it only runs when the
 * environment variable JARVIS_DOCS_TEST=1 is set; otherwise it is skipped. */
#include "../src/jarvis.h"
#include "test.h"
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdlib.h>

static wchar_t dir[MAX_PATH];

static void P(wchar_t *out, const wchar_t *name) { swprintf(out, MAX_PATH, L"%ls\\%ls", dir, name); }

static void write_file(const wchar_t *path, const void *data, DWORD n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD wr;
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, data, n, &wr, NULL); CloseHandle(h); }
}

static void write_bmp(const wchar_t *path)
{
    unsigned char b[54 + 16] = { 'B', 'M' };
    *(DWORD *)(b + 2) = sizeof b; *(DWORD *)(b + 10) = 54; *(DWORD *)(b + 14) = 40;
    *(LONG *)(b + 18) = 2; *(LONG *)(b + 22) = 2; *(WORD *)(b + 26) = 1; *(WORD *)(b + 28) = 24;
    *(DWORD *)(b + 34) = 16;
    for (int i = 54; i < (int)sizeof b; i++) b[i] = (unsigned char)(i * 37);
    write_file(path, b, sizeof b);
}

static int office_pids(DWORD *out, int max)
{
    static const wchar_t *EXE[] = { L"WINWORD.EXE", L"EXCEL.EXE", L"POWERPNT.EXE" };
    int n = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = { .dwSize = sizeof pe };
    if (Process32FirstW(s, &pe))
        do for (int i = 0; i < 3; i++) if (!_wcsicmp(pe.szExeFile, EXE[i]) && n < max) out[n++] = pe.th32ProcessID;
        while (Process32NextW(s, &pe));
    CloseHandle(s);
    return n;
}

static DocResult res;

static bool run(DocOp op, int nsrc, const wchar_t **src, const wchar_t *d0, const wchar_t *d1, int a, int b)
{
    DocTask t;
    memset(&t, 0, sizeof t);
    t.op = op; t.nsrc = nsrc; t.a = a; t.b = b;
    for (int i = 0; i < nsrc; i++) str_copy(t.src[i], MAX_PATH, src[i]);
    if (d0) str_copy(t.dst[0], MAX_PATH, d0);
    if (d1) str_copy(t.dst[1], MAX_PATH, d1);
    DWORD t0 = GetTickCount();
    bool ok = docs_run(&t, &res);
    printf("   op %d -> %s in %lu ms: %ls\n", (int)op, ok ? "ok" : "FAILED", GetTickCount() - t0, res.reply);
    return ok;
}

static int pages(const wchar_t *pdf)
{
    if (!run(DOC_PAGES, 1, &pdf, NULL, NULL, 0, 0)) return -1;
    const wchar_t *h = wcsstr(res.reply, L" has ");
    return h ? _wtoi(h + 5) : -1;
}

int wmain(void)
{
    wchar_t env[8] = L"";
    GetEnvironmentVariableW(L"JARVIS_DOCS_TEST", env, 8);
    if (!str_eq(env, L"1")) { printf("test_docs: skipped (set JARVIS_DOCS_TEST=1 to run)\n"); return 0; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    config_load();

    DWORD before[64];
    int nbefore = office_pids(before, 64);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%lsjarvis_docs_test_%lu", tmp, GetTickCount());
    CreateDirectoryW(dir, NULL);
    printf("dir: %ls\n", dir);

    wchar_t txt[MAX_PATH], pdf1[MAX_PATH], six[MAX_PATH], docx[MAX_PATH], back[MAX_PATH], csv[MAX_PATH],
            csvpdf[MAX_PATH], bmp[MAX_PATH], bmppdf[MAX_PATH], p1[MAX_PATH], p2[MAX_PATH], ex[MAX_PATH],
            sub[MAX_PATH], q[MAX_PATH];
    P(txt, L"notes \x00e9t\x00e9.txt"); P(pdf1, L"notes \x00e9t\x00e9.pdf"); P(six, L"six pages.pdf");
    P(docx, L"six pages.docx"); P(back, L"six pages back.pdf"); P(csv, L"table.csv"); P(csvpdf, L"table.pdf");
    P(bmp, L"pic.bmp"); P(bmppdf, L"pic.pdf"); P(p1, L"six pages part 1.pdf"); P(p2, L"six pages part 2.pdf");
    P(ex, L"six pages pages 2-4.pdf"); P(sub, L"sub");
    const char text[] = "Hello from the JARVIS document test.\r\nSecond line.\r\n";
    write_file(txt, text, sizeof text - 1);
    const char table[] = "name,score\r\nann,1\r\nbob,2\r\n";
    write_file(csv, table, sizeof table - 1);
    write_bmp(bmp);
    CreateDirectoryW(sub, NULL);

    TEST_SECTION("txt -> pdf (Word)");
    CHECK(run(DOC_TO_PDF, 1, (const wchar_t *[]){ txt }, pdf1, NULL, 0, 0) && path_exists(pdf1), "txt converted");
    CHECK(pages(pdf1) == 1, "1 page");

    TEST_SECTION("merge x6");
    const wchar_t *srcs[6] = { pdf1, pdf1, pdf1, pdf1, pdf1, pdf1 };
    CHECK(run(DOC_MERGE, 6, srcs, six, NULL, 0, 0) && path_exists(six), "merged");
    CHECK(pages(six) == 6, "6 pages");

    TEST_SECTION("split at 3");
    CHECK(run(DOC_SPLIT, 1, (const wchar_t *[]){ six }, p1, p2, 3, 0), "split ran");
    CHECK(pages(p1) == 3 && pages(p2) == 3, "3 + 3 pages");
    CHECK(!run(DOC_SPLIT, 1, (const wchar_t *[]){ six }, p1, p2, 6, 0), "split at last page refused");
    CHECK(wcsstr(res.reply, L"only has 6") != NULL, "range message");

    TEST_SECTION("split again: no overwrite");
    CHECK(run(DOC_SPLIT, 1, (const wchar_t *[]){ six }, p1, p2, 2, 0), "split ran");
    P(q, L"six pages part 1 (2).pdf");
    CHECK(path_exists(q) && pages(q) == 2, "part 1 (2) has 2 pages");
    CHECK(pages(p1) == 3, "original part 1 untouched");

    TEST_SECTION("extract 2-4");
    CHECK(run(DOC_EXTRACT, 1, (const wchar_t *[]){ six }, ex, NULL, 2, 4), "extract ran");
    CHECK(pages(ex) == 3, "3 pages");

    TEST_SECTION("pdf -> docx -> pdf (Word)");
    CHECK(run(DOC_TO_DOCX, 1, (const wchar_t *[]){ six }, docx, NULL, 0, 0) && path_exists(docx), "pdf to docx");
    CHECK(run(DOC_TO_PDF, 1, (const wchar_t *[]){ docx }, back, NULL, 0, 0) && path_exists(back), "docx to pdf");
    CHECK(pages(back) >= 1, "has pages");

    TEST_SECTION("csv -> pdf (Excel)");
    CHECK(run(DOC_TO_PDF, 1, (const wchar_t *[]){ csv }, csvpdf, NULL, 0, 0) && path_exists(csvpdf), "csv converted");

    TEST_SECTION("bmp -> pdf (PyMuPDF)");
    CHECK(run(DOC_TO_PDF, 1, (const wchar_t *[]){ bmp }, bmppdf, NULL, 0, 0) && pages(bmppdf) == 1, "image converted");

    TEST_SECTION("rename / copy / move");
    wchar_t ren[MAX_PATH], cp[MAX_PATH], mv[MAX_PATH];
    P(ren, L"renamed.pdf");
    CHECK(run(DOC_RENAME, 1, (const wchar_t *[]){ ex }, ren, NULL, 0, 0) && path_exists(ren) && !path_exists(ex), "renamed");
    swprintf(cp, MAX_PATH, L"%ls\\renamed.pdf", sub);
    CHECK(run(DOC_COPY, 1, (const wchar_t *[]){ ren }, cp, NULL, 0, 0) && path_exists(cp) && path_exists(ren), "copied");
    CHECK(run(DOC_COPY, 1, (const wchar_t *[]){ ren }, cp, NULL, 0, 0), "copied again");
    swprintf(q, MAX_PATH, L"%ls\\renamed (2).pdf", sub);
    CHECK(path_exists(q), "second copy got (2)");
    swprintf(mv, MAX_PATH, L"%ls\\pic.bmp", sub);
    CHECK(run(DOC_MOVE, 1, (const wchar_t *[]){ bmp }, mv, NULL, 0, 0) && path_exists(mv) && !path_exists(bmp), "moved");

    TEST_SECTION("delete -> Recycle Bin, protected folders");
    CHECK(run(DOC_DELETE, 1, (const wchar_t *[]){ q }, NULL, NULL, 0, 0) && !path_exists(q), "deleted to bin");
    CHECK(docs_protected(L"C:\\") != NULL, "drive root protected");
    const wchar_t *dl = kv_get(&g_cfg.folders, L"downloads");
    CHECK(dl && docs_protected(dl) != NULL, "downloads protected");
    CHECK(docs_protected(sub) == NULL, "temp subfolder not protected");

    TEST_SECTION("async");
    DocTask t;
    memset(&t, 0, sizeof t);
    t.op = DOC_PAGES; t.nsrc = 1; str_copy(t.src[0], MAX_PATH, six);
    CHECK(docs_run_async(&t, NULL), "started");
    CHECK(!docs_run_async(&t, NULL) || !docs_busy(), "second refused while busy");
    docs_wait(60000);
    CHECK(!docs_busy(), "finished");

    TEST_SECTION("no Office left running");
    Sleep(1500);
    DWORD after[64];
    int nafter = office_pids(after, 64), leaked = 0;
    for (int i = 0; i < nafter; i++) {
        bool old = false;
        for (int j = 0; j < nbefore; j++) if (after[i] == before[j]) old = true;
        if (!old) { leaked++; printf("   leaked office pid %lu\n", after[i]); }
    }
    CHECK(leaked == 0, "no Office process we started is left");

    /* clean up our temp dir (permanently: it only holds files made here) */
    SHFILEOPSTRUCTW op = { 0 };
    wchar_t from[MAX_PATH + 2] = L"";
    str_copy(from, MAX_PATH, dir); from[wcslen(from) + 1] = 0;
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
    CoUninitialize();
    return TEST_SUMMARY();
}

/* Document commands in dry-run (nothing is executed): parsing, "this" =
 * Explorer selection, delete confirmation, output names. Uses made-up
 * selected paths in %TEMP% that are created (empty) and removed here. */
#include "../src/jarvis.h"
#include "test.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdlib.h>

static CmdResult r;
static wchar_t dir[MAX_PATH];

static const wchar_t *run(const wchar_t *text)
{
    memset(&r, 0, sizeof r);
    cmd_run(text, &r);
    printf("   %-55ls -> %ls\n", text, cmd_last_action());
    return cmd_last_action();
}

static bool has(const wchar_t *s, const wchar_t *sub) { return wcsstr(s, sub) != NULL; }

static void touch(const wchar_t *name, wchar_t *out)
{
    swprintf(out, MAX_PATH, L"%ls\\%ls", dir, name);
    HANDLE h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void select_files(int n, wchar_t (*p)[MAX_PATH]) { cmd_set_selection((const wchar_t (*)[MAX_PATH])p, n); }

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    config_load();
    cmd_init(NULL);
    cmd_set_dry_run(true);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%lsjarvis_cmd_test_%lu", tmp, GetTickCount());
    CreateDirectoryW(dir, NULL);
    wchar_t sel[3][MAX_PATH], want[MAX_PATH];
    touch(L"report.docx", sel[0]);
    touch(L"notes.pdf", sel[1]);
    touch(L"homework.pdf", sel[2]);
    const wchar_t *dl = kv_get(&g_cfg.folders, L"downloads");

    TEST_SECTION("convert");
    select_files(1, sel);
    swprintf(want, MAX_PATH, L"DOC convert-pdf %ls -> %ls\\report.pdf", sel[0], dir);
    CHECK(str_eq(run(L"convert this to pdf"), want), "convert this to pdf");
    swprintf(want, MAX_PATH, L"-> %ls\\Final Report.pdf", dir);
    CHECK(has(run(L"Convert this doc to PDF named Final Report"), want), "named X keeps typed casing");
    swprintf(want, MAX_PATH, L"-> %ls\\cv.pdf", dir);
    CHECK(has(run(L"save this as pdf as cv"), want), "as X");
    CHECK(has(run(L"make this a pdf and save it in downloads"), dl), "save it in downloads");
    CHECK(has(run(L"convert it"), L"DOC convert-pdf"), "convert with no target: docx -> pdf");
    CHECK(!r.close || cmd_last_action()[0], "replied");

    TEST_SECTION("pdf jobs on the selection");
    select_files(2, sel + 1);
    swprintf(want, MAX_PATH, L"DOC merge %ls + %ls -> %ls\\notes merged.pdf", sel[1], sel[2], dir);
    CHECK(str_eq(run(L"merge these"), want), "merge these = selected PDFs");
    CHECK(has(run(L"merge these and call it combined"), L"\\combined.pdf"), "merge + call it");
    CHECK(has(run(L"split this up to page five"), L"@5 ->"), "up to page five");
    CHECK(has(run(L"split this into two parts at page 3"), L"notes part 1.pdf | "), "part names");
    CHECK(has(run(L"split this before page 3"), L"@2 ->"), "before page 3 = after 2");
    CHECK(has(run(L"split this at page 4 and name it chapter"), L"\\chapter part 2.pdf"), "named parts");
    CHECK(has(run(L"extract pages three to seven from this"), L" 3-7 -> "), "pages three to seven");
    CHECK(has(run(L"take pages 2-4 of this pdf"), L" 2-4 -> "), "pages 2-4");
    CHECK(has(run(L"extract page 2 from this"), L"notes page 2.pdf"), "single page name");
    CHECK(has(run(L"convert this to word"), L"DOC convert-docx"), "pdf to word");
    CHECK(has(run(L"how many pages does this have"), L"DOC pages"), "page count");
    CHECK(str_eq(run(L"split this"), L"SAY") && r.listen_again, "split without a page asks");

    TEST_SECTION("files");
    select_files(1, sel);
    swprintf(want, MAX_PATH, L"FILE rename %ls -> %ls\\Final Summary.docx", sel[0], dir);
    CHECK(str_eq(run(L"rename this to Final Summary"), want), "rename keeps extension");
    CHECK(has(run(L"copy this to downloads"), L"FILE copy"), "copy");
    CHECK(has(run(L"move this into documents"), L"FILE move"), "move");
    CHECK(str_eq(run(L"move this to"), L"") || str_eq(cmd_last_action(), L"SAY"), "move with no folder asks");

    TEST_SECTION("delete needs a yes");
    swprintf(want, MAX_PATH, L"ASK delete %ls", sel[0]);
    CHECK(str_eq(run(L"delete this"), want) && r.listen_again, "asks first");
    swprintf(want, MAX_PATH, L"FILE delete %ls", sel[0]);
    CHECK(str_eq(run(L"yes"), want), "yes deletes");
    run(L"delete this");
    CHECK(str_eq(run(L"no"), L"SAY"), "no keeps it");
    CHECK(str_eq(run(L"yes"), L"SAY") || !has(cmd_last_action(), L"FILE delete"), "late yes does nothing");
    run(L"remove this");
    CHECK(has(run(L"open downloads"), L"OPEN "), "another command cancels and runs");
    CHECK(!has(run(L"yes"), L"FILE delete"), "...and the question is gone");
    CHECK(has(run(L"move this to the recycle bin"), L"ASK delete"), "move to recycle bin = delete");
    run(L"cancel");
    wchar_t prot[1][MAX_PATH];
    str_copy(prot[0], MAX_PATH, dl ? dl : L"C:\\");
    select_files(1, prot);
    CHECK(str_eq(run(L"delete this"), L"SAY"), "won't delete Downloads itself");

    TEST_SECTION("open / show this");
    select_files(1, sel + 1);
    swprintf(want, MAX_PATH, L"OPEN %ls", sel[1]);
    CHECK(str_eq(run(L"open this"), want), "open this");
    swprintf(want, MAX_PATH, L"REVEAL %ls", sel[1]);
    CHECK(str_eq(run(L"show this in folder"), want), "show this in folder");

    TEST_SECTION("nothing selected");
    select_files(0, NULL);
    cmd_init(NULL);
    run(L"open this");      /* the last file is still notes.pdf from above */
    CHECK(has(cmd_last_action(), L"notes.pdf"), "falls back to the last file");

    TEST_SECTION("not document commands");
    CHECK(has(run(L"get directions to paris"), L"URL "), "get directions");
    CHECK(has(run(L"open downloads"), L"OPEN "), "open downloads");
    CHECK(has(run(L"turn up the volume"), L"SYS "), "turn up the volume");

    SHFILEOPSTRUCTW op = { 0 };
    wchar_t from[MAX_PATH + 2] = L"";
    str_copy(from, MAX_PATH, dir); from[wcslen(from) + 1] = 0;
    op.wFunc = FO_DELETE; op.pFrom = from; op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
    return TEST_SUMMARY();
}

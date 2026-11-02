/* JARVIS entry point.
 *
 *   jarvis.exe                    open the overlay and start listening
 *   jarvis.exe --type             open the overlay without listening
 *   jarvis.exe "open downloads"   open the overlay and run that command
 *   jarvis.exe --reindex          rebuild the file index and exit
 *   jarvis.exe --search <words>   print matching files to the console (testing)
 *   jarvis.exe --dry-run <words>  print what a command WOULD do (testing)
 *
 * Launching it again while it's open toggles the microphone instead of
 * opening a second window (that's what the Copilot key does). */
#include "jarvis.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

static void console_out(const wchar_t *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    char *u = utf8_from_wide(s);
    if (!u) return;
    DWORD wr;
    WriteFile(h, u, (DWORD)strlen(u), &wr, NULL);
    free(u);
}

static void join_args(int argc, wchar_t **argv, int from, wchar_t *out, size_t cap)
{
    out[0] = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) str_cat(out, cap, L" ");
        str_cat(out, cap, argv[i]);
    }
}

static int run_console(const wchar_t *mode, const wchar_t *text)
{
    /* GUI app: borrow the parent console if started from a terminal */
    AttachConsole(ATTACH_PARENT_PROCESS);
    search_init();
    search_wait(120000);
    wchar_t line[MAX_PATH + 64];

    if (str_eq(mode, L"--reindex")) {
        search_rebuild_async();
        search_wait(600000);
        swprintf(line, MAX_PATH + 64, L"indexed %d files and folders\n", search_count());
        console_out(line);
        return 0;
    }
    if (str_eq(mode, L"--search")) {
        Hit hits[MAX_HITS];
        SearchQuery q = { .text = text };
        int n = search_files(&q, hits, MAX_HITS);
        for (int i = 0; i < n; i++) {
            swprintf(line, MAX_PATH + 64, L"%3d  %ls\n", hits[i].score, hits[i].path);
            console_out(line);
        }
        if (!n) console_out(L"no matches\n");
        return 0;
    }
    /* --dry-run */
    cmd_set_dry_run(true);
    CmdResult r;
    memset(&r, 0, sizeof r);
    cmd_run(text, &r);
    swprintf(line, MAX_PATH + 64, L"action: %ls\nreply:  %ls\n", cmd_last_action(), r.reply);
    console_out(line);
    for (int i = 0; i < r.nrows; i++) {
        const Row *row = &r.rows[i];
        swprintf(line, MAX_PATH + 64, L"  %d. %ls\n", i + 1, row->kind == ROW_HIT ? row->hit.path : row->title);
        console_out(line);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *mode = argc > 1 ? argv[1] : L"";
    bool console = str_eq(mode, L"--reindex") || str_eq(mode, L"--search") || str_eq(mode, L"--dry-run");

    /* remember what had focus BEFORE we show anything (Upload dialog?) */
    HWND fg = GetForegroundWindow();

    if (!console) {
        HANDLE mtx = CreateMutexW(NULL, TRUE, APP_MUTEX);
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND w = FindWindowW(APP_CLASS, NULL);
            if (w) {
                DWORD pid = 0;
                GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                PostMessageW(w, WM_APP_TOGGLE, 0, 0);
            }
            return 0;
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    config_load();

    if (console) {
        wchar_t text[1024];
        join_args(argc, argv, 2, text, 1024);
        cmd_init(NULL);
        int rc = run_console(mode, text);
        LocalFree(argv);
        return rc;
    }

    HWND dlg = dialog_detect(fg);
    bool listen = g_cfg.listen_on_start && !str_eq(mode, L"--type");
    wchar_t initial[1024] = L"";
    if (argc > 1 && mode[0] != L'-') join_args(argc, argv, 1, initial, 1024);
    if (initial[0]) listen = false;

    search_init();
    cmd_init(dlg);
    log_line(L"start%ls%ls", dlg ? L" (file dialog detected)" : L"", initial[0] ? L" with command" : L"");

    if (!ui_create(inst, dlg, listen, initial[0] ? initial : NULL)) return 1;
    int rc = ui_loop();
    LocalFree(argv);
    return rc;
}

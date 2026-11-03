/* Everyday document jobs: convert to / from PDF, split, extract, merge,
 * count pages, rename, move, copy, delete (to the Recycle Bin).
 *
 *  - Word / PowerPoint / Excel conversions use Office through late-bound COM
 *    (IDispatch). We always start our own hidden instance and quit only that
 *    one; a watchdog kills it if Office hangs on an invisible dialog.
 *  - PDF page work runs Python + PyMuPDF as a hidden child process. Paths and
 *    numbers go in argv, never into the script text.
 *  - Outputs never overwrite anything: "a.pdf" -> "a (2).pdf". */
#define COBJMACROS
#include "jarvis.h"
#include <ole2.h>
#include <oleauto.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <servprov.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

#define PY_TIMEOUT_MS      120000
#define OFFICE_TIMEOUT_MS  180000

/* ------------------------------------------------------------ small helpers */
static const wchar_t *ext_of(const wchar_t *p)
{
    const wchar_t *n = path_name(p), *d = wcsrchr(n, L'.');
    return d && d != n ? d + 1 : L"";
}

static bool ext_in(const wchar_t *p, const wchar_t *const *list)
{
    const wchar_t *e = ext_of(p);
    for (; *list; list++) if (_wcsicmp(e, *list) == 0) return true;
    return false;
}

static const wchar_t *const WORD_EXT[]  = { L"doc", L"docx", L"docm", L"dotx", L"rtf", L"odt", L"txt", NULL };
static const wchar_t *const PPT_EXT[]   = { L"ppt", L"pptx", L"pptm", L"pps", L"ppsx", L"odp", NULL };
static const wchar_t *const SHEET_EXT[] = { L"xls", L"xlsx", L"xlsm", L"csv", L"ods", NULL };
static const wchar_t *const IMG_EXT[]   = { L"jpg", L"jpeg", L"png", L"bmp", L"gif", L"tif", L"tiff", L"webp", NULL };

/* display name: file name only, capped */
static void nm(const wchar_t *path, wchar_t *out)
{
    str_copy(out, 120, path_name(path));
    size_t n = wcslen(out);
    if (n > 3 && out[n - 1] == L'\\') out[n - 1] = 0;
}

static void say(DocResult *r, const wchar_t *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vswprintf(r->reply, 300, fmt, ap);
    va_end(ap);
    r->reply[299] = 0;
}

static void add_out(DocResult *r, const wchar_t *p)
{
    if (r->nout < 2) str_copy(r->out[r->nout++], MAX_PATH, p);
}

void docs_unique(const wchar_t *want, wchar_t *out)
{
    str_copy(out, MAX_PATH, want);
    if (!path_exists(want)) return;
    wchar_t base[MAX_PATH];
    str_copy(base, MAX_PATH, want);
    const wchar_t *name = path_name(base);
    wchar_t *dot = wcsrchr(name, L'.');
    wchar_t ext[64] = L"";
    if (dot && dot != name && wcslen(dot) < 64 && !path_is_dir(want)) { str_copy(ext, 64, dot); *dot = 0; }
    for (int i = 2; i < 10000; i++) {
        swprintf(out, MAX_PATH, L"%ls (%d)%ls", base, i, ext);
        if (!path_exists(out)) return;
    }
}

/* ------------------------------------------------------- protected folders */
static bool same_path(const wchar_t *a, const wchar_t *b)
{
    wchar_t x[MAX_PATH], y[MAX_PATH];
    str_copy(x, MAX_PATH, a); str_copy(y, MAX_PATH, b);
    size_t n;
    while ((n = wcslen(x)) > 3 && (x[n - 1] == L'\\' || x[n - 1] == L'/')) x[n - 1] = 0;
    while ((n = wcslen(y)) > 3 && (y[n - 1] == L'\\' || y[n - 1] == L'/')) y[n - 1] = 0;
    return _wcsicmp(x, y) == 0;
}

/* is a an ancestor of (or equal to) b? */
static bool is_parent_of(const wchar_t *a, const wchar_t *b)
{
    size_t n = wcslen(a);
    while (n > 3 && (a[n - 1] == L'\\')) n--;
    return _wcsnicmp(a, b, n) == 0 && (b[n] == 0 || b[n] == L'\\');
}

const wchar_t *docs_protected(const wchar_t *path)
{
    if (!path || !path[0]) return L"nothing to work on";
    if (wcslen(path) <= 3 || PathIsRootW(path)) return L"that's a whole drive";
    wchar_t p[MAX_PATH];
    expand_env(L"%USERPROFILE%", p, MAX_PATH);
    if (is_parent_of(path, p)) return L"that's your user folder";
    expand_env(L"%SystemRoot%", p, MAX_PATH);
    if (is_parent_of(p, path) || same_path(p, path)) return L"that's part of Windows";
    expand_env(L"%ProgramFiles%", p, MAX_PATH);
    if (same_path(p, path)) return L"that's the Program Files folder";
    expand_env(L"%ProgramFiles(x86)%", p, MAX_PATH);
    if (same_path(p, path)) return L"that's the Program Files folder";
    for (int i = 0; i < g_cfg.folders.count; i++)
        if (same_path(g_cfg.folders.items[i].val, path)) return L"that's one of your main folders";
    for (int i = 0; i < g_cfg.nroots; i++)
        if (same_path(g_cfg.roots[i], path)) return L"that's one of your main folders";
    return NULL;
}

/* ------------------------------------------------------- python + PyMuPDF */
static const char PY_SCRIPT[] =
"import sys\n"
"try:\n"
"    import fitz\n"
"except Exception:\n"
"    print('ERR nofitz'); sys.exit(3)\n"
"a = sys.argv[1:]\n"
"m = a[0]\n"
"def src(p):\n"
"    d = fitz.open(p)\n"
"    if d.needs_pass:\n"
"        print('ERR locked'); sys.exit(2)\n"
"    if not d.is_pdf:\n"
"        print('ERR notpdf'); sys.exit(2)\n"
"    return d\n"
"def part(s, f, t, o):\n"
"    d = fitz.open(); d.insert_pdf(s, from_page=f, to_page=t); d.save(o, garbage=3, deflate=True); d.close()\n"
"try:\n"
"    if m == 'check':\n"
"        print('OK 1')\n"
"    elif m == 'count':\n"
"        print('OK', src(a[1]).page_count)\n"
"    elif m == 'split':\n"
"        s = src(a[1]); n = int(a[2]); c = s.page_count\n"
"        if n < 1 or n >= c:\n"
"            print('ERR range', c); sys.exit(2)\n"
"        part(s, 0, n - 1, a[3]); part(s, n, c - 1, a[4]); print('OK', c)\n"
"    elif m == 'extract':\n"
"        s = src(a[1]); x = int(a[2]); y = int(a[3]); c = s.page_count\n"
"        if x < 1 or y < x or y > c:\n"
"            print('ERR range', c); sys.exit(2)\n"
"        part(s, x - 1, y - 1, a[4]); print('OK', y - x + 1)\n"
"    elif m == 'merge':\n"
"        d = fitz.open()\n"
"        for p in a[2:]:\n"
"            d.insert_pdf(src(p))\n"
"        d.save(a[1], garbage=3, deflate=True); print('OK', d.page_count)\n"
"    elif m == 'img':\n"
"        s = fitz.open(a[1]); d = fitz.open('pdf', s.convert_to_pdf()); d.save(a[2]); print('OK', d.page_count)\n"
"    else:\n"
"        print('ERR mode'); sys.exit(2)\n"
"except SystemExit:\n"
"    raise\n"
"except Exception as e:\n"
"    print('ERR fail', type(e).__name__); sys.exit(1)\n";

/* Appends arg to cmd quoted the way the C runtime splits argv. */
static void argv_quote(wchar_t *cmd, size_t cap, const wchar_t *arg)
{
    size_t n = wcslen(cmd);
    if (n && n + 1 < cap) cmd[n++] = L' ';
    if (n + 1 < cap) cmd[n++] = L'"';
    for (const wchar_t *p = arg;; p++) {
        int bs = 0;
        while (*p == L'\\') { bs++; p++; }
        int times = !*p ? bs * 2 : *p == L'"' ? bs * 2 + 1 : bs;
        for (int i = 0; i < times && n + 1 < cap; i++) cmd[n++] = L'\\';
        if (!*p) break;
        if (n + 1 < cap) cmd[n++] = *p;
    }
    if (n + 1 < cap) cmd[n++] = L'"';
    cmd[n < cap ? n : cap - 1] = 0;
}

/* Runs python -I -c SCRIPT args...; stdout+stderr into out. Returns exit
 * code, -1 if it could not start, -2 on timeout. */
static int run_python(const wchar_t *py, const wchar_t *const *args, int nargs, char *out, size_t cap)
{
    out[0] = 0;
    size_t clen = 8192 + (size_t)nargs * (MAX_PATH * 2 + 8);
    wchar_t *cmd = malloc(clen * sizeof(wchar_t));
    wchar_t *script = wide_from_utf8(PY_SCRIPT, -1);
    if (!cmd || !script) { free(cmd); free(script); return -1; }
    cmd[0] = 0;
    argv_quote(cmd, clen, py);
    str_cat(cmd, clen, L" -I -c");
    argv_quote(cmd, clen, script);
    for (int i = 0; i < nargs; i++) argv_quote(cmd, clen, args[i]);
    free(script);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { free(cmd); return -1; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si = { .cb = sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmd);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return -1; }

    size_t n = 0;
    DWORD start = GetTickCount();
    int rc = -2;
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
            char buf[512];
            DWORD got = 0;
            if (!ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL) || !got) break;
            for (DWORD i = 0; i < got && n + 1 < cap; i++) out[n++] = buf[i];
            out[n] = 0;
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 50);
        if (w == WAIT_OBJECT_0) {
            /* drain what is left */
            DWORD got;
            char buf[512];
            while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail &&
                   ReadFile(rd, buf, avail < sizeof buf ? avail : sizeof buf, &got, NULL) && got)
                for (DWORD i = 0; i < got && n + 1 < cap; i++) out[n++] = buf[i];
            out[n] = 0;
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            rc = (int)code;
            break;
        }
        if (GetTickCount() - start > PY_TIMEOUT_MS) {
            TerminateProcess(pi.hProcess, 99);
            WaitForSingleObject(pi.hProcess, 2000);
            rc = -2;
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return rc;
}

static int     s_py_state;            /* 0 unknown, 1 ok, -1 none */
static wchar_t s_py[MAX_PATH];

static bool py_works(const wchar_t *py)
{
    if (!py[0] || !path_exists(py)) return false;
    char out[256];
    const wchar_t *args[] = { L"check" };
    int rc = run_python(py, args, 1, out, sizeof out);
    bool ok = rc == 0 && strstr(out, "OK") != NULL;
    log_line(L"docs: python %ls %ls", py, ok ? L"has PyMuPDF" : L"unusable");
    return ok;
}

/* python.exe with PyMuPDF, found once per run */
static const wchar_t *find_python(void)
{
    if (s_py_state) return s_py_state > 0 ? s_py : NULL;
    s_py_state = -1;
    wchar_t c[MAX_PATH];
    if (g_cfg.docs_python[0]) {
        if (py_works(g_cfg.docs_python)) { str_copy(s_py, MAX_PATH, g_cfg.docs_python); s_py_state = 1; }
        return s_py_state > 0 ? s_py : NULL;
    }
    static const wchar_t *const FIXED[] = {
        L"%USERPROFILE%\\anaconda3\\python.exe", L"%USERPROFILE%\\miniconda3\\python.exe",
        L"%ProgramData%\\anaconda3\\python.exe", L"%ProgramData%\\miniconda3\\python.exe", NULL };
    for (const wchar_t *const *f = FIXED; *f; f++) {
        expand_env(*f, c, MAX_PATH);
        if (py_works(c)) { str_copy(s_py, MAX_PATH, c); s_py_state = 1; return s_py; }
    }
    /* %LOCALAPPDATA%\Programs\Python\Python3*\python.exe, newest first */
    wchar_t dir[MAX_PATH], pat[MAX_PATH], best[8][MAX_PATH];
    int nb = 0;
    expand_env(L"%LOCALAPPDATA%\\Programs\\Python", dir, MAX_PATH);
    swprintf(pat, MAX_PATH, L"%ls\\Python3*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nb < 8)
                swprintf(best[nb++], MAX_PATH, L"%ls\\%ls\\python.exe", dir, fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = nb - 1; i >= 0; i--)
        if (py_works(best[i])) { str_copy(s_py, MAX_PATH, best[i]); s_py_state = 1; return s_py; }
    /* PATH (skip the Microsoft Store stub) */
    if (SearchPathW(NULL, L"python.exe", NULL, MAX_PATH, c, NULL) && !StrStrIW(c, L"WindowsApps") && py_works(c)) {
        str_copy(s_py, MAX_PATH, c);
        s_py_state = 1;
        return s_py;
    }
    return NULL;
}

/* runs one PyMuPDF job; on success returns the number after "OK" */
static bool py_job(const wchar_t *const *args, int nargs, int *num, DocResult *r, const wchar_t *what)
{
    const wchar_t *py = find_python();
    if (!py) { say(r, L"PDF splitting needs Python with PyMuPDF (pip install pymupdf)."); return false; }
    char out[2048];
    int rc = run_python(py, args, nargs, out, sizeof out);
    char *ok = strstr(out, "OK ");
    if (rc == 0 && ok) { if (num) *num = atoi(ok + 3); return true; }
    wchar_t n0[128];
    nm(args[1], n0);
    char *err = strstr(out, "ERR ");
    if (rc == -2) say(r, L"%ls took too long, I stopped it.", what);
    else if (err && !strncmp(err, "ERR range", 9)) say(r, L"%ls only has %d pages.", n0, atoi(err + 10));
    else if (err && !strncmp(err, "ERR locked", 10)) say(r, L"%ls is password protected.", n0);
    else if (err && !strncmp(err, "ERR notpdf", 10)) say(r, L"%ls isn't a PDF.", n0);
    else if (err && !strncmp(err, "ERR nofitz", 10)) say(r, L"PDF splitting needs Python with PyMuPDF (pip install pymupdf).");
    else say(r, L"I couldn't %ls %ls.", what, n0);
    log_line(L"docs: python job failed rc=%d", rc);
    return false;
}

/* -------------------------------------------------------- late-bound COM */
static VARIANT v_str(const wchar_t *s) { VARIANT v; VariantInit(&v); v.vt = VT_BSTR; v.bstrVal = SysAllocString(s); return v; }
static VARIANT v_int(long i)           { VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = i; return v; }
static VARIANT v_bool(bool b)          { VARIANT v; VariantInit(&v); v.vt = VT_BOOL; v.boolVal = b ? VARIANT_TRUE : VARIANT_FALSE; return v; }
static VARIANT v_miss(void)            { VARIANT v; VariantInit(&v); v.vt = VT_ERROR; v.scode = DISP_E_PARAMNOTFOUND; return v; }

static bool retry_hr(HRESULT hr) { return hr == RPC_E_CALL_REJECTED || hr == RPC_E_SERVERCALL_RETRYLATER; }

/* d.name(args...) — args in natural (left to right) order */
static HRESULT disp_call(IDispatch *d, WORD kind, const wchar_t *name, VARIANT *ret, int n, VARIANT *args)
{
    if (!d) return E_POINTER;
    DISPID id;
    LPOLESTR on = (LPOLESTR)name;
    HRESULT hr;
    for (int t = 0; retry_hr(hr = IDispatch_GetIDsOfNames(d, &IID_NULL, &on, 1, LOCALE_USER_DEFAULT, &id)) && t < 100; t++)
        Sleep(200);
    if (FAILED(hr)) { log_line(L"docs: no member %ls (0x%08lx)", name, (unsigned long)hr); return hr; }
    VARIANT rev[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) rev[i] = args[n - 1 - i];
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS dp = { n ? rev : NULL, NULL, (UINT)n, 0 };
    if (kind & DISPATCH_PROPERTYPUT) { dp.cNamedArgs = 1; dp.rgdispidNamedArgs = &put; }
    VARIANT tmp;
    VariantInit(&tmp);
    if (!ret) ret = &tmp; else VariantInit(ret);
    EXCEPINFO ei;
    UINT argerr = 0;
    for (int t = 0;; t++) {
        memset(&ei, 0, sizeof ei);
        hr = IDispatch_Invoke(d, id, &IID_NULL, LOCALE_USER_DEFAULT, kind, &dp, ret, &ei, &argerr);
        if (!retry_hr(hr) || t >= 100) break;
        Sleep(200);
    }
    if (hr == DISP_E_EXCEPTION) {
        log_line(L"docs: %ls failed: %ls", name, ei.bstrDescription ? ei.bstrDescription : L"?");
        SysFreeString(ei.bstrSource); SysFreeString(ei.bstrDescription); SysFreeString(ei.bstrHelpFile);
    } else if (FAILED(hr)) log_line(L"docs: %ls failed (0x%08lx)", name, (unsigned long)hr);
    VariantClear(&tmp);
    return hr;
}

static HRESULT disp_put(IDispatch *d, const wchar_t *name, VARIANT v)
{
    HRESULT hr = disp_call(d, DISPATCH_PROPERTYPUT, name, NULL, 1, &v);
    VariantClear(&v);
    return hr;
}

static IDispatch *disp_get(IDispatch *d, const wchar_t *name)
{
    VARIANT r;
    if (FAILED(disp_call(d, DISPATCH_PROPERTYGET, name, &r, 0, NULL))) return NULL;
    if (r.vt == VT_DISPATCH && r.pdispVal) return r.pdispVal;
    VariantClear(&r);
    return NULL;
}

static IDispatch *disp_method_obj(IDispatch *d, const wchar_t *name, int n, VARIANT *args)
{
    VARIANT r;
    HRESULT hr = disp_call(d, DISPATCH_METHOD | DISPATCH_PROPERTYGET, name, &r, n, args);
    if (FAILED(hr)) return NULL;
    if (r.vt == VT_DISPATCH && r.pdispVal) return r.pdispVal;
    VariantClear(&r);
    return NULL;
}

static HRESULT disp_method(IDispatch *d, const wchar_t *name, int n, VARIANT *args)
{
    return disp_call(d, DISPATCH_METHOD, name, NULL, n, args);
}

static void clear_args(VARIANT *a, int n) { for (int i = 0; i < n; i++) VariantClear(&a[i]); }
static void rel(IDispatch **d) { if (*d) { IDispatch_Release(*d); *d = NULL; } }

/* ------------------------------------------------------ Office instances */
static int pids_of(const wchar_t *exe, DWORD *out, int max)
{
    int n = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { .dwSize = sizeof pe };
    if (Process32FirstW(s, &pe))
        do { if (_wcsicmp(pe.szExeFile, exe) == 0 && n < max) out[n++] = pe.th32ProcessID; } while (Process32NextW(s, &pe));
    CloseHandle(s);
    return n;
}

typedef struct {
    IDispatch *app;
    HANDLE proc;      /* the Office process we started (NULL: reused the user's) */
    HANDLE stop;      /* watchdog: signalled when we're done */
    HANDLE dog;
} Office;

static DWORD WINAPI watchdog(LPVOID p)
{
    Office *o = p;
    if (WaitForSingleObject(o->stop, OFFICE_TIMEOUT_MS) == WAIT_TIMEOUT && o->proc) {
        log_line(L"docs: Office hung, ending it");
        TerminateProcess(o->proc, 1);
    }
    return 0;
}

static bool office_start(Office *o, const wchar_t *progid, const wchar_t *exe)
{
    memset(o, 0, sizeof *o);
    CLSID cls;
    if (FAILED(CLSIDFromProgID(progid, &cls))) return false;
    DWORD before[64], after[64];
    int nb = pids_of(exe, before, 64);
    HRESULT hr = CoCreateInstance(&cls, NULL, CLSCTX_LOCAL_SERVER, &IID_IDispatch, (void **)&o->app);
    if (FAILED(hr) || !o->app) { log_line(L"docs: can't start %ls (0x%08lx)", progid, (unsigned long)hr); o->app = NULL; return false; }
    int na = pids_of(exe, after, 64);
    for (int i = 0; i < na && !o->proc; i++) {
        bool old = false;
        for (int j = 0; j < nb; j++) if (after[i] == before[j]) old = true;
        if (!old) o->proc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, after[i]);
    }
    /* a running instance we were handed (PowerPoint is single-instance) is
     * the user's: never quit it, and don't kill it from the watchdog */
    if (o->proc) {
        o->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (o->stop) o->dog = CreateThread(NULL, 0, watchdog, o, 0, NULL);
    }
    return true;
}

static void office_end(Office *o, bool quit_with_arg)
{
    if (o->app && o->proc) {
        VARIANT a = v_int(0);   /* wdDoNotSaveChanges (Word); others take no args */
        disp_method(o->app, L"Quit", quit_with_arg ? 1 : 0, &a);
    }
    rel(&o->app);
    if (o->stop) { SetEvent(o->stop); }
    if (o->dog) { WaitForSingleObject(o->dog, INFINITE); CloseHandle(o->dog); }
    if (o->stop) CloseHandle(o->stop);
    if (o->proc) {
        CoFreeUnusedLibraries();
        if (WaitForSingleObject(o->proc, 15000) == WAIT_TIMEOUT) {
            log_line(L"docs: Office didn't quit, ending it");
            TerminateProcess(o->proc, 1);
        }
        CloseHandle(o->proc);
    }
    memset(o, 0, sizeof *o);
}

/* Word: anything Word opens -> PDF (17) or .docx (16). PDFs open via PDF reflow. */
static bool word_convert(const wchar_t *src, const wchar_t *dst, int fmt)
{
    Office o;
    if (!office_start(&o, L"Word.Application", L"WINWORD.EXE")) return false;
    bool ok = false;
    IDispatch *docs = NULL, *doc = NULL;
    disp_put(o.app, L"DisplayAlerts", v_int(0));            /* wdAlertsNone */
    /* Opening a PDF shows "Word will now convert your PDF..." even with alerts
     * off; Word's own option to skip it is switched on just for this open. */
    HKEY key = NULL;
    DWORD old = 0, oldsz = sizeof old;
    bool had = false;
    if (_wcsicmp(ext_of(src), L"pdf") == 0) {
        VARIANT ver;
        if (SUCCEEDED(disp_call(o.app, DISPATCH_PROPERTYGET, L"Version", &ver, 0, NULL)) && ver.vt == VT_BSTR && ver.bstrVal) {
            wchar_t kp[160];
            swprintf(kp, 160, L"Software\\Microsoft\\Office\\%ls\\Word\\Options", ver.bstrVal);
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kp, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
                had = RegQueryValueExW(key, L"DisableConvertPdfWarning", NULL, NULL, (BYTE *)&old, &oldsz) == ERROR_SUCCESS;
                DWORD one = 1;
                RegSetValueExW(key, L"DisableConvertPdfWarning", 0, REG_DWORD, (const BYTE *)&one, sizeof one);
            } else key = NULL;
        }
        VariantClear(&ver);
    }
    docs = disp_get(o.app, L"Documents");
    if (docs) {
        /* Open(FileName, ConfirmConversions, ReadOnly, AddToRecentFiles,
         *      PasswordDocument, PasswordTemplate, Revert, WritePasswordDocument,
         *      WritePasswordTemplate, Format, Encoding, Visible) */
        VARIANT a[12] = { v_str(src), v_bool(false), v_bool(true), v_bool(false),
                          v_str(L"jarvis-no-password"), v_miss(), v_bool(false), v_miss(),
                          v_miss(), v_miss(), v_miss(), v_bool(false) };
        doc = disp_method_obj(docs, L"Open", 12, a);
        clear_args(a, 12);
    }
    if (key) {
        if (had) RegSetValueExW(key, L"DisableConvertPdfWarning", 0, REG_DWORD, (const BYTE *)&old, sizeof old);
        else RegDeleteValueW(key, L"DisableConvertPdfWarning");
        RegCloseKey(key);
    }
    if (doc) {
        if (fmt == 17) {
            VARIANT a[2] = { v_str(dst), v_int(17) };       /* ExportAsFixedFormat(path, wdExportFormatPDF) */
            ok = SUCCEEDED(disp_method(doc, L"ExportAsFixedFormat", 2, a));
            clear_args(a, 2);
        } else {
            VARIANT a[2] = { v_str(dst), v_int(fmt) };      /* SaveAs2(path, wdFormatXMLDocument) */
            ok = SUCCEEDED(disp_method(doc, L"SaveAs2", 2, a));
            clear_args(a, 2);
        }
        VARIANT c = v_int(0);
        disp_method(doc, L"Close", 1, &c);
    }
    rel(&doc);
    rel(&docs);
    office_end(&o, true);
    return ok && path_exists(dst);
}

static bool ppt_to_pdf(const wchar_t *src, const wchar_t *dst)
{
    Office o;
    if (!office_start(&o, L"PowerPoint.Application", L"POWERPNT.EXE")) return false;
    bool ok = false;
    IDispatch *pres = NULL, *pp = NULL;
    if (o.proc) disp_put(o.app, L"DisplayAlerts", v_int(1));   /* ppAlertsNone */
    pres = disp_get(o.app, L"Presentations");
    if (pres) {
        VARIANT a[4] = { v_str(src), v_int(-1), v_int(0), v_int(0) };  /* ReadOnly, Untitled, WithWindow */
        pp = disp_method_obj(pres, L"Open", 4, a);
        clear_args(a, 4);
    }
    if (pp) {
        VARIANT a[2] = { v_str(dst), v_int(32) };          /* ppSaveAsPDF */
        ok = SUCCEEDED(disp_method(pp, L"SaveAs", 2, a));
        clear_args(a, 2);
        disp_method(pp, L"Close", 0, NULL);
    }
    rel(&pp);
    rel(&pres);
    office_end(&o, false);
    return ok && path_exists(dst);
}

static bool excel_to_pdf(const wchar_t *src, const wchar_t *dst)
{
    Office o;
    if (!office_start(&o, L"Excel.Application", L"EXCEL.EXE")) return false;
    bool ok = false;
    IDispatch *wbs = NULL, *wb = NULL;
    disp_put(o.app, L"DisplayAlerts", v_bool(false));
    wbs = disp_get(o.app, L"Workbooks");
    if (wbs) {
        VARIANT a[3] = { v_str(src), v_int(0), v_bool(true) };  /* UpdateLinks=0, ReadOnly */
        wb = disp_method_obj(wbs, L"Open", 3, a);
        clear_args(a, 3);
    }
    if (wb) {
        VARIANT a[2] = { v_int(0), v_str(dst) };           /* xlTypePDF, Filename */
        ok = SUCCEEDED(disp_method(wb, L"ExportAsFixedFormat", 2, a));
        clear_args(a, 2);
        VARIANT c = v_bool(false);
        disp_method(wb, L"Close", 1, &c);
    }
    rel(&wb);
    rel(&wbs);
    office_end(&o, false);
    return ok && path_exists(dst);
}

/* ------------------------------------------------------------- the tasks */
static void folder_disp(const wchar_t *path, wchar_t *out)
{
    wchar_t d[MAX_PATH];
    str_copy(d, MAX_PATH, path);
    wchar_t *s = wcsrchr(d, L'\\');
    if (s && s > d + 2) *s = 0; else if (s) s[1] = 0;
    nm(d, out);
}

static bool task_to_pdf(const DocTask *t, DocResult *r)
{
    const wchar_t *src = t->src[0];
    wchar_t dst[MAX_PATH], n0[128], n1[128];
    docs_unique(t->dst[0], dst);
    nm(src, n0);
    bool ok;
    const wchar_t *app;
    if (ext_in(src, WORD_EXT))       { app = L"Word";       ok = word_convert(src, dst, 17); }
    else if (ext_in(src, PPT_EXT))   { app = L"PowerPoint"; ok = ppt_to_pdf(src, dst); }
    else if (ext_in(src, SHEET_EXT)) { app = L"Excel";      ok = excel_to_pdf(src, dst); }
    else if (ext_in(src, IMG_EXT)) {
        const wchar_t *args[] = { L"img", src, dst };
        if (!py_job(args, 3, NULL, r, L"convert")) return false;
        ok = true; app = NULL;
    } else { say(r, L"I can't turn %ls into a PDF.", n0); return false; }
    if (!ok) { say(r, L"I couldn't convert %ls. Is %ls working?", n0, app ? app : L"Python"); return false; }
    nm(dst, n1);
    add_out(r, dst);
    say(r, L"Saved %ls.", n1);
    return true;
}

static bool task_to_docx(const DocTask *t, DocResult *r)
{
    wchar_t dst[MAX_PATH], n0[128], n1[128];
    docs_unique(t->dst[0], dst);
    nm(t->src[0], n0);
    if (!word_convert(t->src[0], dst, 16)) { say(r, L"I couldn't turn %ls into a Word document.", n0); return false; }
    nm(dst, n1);
    add_out(r, dst);
    say(r, L"Saved %ls.", n1);
    return true;
}

static bool task_split(const DocTask *t, DocResult *r)
{
    wchar_t d1[MAX_PATH], d2[MAX_PATH], a[16], n1[128], n2[128];
    docs_unique(t->dst[0], d1);
    docs_unique(t->dst[1], d2);
    if (_wcsicmp(d1, d2) == 0) { say(r, L"Both parts would get the same name."); return false; }
    swprintf(a, 16, L"%d", t->a);
    const wchar_t *args[] = { L"split", t->src[0], a, d1, d2 };
    int total = 0;
    if (!py_job(args, 5, &total, r, L"split")) return false;
    nm(d1, n1); nm(d2, n2);
    add_out(r, d1);
    add_out(r, d2);
    if (t->a + 1 == total) say(r, L"Done: %ls has pages 1 to %d, %ls has page %d.", n1, t->a, n2, total);
    else say(r, L"Done: %ls has pages 1 to %d, %ls has pages %d to %d.", n1, t->a, n2, t->a + 1, total);
    return true;
}

static bool task_extract(const DocTask *t, DocResult *r)
{
    wchar_t d[MAX_PATH], x[16], y[16], n1[128];
    docs_unique(t->dst[0], d);
    swprintf(x, 16, L"%d", t->a);
    swprintf(y, 16, L"%d", t->b);
    const wchar_t *args[] = { L"extract", t->src[0], x, y, d };
    if (!py_job(args, 5, NULL, r, L"extract pages from")) return false;
    nm(d, n1);
    add_out(r, d);
    if (t->a == t->b) say(r, L"Saved page %d as %ls.", t->a, n1);
    else say(r, L"Saved pages %d to %d as %ls.", t->a, t->b, n1);
    return true;
}

static bool task_merge(const DocTask *t, DocResult *r)
{
    wchar_t d[MAX_PATH], n1[128];
    docs_unique(t->dst[0], d);
    const wchar_t *args[2 + DOC_MAX_SRC] = { L"merge", d };
    int n = 2;
    for (int i = 0; i < t->nsrc && i < DOC_MAX_SRC; i++) args[n++] = t->src[i];
    int pages = 0;
    if (!py_job(args, n, &pages, r, L"merge")) return false;
    nm(d, n1);
    add_out(r, d);
    say(r, L"Merged %d PDFs into %ls, %d pages.", t->nsrc, n1, pages);
    return true;
}

static bool task_pages(const DocTask *t, DocResult *r)
{
    const wchar_t *args[] = { L"count", t->src[0] };
    int pages = 0;
    wchar_t n0[128];
    if (!py_job(args, 2, &pages, r, L"read")) return false;
    nm(t->src[0], n0);
    say(r, pages == 1 ? L"%ls has %d page." : L"%ls has %d pages.", n0, pages);
    add_out(r, t->src[0]);
    return true;
}

static bool task_file(const DocTask *t, DocResult *r)
{
    const wchar_t *src = t->src[0];
    wchar_t dst[MAX_PATH], n0[128], n1[128], fd[128];
    nm(src, n0);
    if (!path_exists(src)) { say(r, L"%ls isn't there any more.", n0); return false; }
    const wchar_t *why = t->op != DOC_COPY ? docs_protected(src) : NULL;
    if (why) { say(r, L"I won't touch %ls, %ls.", n0, why); return false; }

    if (t->op == DOC_DELETE) {
        wchar_t root[8] = L"";
        if (src[0] && src[1] == L':') swprintf(root, 8, L"%lc:\\", src[0]);
        if (!root[0] || GetDriveTypeW(root) != DRIVE_FIXED) {
            say(r, L"%ls isn't on a local drive with a Recycle Bin, so I won't delete it.", n0);
            return false;
        }
        /* double-NUL-terminated list; FOF_ALLOWUNDO = Recycle Bin, never permanent */
        wchar_t from[MAX_PATH + 2] = L"";
        str_copy(from, MAX_PATH, src);
        from[wcslen(from) + 1] = 0;
        SHFILEOPSTRUCTW op = { 0 };
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_SILENT;
        int rc = SHFileOperationW(&op);
        if (rc || op.fAnyOperationsAborted || path_exists(src)) { say(r, L"I couldn't delete %ls.", n0); return false; }
        say(r, L"Moved %ls to the Recycle Bin.", n0);
        return true;
    }

    docs_unique(t->dst[0], dst);
    if (_wcsicmp(src, dst) == 0) { say(r, L"%ls already has that name.", n0); return false; }
    nm(dst, n1);
    folder_disp(dst, fd);
    BOOL ok;
    if (t->op == DOC_COPY) {
        if (path_is_dir(src)) {
            wchar_t from[MAX_PATH + 2] = L"", to[MAX_PATH + 2] = L"";
            str_copy(from, MAX_PATH, src); from[wcslen(from) + 1] = 0;
            str_copy(to, MAX_PATH, dst);   to[wcslen(to) + 1] = 0;
            SHFILEOPSTRUCTW op = { 0 };
            op.wFunc = FO_COPY;
            op.pFrom = from;
            op.pTo = to;
            op.fFlags = FOF_SILENT | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI;
            ok = SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
        } else ok = CopyFileW(src, dst, TRUE);
    } else ok = MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED);
    if (!ok || !path_exists(dst)) {
        DWORD e = GetLastError();
        say(r, e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED ?
               L"I couldn't do that, %ls is open or locked." : L"I couldn't do that with %ls.", n0);
        return false;
    }
    add_out(r, dst);
    if (t->op == DOC_RENAME) say(r, L"Renamed it to %ls.", n1);
    else if (t->op == DOC_MOVE) say(r, L"Moved %ls to %ls.", n1, fd);
    else say(r, L"Copied %ls to %ls.", n1, fd);
    return true;
}

bool docs_run(const DocTask *t, DocResult *r)
{
    memset(r, 0, sizeof *r);
    r->op = t->op;
    for (int i = 0; i < t->nsrc; i++)
        if (!path_exists(t->src[i])) {
            wchar_t n0[128];
            nm(t->src[i], n0);
            say(r, L"I can't find %ls.", n0);
            return false;
        }
    switch (t->op) {
    case DOC_TO_PDF:  r->ok = task_to_pdf(t, r); break;
    case DOC_TO_DOCX: r->ok = task_to_docx(t, r); break;
    case DOC_SPLIT:   r->ok = task_split(t, r); break;
    case DOC_EXTRACT: r->ok = task_extract(t, r); break;
    case DOC_MERGE:   r->ok = task_merge(t, r); break;
    case DOC_PAGES:   r->ok = task_pages(t, r); break;
    case DOC_RENAME: case DOC_MOVE: case DOC_COPY: case DOC_DELETE:
        r->ok = task_file(t, r); break;
    default: say(r, L"I don't know how to do that."); break;
    }
    log_line(L"docs: op %d %ls: %ls", (int)t->op, r->ok ? L"ok" : L"failed", r->reply);
    return r->ok;
}

/* ------------------------------------------------------------- async */
static volatile LONG s_busy;
static HANDLE s_thread;

typedef struct { DocTask task; HWND notify; } Job;

static DWORD WINAPI worker(LPVOID p)
{
    Job *j = p;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    DocResult *res = malloc(sizeof *res);
    if (res) {
        docs_run(&j->task, res);
        if (!j->notify || !PostMessageW(j->notify, WM_APP_TASK, 0, (LPARAM)res)) free(res);
    }
    CoUninitialize();
    free(j);
    InterlockedExchange(&s_busy, 0);
    return 0;
}

bool docs_run_async(const DocTask *t, HWND notify)
{
    if (InterlockedCompareExchange(&s_busy, 1, 0) != 0) return false;
    Job *j = malloc(sizeof *j);
    if (!j) { InterlockedExchange(&s_busy, 0); return false; }
    j->task = *t;
    j->notify = notify;
    if (s_thread) { CloseHandle(s_thread); s_thread = NULL; }
    s_thread = CreateThread(NULL, 0, worker, j, 0, NULL);
    if (!s_thread) { free(j); InterlockedExchange(&s_busy, 0); return false; }
    return true;
}

bool docs_busy(void) { return InterlockedCompareExchange(&s_busy, 0, 0) != 0; }

void docs_wait(DWORD ms)
{
    if (s_thread && docs_busy()) WaitForSingleObject(s_thread, ms);
}

/* -------------------------------------------------- Explorer selection */
static const GUID SID_TopBrowser = { 0x4C96BE40, 0x915C, 0x11CF, { 0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37 } };

static IShellBrowser *browser_of(IDispatch *d)
{
    IServiceProvider *sp = NULL;
    IShellBrowser *sb = NULL;
    if (SUCCEEDED(IDispatch_QueryInterface(d, &IID_IServiceProvider, (void **)&sp)) && sp) {
        IServiceProvider_QueryService(sp, &SID_TopBrowser, &IID_IShellBrowser, (void **)&sb);
        IServiceProvider_Release(sp);
    }
    return sb;
}

static int selection_of(IShellBrowser *sb, wchar_t (*out)[MAX_PATH], int max)
{
    int n = 0;
    IShellView *sv = NULL;
    IFolderView2 *fv = NULL;
    IShellItemArray *arr = NULL;
    if (FAILED(IShellBrowser_QueryActiveShellView(sb, &sv)) || !sv) return 0;
    if (SUCCEEDED(IShellView_QueryInterface(sv, &IID_IFolderView2, (void **)&fv)) && fv) {
        if (IFolderView2_GetSelection(fv, FALSE, &arr) == S_OK && arr) {
            DWORD c = 0;
            IShellItemArray_GetCount(arr, &c);
            for (DWORD i = 0; i < c && n < max; i++) {
                IShellItem *it = NULL;
                if (FAILED(IShellItemArray_GetItemAt(arr, i, &it)) || !it) continue;
                PWSTR p = NULL;
                if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p)) && p) {
                    str_copy(out[n++], MAX_PATH, p);
                    CoTaskMemFree(p);
                }
                IShellItem_Release(it);
            }
            IShellItemArray_Release(arr);
        }
        IFolderView2_Release(fv);
    }
    IShellView_Release(sv);
    return n;
}

int shell_selection(HWND fg, wchar_t (*out)[MAX_PATH], int max)
{
    if (!fg || max <= 0) return 0;
    wchar_t cls[64] = L"";
    GetClassNameW(fg, cls, 64);
    bool desk = str_eq(cls, L"Progman") || str_eq(cls, L"WorkerW");
    if (!desk && !str_eq(cls, L"CabinetWClass")) return 0;
    IShellWindows *sw = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellWindows, NULL, CLSCTX_ALL, &IID_IShellWindows, (void **)&sw)) || !sw)
        return 0;
    int n = 0;
    if (desk) {
        VARIANT loc = v_int(CSIDL_DESKTOP), empty;
        VariantInit(&empty);
        long hw = 0;
        IDispatch *d = NULL;
        if (SUCCEEDED(IShellWindows_FindWindowSW(sw, &loc, &empty, SWC_DESKTOP, &hw, SWFO_NEEDDISPATCH, &d)) && d) {
            IShellBrowser *sb = browser_of(d);
            if (sb) { n = selection_of(sb, out, max); IShellBrowser_Release(sb); }
            IDispatch_Release(d);
        }
    } else {
        long count = 0;
        IShellWindows_get_Count(sw, &count);
        IShellBrowser *pick = NULL;
        for (long i = 0; i < count; i++) {
            VARIANT vi = v_int(i);
            IDispatch *d = NULL;
            if (FAILED(IShellWindows_Item(sw, vi, &d)) || !d) continue;
            IWebBrowserApp *wb = NULL;
            SHANDLE_PTR h = 0;
            if (SUCCEEDED(IDispatch_QueryInterface(d, &IID_IWebBrowserApp, (void **)&wb)) && wb) {
                IWebBrowserApp_get_HWND(wb, &h);
                IWebBrowserApp_Release(wb);
            }
            if ((HWND)h == fg) {
                IShellBrowser *sb = browser_of(d);
                if (sb) {
                    /* Windows 11 tabs share the frame: the active tab's view is visible */
                    HWND tab = NULL;
                    IShellBrowser_GetWindow(sb, &tab);
                    if (!pick || (tab && IsWindowVisible(tab))) {
                        if (pick) IShellBrowser_Release(pick);
                        pick = sb;
                        if (tab && IsWindowVisible(tab)) { IDispatch_Release(d); break; }
                    } else IShellBrowser_Release(sb);
                }
            }
            IDispatch_Release(d);
        }
        if (pick) { n = selection_of(pick, out, max); IShellBrowser_Release(pick); }
    }
    IShellWindows_Release(sw);
    return n;
}

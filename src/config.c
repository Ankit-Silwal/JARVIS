/* Settings: jarvis.ini next to the exe (created with defaults on first run). */
#include "jarvis.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>

Config g_cfg;

static const wchar_t DEFAULT_INI[] =
L"; JARVIS settings. Edit freely - changes apply the next time JARVIS opens.\r\n"
L"; Names on the left are what you SAY (lowercase). Delete this file to restore defaults.\r\n"
L"\r\n"
L"[general]\r\n"
L"; speak replies out loud (1/0)\r\n"
L"speak=1\r\n"
L"; start listening as soon as JARVIS opens (1/0)\r\n"
L"listen_on_start=1\r\n"
L"; voice to use (part of the name: David, Zira, ...) and speed -10..10\r\n"
L"voice=David\r\n"
L"voice_rate=1\r\n"
L"; how long the reply stays on screen before JARVIS closes (ms)\r\n"
L"close_delay_ms=1100\r\n"
L"; browser used for websites and web searches\r\n"
L"browser=chrome.exe\r\n"
L"; re-scan your folders when the file index is older than this (minutes)\r\n"
L"index_max_age_min=30\r\n"
L"\r\n"
L"[search]\r\n"
L"; Desktop, Documents, Downloads, Pictures, Videos, Music and OneDrive are always indexed.\r\n"
L"; Add more folders here, separated by ;\r\n"
L"roots=%ROOTS%\r\n"
L"; folder names that are never scanned\r\n"
L"skip=node_modules;__pycache__;venv;.venv;env;site-packages;AppData;.next;.git;.svn;.cache;$RECYCLE.BIN;System Volume Information\r\n"
L"\r\n"
L"[sites]\r\n"
L"youtube=https://www.youtube.com\r\n"
L"gmail=https://mail.google.com\r\n"
L"mail=https://mail.google.com\r\n"
L"google=https://www.google.com\r\n"
L"drive=https://drive.google.com\r\n"
L"google drive=https://drive.google.com\r\n"
L"docs=https://docs.google.com\r\n"
L"google docs=https://docs.google.com\r\n"
L"slides=https://docs.google.com/presentation\r\n"
L"google slides=https://docs.google.com/presentation\r\n"
L"sheets=https://docs.google.com/spreadsheets\r\n"
L"google sheets=https://docs.google.com/spreadsheets\r\n"
L"classroom=https://classroom.google.com\r\n"
L"google classroom=https://classroom.google.com\r\n"
L"calendar=https://calendar.google.com\r\n"
L"maps=https://maps.google.com\r\n"
L"google maps=https://maps.google.com\r\n"
L"translate=https://translate.google.com\r\n"
L"github=https://github.com\r\n"
L"chatgpt=https://chatgpt.com\r\n"
L"claude=https://claude.ai\r\n"
L"gemini=https://gemini.google.com\r\n"
L"whatsapp=https://web.whatsapp.com\r\n"
L"instagram=https://www.instagram.com\r\n"
L"facebook=https://www.facebook.com\r\n"
L"linkedin=https://www.linkedin.com\r\n"
L"stack overflow=https://stackoverflow.com\r\n"
L"wikipedia=https://www.wikipedia.org\r\n"
L"canva=https://www.canva.com\r\n"
L"notion=https://www.notion.so\r\n"
L"outlook=https://outlook.live.com\r\n"
L"netflix=https://www.netflix.com\r\n"
L"spotify web=https://open.spotify.com\r\n"
L"\r\n"
L"[apps]\r\n"
L"chrome=chrome.exe\r\n"
L"google chrome=chrome.exe\r\n"
L"incognito=chrome.exe --incognito\r\n"
L"edge=msedge.exe\r\n"
L"vs code=%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe\r\n"
L"vscode=%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe\r\n"
L"visual studio code=%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe\r\n"
L"code=%LOCALAPPDATA%\\Programs\\Microsoft VS Code\\Code.exe\r\n"
L"notepad=notepad.exe\r\n"
L"calculator=calc.exe\r\n"
L"paint=mspaint.exe\r\n"
L"explorer=explorer.exe\r\n"
L"file explorer=explorer.exe\r\n"
L"files=explorer.exe\r\n"
L"terminal=wt.exe\r\n"
L"command prompt=cmd.exe\r\n"
L"cmd=cmd.exe\r\n"
L"powershell=powershell.exe\r\n"
L"task manager=taskmgr.exe\r\n"
L"settings=ms-settings:\r\n"
L"bluetooth=ms-settings:bluetooth\r\n"
L"wifi=ms-settings:network-wifi\r\n"
L"wi-fi=ms-settings:network-wifi\r\n"
L"display settings=ms-settings:display\r\n"
L"sound settings=ms-settings:sound\r\n"
L"control panel=control.exe\r\n"
L"snipping tool=ms-screenclip:\r\n"
L"word=winword.exe\r\n"
L"powerpoint=powerpnt.exe\r\n"
L"excel=excel.exe\r\n"
L"\r\n"
L"[folders]\r\n"
L"; your own spoken shortcuts to folders, e.g.\r\n"
L"; projects=C:\\Studies\\Web Development\r\n"
L"%FOLDERS%";

static void kv_add(KVList *l, const wchar_t *k, const wchar_t *v)
{
    wchar_t key[128];
    str_copy(key, 128, k);
    str_lower(key);
    str_collapse_spaces(key);
    if (!key[0]) return;
    for (int i = 0; i < l->count; i++)
        if (str_eq(l->items[i].key, key)) {  /* later entries override */
            free(l->items[i].val);
            l->items[i].val = wcs_dup(v);
            return;
        }
    if (l->count == l->cap) {
        int nc = l->cap ? l->cap * 2 : 32;
        KV *n = realloc(l->items, nc * sizeof(KV));
        if (!n) return;
        l->items = n; l->cap = nc;
    }
    l->items[l->count].key = wcs_dup(key);
    l->items[l->count].val = wcs_dup(v);
    l->count++;
}

const wchar_t *kv_get(const KVList *l, const wchar_t *key)
{
    for (int i = 0; i < l->count; i++)
        if (str_eq(l->items[i].key, key)) return l->items[i].val;
    return NULL;
}

static void load_section(const wchar_t *sec, KVList *l, bool expand)
{
    static wchar_t buf[32768];
    DWORD n = GetPrivateProfileSectionW(sec, buf, 32768, g_cfg.ini_path);
    if (!n) return;
    for (wchar_t *p = buf; *p; p += wcslen(p) + 1) {
        if (*p == L';' || *p == L'#') continue;
        wchar_t line[1024];
        str_copy(line, 1024, p);
        wchar_t *eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t *val = eq + 1;
        str_trim(line); str_trim(val);
        if (!line[0] || !val[0]) continue;
        if (expand) {
            wchar_t ex[1024];
            expand_env(val, ex, 1024);
            kv_add(l, line, ex);
        } else {
            kv_add(l, line, val);
        }
    }
}

static int read_int(const wchar_t *sec, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(sec, key, def, g_cfg.ini_path);
}

static void read_str(const wchar_t *sec, const wchar_t *key, const wchar_t *def, wchar_t *out, DWORD cap)
{
    GetPrivateProfileStringW(sec, key, def, out, cap, g_cfg.ini_path);
    str_trim(out);
}

static void add_list(wchar_t ***arr, int *n, const wchar_t *s)
{
    wchar_t tmp[4096];
    str_copy(tmp, 4096, s);
    wchar_t *ctx = NULL;
    for (wchar_t *t = wcstok(tmp, L";", &ctx); t; t = wcstok(NULL, L";", &ctx)) {
        wchar_t item[MAX_PATH], ex[MAX_PATH];
        str_copy(item, MAX_PATH, t);
        str_trim(item);
        if (!item[0]) continue;
        expand_env(item, ex, MAX_PATH);
        size_t L = wcslen(ex);
        while (L > 3 && (ex[L - 1] == L'\\' || ex[L - 1] == L'/')) ex[--L] = 0;
        bool dup = false;
        for (int i = 0; i < *n; i++) if (_wcsicmp((*arr)[i], ex) == 0) dup = true;
        if (dup) continue;
        wchar_t **na = realloc(*arr, (*n + 1) * sizeof(wchar_t *));
        if (!na) return;
        *arr = na;
        (*arr)[(*n)++] = wcs_dup(ex);
    }
}

static bool known_folder(REFKNOWNFOLDERID id, wchar_t *out)
{
    PWSTR p = NULL;
    bool ok = SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &p)) && p;
    if (ok) str_copy(out, MAX_PATH, p);
    CoTaskMemFree(p);
    return ok;
}

/* Writes the default ini (UTF-16 LE with BOM so names can be any language). */
static void write_default_ini(void)
{
    size_t cap = wcslen(DEFAULT_INI) + 4096;
    wchar_t *text = malloc(cap * sizeof(wchar_t));
    if (!text) return;

    wchar_t roots[MAX_PATH] = L"";
    if (path_is_dir(L"C:\\Studies")) str_copy(roots, MAX_PATH, L"C:\\Studies");

    wchar_t folders[1024] = L"";
    if (path_is_dir(L"C:\\Studies")) str_cat(folders, 1024, L"studies=C:\\Studies\r\n");
    if (path_is_dir(L"C:\\Studies\\Web Development"))
        str_cat(folders, 1024, L"web development=C:\\Studies\\Web Development\r\n");

    /* simple template replacement */
    text[0] = 0;
    const wchar_t *src = DEFAULT_INI;
    while (*src) {
        if (str_starts(src, L"%ROOTS%")) { str_cat(text, cap, roots); src += 7; continue; }
        if (str_starts(src, L"%FOLDERS%")) { str_cat(text, cap, folders); src += 9; continue; }
        size_t L = wcslen(text);
        if (L + 1 < cap) { text[L] = *src; text[L + 1] = 0; }
        src++;
    }

    HANDLE h = CreateFileW(g_cfg.ini_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wr;
        WORD bom = 0xFEFF;
        WriteFile(h, &bom, 2, &wr, NULL);
        WriteFile(h, text, (DWORD)(wcslen(text) * sizeof(wchar_t)), &wr, NULL);
        CloseHandle(h);
    }
    free(text);
}

void config_load(void)
{
    /* paths */
    GetModuleFileNameW(NULL, g_cfg.exe_dir, MAX_PATH);
    wchar_t *slash = wcsrchr(g_cfg.exe_dir, L'\\');
    if (slash) *slash = 0;
    swprintf(g_cfg.ini_path, MAX_PATH, L"%ls\\jarvis.ini", g_cfg.exe_dir);
    expand_env(L"%LOCALAPPDATA%\\JARVIS", g_cfg.data_dir, MAX_PATH);
    CreateDirectoryW(g_cfg.data_dir, NULL);

    if (!path_exists(g_cfg.ini_path)) write_default_ini();

    /* general */
    g_cfg.speak = read_int(L"general", L"speak", 1) != 0;
    g_cfg.listen_on_start = read_int(L"general", L"listen_on_start", 1) != 0;
    {
        wchar_t mode[32];
        read_str(L"general", L"mode", L"normal", mode, 32);
        g_cfg.background = _wcsicmp(mode, L"background") == 0 || _wcsicmp(mode, L"hidden") == 0;
    }
    g_cfg.voice_rate = read_int(L"general", L"voice_rate", 1);
    g_cfg.close_delay_ms = read_int(L"general", L"close_delay_ms", 1100);
    g_cfg.index_max_age_min = read_int(L"general", L"index_max_age_min", 30);
    read_str(L"general", L"voice", L"David", g_cfg.voice_name, 64);
    read_str(L"general", L"browser", L"chrome.exe", g_cfg.browser, MAX_PATH);

    /* built-in folders (spoken name -> path); [folders] in the ini can add/override */
    wchar_t p[MAX_PATH];
    if (known_folder(&FOLDERID_Desktop, p))   { kv_add(&g_cfg.folders, L"desktop", p); }
    if (known_folder(&FOLDERID_Downloads, p)) { kv_add(&g_cfg.folders, L"downloads", p); kv_add(&g_cfg.folders, L"download", p); kv_add(&g_cfg.folders, L"download folder", p); }
    if (known_folder(&FOLDERID_Documents, p)) { kv_add(&g_cfg.folders, L"documents", p); kv_add(&g_cfg.folders, L"my documents", p); }
    if (known_folder(&FOLDERID_Pictures, p))  { kv_add(&g_cfg.folders, L"pictures", p); kv_add(&g_cfg.folders, L"photos", p); kv_add(&g_cfg.folders, L"my pictures", p); kv_add(&g_cfg.folders, L"my photos", p); }
    if (known_folder(&FOLDERID_Videos, p))    { kv_add(&g_cfg.folders, L"videos", p); kv_add(&g_cfg.folders, L"my videos", p); }
    if (known_folder(&FOLDERID_Music, p))     { kv_add(&g_cfg.folders, L"music", p); kv_add(&g_cfg.folders, L"my music", p); }
    if (known_folder(&FOLDERID_Profile, p))   { kv_add(&g_cfg.folders, L"home", p); kv_add(&g_cfg.folders, L"user folder", p); kv_add(&g_cfg.folders, L"home folder", p); }
    {
        wchar_t od[MAX_PATH];
        if (GetEnvironmentVariableW(L"OneDrive", od, MAX_PATH) && path_is_dir(od))
            kv_add(&g_cfg.folders, L"onedrive", od), kv_add(&g_cfg.folders, L"one drive", od);
    }
    kv_add(&g_cfg.folders, L"this pc", L"shell:MyComputerFolder");
    kv_add(&g_cfg.folders, L"my computer", L"shell:MyComputerFolder");
    kv_add(&g_cfg.folders, L"computer", L"shell:MyComputerFolder");
    kv_add(&g_cfg.folders, L"recycle bin", L"shell:RecycleBinFolder");
    kv_add(&g_cfg.folders, L"trash", L"shell:RecycleBinFolder");
    kv_add(&g_cfg.folders, L"recent", L"shell:Recent");
    kv_add(&g_cfg.folders, L"startup", L"shell:Startup");

    load_section(L"folders", &g_cfg.folders, true);
    load_section(L"sites", &g_cfg.sites, false);
    load_section(L"apps", &g_cfg.apps, true);

    /* index roots: standard user folders + OneDrive + [search] roots */
    const KNOWNFOLDERID *ids[] = { &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents,
                                   &FOLDERID_Pictures, &FOLDERID_Videos, &FOLDERID_Music };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (known_folder(ids[i], p)) add_list(&g_cfg.roots, &g_cfg.nroots, p);
    if (GetEnvironmentVariableW(L"OneDrive", p, MAX_PATH) && path_is_dir(p))
        add_list(&g_cfg.roots, &g_cfg.nroots, p);

    wchar_t extra[4096];
    read_str(L"search", L"roots", L"", extra, 4096);
    add_list(&g_cfg.roots, &g_cfg.nroots, extra);
    read_str(L"search", L"skip", L"node_modules;__pycache__;venv;.venv;site-packages;AppData;.git", extra, 4096);
    add_list(&g_cfg.skip, &g_cfg.nskip, extra);
}

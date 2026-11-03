/* JARVIS - lightweight offline voice launcher for Windows.
 * Shared declarations for all modules. */
#ifndef JARVIS_H
#define JARVIS_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <wchar.h>

#define APP_NAME      L"JARVIS"
#define APP_CLASS     L"JarvisOverlayWnd"
#define APP_MUTEX     L"Local\\JarvisVoiceLauncher_SingleInstance"
#define MAX_HITS      8
#define LINE_MAX_CH   512

/* Custom window messages */
#define WM_APP_TOGGLE   (WM_APP + 1)   /* another instance was launched: toggle mic */
#define WM_APP_VOICE    (WM_APP + 2)   /* event from voice thread, wParam = VoiceEvent */
#define WM_APP_RUN      (WM_APP + 3)   /* run a command string (lParam = heap wchar_t*) */
#define WM_APP_TASK     (WM_APP + 4)   /* document task finished (lParam = heap DocResult*) */

/* ------------------------------------------------------------------ util.c */
typedef struct {
    wchar_t w[64];     /* the word as spoken/typed */
    wchar_t alt[16];   /* alternate spelling, e.g. "two" -> "2" */
} Token;

void   str_lower(wchar_t *s);
void   str_trim(wchar_t *s);
void   str_collapse_spaces(wchar_t *s);
bool   str_starts(const wchar_t *s, const wchar_t *prefix);
bool   str_ends(const wchar_t *s, const wchar_t *suffix);
/* If s starts with word-prefix (followed by space or end) returns pointer to rest, else NULL */
const wchar_t *str_after_word(const wchar_t *s, const wchar_t *prefix);
bool   str_eq(const wchar_t *a, const wchar_t *b);
void   str_copy(wchar_t *dst, size_t cap, const wchar_t *src);
void   str_cat(wchar_t *dst, size_t cap, const wchar_t *src);
int    tokenize(const wchar_t *s, Token *out, int max);
int    edit_distance(const wchar_t *a, int la, const wchar_t *b, int lb, int limit);
int    phonetic_key(const wchar_t *w, int len, wchar_t *out, int cap); /* fizzics -> fisiks */
int    number_word(const wchar_t *w);   /* "two"/"second"/"2" -> 2, else 0 */
wchar_t *wcs_dup(const wchar_t *s);
char  *utf8_from_wide(const wchar_t *w);
wchar_t *wide_from_utf8(const char *s, int len);
void   expand_env(const wchar_t *in, wchar_t *out, size_t cap);
bool   path_is_dir(const wchar_t *p);
bool   path_exists(const wchar_t *p);
const wchar_t *path_name(const wchar_t *p);
void   log_line(const wchar_t *fmt, ...);

/* ---------------------------------------------------------------- config.c */
typedef struct { wchar_t *key; wchar_t *val; } KV;
typedef struct { KV *items; int count, cap; } KVList;

typedef struct {
    KVList sites;     /* spoken name -> url */
    KVList apps;      /* spoken name -> program / uri */
    KVList folders;   /* spoken name -> folder path */
    wchar_t **roots;  int nroots;   /* folders to index */
    wchar_t **skip;   int nskip;    /* folder names to skip while indexing */
    bool  speak;
    bool  listen_on_start;
    bool  background;            /* mode=background: no window, just listen + talk */
    bool  stay_open;             /* keep running after a command until the user closes JARVIS */
    int   voice_rate;            /* -10..10 */
    wchar_t voice_name[64];      /* substring of voice name, e.g. "David" */
    int   index_max_age_min;
    int   close_delay_ms;
    wchar_t browser[MAX_PATH];
    wchar_t docs_python[MAX_PATH];  /* [docs] python= (empty: auto-detect) */
    wchar_t ini_path[MAX_PATH];
    wchar_t data_dir[MAX_PATH];
    wchar_t exe_dir[MAX_PATH];
} Config;

extern Config g_cfg;
void config_load(void);
const wchar_t *kv_get(const KVList *l, const wchar_t *key);

/* ---------------------------------------------------------------- search.c */
enum {
    SF_FILES_ONLY = 1,
    SF_DIRS_ONLY  = 2,
    SF_RECENT     = 4,   /* sort by newest first */
};

typedef enum {
    EXT_ANY = 0, EXT_IMAGE, EXT_PDF, EXT_PPT, EXT_DOC, EXT_SHEET,
    EXT_VIDEO, EXT_AUDIO, EXT_ARCHIVE, EXT_CODE, EXT_TEXT
} ExtClass;

typedef struct {
    wchar_t  path[MAX_PATH];
    int      score;
    FILETIME mtime;
    bool     is_dir;
} Hit;

typedef struct {
    const wchar_t *text;       /* name words to match (may be empty) */
    const wchar_t *in_folder;  /* restrict to this folder (may be NULL) */
    ExtClass ext;
    unsigned flags;
} SearchQuery;

void search_init(void);          /* load index from disk, refresh in background if stale */
void search_rebuild_async(void);
bool search_busy(void);          /* index being (re)built */
bool search_ready(void);         /* some index available */
void search_wait(DWORD ms);
int  search_count(void);
int  search_files(const SearchQuery *q, Hit *out, int max);
/* Start Menu apps: returns score (0 = none) and fills path of the .lnk */
int  search_app(const wchar_t *query, wchar_t *out_path, wchar_t *out_name);
ExtClass ext_class_of(const wchar_t *path);

/* ---------------------------------------------------------------- dialog.c */
HWND dialog_detect(HWND fg);                 /* returns the file dialog or NULL */
bool dialog_fill(HWND dlg, const wchar_t *path);
bool dialog_alive(HWND dlg);

/* ----------------------------------------------------------------- voice.c */
typedef enum {
    VE_READY = 1,     /* lParam: 1 ok, 0 failed */
    VE_LISTENING,     /* lParam: 1 on, 0 off */
    VE_HYPOTHESIS,    /* lParam: heap wchar_t* (receiver frees) */
    VE_RESULT,        /* lParam: heap wchar_t* (receiver frees) */
    VE_NOMATCH,
    VE_LEVEL,         /* lParam: 0..100 */
    VE_TIMEOUT,
    VE_SPOKEN,        /* text-to-speech finished */
} VoiceEvent;

bool voice_start(HWND notify);     /* starts voice thread (async init) */
void voice_listen(bool on);
void voice_say(const wchar_t *text, bool listen_after);
void voice_stop(DWORD wait_ms);    /* finishes speech (up to wait_ms) and ends thread */
bool voice_available(void);

/* -------------------------------------------------------------- commands.c */
typedef enum { ROW_HIT, ROW_INFO } RowKind;

typedef struct {
    RowKind kind;
    Hit     hit;              /* for ROW_HIT */
    wchar_t title[MAX_PATH];  /* for ROW_INFO */
    wchar_t sub[MAX_PATH];
} Row;

typedef enum {
    PICK_OPEN,      /* open the file / folder */
    PICK_REVEAL,    /* show it selected in Explorer */
    PICK_DIALOG,    /* put it into the Upload/Open dialog */
} PickMode;

typedef struct {
    wchar_t reply[300];       /* shown + spoken */
    bool    speak;            /* speak reply */
    bool    close;            /* close overlay after replying (ignored with stay_open) */
    bool    quit;             /* the user asked to close JARVIS ("close", "exit", "bye"...) */
    bool    listen_again;     /* expect a follow-up (e.g. "number two") */
    int     nrows;
    Row     rows[MAX_HITS];
    PickMode pick_mode;
    bool    rows_replace;     /* if false, keep current rows */
} CmdResult;

void cmd_init(HWND dialog);
/* window that receives WM_APP_TASK when a background document task ends */
void cmd_set_notify(HWND hwnd);
/* files selected in Explorer / on the Desktop when JARVIS started ("this", "these") */
void cmd_set_selection(const wchar_t (*paths)[MAX_PATH], int n);
HWND cmd_dialog(void);
/* Dry-run (tests / --dry-run): actions are NOT performed, they are recorded
 * instead and can be read with cmd_last_action(). Format, one action:
 *   "OPEN <path>"        open file/folder (ShellExecute)
 *   "REVEAL <path>"      show file selected in Explorer
 *   "DIALOG <path>"      type path into the Upload/Open dialog
 *   "URL <url>"          open url in the configured browser
 *   "URL <url> [<exe>]"  open url in a browser named in the command
 *                        ("open youtube in brave" -> "URL https://www.youtube.com [brave.exe]")
 *   "APP <target>"       launch program / uri / .lnk (target as configured, args after a space allowed)
 *   "SYS <name>"         system action: lock, screenshot, volume_up, volume_down, mute, reindex
 *   "LIST <n>"           showed n rows for the user to pick from
 *   "SAY"                only replied (time, date, battery, help, not found...)
 *   "CLOSE"              closed without doing anything
 *   "DOC convert-pdf <src> -> <dst>"           Word/PowerPoint/Excel/image to PDF
 *   "DOC convert-docx <src> -> <dst>"          PDF to Word
 *   "DOC split <src> @<N> -> <dst1> | <dst2>"  pages 1..N and N+1..end
 *   "DOC extract <src> <A>-<B> -> <dst>"       pages A..B into a new PDF
 *   "DOC merge <src1> + <src2> [+ ...] -> <dst>"
 *   "DOC pages <src>"                          count the pages of a PDF
 *   "FILE rename|move|copy <src> -> <dst>"     never overwrites (" (2)" added)
 *   "ASK delete <path>"  asked "move X to the Recycle Bin?" (nothing deleted yet)
 *   "FILE delete <path>" sent to the Recycle Bin after the user said yes
 * Document actions run on a worker thread (outside dry-run): the reply is
 * "Converting X..." and the outcome arrives later as WM_APP_TASK.
 * Empty string if nothing happened yet. */
void cmd_set_dry_run(bool on);
const wchar_t *cmd_last_action(void);
void cmd_run(const wchar_t *text, CmdResult *r);
void cmd_pick(const Row *row, PickMode mode, CmdResult *r);
/* WM_APP_TASK arrived: turns the DocResult into a reply + rows (caller frees d) */
struct DocResult;
void cmd_task_done(const struct DocResult *d, CmdResult *r);
/* Live results while typing; returns number of rows */
int  cmd_preview(const wchar_t *text, Row *rows, int max);
/* phrases for the voice command grammar */
typedef void (*PhraseFn)(const wchar_t *verb, const wchar_t *name, void *ctx);
void cmd_grammar(PhraseFn fn, void *ctx);

/* ------------------------------------------------------------------ docs.c */
/* Everyday document jobs. Conversions go through Microsoft Office (COM),
 * PDF page work through Python + PyMuPDF (hidden child process). Outputs
 * never overwrite: "a.pdf" becomes "a (2).pdf" when the name is taken. */
typedef enum {
    DOC_NONE, DOC_TO_PDF, DOC_TO_DOCX, DOC_SPLIT, DOC_EXTRACT, DOC_MERGE, DOC_PAGES,
    DOC_RENAME, DOC_MOVE, DOC_COPY, DOC_DELETE
} DocOp;

#define DOC_MAX_SRC 8

typedef struct {
    DocOp   op;
    int     nsrc;
    wchar_t src[DOC_MAX_SRC][MAX_PATH];
    wchar_t dst[2][MAX_PATH];   /* wanted output path(s); made unique when run */
    int     a, b;               /* split: a = last page of part 1; extract: pages a..b */
} DocTask;

typedef struct DocResult {
    DocOp   op;
    bool    ok;
    wchar_t reply[300];
    int     nout;
    wchar_t out[2][MAX_PATH];   /* files produced */
} DocResult;

/* free name: the path itself, or "a (2).pdf", "a (3).pdf" ... */
void docs_unique(const wchar_t *want, wchar_t *out);
/* runs the task on this thread (COM must be initialized for Office jobs) */
bool docs_run(const DocTask *t, DocResult *res);
/* runs it on a worker thread and posts WM_APP_TASK (lParam = malloc'd
 * DocResult, receiver frees) to notify. false if a task is already running. */
bool docs_run_async(const DocTask *t, HWND notify);
bool docs_busy(void);
void docs_wait(DWORD ms);
/* why this path must not be deleted (drive root, user folder...), or NULL */
const wchar_t *docs_protected(const wchar_t *path);
/* files selected in the Explorer window / Desktop fg; returns the count */
int  shell_selection(HWND fg, wchar_t (*out)[MAX_PATH], int max);

/* --------------------------------------------------------------------- ui.c */
/* initial_cmd: optional command to run right away (from the command line).
 * background: start hidden (no window; listen + speak, show a small
 * non-activating "listening" pill; the bar appears only when needed). */
bool ui_create(HINSTANCE inst, HWND dialog, bool listen, const wchar_t *initial_cmd, bool background);
void ui_toggle_listen(void);
int  ui_loop(void);

#endif

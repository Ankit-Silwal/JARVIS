/* Whisper speech recognition (offline, much more accurate than the old
 * Windows desktop recognizer, especially with accents).
 *
 * whisper.cpp's whisper-cli.exe + a ggml model live in
 * %LOCALAPPDATA%\JARVIS\whisper (tools\get_whisper.ps1 downloads them).
 * JARVIS records the microphone itself, finds where the user starts and
 * stops talking (vad_frame below), writes that piece to a small WAV file and
 * runs whisper-cli on it in a hidden process. The prompt lists command words
 * and the names from jarvis.ini so Whisper spells them the way JARVIS
 * expects ("vs code", "gmail", "downloads").
 *
 * Everything here is plain functions; voice.c drives them from its thread. */
#include "jarvis.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wctype.h>

/* --------------------------------------------------------------- install */
/* models, best first (small.en copes with accents best; base.en is faster) */
static const wchar_t *const MODELS[] = {
    L"ggml-small.en-q5_1.bin", L"ggml-small.en.bin", L"ggml-base.en.bin", L"ggml-base.en-q5_1.bin",
    L"ggml-small-q5_1.bin", L"ggml-small.bin", L"ggml-base.bin", L"ggml-tiny.en.bin", L"ggml-tiny.bin", NULL };

void whisper_dir(wchar_t *out)
{
    if (g_cfg.whisper_dir[0]) str_copy(out, MAX_PATH, g_cfg.whisper_dir);
    else swprintf(out, MAX_PATH, L"%ls\\whisper", g_cfg.data_dir);
}

bool whisper_find(wchar_t *exe, wchar_t *model)
{
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    whisper_dir(dir);
    exe[0] = model[0] = 0;
    const wchar_t *const EXES[] = { L"bin\\whisper-cli.exe", L"whisper-cli.exe", L"bin\\Release\\whisper-cli.exe", NULL };
    for (int i = 0; EXES[i]; i++) {
        swprintf(p, MAX_PATH, L"%ls\\%ls", dir, EXES[i]);
        if (path_exists(p)) { str_copy(exe, MAX_PATH, p); break; }
    }
    if (!exe[0]) return false;
    if (g_cfg.whisper_model[0]) {   /* [voice] model= : a file name in the folder or a full path */
        if (wcschr(g_cfg.whisper_model, L'\\')) str_copy(p, MAX_PATH, g_cfg.whisper_model);
        else swprintf(p, MAX_PATH, L"%ls\\%ls", dir, g_cfg.whisper_model);
        if (path_exists(p)) { str_copy(model, MAX_PATH, p); return true; }
        log_line(L"whisper: model %ls not found, picking one", p);
    }
    for (int i = 0; MODELS[i]; i++) {
        swprintf(p, MAX_PATH, L"%ls\\%ls", dir, MODELS[i]);
        if (path_exists(p)) { str_copy(model, MAX_PATH, p); return true; }
    }
    return false;
}

/* ------------------------------------------------------------------- VAD */
static double frame_rms(const short *f)
{
    double sum = 0;
    for (int i = 0; i < VAD_FRAME; i++) sum += (double)f[i] * f[i];
    return sqrt(sum / VAD_FRAME);
}

void vad_reset(Vad *v)
{
    memset(v, 0, sizeof *v);
}

#define CALIB_FRAMES   8      /* 240 ms to learn the room noise */
#define START_FRAMES   3      /* 90 ms loud = speech started */
#define END_FRAMES     25     /* 750 ms quiet = done talking */
#define MIN_VOICED     7      /* < 210 ms of voice = a click or a cough: ignore */
#define MAX_FRAMES     400    /* 12 s: stop there */
#define PREROLL_FRAMES 10     /* keep 300 ms before the start (soft first consonant) */

int vad_frame(Vad *v, const short *f)
{
    double rms = frame_rms(f);
    double db = rms > 0 ? 20.0 * log10(rms / 32768.0) : -96.0;
    int lv = (int)((db + 55.0) * 100.0 / 45.0);
    v->level = lv < 0 ? 0 : lv > 100 ? 100 : lv;
    int i = v->frames++;

    if (i < CALIB_FRAMES) {
        /* the quietest frame, so speech right at the start doesn't raise it */
        if (i == 0 || rms < v->floor) v->floor = rms;
        if (v->floor < 20) v->floor = 20;
        return VAD_NONE;
    }
    double start_t = v->floor * 3.0, cont_t = v->floor * 2.0;
    if (start_t < 200) start_t = 200;
    if (cont_t < 140) cont_t = 140;

    if (!v->speech) {
        if (rms >= start_t) {
            if (++v->above >= START_FRAMES) {
                v->speech = true;
                v->voiced = v->above;
                v->silent = 0;
                v->start = i - (START_FRAMES - 1) - PREROLL_FRAMES;
                if (v->start < 0) v->start = 0;
                return VAD_START;
            }
        } else {
            v->above = 0;
            /* follow the room: quickly down, slowly up */
            if (rms < v->floor) v->floor = v->floor * 0.7 + rms * 0.3;
            else v->floor = v->floor * 0.995 + rms * 0.005;
            if (v->floor < 20) v->floor = 20;
            if (v->floor > 2000) v->floor = 2000;
        }
        return VAD_NONE;
    }
    if (rms >= cont_t) { v->voiced++; v->silent = 0; }
    else v->silent++;
    if (v->silent >= END_FRAMES || i - v->start >= MAX_FRAMES) {
        v->speech = false;
        v->above = 0;
        if (v->voiced < MIN_VOICED) return VAD_DISCARD;
        v->end = i - v->silent + 8;   /* keep 240 ms of the tail */
        if (v->end > i + 1) v->end = i + 1;
        return VAD_END;
    }
    return VAD_NONE;
}

/* ------------------------------------------------------------------- WAV */
bool wav_write(const wchar_t *path, const short *pcm, int n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD data = (DWORD)n * 2, wr;
    struct {
        char riff[4]; DWORD size; char wave[4]; char fmt[4]; DWORD fsize;
        WORD tag, ch; DWORD rate, bps; WORD align, bits; char dat[4]; DWORD dsize;
    } hd = { {'R','I','F','F'}, 36 + data, {'W','A','V','E'}, {'f','m','t',' '}, 16,
             1, 1, 16000, 32000, 2, 16, {'d','a','t','a'}, data };
    bool ok = WriteFile(h, &hd, sizeof hd, &wr, NULL) && wr == sizeof hd &&
              WriteFile(h, pcm, data, &wr, NULL) && wr == data;
    CloseHandle(h);
    return ok;
}

/* Reads a 16 kHz mono 16-bit PCM WAV (what JARVIS writes and the tests use).
 * Returns malloc'd samples, *n = count; NULL if the format is different. */
short *wav_read(const wchar_t *path, int *n)
{
    *n = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL), rd;
    unsigned char *buf = size > 44 && size < 64u * 1024 * 1024 ? malloc(size) : NULL;
    bool ok = buf && ReadFile(h, buf, size, &rd, NULL) && rd == size;
    CloseHandle(h);
    if (!ok || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { free(buf); return NULL; }
    short *out = NULL;
    bool fmt_ok = false;
    for (DWORD p = 12; p + 8 <= size;) {
        DWORD len;
        memcpy(&len, buf + p + 4, 4);
        if (!memcmp(buf + p, "fmt ", 4) && len >= 16 && p + 8 + 16 <= size) {
            WORD tag, ch, bits; DWORD rate;
            memcpy(&tag, buf + p + 8, 2); memcpy(&ch, buf + p + 10, 2);
            memcpy(&rate, buf + p + 12, 4); memcpy(&bits, buf + p + 22, 2);
            fmt_ok = tag == 1 && ch == 1 && rate == 16000 && bits == 16;
        } else if (!memcmp(buf + p, "data", 4) && fmt_ok) {
            if (len > size - p - 8) len = size - p - 8;
            out = malloc(len ? len : 2);
            if (out) { memcpy(out, buf + p + 8, len); *n = (int)(len / 2); }
            break;
        }
        p += 8 + len + (len & 1);
    }
    free(buf);
    return out;
}

/* ---------------------------------------------------------------- prompt */
/* Whisper's "initial prompt": examples of the commands plus the names the
 * user says. Biases spelling and vocabulary without forcing anything. */
static void add_name(wchar_t *out, size_t cap, const wchar_t *name)
{
    if (!name[0] || wcslen(out) + wcslen(name) + 3 >= cap) return;
    for (const wchar_t *c = name; *c; c++) if (*c == L'"' || *c == L'\\' || *c < L' ') return;
    if (wcsstr(out, name)) return;
    str_cat(out, cap, name);
    str_cat(out, cap, L", ");
}

void whisper_prompt(wchar_t *out, size_t cap)
{
    str_copy(out, cap, L"Jarvis commands: open downloads. find physics notes. open latest photo. "
                       L"google black holes. youtube lofi music. convert my resume to pdf. "
                       L"split notes at page 5. mute jarvis. what time is it. number two. yes. Names: ");
    const KVList *lists[] = { &g_cfg.apps, &g_cfg.sites, &g_cfg.folders };
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < lists[l]->count; i++) add_name(out, cap, lists[l]->items[i].key);
    size_t n = wcslen(out);
    if (n >= 2 && out[n - 2] == L',') { out[n - 2] = L'.'; out[n - 1] = 0; }
}

/* ----------------------------------------------------------- transcribe */
/* per process, so a --transcribe or a test never trips over the running JARVIS */
static wchar_t s_wav[MAX_PATH], s_out[MAX_PATH];

static int threads(void)
{
    if (g_cfg.whisper_threads > 0) return g_cfg.whisper_threads;
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n >= 8 ? 8 : n >= 2 ? (int)n : 2;
}

HANDLE whisper_start(const short *pcm, int n, const wchar_t *exe, const wchar_t *model, const wchar_t *prompt)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *wav = s_wav;
    str_copy(dir, MAX_PATH, g_cfg.data_dir);
    swprintf(s_wav, MAX_PATH, L"%ls\\utterance_%lu.wav", dir, GetCurrentProcessId());
    swprintf(s_out, MAX_PATH, L"%ls\\utterance_%lu.txt", dir, GetCurrentProcessId());
    if (!wav_write(wav, pcm, n)) { log_line(L"whisper: can't write %ls", wav); return NULL; }

    /* audio context sized to the clip (Whisper pads everything to 30 s
     * otherwise): ~3x faster for short commands. 50 frames per second. */
    int ac = ((n / 16000.0) * 50 + 128 + 63) / 64 * 64;
    if (ac < 384) ac = 384;
    if (ac > 1500) ac = 1500;

    static wchar_t cmd[4096];
    swprintf(cmd, 4096, L"\"%ls\" -m \"%ls\" -f \"%ls\" -l %ls -nt -np -sns -t %d -ac %d --prompt \"%ls\"",
             exe, model, wav, g_cfg.whisper_lang[0] ? g_cfg.whisper_lang : L"en", threads(), ac,
             prompt ? prompt : L"");

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE out = CreateFileW(s_out, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        log_line(L"whisper: can't create %ls (%lu)", s_out, GetLastError());
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return NULL;
    }
    STARTUPINFOW si = { .cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES,
                        .hStdInput = NULL, .hStdOutput = out, .hStdError = nul != INVALID_HANDLE_VALUE ? nul : out };
    PROCESS_INFORMATION pi;
    wchar_t bindir[MAX_PATH];
    str_copy(bindir, MAX_PATH, exe);
    wchar_t *sl = wcsrchr(bindir, L'\\');
    if (sl) *sl = 0;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, bindir, &si, &pi);
    CloseHandle(out);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { log_line(L"whisper: can't start %ls (%lu)", exe, GetLastError()); return NULL; }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

/* Text cleanup: drops "[BLANK_AUDIO]" / "(music)" style tags, joins lines.
 * false when nothing usable is left (or it's a typical Whisper "hallucination"
 * on noise like "Thank you." / "you"). */
bool whisper_clean(wchar_t *t)
{
    wchar_t *w = t;
    int depth = 0;
    for (wchar_t *r = t; *r; r++) {
        if (*r == L'[' || *r == L'(' || *r == L'*') { if (*r == L'*') depth = !depth; else depth++; continue; }
        if (*r == L']' || *r == L')') { if (depth > 0) depth--; continue; }
        if (depth) continue;
        *w++ = (*r == L'\r' || *r == L'\n' || *r == L'\t') ? L' ' : *r;
    }
    *w = 0;
    str_collapse_spaces(t);
    str_trim(t);
    /* "15%" -> "15 percent" (the command parser drops symbols) */
    wchar_t buf[LINE_MAX_CH] = L"";
    for (wchar_t *r = t; *r; r++) {
        wchar_t c[2] = { *r, 0 };
        if (*r == L'%') str_cat(buf, LINE_MAX_CH, L" percent");
        else str_cat(buf, LINE_MAX_CH, c);
    }
    str_copy(t, LINE_MAX_CH, buf);

    wchar_t low[LINE_MAX_CH];
    int n = 0;
    for (wchar_t *r = t; *r && n < LINE_MAX_CH - 1; r++)
        if (iswalnum(*r) || *r == L' ') low[n++] = (wchar_t)towlower(*r);
    low[n] = 0;
    str_trim(low);
    static const wchar_t *const JUNK[] = { L"", L"you", L"thank you", L"thanks for watching",
        L"thank you for watching", L"thank you so much for watching", L"subtitles by the amaraorg community", NULL };
    for (int i = 0; JUNK[i]; i++) if (str_eq(low, JUNK[i])) return false;
    return true;
}

bool whisper_finish(HANDLE proc, wchar_t *out, size_t cap)
{
    out[0] = 0;
    DWORD code = 1;
    if (proc) { GetExitCodeProcess(proc, &code); CloseHandle(proc); }
    /* keep what was heard for troubleshooting: jarvis.exe --transcribe <data>\last_utterance.wav */
    wchar_t last[MAX_PATH];
    swprintf(last, MAX_PATH, L"%ls\\last_utterance.wav", g_cfg.data_dir);
    MoveFileExW(s_wav, last, MOVEFILE_REPLACE_EXISTING);
    if (code != 0) { log_line(L"whisper: exit code %lu", code); DeleteFileW(s_out); return false; }
    HANDLE h = CreateFileW(s_out, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[4096];
    DWORD rd = 0;
    ReadFile(h, buf, sizeof buf - 1, &rd, NULL);
    CloseHandle(h);
    DeleteFileW(s_out);
    buf[rd] = 0;
    wchar_t *w = wide_from_utf8(buf, (int)rd);
    if (!w) return false;
    str_copy(out, cap, w);
    free(w);
    return whisper_clean(out);
}

/* runs it to the end on this thread (--transcribe, tests) */
bool whisper_run(const short *pcm, int n, wchar_t *out, size_t cap, DWORD timeout_ms)
{
    wchar_t exe[MAX_PATH], model[MAX_PATH], prompt[1024];
    out[0] = 0;
    if (!whisper_find(exe, model)) return false;
    whisper_prompt(prompt, 1024);
    HANDLE p = whisper_start(pcm, n, exe, model, prompt);
    if (!p) return false;
    if (WaitForSingleObject(p, timeout_ms) != WAIT_OBJECT_0) {
        TerminateProcess(p, 1);
        CloseHandle(p);
        log_line(L"whisper: timed out");
        return false;
    }
    return whisper_finish(p, out, cap);
}

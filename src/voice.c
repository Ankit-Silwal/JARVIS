/* Voice: speech recognition + text-to-speech. No internet.
 *
 * Recognition, best first:
 *   - Whisper (whisper.cpp + a ggml model in %LOCALAPPDATA%\JARVIS\whisper,
 *     see whisper.c): JARVIS records the microphone with waveIn, a small
 *     voice-activity detector cuts out what was said, and whisper-cli.exe
 *     transcribes it in a hidden process (~1 s). Far more accurate than the
 *     old Windows recognizer, especially with accents.
 *   - The Microsoft Speech API (SAPI 5) recognizer that ships with Windows,
 *     when Whisper isn't installed (or [voice] engine=windows).
 * Text-to-speech is always SAPI (the David / Zira voices).
 *
 * Everything lives on one worker thread so the overlay stays smooth while
 * the recognizer starts up. The UI talks to it with PostThreadMessage; it
 * answers with WM_APP_VOICE messages. */
#define COBJMACROS
#include "jarvis.h"
#include <initguid.h>
#include <sapi.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <wctype.h>

#define VM_LISTEN (WM_APP + 50)
#define VM_SAY    (WM_APP + 51)
#define VM_QUIT   (WM_APP + 52)
#define VM_HUSH   (WM_APP + 53)   /* stop speaking now; wParam: listen afterwards */

#define LISTEN_MS        9000   /* give up when nothing was said for this long */
#define LISTEN_EXTEND_MS 4000   /* extra time once speech started (SAPI) */
#define UTTERANCE_MS    16000   /* Whisper: an utterance may take this long at most */
#define WHISPER_WAIT_MS 20000   /* a transcription that takes longer is broken */

/* SPFEI() in the SDK header uses an MSVC-only literal; same thing, portable: */
#define FEI(e) ((1ULL << (e)) | (1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2))

static HWND   g_notify;
static HANDLE g_thread, g_qready;
static DWORD  g_tid;
static volatile LONG g_ok;   /* recognizer available */
static wchar_t g_engine_name[80];

/* worker-thread state */
static ISpRecognizer  *g_rec;
static ISpRecoContext *g_ctx;
static ISpRecoGrammar *g_dict, *g_cmds;
static ISpVoice       *g_tts;
static bool  g_listening, g_listen_after_speech, g_speaking;
static DWORD g_deadline;

/* Whisper path */
#define NBUF 8
#define PCM_CAP (16000 * UTTERANCE_MS / 1000)
static bool    g_use_whisper;
static wchar_t g_wexe[MAX_PATH], g_wmodel[MAX_PATH], g_wprompt[1024];
static HWAVEIN g_wave;
static HANDLE  g_wave_ev;
static WAVEHDR g_hdr[NBUF];
static short   g_buf[NBUF][VAD_FRAME];
static short   g_pcm[PCM_CAP];
static int     g_npcm;
static Vad     g_vad;
static HANDLE  g_wproc;                 /* whisper-cli running on the last utterance */
static DWORD   g_wstart;
static bool    g_listen_after_whisper;  /* listen was asked for while transcribing */

static void post(VoiceEvent e, LPARAM lp) { PostMessageW(g_notify, WM_APP_VOICE, (WPARAM)e, lp); }

/* VE_HYPOTHESIS / VE_RESULT carry a heap string the receiver frees */
static void post_text(VoiceEvent e, wchar_t *t)
{
    if (!PostMessageW(g_notify, WM_APP_VOICE, (WPARAM)e, (LPARAM)t)) free(t);
}

static void free_event(SPEVENT *ev)
{
    switch (ev->elParamType) {
    case SPET_LPARAM_IS_POINTER:
    case SPET_LPARAM_IS_STRING:
        CoTaskMemFree((void *)ev->lParam); break;
    case SPET_LPARAM_IS_TOKEN:
    case SPET_LPARAM_IS_OBJECT:
        if (ev->lParam) IUnknown_Release((IUnknown *)ev->lParam);
        break;
    default: break;
    }
}

/* ------------------------------------------------------------ microphone */
static bool mic_open(void)
{
    if (g_wave) return true;
    if (!g_wave_ev) g_wave_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
    MMRESULT r = waveInOpen(&g_wave, WAVE_MAPPER, &wf, (DWORD_PTR)g_wave_ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        log_line(L"mic: waveInOpen failed (%u)", r);
        g_wave = NULL;
        return false;
    }
    for (int i = 0; i < NBUF; i++) {
        memset(&g_hdr[i], 0, sizeof(WAVEHDR));
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = VAD_FRAME * sizeof(short);
        waveInPrepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    }
    waveInStart(g_wave);
    return true;
}

static void mic_close(void)
{
    if (!g_wave) return;
    waveInStop(g_wave);
    waveInReset(g_wave);
    for (int i = 0; i < NBUF; i++) waveInUnprepareHeader(g_wave, &g_hdr[i], sizeof(WAVEHDR));
    waveInClose(g_wave);
    g_wave = NULL;
}

static void set_listen(bool on)
{
    if (on == g_listening) return;
    if (g_use_whisper) {
        if (on) {
            if (g_wproc) { g_listen_after_whisper = true; return; }   /* after the result */
            g_npcm = 0;
            vad_reset(&g_vad);
            if (!mic_open()) { post(VE_LISTENING, 0); return; }
        } else {
            mic_close();
        }
    } else {
        if (!g_rec) return;
        ISpRecognizer_SetRecoState(g_rec, on ? SPRST_ACTIVE : SPRST_INACTIVE);
    }
    g_listening = on;
    if (on) g_deadline = GetTickCount() + LISTEN_MS;
    post(VE_LISTENING, on);
}

/* the utterance is in g_pcm[from..to): stop the mic, start whisper-cli */
static void transcribe(int from, int to)
{
    if (from < 0) from = 0;
    if (to > g_npcm) to = g_npcm;
    set_listen(false);
    if (to - from < VAD_FRAME * 4) { post(VE_NOMATCH, 0); return; }
    g_wproc = whisper_start(g_pcm + from, to - from, g_wexe, g_wmodel, g_wprompt);
    if (!g_wproc) { post(VE_NOMATCH, 0); return; }
    g_wstart = GetTickCount();
    post(VE_THINKING, 1);
}

static void pump_mic(void)
{
    if (!g_wave) return;
    for (int i = 0; i < NBUF; i++) {
        WAVEHDR *h = &g_hdr[i];
        if (!(h->dwFlags & WHDR_DONE)) continue;
        if (g_listening && h->dwBytesRecorded == VAD_FRAME * sizeof(short)) {
            const short *f = (const short *)h->lpData;
            bool full = g_npcm + VAD_FRAME > PCM_CAP;
            if (!full) { memcpy(g_pcm + g_npcm, f, VAD_FRAME * sizeof(short)); g_npcm += VAD_FRAME; }
            int ev = vad_frame(&g_vad, f);
            post(VE_LEVEL, g_vad.level);
            if (ev == VAD_START) {
                g_deadline = GetTickCount() + UTTERANCE_MS;
                log_line(L"mic: speech (room noise %.0f)", g_vad.floor);
            } else if (ev == VAD_END) {
                log_line(L"mic: utterance %d ms", (g_vad.end - g_vad.start) * 30);
                transcribe(g_vad.start * VAD_FRAME, g_vad.end * VAD_FRAME);
                return;
            } else if (ev == VAD_DISCARD) {
                log_line(L"mic: short noise ignored");
            } else if (full && g_vad.speech) {
                log_line(L"mic: utterance cut at %d s", UTTERANCE_MS / 1000);
                transcribe(g_vad.start * VAD_FRAME, g_npcm);
                return;
            }
        }
        if (!g_wave) return;          /* closed meanwhile */
        waveInAddBuffer(g_wave, h, sizeof(WAVEHDR));
    }
}

static void finish_transcribe(void)
{
    wchar_t text[LINE_MAX_CH];
    bool ok = whisper_finish(g_wproc, text, LINE_MAX_CH);
    g_wproc = NULL;
    post(VE_THINKING, 0);
    if (ok) {
        log_line(L"whisper: \"%ls\" (%lu ms)", text, GetTickCount() - g_wstart);
        post_text(VE_RESULT, wcs_dup(text));
    } else {
        log_line(L"whisper: nothing usable (\"%ls\", %lu ms)", text, GetTickCount() - g_wstart);
        post(VE_NOMATCH, 0);
    }
    if (g_listen_after_whisper) { g_listen_after_whisper = false; set_listen(true); }
}

static void kill_transcribe(void)
{
    if (!g_wproc) return;
    TerminateProcess(g_wproc, 1);
    CloseHandle(g_wproc);
    g_wproc = NULL;
    post(VE_THINKING, 0);
}

/* ---------------------------------------------------- command grammar */
typedef struct { SPSTATEHANDLE init, mid, single; int n; } GramCtx;

static void add_phrase(const wchar_t *verb, const wchar_t *name, void *p)
{
    GramCtx *g = p;
    HRESULT hr;
    if (verb && !name)      hr = ISpRecoGrammar_AddWordTransition(g_cmds, g->init, g->mid, verb, L" ", SPWT_LEXICAL, 1.0f, NULL);
    else if (!verb && name) hr = ISpRecoGrammar_AddWordTransition(g_cmds, g->mid, NULL, name, L" ", SPWT_LEXICAL, 1.0f, NULL);
    else if (verb && name) {
        wchar_t full[256];
        if (name[0]) swprintf(full, 256, L"%ls %ls", verb, name);
        else str_copy(full, 256, verb);
        hr = ISpRecoGrammar_AddWordTransition(g_cmds, g->single, NULL, full, L" ", SPWT_LEXICAL, 1.0f, NULL);
    } else return;
    if (SUCCEEDED(hr)) g->n++;
}

static void build_command_grammar(void)
{
    if (FAILED(ISpRecoContext_CreateGrammar(g_ctx, 2, &g_cmds))) { g_cmds = NULL; return; }
    GramCtx g = {0};
    if (FAILED(ISpRecoGrammar_GetRule(g_cmds, L"verbcmd", 0, SPRAF_TopLevel | SPRAF_Active | SPRAF_Dynamic, TRUE, &g.init)) ||
        FAILED(ISpRecoGrammar_CreateNewState(g_cmds, g.init, &g.mid)) ||
        FAILED(ISpRecoGrammar_GetRule(g_cmds, L"single", 0, SPRAF_TopLevel | SPRAF_Active | SPRAF_Dynamic, TRUE, &g.single))) {
        ISpRecoGrammar_Release(g_cmds); g_cmds = NULL; return;
    }
    cmd_grammar(add_phrase, &g);
    if (FAILED(ISpRecoGrammar_Commit(g_cmds, 0)) ||
        FAILED(ISpRecoGrammar_SetRuleState(g_cmds, NULL, NULL, SPRS_ACTIVE))) {
        ISpRecoGrammar_Release(g_cmds); g_cmds = NULL; return;
    }
    log_line(L"voice: command grammar with %d words", g.n);
}

/* --------------------------------------------------------------- init */
static bool init_sr(void)
{
    IUnknown *audio = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpInprocRecognizer, NULL, CLSCTX_ALL, &IID_ISpRecognizer, (void **)&g_rec))) goto fail;
    if (FAILED(CoCreateInstance(&CLSID_SpMMAudioIn, NULL, CLSCTX_ALL, &IID_IUnknown, (void **)&audio))) goto fail;
    if (FAILED(ISpRecognizer_SetInput(g_rec, audio, TRUE))) goto fail;
    IUnknown_Release(audio); audio = NULL;
    if (FAILED(ISpRecognizer_CreateRecoContext(g_rec, &g_ctx))) goto fail;

    ULONGLONG want = FEI(SPEI_RECOGNITION) | FEI(SPEI_FALSE_RECOGNITION) | FEI(SPEI_HYPOTHESIS) |
                     FEI(SPEI_SOUND_START) | FEI(SPEI_SR_AUDIO_LEVEL);
    if (FAILED(ISpRecoContext_SetInterest(g_ctx, want, want))) goto fail;
    if (FAILED(ISpRecoContext_SetNotifyWin32Event(g_ctx))) goto fail;

    if (FAILED(ISpRecoContext_CreateGrammar(g_ctx, 1, &g_dict))) goto fail;
    if (FAILED(ISpRecoGrammar_LoadDictation(g_dict, NULL, SPLO_STATIC))) goto fail;
    if (FAILED(ISpRecoGrammar_SetDictationState(g_dict, SPRS_ACTIVE))) goto fail;
    build_command_grammar();   /* optional: improves accuracy for fixed phrases */

    ISpRecognizer_SetRecoState(g_rec, SPRST_INACTIVE);
    return true;
fail:
    if (audio) IUnknown_Release(audio);
    if (g_dict) { ISpRecoGrammar_Release(g_dict); g_dict = NULL; }
    if (g_ctx) { ISpRecoContext_Release(g_ctx); g_ctx = NULL; }
    if (g_rec) { ISpRecognizer_Release(g_rec); g_rec = NULL; }
    log_line(L"voice: speech recognition unavailable");
    return false;
}

/* Whisper when installed (and not switched off): checks the mic opens too */
static bool init_whisper(void)
{
    if (g_cfg.engine == ENGINE_WINDOWS) return false;
    if (!whisper_find(g_wexe, g_wmodel)) {
        wchar_t dir[MAX_PATH];
        whisper_dir(dir);
        log_line(L"voice: Whisper not installed in %ls (tools\\get_whisper.ps1)%ls", dir,
                 g_cfg.engine == ENGINE_WHISPER ? L", using the Windows recognizer" : L"");
        return false;
    }
    if (!mic_open()) return false;
    mic_close();
    whisper_prompt(g_wprompt, 1024);
    wchar_t m[64];
    str_copy(m, 64, path_name(g_wmodel));
    if (str_starts(m, L"ggml-")) memmove(m, m + 5, (wcslen(m + 5) + 1) * sizeof(wchar_t));
    wchar_t *dot = wcsrchr(m, L'.');
    if (dot && !_wcsicmp(dot, L".bin")) *dot = 0;
    swprintf(g_engine_name, 80, L"Whisper %ls", m);
    log_line(L"voice: %ls (%ls)", g_engine_name, g_wmodel);
    return true;
}

static void pick_voice(void)
{
    if (!g_cfg.voice_name[0]) return;
    ISpObjectTokenCategory *cat = NULL;
    IEnumSpObjectTokens *en = NULL;
    if (FAILED(CoCreateInstance(&CLSID_SpObjectTokenCategory, NULL, CLSCTX_ALL, &IID_ISpObjectTokenCategory, (void **)&cat))) return;
    if (SUCCEEDED(ISpObjectTokenCategory_SetId(cat, SPCAT_VOICES, FALSE)) &&
        SUCCEEDED(ISpObjectTokenCategory_EnumTokens(cat, NULL, NULL, &en))) {
        wchar_t want[64];
        str_copy(want, 64, g_cfg.voice_name);
        str_lower(want);
        ISpObjectToken *tok;
        ULONG got;
        while (IEnumSpObjectTokens_Next(en, 1, &tok, &got) == S_OK && got) {
            WCHAR *desc = NULL;
            bool match = false;
            if (SUCCEEDED(ISpObjectToken_GetStringValue(tok, NULL, &desc)) && desc) {
                str_lower(desc);
                match = wcsstr(desc, want) != NULL;
                CoTaskMemFree(desc);
            }
            if (match) { ISpVoice_SetVoice(g_tts, tok); ISpObjectToken_Release(tok); break; }
            ISpObjectToken_Release(tok);
        }
        IEnumSpObjectTokens_Release(en);
    }
    ISpObjectTokenCategory_Release(cat);
}

static bool init_tts(void)
{
    if (FAILED(CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void **)&g_tts))) { g_tts = NULL; return false; }
    ULONGLONG want = FEI(SPEI_END_INPUT_STREAM);
    ISpVoice_SetInterest(g_tts, want, want);
    ISpVoice_SetNotifyWin32Event(g_tts);
    pick_voice();
    int rate = g_cfg.voice_rate;
    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    ISpVoice_SetRate(g_tts, rate);
    return true;
}

/* ------------------------------------------------------------ events */
static wchar_t *result_text(SPEVENT *ev)
{
    ISpRecoResult *res = (ISpRecoResult *)ev->lParam;
    if (!res) return NULL;
    WCHAR *txt = NULL;
    if (FAILED(ISpRecoResult_GetText(res, SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &txt, NULL)) || !txt)
        return NULL;
    wchar_t *copy = wcs_dup(txt);
    CoTaskMemFree(txt);
    return copy;
}

static void pump_sr(void)
{
    SPEVENT ev;
    ULONG got = 0;
    while (SUCCEEDED(ISpRecoContext_GetEvents(g_ctx, 1, &ev, &got)) && got) {
        switch (ev.eEventId) {
        case SPEI_SOUND_START:
            if (g_listening) g_deadline = GetTickCount() + LISTEN_MS;
            break;
        case SPEI_SR_AUDIO_LEVEL:
            if (g_listening) post(VE_LEVEL, (LPARAM)ev.wParam);
            break;
        case SPEI_HYPOTHESIS:
            if (g_listening) {
                g_deadline = GetTickCount() + LISTEN_EXTEND_MS;
                wchar_t *t = result_text(&ev);
                if (t) post_text(VE_HYPOTHESIS, t);
            }
            break;
        case SPEI_RECOGNITION:
            if (g_listening) {
                wchar_t *t = result_text(&ev);
                set_listen(false);
                if (t) post_text(VE_RESULT, t); else post(VE_NOMATCH, 0);
            }
            break;
        case SPEI_FALSE_RECOGNITION:
            if (g_listening) post(VE_NOMATCH, 0);
            break;
        default: break;
        }
        free_event(&ev);
    }
}

static void pump_tts(void)
{
    SPEVENT ev;
    ULONG got = 0;
    while (SUCCEEDED(ISpVoice_GetEvents(g_tts, 1, &ev, &got)) && got) {
        if (ev.eEventId == SPEI_END_INPUT_STREAM) {
            SPVOICESTATUS st;
            if (!g_speaking) { free_event(&ev); continue; }   /* already hushed */
            if (SUCCEEDED(ISpVoice_GetStatus(g_tts, &st, NULL)) && st.dwRunningState != SPRS_DONE) {
                free_event(&ev);
                continue;   /* another sentence still queued */
            }
            g_speaking = false;
            post(VE_SPOKEN, 0);
            if (g_listen_after_speech) { g_listen_after_speech = false; set_listen(true); }
        }
        free_event(&ev);
    }
}

static void do_say(wchar_t *text, bool listen_after)
{
    if (g_listening) set_listen(false);   /* never listen to ourselves */
    g_listen_after_speech = listen_after;
    if (g_tts && text && text[0]) {
        g_speaking = true;
        if (FAILED(ISpVoice_Speak(g_tts, text, SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML, NULL))) {
            g_speaking = false;
        }
    }
    if (!g_speaking) {
        post(VE_SPOKEN, 0);
        if (g_listen_after_speech) { g_listen_after_speech = false; set_listen(true); }
    }
    free(text);
}

/* cut the current sentence (and anything queued) short */
static void do_hush(bool listen)
{
    if (g_tts && g_speaking) ISpVoice_Speak(g_tts, NULL, SPF_ASYNC | SPF_PURGEBEFORESPEAK, NULL);
    bool was = g_speaking;
    g_speaking = false;
    bool again = listen || g_listen_after_speech;
    g_listen_after_speech = false;
    if (was) post(VE_SPOKEN, 0);
    if (again) set_listen(true);
}

static DWORD WINAPI voice_thread(LPVOID unused)
{
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* create the queue */
    SetEvent(g_qready);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    init_tts();
    bool sr;
    if (init_whisper()) {
        g_use_whisper = true;
        sr = true;
    } else {
        sr = init_sr();
        str_copy(g_engine_name, 80, L"Windows");
    }
    InterlockedExchange(&g_ok, sr);
    post(VE_READY, sr);

    for (;;) {
        /* checked every pass: with the mic on, level events wake us
         * constantly, so WAIT_TIMEOUT alone may never be seen */
        if (g_listening && (int)(GetTickCount() - g_deadline) >= 0) {
            if (g_use_whisper) log_line(L"mic: nothing heard%ls", g_vad.speech ? L" to the end" : L"");
            set_listen(false);
            post(VE_TIMEOUT, 0);
        }
        if (g_wproc && GetTickCount() - g_wstart > WHISPER_WAIT_MS) {
            log_line(L"whisper: timed out");
            kill_transcribe();
            post(VE_NOMATCH, 0);
        }
        DWORD timeout = INFINITE;
        if (g_listening) {
            DWORD now = GetTickCount();
            timeout = (int)(g_deadline - now) > 0 ? g_deadline - now : 0;
        } else if (g_wproc) {
            timeout = 500;
        }
        HANDLE hs[4];
        int nh = 0, i_sr = -1, i_tts = -1, i_mic = -1, i_w = -1;
        if (g_ctx)  { i_sr = nh;  hs[nh++] = ISpRecoContext_GetNotifyEventHandle(g_ctx); }
        if (g_tts)  { i_tts = nh; hs[nh++] = ISpVoice_GetNotifyEventHandle(g_tts); }
        if (g_wave) { i_mic = nh; hs[nh++] = g_wave_ev; }
        if (g_wproc) { i_w = nh;  hs[nh++] = g_wproc; }
        /* MWMO_INPUTAVAILABLE: also wake for messages already in the queue */
        DWORD w = MsgWaitForMultipleObjectsEx(nh, hs, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (w == WAIT_TIMEOUT) continue;
        /* no `continue`: a busy handle must not starve the message queue */
        if (i_sr >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_sr) pump_sr();
        if (i_tts >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_tts) pump_tts();
        if (i_mic >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_mic) pump_mic();
        if (i_w >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_w) finish_transcribe();

        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            switch (m.message) {
            case VM_LISTEN:
                if (m.wParam && g_speaking) g_listen_after_speech = true;
                else set_listen(m.wParam != 0);
                if (!m.wParam) g_listen_after_whisper = false;
                break;
            case VM_SAY:
                do_say((wchar_t *)m.lParam, m.wParam != 0);
                break;
            case VM_HUSH:
                do_hush(m.wParam != 0);
                break;
            case VM_QUIT:
                set_listen(false);
                kill_transcribe();
                if (g_tts && m.wParam) ISpVoice_WaitUntilDone(g_tts, (ULONG)m.wParam);
                goto done;
            default:
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
done:
    mic_close();
    if (g_wave_ev) CloseHandle(g_wave_ev);
    if (g_cmds) ISpRecoGrammar_Release(g_cmds);
    if (g_dict) ISpRecoGrammar_Release(g_dict);
    if (g_ctx) ISpRecoContext_Release(g_ctx);
    if (g_rec) ISpRecognizer_Release(g_rec);
    if (g_tts) ISpVoice_Release(g_tts);
    CoUninitialize();
    return 0;
}

bool voice_start(HWND notify)
{
    g_notify = notify;
    g_qready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, voice_thread, NULL, 0, &g_tid);
    if (!g_thread) return false;
    WaitForSingleObject(g_qready, 2000);
    return true;
}

bool voice_available(void) { return g_ok != 0; }

const wchar_t *voice_engine_name(void) { return g_engine_name; }

void voice_listen(bool on)
{
    if (g_tid) PostThreadMessageW(g_tid, VM_LISTEN, on, 0);
}

void voice_say(const wchar_t *text, bool listen_after)
{
    if (!g_tid) return;
    wchar_t *copy = wcs_dup(g_cfg.speak && text ? text : L"");
    if (!PostThreadMessageW(g_tid, VM_SAY, listen_after, (LPARAM)copy)) free(copy);
}

void voice_hush(bool listen_after)
{
    if (g_tid) PostThreadMessageW(g_tid, VM_HUSH, listen_after, 0);
}

void voice_stop(DWORD wait_ms)
{
    if (!g_thread) return;
    PostThreadMessageW(g_tid, VM_QUIT, wait_ms, 0);
    WaitForSingleObject(g_thread, wait_ms + 1500);
}

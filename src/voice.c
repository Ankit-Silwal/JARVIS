/* Voice: offline speech recognition + text-to-speech using the Microsoft
 * Speech API (SAPI 5) that ships with Windows. No downloads, no internet.
 *
 * Everything SAPI-related lives on one worker thread so the overlay stays
 * smooth while the recognizer starts up (~1 s). The UI talks to it with
 * PostThreadMessage; it answers with WM_APP_VOICE messages. */
#define COBJMACROS
#include "jarvis.h"
#include <initguid.h>
#include <sapi.h>
#include <stdlib.h>
#include <wctype.h>

#define VM_LISTEN (WM_APP + 50)
#define VM_SAY    (WM_APP + 51)
#define VM_QUIT   (WM_APP + 52)

#define LISTEN_MS        9000   /* give up when nothing was said for this long */
#define LISTEN_EXTEND_MS 4000   /* extra time once speech started */

/* SPFEI() in the SDK header uses an MSVC-only literal; same thing, portable: */
#define FEI(e) ((1ULL << (e)) | (1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2))

static HWND   g_notify;
static HANDLE g_thread, g_qready;
static DWORD  g_tid;
static volatile LONG g_ok;   /* recognizer available */

/* worker-thread state */
static ISpRecognizer  *g_rec;
static ISpRecoContext *g_ctx;
static ISpRecoGrammar *g_dict, *g_cmds;
static ISpVoice       *g_tts;
static bool  g_listening, g_listen_after_speech, g_speaking;
static DWORD g_deadline;

static void post(VoiceEvent e, LPARAM lp) { PostMessageW(g_notify, WM_APP_VOICE, (WPARAM)e, lp); }

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

static void set_listen(bool on)
{
    if (!g_rec) return;
    if (on == g_listening) return;
    ISpRecognizer_SetRecoState(g_rec, on ? SPRST_ACTIVE : SPRST_INACTIVE);
    g_listening = on;
    if (on) g_deadline = GetTickCount() + LISTEN_MS;
    post(VE_LISTENING, on);
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
                if (t) post(VE_HYPOTHESIS, (LPARAM)t);
            }
            break;
        case SPEI_RECOGNITION:
            if (g_listening) {
                wchar_t *t = result_text(&ev);
                set_listen(false);
                if (t) post(VE_RESULT, (LPARAM)t); else post(VE_NOMATCH, 0);
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

static DWORD WINAPI voice_thread(LPVOID unused)
{
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* create the queue */
    SetEvent(g_qready);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    init_tts();
    bool sr = init_sr();
    InterlockedExchange(&g_ok, sr);
    post(VE_READY, sr);

    HANDLE hs[2];
    int nh = 0, i_sr = -1, i_tts = -1;
    if (g_ctx) { i_sr = nh; hs[nh++] = ISpRecoContext_GetNotifyEventHandle(g_ctx); }
    if (g_tts) { i_tts = nh; hs[nh++] = ISpVoice_GetNotifyEventHandle(g_tts); }

    for (;;) {
        DWORD timeout = INFINITE;
        if (g_listening) {
            DWORD now = GetTickCount();
            timeout = (int)(g_deadline - now) > 0 ? g_deadline - now : 0;
        }
        DWORD w = MsgWaitForMultipleObjects(nh, hs, FALSE, timeout, QS_ALLINPUT);
        if (w == WAIT_TIMEOUT) {
            if (g_listening && (int)(GetTickCount() - g_deadline) >= 0) { set_listen(false); post(VE_TIMEOUT, 0); }
            continue;
        }
        if (i_sr >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_sr) { pump_sr(); continue; }
        if (i_tts >= 0 && w == WAIT_OBJECT_0 + (DWORD)i_tts) { pump_tts(); continue; }

        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            switch (m.message) {
            case VM_LISTEN:
                if (m.wParam && g_speaking) g_listen_after_speech = true;
                else set_listen(m.wParam != 0);
                break;
            case VM_SAY:
                do_say((wchar_t *)m.lParam, m.wParam != 0);
                break;
            case VM_QUIT:
                set_listen(false);
                if (g_tts && m.wParam) ISpVoice_WaitUntilDone(g_tts, (ULONG)m.wParam);
                goto done;
            default:
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
    }
done:
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

void voice_stop(DWORD wait_ms)
{
    if (!g_thread) return;
    PostThreadMessageW(g_tid, VM_QUIT, wait_ms, 0);
    WaitForSingleObject(g_thread, wait_ms + 1500);
}

/* srtest <desk|dnn> <dict|cfg> <wav>...: recognize WAV files, print text + confidence */
#define COBJMACROS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <initguid.h>
#include <sapi.h>
#include <stdio.h>
static CLSID C_REC, C_TOK, C_STREAM;
static void clsids(int one){
 if(one){CLSIDFromString(L"{8A44FFB7-8140-432F-BEC5-1B5FF725BAC8}",&C_REC);CLSIDFromString(L"{9FF65B15-F7B4-4858-BFE6-DB2083DEDF68}",&C_TOK);CLSIDFromString(L"{0DD1B55E-2EB8-42E2-B8D0-F11594A29477}",&C_STREAM);}
 else {C_REC=CLSID_SpInprocRecognizer;C_TOK=CLSID_SpObjectToken;C_STREAM=CLSID_SpStream;}}
static const GUID SPDFID_WaveFormatEx_ = {0xC31ADBAE,0x527F,0x4ff5,{0xA2,0x30,0xF6,0x2B,0xB6,0x1F,0xF7,0x0C}};

#define FEI(e) ((1ULL << (e)) | (1ULL << SPEI_RESERVED1) | (1ULL << SPEI_RESERVED2))
#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { wprintf(L"FAIL %hs: 0x%08lx\n", #x, _h); return 1; } } while (0)

static const wchar_t *VERBS[] = { L"open", L"find", L"google", L"youtube", L"convert", L"set a timer for", L"stop", NULL };

static const wchar_t *g_xml;
static int run(const wchar_t *eng, const wchar_t *mode, const wchar_t *wav)
{
    clsids(!wcscmp(eng, L"one"));
    ISpRecognizer *rec; ISpRecoContext *ctx; ISpRecoGrammar *g; ISpStream *st;
    CK(CoCreateInstance(&C_REC, NULL, CLSCTX_ALL, &IID_ISpRecognizer, (void **)&rec));
    if (wcscmp(eng, L"desk")) {
        ISpObjectToken *tok;
        CK(CoCreateInstance(&C_TOK, NULL, CLSCTX_ALL, &IID_ISpObjectToken, (void **)&tok));
        CK(ISpObjectToken_SetId(tok, NULL, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Recognizers\\Tokens\\MS-1033-110-WINMO-DNN", FALSE));
        CK(ISpRecognizer_SetRecognizer(rec, tok));
    }
    CK(CoCreateInstance(&C_STREAM, NULL, CLSCTX_ALL, &IID_ISpStream, (void **)&st));
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
    CK(ISpStream_BindToFile(st, wav, SPFM_OPEN_READONLY, &SPDFID_WaveFormatEx_, &wf, 0));
    CK(ISpRecognizer_SetInput(rec, (IUnknown *)st, TRUE));
    CK(ISpRecognizer_CreateRecoContext(rec, &ctx));
    ULONGLONG want = FEI(SPEI_RECOGNITION) | FEI(SPEI_FALSE_RECOGNITION) | FEI(SPEI_END_SR_STREAM);
    CK(ISpRecoContext_SetInterest(ctx, want, want));
    CK(ISpRecoContext_SetNotifyWin32Event(ctx));
    CK(ISpRecoContext_CreateGrammar(ctx, 1, &g));
    if (!wcscmp(mode, L"xml")) {
        CK(ISpRecoGrammar_LoadCmdFromFile(g, g_xml, SPLO_STATIC));
        CK(ISpRecoGrammar_SetRuleState(g, NULL, NULL, SPRS_ACTIVE));
    } else if (!wcscmp(mode, L"dict")) {
        CK(ISpRecoGrammar_LoadDictation(g, NULL, SPLO_STATIC));
        CK(ISpRecoGrammar_SetDictationState(g, SPRS_ACTIVE));
    } else {
        SPSTATEHANDLE init, mid, loop, any;
        CK(ISpRecoGrammar_GetRule(g, L"cmd", 0, SPRAF_TopLevel | SPRAF_Active | SPRAF_Dynamic, TRUE, &init));
        CK(ISpRecoGrammar_CreateNewState(g, init, &mid));
        for (int i = 0; VERBS[i]; i++)
            CK(ISpRecoGrammar_AddWordTransition(g, init, mid, VERBS[i], L" ", SPWT_LEXICAL, 1.0f, NULL));
        /* one or more dictated words */
        CK(ISpRecoGrammar_CreateNewState(g, init, &loop));
        CK(ISpRecoGrammar_AddRuleTransition(g, mid, loop, SPRULETRANS_DICTATION, 1.0f, NULL));
        CK(ISpRecoGrammar_AddRuleTransition(g, loop, loop, SPRULETRANS_DICTATION, 1.0f, NULL));
        CK(ISpRecoGrammar_AddWordTransition(g, loop, NULL, NULL, NULL, SPWT_LEXICAL, 1.0f, NULL));
        CK(ISpRecoGrammar_AddWordTransition(g, mid, NULL, NULL, NULL, SPWT_LEXICAL, 1.0f, NULL));
        /* free speech fallback */
        CK(ISpRecoGrammar_GetRule(g, L"free", 0, SPRAF_TopLevel | SPRAF_Active | SPRAF_Dynamic, TRUE, &any));
        SPSTATEHANDLE floop;
        CK(ISpRecoGrammar_CreateNewState(g, any, &floop));
        CK(ISpRecoGrammar_AddRuleTransition(g, any, floop, SPRULETRANS_DICTATION, 0.5f, NULL));
        CK(ISpRecoGrammar_AddRuleTransition(g, floop, floop, SPRULETRANS_DICTATION, 1.0f, NULL));
        CK(ISpRecoGrammar_AddWordTransition(g, floop, NULL, NULL, NULL, SPWT_LEXICAL, 1.0f, NULL));
        CK(ISpRecoGrammar_Commit(g, 0));
        CK(ISpRecoGrammar_SetRuleState(g, NULL, NULL, SPRS_ACTIVE));
    }
    CK(ISpRecognizer_SetRecoState(rec, SPRST_ACTIVE));
    HANDLE h = ISpRecoContext_GetNotifyEventHandle(ctx);
    DWORD t0 = GetTickCount();
    wchar_t out[512] = L"";
    for (int done = 0; !done && GetTickCount() - t0 < 15000;) {
        WaitForSingleObject(h, 500);
        SPEVENT ev; ULONG got;
        while (SUCCEEDED(ISpRecoContext_GetEvents(ctx, 1, &ev, &got)) && got) {
            if (ev.eEventId == SPEI_RECOGNITION || ev.eEventId == SPEI_FALSE_RECOGNITION) {
                ISpRecoResult *r = (ISpRecoResult *)ev.lParam;
                WCHAR *txt = NULL; SPPHRASE *ph = NULL;
                if (r) ISpRecoResult_GetText(r, SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &txt, NULL);
                if (r) ISpRecoResult_GetPhrase(r, &ph);
                size_t n = wcslen(out);
                _snwprintf(out + n, 512 - n, L"%ls[%ls %ls conf=%.2f] ", ev.eEventId == SPEI_FALSE_RECOGNITION ? L"(false)" : L"",
                           txt ? txt : L"", ph && ph->Rule.pszName ? ph->Rule.pszName : L"-", ph ? ph->Rule.SREngineConfidence : 0.f);
                if (txt) CoTaskMemFree(txt);
                if (ph) CoTaskMemFree(ph);
            }
            if (ev.eEventId == SPEI_END_SR_STREAM) done = 1;
            if (ev.elParamType == SPET_LPARAM_IS_OBJECT && ev.lParam) IUnknown_Release((IUnknown *)ev.lParam);
            else if (ev.elParamType == SPET_LPARAM_IS_POINTER || ev.elParamType == SPET_LPARAM_IS_STRING) CoTaskMemFree((void *)ev.lParam);
        }
    }
    wprintf(L"%-4ls %-4ls %4lums  %ls\n", eng, mode, GetTickCount() - t0, out);
    ISpRecoGrammar_Release(g); ISpRecoContext_Release(ctx); ISpRecognizer_Release(rec); ISpStream_Release(st);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_xml = _wgetenv(L"GRXML");
    for (int i = 3; i < argc; i++) { wprintf(L"--- %ls\n", argv[i]); run(argv[1], argv[2], argv[i]); }
    return 0;
}

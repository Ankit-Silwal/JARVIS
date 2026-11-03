/* Whisper support: the voice-activity detector on made-up audio, WAV
 * read/write, text cleanup, the prompt. With JARVIS_WHISPER_TEST=1 and
 * Whisper installed, it also speaks a few commands with the Windows voice
 * into WAV files and checks Whisper hears them. */
#define COBJMACROS
#include "../src/jarvis.h"
#include "test.h"
#include <initguid.h>
#include <sapi.h>
#include <math.h>
#include <stdlib.h>

/* SPDFID_WaveFormatEx isn't in MinGW's import libs */
static const GUID WAVE_FMT_ID = { 0xC31ADBAE, 0x527F, 0x4ff5, { 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C } };

static Vad v;
static short frame[VAD_FRAME];

/* feeds n frames of noise (amplitude a) with an optional tone on top; returns the last non-NONE event */
static int feed(int n, int a, int tone, int *first_start, int *end_ev)
{
    int last = VAD_NONE;
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < VAD_FRAME; i++) {
            double t = (v.frames * VAD_FRAME + i) / 16000.0;
            int s = (rand() % (2 * a + 1)) - a;
            if (tone) s += (int)(tone * sin(2 * 3.14159265 * 220 * t));
            frame[i] = (short)s;
        }
        int e = vad_frame(&v, frame);
        if (e != VAD_NONE) last = e;
        if (e == VAD_START && first_start && *first_start < 0) *first_start = v.frames - 1;
        if ((e == VAD_END || e == VAD_DISCARD) && end_ev) *end_ev = e;
    }
    return last;
}

static bool say_to_wav(const wchar_t *text, const wchar_t *path)
{
    ISpVoice *voice = NULL; ISpStream *st = NULL;
    bool ok = false;
    if (FAILED(CoCreateInstance(&CLSID_SpVoice, NULL, CLSCTX_ALL, &IID_ISpVoice, (void **)&voice))) return false;
    if (SUCCEEDED(CoCreateInstance(&CLSID_SpStream, NULL, CLSCTX_ALL, &IID_ISpStream, (void **)&st))) {
        WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0 };
        if (SUCCEEDED(ISpStream_BindToFile(st, path, SPFM_CREATE_ALWAYS, &WAVE_FMT_ID, &wf, 0)) &&
            SUCCEEDED(ISpVoice_SetOutput(voice, (IUnknown *)st, TRUE)) &&
            SUCCEEDED(ISpVoice_Speak(voice, text, SPF_IS_NOT_XML, NULL)))
            ok = true;
        ISpStream_Close(st);
        ISpStream_Release(st);
    }
    ISpVoice_Release(voice);
    return ok;
}

int wmain(void)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    config_load();
    srand(7);

    TEST_SECTION("vad: silence");
    vad_reset(&v);
    CHECK(feed(120, 60, 0, NULL, NULL) == VAD_NONE, "3.6 s of room noise: nothing");
    CHECK(!v.speech, "not in speech");

    TEST_SECTION("vad: one utterance");
    vad_reset(&v);
    int start = -1, endev = 0;
    feed(30, 60, 0, &start, &endev);                   /* 0.9 s quiet */
    int t0 = v.frames;
    feed(40, 60, 6000, &start, &endev);                /* 1.2 s talking */
    int t1 = v.frames;
    CHECK(v.speech && start >= 0, "speech started while loud");
    CHECKF(start >= t0 && start < t0 + 6, "started near the burst (%d vs %d)", start, t0);
    CHECK(endev == 0, "not ended while still loud");
    feed(40, 60, 0, &start, &endev);                   /* 1.2 s quiet */
    CHECK(endev == VAD_END, "ended after the pause");
    CHECKF(v.start <= t0 && v.start >= t0 - 12, "utterance start keeps a little pre-roll (%d vs %d)", v.start, t0);
    CHECKF(v.end >= t1 && v.end <= t1 + 10, "utterance end just after the voice (%d vs %d)", v.end, t1);

    TEST_SECTION("vad: a click is not speech");
    vad_reset(&v);
    start = -1; endev = 0;
    feed(30, 60, 0, &start, &endev);
    feed(2, 60, 9000, &start, &endev);                 /* 60 ms */
    CHECK(start < 0, "too short to start");
    feed(4, 60, 9000, &start, &endev);                 /* 120 ms more */
    feed(40, 60, 0, &start, &endev);
    CHECK(start < 0 || endev == VAD_DISCARD, "started but discarded, or never started");

    TEST_SECTION("vad: loud room");
    vad_reset(&v);
    start = -1; endev = 0;
    feed(60, 900, 0, &start, &endev);                  /* fan noise */
    CHECK(start < 0, "steady noise doesn't start speech");
    feed(40, 900, 9000, &start, &endev);
    feed(40, 900, 0, &start, &endev);
    CHECK(start >= 0 && endev == VAD_END, "voice over the noise is still found");

    TEST_SECTION("wav");
    wchar_t tmp[MAX_PATH], wav[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(wav, MAX_PATH, L"%lsjarvis_test_%lu.wav", tmp, GetTickCount());
    static short pcm[16000];
    for (int i = 0; i < 16000; i++) pcm[i] = (short)(i * 7 - 5000);
    CHECK(wav_write(wav, pcm, 16000), "write");
    int n = 0;
    short *back = wav_read(wav, &n);
    CHECK(back && n == 16000 && memcmp(back, pcm, sizeof pcm) == 0, "read back identical");
    free(back);
    DeleteFileW(wav);
    CHECK(wav_read(L"C:\\nonexistent\\x.wav", &n) == NULL, "missing file -> NULL");

    TEST_SECTION("clean");
    wchar_t t[LINE_MAX_CH];
    str_copy(t, LINE_MAX_CH, L" Open downloads.\n");
    CHECK(whisper_clean(t) && str_eq(t, L"Open downloads."), "trim + newline");
    str_copy(t, LINE_MAX_CH, L"[BLANK_AUDIO]");
    CHECK(!whisper_clean(t), "blank audio tag -> nothing");
    str_copy(t, LINE_MAX_CH, L" (keyboard clicking) find physics notes [laughs]");
    CHECK(whisper_clean(t) && str_eq(t, L"find physics notes"), "noise tags removed");
    str_copy(t, LINE_MAX_CH, L" Thank you.");
    CHECK(!whisper_clean(t), "'Thank you.' on silence is dropped");
    str_copy(t, LINE_MAX_CH, L"you");
    CHECK(!whisper_clean(t), "'you' on silence is dropped");
    str_copy(t, LINE_MAX_CH, L"Set volume to 15%");
    CHECK(whisper_clean(t) && str_eq(t, L"Set volume to 15 percent"), "percent sign spelled out");
    str_copy(t, LINE_MAX_CH, L"Thank you Jarvis, open gmail");
    CHECK(whisper_clean(t), "a real sentence with thank you stays");

    TEST_SECTION("prompt");
    wchar_t p[1024];
    whisper_prompt(p, 1024);
    CHECK(wcsstr(p, L"youtube") != NULL, "site names in the prompt");
    CHECK(wcsstr(p, L"downloads") != NULL, "folder names in the prompt");
    CHECK(wcslen(p) < 1024, "fits");
    CHECK(wcschr(p, L'"') == NULL, "no quotes (goes on a command line)");

    wchar_t exe[MAX_PATH], model[MAX_PATH];
    wchar_t env[8] = L"";
    GetEnvironmentVariableW(L"JARVIS_WHISPER_TEST", env, 8);
    if (!str_eq(env, L"1")) {
        printf("real transcription: skipped (set JARVIS_WHISPER_TEST=1 to run)\n");
    } else if (!whisper_find(exe, model)) {
        printf("real transcription: skipped (Whisper not installed, run tools\\get_whisper.ps1)\n");
    } else {
        TEST_SECTION("real transcription (Windows voice -> Whisper)");
        printf("   model %ls\n", path_name(model));
        static const wchar_t *const SAY[] = { L"open downloads", L"find physics notes", L"convert my resume to pdf",
                                              L"mute jarvis", L"what time is it", NULL };
        for (int i = 0; SAY[i]; i++) {
            CHECKF(say_to_wav(SAY[i], wav), "tts to wav: %ls", SAY[i]);
            int cnt = 0;
            short *s = wav_read(wav, &cnt);
            DeleteFileW(wav);
            wchar_t out[LINE_MAX_CH] = L"";
            DWORD t0 = GetTickCount();
            bool ok = s && whisper_run(s, cnt, out, LINE_MAX_CH, 60000);
            free(s);
            wchar_t low[LINE_MAX_CH];
            str_copy(low, LINE_MAX_CH, out);
            str_lower(low);
            for (wchar_t *c = low; *c; c++) if (*c == L'.' || *c == L',') *c = L' ';
            str_collapse_spaces(low); str_trim(low);
            printf("   %-28ls -> \"%ls\"  (%lu ms)\n", SAY[i], out, GetTickCount() - t0);
            CHECKF(ok && str_eq(low, SAY[i]), "heard \"%ls\"", SAY[i]);
        }
    }
    return TEST_SUMMARY();
}

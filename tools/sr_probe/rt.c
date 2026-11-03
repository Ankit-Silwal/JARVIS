/* rt <topic|grxml FILE|list> [n]: WinRT SpeechRecognizer from the microphone */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.media.speechrecognition.h>
#include <stdio.h>

#define NS(x) __x_ABI_CWindows_CMedia_CSpeechRecognition_C##x
typedef __x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognizer Rec;
typedef __x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognitionResult Res;
typedef __x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognitionCompilationResult Comp;

#define CK(x) do { HRESULT _h = (x); if (FAILED(_h)) { wprintf(L"FAIL %hs: 0x%08lx\n", #x, _h); return 1; } } while (0)

static HSTRING hs(const wchar_t *s) { HSTRING h; WindowsCreateString(s, (UINT32)wcslen(s), &h); return h; }

/* poll an IAsyncInfo until done */
static HRESULT wait_async(IUnknown *op, DWORD ms)
{
    IAsyncInfo *info;
    HRESULT hr = IUnknown_QueryInterface(op, &IID_IAsyncInfo, (void **)&info);
    if (FAILED(hr)) return hr;
    AsyncStatus st = Started;
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(IAsyncInfo_get_Status(info, &st)) && st == Started && GetTickCount() - t0 < ms) Sleep(20);
    if (st == Error) { IAsyncInfo_get_ErrorCode(info, &hr); }
    else hr = st == Completed ? S_OK : E_ABORT;
    IAsyncInfo_Release(info);
    return hr;
}

int wmain(int argc, wchar_t **argv)
{
    CK(RoInitialize(RO_INIT_MULTITHREADED));
    IInspectable *insp;
    HSTRING cls = hs(RuntimeClass_Windows_Media_SpeechRecognition_SpeechRecognizer);
    CK(RoActivateInstance(cls, &insp));
    Rec *rec;
    CK(IInspectable_QueryInterface(insp, &IID___x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognizer, (void **)&rec));

    __FIVector_1_Windows__CMedia__CSpeechRecognition__CISpeechRecognitionConstraint *cons;
    CK(NS(ISpeechRecognizer_get_Constraints)(rec, &cons));
    IInspectable *c = NULL;
    if (c) {
        __x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognitionConstraint *ic;
        CK(IInspectable_QueryInterface(c, &IID___x_ABI_CWindows_CMedia_CSpeechRecognition_CISpeechRecognitionConstraint, (void **)&ic));
        CK(__FIVector_1_Windows__CMedia__CSpeechRecognition__CISpeechRecognitionConstraint_Append(cons, ic));
    }
    __FIAsyncOperation_1_Windows__CMedia__CSpeechRecognition__CSpeechRecognitionCompilationResult *cop;
    CK(NS(ISpeechRecognizer_CompileConstraintsAsync)(rec, &cop));
    CK(wait_async((IUnknown *)cop, 20000));
    Comp *comp;
    CK(__FIAsyncOperation_1_Windows__CMedia__CSpeechRecognition__CSpeechRecognitionCompilationResult_GetResults(cop, &comp));
    __x_ABI_CWindows_CMedia_CSpeechRecognition_CSpeechRecognitionResultStatus cst;
    CK(NS(ISpeechRecognitionCompilationResult_get_Status)(comp, &cst));
    wprintf(L"compile status %d (0 = success)\n", cst);

    int n = argc > 2 ? _wtoi(argv[2]) : 1;
    for (int i = 0; i < n; i++) {
        __FIAsyncOperation_1_Windows__CMedia__CSpeechRecognition__CSpeechRecognitionResult *rop;
        DWORD t0 = GetTickCount();
        CK(NS(ISpeechRecognizer_RecognizeAsync)(rec, &rop));
        HRESULT hr = wait_async((IUnknown *)rop, 30000);
        if (FAILED(hr)) { wprintf(L"recognize failed 0x%08lx\n", hr); continue; }
        Res *res;
        CK(__FIAsyncOperation_1_Windows__CMedia__CSpeechRecognition__CSpeechRecognitionResult_GetResults(rop, &res));
        HSTRING txt; __x_ABI_CWindows_CMedia_CSpeechRecognition_CSpeechRecognitionResultStatus st; __x_ABI_CWindows_CMedia_CSpeechRecognition_CSpeechRecognitionConfidence conf; DOUBLE raw;
        NS(ISpeechRecognitionResult_get_Status)(res, &st);
        NS(ISpeechRecognitionResult_get_Text)(res, &txt);
        NS(ISpeechRecognitionResult_get_Confidence)(res, &conf);
        NS(ISpeechRecognitionResult_get_RawConfidence)(res, &raw);
        wprintf(L"[%lums] status=%d conf=%d raw=%.2f text=\"%ls\"\n", GetTickCount() - t0, st, conf, raw, WindowsGetStringRawBuffer(txt, NULL));
        fflush(stdout);
    }
    return 0;
}

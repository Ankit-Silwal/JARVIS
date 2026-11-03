/* Small OS actions behind the extra commands: master volume level, media
 * keys, sleep / shutdown / restart / sign out, empty the Recycle Bin.
 * Nothing here is called in dry-run; commands.c records the action instead. */
#define COBJMACROS
#include "jarvis.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>

/* ---------------------------------------------------------------- volume */
static IAudioEndpointVolume *endpoint_volume(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioEndpointVolume *vol = NULL;
    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)))
        return NULL;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) {
        IMMDevice_Activate(dev, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **)&vol);
        IMMDevice_Release(dev);
    }
    IMMDeviceEnumerator_Release(en);
    return vol;
}

bool sys_volume_get(int *pct, bool *muted)
{
    IAudioEndpointVolume *v = endpoint_volume();
    if (!v) return false;
    float f = 0;
    BOOL m = FALSE;
    bool ok = SUCCEEDED(IAudioEndpointVolume_GetMasterVolumeLevelScalar(v, &f));
    IAudioEndpointVolume_GetMute(v, &m);
    IAudioEndpointVolume_Release(v);
    if (ok) { *pct = (int)(f * 100 + 0.5f); if (muted) *muted = m != FALSE; }
    return ok;
}

bool sys_volume_set(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    IAudioEndpointVolume *v = endpoint_volume();
    if (!v) return false;
    bool ok = SUCCEEDED(IAudioEndpointVolume_SetMasterVolumeLevelScalar(v, pct / 100.0f, NULL));
    if (ok && pct > 0) IAudioEndpointVolume_SetMute(v, FALSE, NULL);
    IAudioEndpointVolume_Release(v);
    return ok;
}

/* ----------------------------------------------------------------- media */
static void tap(WORD vk)
{
    INPUT in[2];
    memset(in, 0, sizeof in);
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void sys_media(const wchar_t *what)
{
    if (str_eq(what, L"play_pause")) tap(VK_MEDIA_PLAY_PAUSE);
    else if (str_eq(what, L"next")) tap(VK_MEDIA_NEXT_TRACK);
    else if (str_eq(what, L"previous")) tap(VK_MEDIA_PREV_TRACK);
    else if (str_eq(what, L"stop")) tap(VK_MEDIA_STOP);
}

/* ----------------------------------------------------------------- power */
static bool run_shutdown(const wchar_t *args)
{
    return (INT_PTR)ShellExecuteW(NULL, L"open", L"shutdown.exe", args, NULL, SW_HIDE) > 32;
}

bool sys_power(const wchar_t *what)
{
    /* shutdown.exe with a short delay, so the spoken reply isn't cut off */
    if (str_eq(what, L"shutdown"))  return run_shutdown(L"/s /t 3");
    if (str_eq(what, L"restart"))   return run_shutdown(L"/r /t 3");
    if (str_eq(what, L"signout"))   return run_shutdown(L"/l");
    if (str_eq(what, L"abort"))     return run_shutdown(L"/a");
    if (str_eq(what, L"sleep"))     return SetSuspendState(FALSE, FALSE, FALSE) != FALSE;
    if (str_eq(what, L"hibernate")) return SetSuspendState(TRUE, FALSE, FALSE) != FALSE;
    return false;
}

/* ----------------------------------------------------------- recycle bin */
bool sys_empty_recycle_bin(void)
{
    HRESULT hr = SHEmptyRecycleBinW(NULL, NULL, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
    return SUCCEEDED(hr) || hr == E_UNEXPECTED;   /* E_UNEXPECTED: it was already empty */
}

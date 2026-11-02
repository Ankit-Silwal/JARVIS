/* Detects a standard Windows Open/Upload file dialog (the one Chrome, Drive,
 * Gmail, Office... show) and types a path into it. */
#include "jarvis.h"

#define ID_FILENAME_COMBO 0x47C   /* cmb13: "File name" combo box */
#define ID_FILENAME_EDIT  0x480   /* edt1 (old-style dialogs) */

typedef struct { HWND edit; } FindCtx;

static BOOL CALLBACK find_edit(HWND h, LPARAM lp)
{
    FindCtx *c = (FindCtx *)lp;
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"Edit") == 0) {
        HWND parent = GetParent(h);
        int pid = parent ? GetDlgCtrlID(parent) : 0;
        /* the Edit inside ComboBoxEx32 -> ComboBox (id 0x47C on the outer combo) */
        HWND gp = parent ? GetParent(parent) : NULL;
        int gid = gp ? GetDlgCtrlID(gp) : 0;
        if (GetDlgCtrlID(h) == ID_FILENAME_EDIT || pid == ID_FILENAME_COMBO || gid == ID_FILENAME_COMBO) {
            c->edit = h;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND filename_edit(HWND dlg)
{
    FindCtx c = { NULL };
    EnumChildWindows(dlg, find_edit, (LPARAM)&c);
    return c.edit;
}

HWND dialog_detect(HWND fg)
{
    if (!fg) return NULL;
    /* the foreground window may be a child; walk to the top-level owner chain */
    HWND top = GetAncestor(fg, GA_ROOT);
    if (!top) top = fg;
    wchar_t cls[64];
    GetClassNameW(top, cls, 64);
    if (wcscmp(cls, L"#32770") != 0) return NULL;
    return filename_edit(top) ? top : NULL;
}

bool dialog_alive(HWND dlg) { return dlg && IsWindow(dlg) && IsWindowVisible(dlg); }

bool dialog_fill(HWND dlg, const wchar_t *path)
{
    if (!dialog_alive(dlg)) return false;
    HWND edit = filename_edit(dlg);
    if (!edit) return false;

    /* bring the dialog back to the front first */
    DWORD me = GetCurrentThreadId();
    DWORD fgt = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    if (fgt && fgt != me) AttachThreadInput(me, fgt, TRUE);
    SetForegroundWindow(dlg);
    if (fgt && fgt != me) AttachThreadInput(me, fgt, FALSE);

    /* paths with spaces must be quoted or the dialog treats them as a list */
    wchar_t text[MAX_PATH + 4];
    if (wcschr(path, L' ')) swprintf(text, MAX_PATH + 4, L"\"%ls\"", path);
    else str_copy(text, MAX_PATH + 4, path);

    DWORD_PTR res;
    if (!SendMessageTimeoutW(edit, WM_SETTEXT, 0, (LPARAM)text, SMTO_ABORTIFHUNG, 1500, &res))
        return false;

    /* press "Open" - for a folder this navigates into it instead */
    HWND ok = GetDlgItem(dlg, IDOK);
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok);
    return true;
}

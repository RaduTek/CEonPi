//
// voltray.cpp - volume tray icon (src\apps\voltray, voltray.exe)
//
// Puts a speaker icon in the taskbar notification area. A left click opens
// a Windows XP style popup just above the taskbar, centered on the click's X
// coordinate: no caption, a vertical volume slider and a "Mute" check box.
// The slider has 6 ticks (0, 20, ..., 100 %) like the Volume & Sounds
// control panel, but moves in 1 % steps. When the slider is released at a
// new level the default system sound plays (unless muted). The popup closes
// when it loses focus or on Esc. Right click: menu with "Volume & Sounds
// Settings" (opens that control panel applet) and Exit.
//
// The tray icon changes only when mute is switched; the tooltip's level is
// refreshed when the popup closes (updating the tray on every slider move
// made the taskbar flicker).
//
// Volume: waveOutSetVolume on wave output device 0 (the same level for left
// and right). The level is saved in HKCU\ControlPanel\Volume "Volume", the
// value the Volume & Sounds control panel uses, when the popup closes (not
// on every slider move: the registry hive may live on the SD card). Mute
// sets the device volume to 0 and keeps the level; it is saved in
// HKCU\Software\CEonPi\VolTray "Mute" and applied again at start.
//
// Started from \Windows\StartUp (platform.dat) by Explorer. One instance.
//
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>
#include "resource.h"

#define WM_TRAYNOTIFY       (WM_USER + 1)
#define TRAY_ID             1
#define TIMER_TRAY_RETRY    1

#define VOL_MAX             100         // slider range 0..100 (%)
#define VOL_TICK_FREQ       20          // 6 ticks: 0, 20, 40, 60, 80, 100
#define VOL_PAGE            10

#define REG_CPL_KEY         L"ControlPanel\\Volume"
#define REG_CPL_VOLUME      L"Volume"
#define REG_APP_KEY         L"Software\\CEonPi\\VolTray"
#define REG_APP_MUTE        L"Mute"

// Volume & Sounds: applet 10 (CPL_Sounds) of cplmain.cpl, started the way
// Explorer's Start menu starts applets (ctlpnl.exe "<cpl>,<index>"). The
// index is into cplmain's fixed applet table (public\wceshellfe\oak\ctlpnl\
// cplmain\cpltable.cpp); applets whose component is missing are hidden, not
// renumbered.
#define CPL_LAUNCHER        L"ctlpnl.exe"
#define CPL_SOUNDS          L"cplmain.cpl,10"

#define CLASS_NAME          L"CEonPiVolTray"
#define MUTEX_NAME          L"CEonPiVolTrayInstance"

static HINSTANCE g_hInst;
static HWND      g_hwndMain;            // hidden window for tray messages
static HWND      g_hdlg;                // volume popup (modeless dialog)
static HICON     g_hIconOn;
static HICON     g_hIconMute;
static int       g_nLevel = VOL_MAX;    // 0..VOL_MAX
static BOOL      g_fMute;
static BOOL      g_fTrayAdded;
static DWORD     g_dwHiddenAt;          // tick count when the popup last closed
static int       g_nSoundLevel;         // level the feedback sound last played at
static UINT      g_nTrayRetries;

// ---------------------------------------------------------------------------
// Volume and registry
// ---------------------------------------------------------------------------

// Level 0..100 to a waveOutSetVolume value (low word left, high word right).
static DWORD LevelToVolume(int nLevel)
{
    DWORD w = (DWORD)nLevel * 0xFFFF / VOL_MAX;
    return w | (w << 16);
}

// waveOutSetVolume value to level 0..100 (the louder channel).
static int VolumeToLevel(DWORD dwVolume)
{
    DWORD l = LOWORD(dwVolume), r = HIWORD(dwVolume);
    DWORD w = (l > r) ? l : r;
    return (int)((w * VOL_MAX + 0x7FFF) / 0xFFFF);
}

static void ApplyVolume(void)
{
    waveOutSetVolume((HWAVEOUT)0, g_fMute ? 0 : LevelToVolume(g_nLevel));
}

static BOOL RegReadDword(HKEY hRoot, LPCWSTR pszKey, LPCWSTR pszValue, DWORD *pdw)
{
    HKEY hKey;
    DWORD dwType, dwSize = sizeof(DWORD);
    BOOL fOk = FALSE;

    if (RegOpenKeyEx(hRoot, pszKey, 0, 0, &hKey) == ERROR_SUCCESS) {
        fOk = RegQueryValueEx(hKey, pszValue, NULL, &dwType, (LPBYTE)pdw, &dwSize) ==
                  ERROR_SUCCESS && dwType == REG_DWORD;
        RegCloseKey(hKey);
    }
    return fOk;
}

static void RegWriteDword(HKEY hRoot, LPCWSTR pszKey, LPCWSTR pszValue, DWORD dw)
{
    HKEY hKey;
    DWORD dwDisp;

    if (RegCreateKeyEx(hRoot, pszKey, 0, NULL, 0, 0, NULL, &hKey, &dwDisp) == ERROR_SUCCESS) {
        RegSetValueEx(hKey, pszValue, 0, REG_DWORD, (const BYTE *)&dw, sizeof(dw));
        RegCloseKey(hKey);
    }
}

// Start-up state: the level from the control panel's registry value (or the
// device if it is missing), the mute flag from our own key.
static void LoadState(void)
{
    DWORD dw;

    if (RegReadDword(HKEY_CURRENT_USER, REG_CPL_KEY, REG_CPL_VOLUME, &dw))
        g_nLevel = VolumeToLevel(dw);
    else if (waveOutGetVolume((HWAVEOUT)0, &dw) == MMSYSERR_NOERROR)
        g_nLevel = VolumeToLevel(dw);

    g_fMute = RegReadDword(HKEY_CURRENT_USER, REG_APP_KEY, REG_APP_MUTE, &dw) && dw != 0;
}

static void SaveState(void)
{
    DWORD dwOld;
    DWORD dwNew = LevelToVolume(g_nLevel);

    // Only write what changed: the hive may be on the SD card.
    if (!RegReadDword(HKEY_CURRENT_USER, REG_CPL_KEY, REG_CPL_VOLUME, &dwOld) ||
        dwOld != dwNew)
        RegWriteDword(HKEY_CURRENT_USER, REG_CPL_KEY, REG_CPL_VOLUME, dwNew);

    if (!RegReadDword(HKEY_CURRENT_USER, REG_APP_KEY, REG_APP_MUTE, &dwOld) ||
        (dwOld != 0) != (g_fMute != 0))
        RegWriteDword(HKEY_CURRENT_USER, REG_APP_KEY, REG_APP_MUTE, g_fMute ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------
// Adds or modifies the tray icon. uFlags selects what is set: NIF_ICON (the
// on/muted icon), NIF_TIP (the tooltip with the level), NIF_MESSAGE.
static BOOL TrayIcon(DWORD dwMessage, UINT uFlags)
{
    NOTIFYICONDATA nid;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwndMain;
    nid.uID              = TRAY_ID;
    nid.uFlags           = uFlags;
    nid.uCallbackMessage = WM_TRAYNOTIFY;
    nid.hIcon            = g_fMute ? g_hIconMute : g_hIconOn;
    if (g_fMute)
        wsprintf(nid.szTip, L"Volume: muted (%d%%)", g_nLevel);
    else
        wsprintf(nid.szTip, L"Volume: %d%%", g_nLevel);
    return Shell_NotifyIcon(dwMessage, &nid);
}

// Mute changed: new icon and tooltip.
static void UpdateTrayIcon(void)
{
    if (g_fTrayAdded)
        TrayIcon(NIM_MODIFY, NIF_ICON | NIF_TIP);
}

// Level changed: the tooltip only, and only when the popup closes. Updating
// the tray on every slider move made the taskbar flicker.
static void UpdateTrayTip(void)
{
    if (g_fTrayAdded)
        TrayIcon(NIM_MODIFY, NIF_TIP);
}

// ---------------------------------------------------------------------------
// Volume popup
// ---------------------------------------------------------------------------
static void HidePopup(void)
{
    if (g_hdlg && IsWindowVisible(g_hdlg)) {
        ShowWindow(g_hdlg, SW_HIDE);
        g_dwHiddenAt = GetTickCount();
        SaveState();
        UpdateTrayTip();
    }
}

// xCenter: screen X coordinate the popup is centered on (the tray click).
static void ShowPopup(int xCenter)
{
    RECT rcWork, rcDlg;
    DWORD dw;
    int cx, cy, x;

    // Pick up changes made elsewhere (Volume & Sounds) while not muted.
    if (!g_fMute && waveOutGetVolume((HWAVEOUT)0, &dw) == MMSYSERR_NOERROR)
        g_nLevel = VolumeToLevel(dw);

    // Vertical trackbar: the top is the minimum, so invert.
    SendDlgItemMessage(g_hdlg, IDC_SLIDER, TBM_SETPOS, TRUE, VOL_MAX - g_nLevel);
    CheckDlgButton(g_hdlg, IDC_MUTE, g_fMute ? BST_CHECKED : BST_UNCHECKED);

    // Centered on the click horizontally (kept inside the work area), just
    // above the taskbar vertically.
    SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0);
    GetWindowRect(g_hdlg, &rcDlg);
    cx = rcDlg.right - rcDlg.left;
    cy = rcDlg.bottom - rcDlg.top;
    x  = xCenter - cx / 2;
    if (x > rcWork.right - cx - 2)
        x = rcWork.right - cx - 2;
    if (x < rcWork.left + 2)
        x = rcWork.left + 2;
    SetWindowPos(g_hdlg, HWND_TOPMOST, x, rcWork.bottom - cy - 2, 0, 0, SWP_NOSIZE);
    g_nSoundLevel = g_nLevel;

    ShowWindow(g_hdlg, SW_SHOW);
    SetForegroundWindow(g_hdlg);
    SetFocus(GetDlgItem(g_hdlg, IDC_SLIDER));
}

static BOOL CALLBACK VolumeDlgProc(HWND hdlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_INITDIALOG:
        SendDlgItemMessage(hdlg, IDC_SLIDER, TBM_SETRANGE, FALSE, MAKELONG(0, VOL_MAX));
        SendDlgItemMessage(hdlg, IDC_SLIDER, TBM_SETTICFREQ, VOL_TICK_FREQ, 0);
        SendDlgItemMessage(hdlg, IDC_SLIDER, TBM_SETLINESIZE, 0, 1);
        SendDlgItemMessage(hdlg, IDC_SLIDER, TBM_SETPAGESIZE, 0, VOL_PAGE);
        return TRUE;

    case WM_VSCROLL:
        if ((HWND)lParam == GetDlgItem(hdlg, IDC_SLIDER)) {
            int nPos = (int)SendMessage((HWND)lParam, TBM_GETPOS, 0, 0);
            g_nLevel = VOL_MAX - nPos;
            if (!g_fMute)
                ApplyVolume();

            // Once the slider is released (mouse up or key up), play the
            // default sound at the new level, as Windows XP does. Not on
            // every step of a drag, which would stack dozens of sounds.
            if (LOWORD(wParam) == TB_ENDTRACK && g_nLevel != g_nSoundLevel) {
                g_nSoundLevel = g_nLevel;
                if (!g_fMute)
                    MessageBeep(MB_OK);
            }
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MUTE:
            if (HIWORD(wParam) == BN_CLICKED) {
                g_fMute = IsDlgButtonChecked(hdlg, IDC_MUTE) == BST_CHECKED;
                ApplyVolume();
                UpdateTrayIcon();
            }
            return TRUE;
        case IDOK:
        case IDCANCEL:
            HidePopup();
            return TRUE;
        }
        break;

    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE)
            HidePopup();
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Hidden main window
// ---------------------------------------------------------------------------
// Where the tray icon was clicked. The tray callback is sent by Explorer, so
// GetMessagePos() does not carry the click (it returned 0,0); GetCursorPos
// returns the last mouse/touch position. If it fails or gives 0,0 (an OS
// design without cursor support), use the right end of the taskbar, where
// the notification area is.
static POINT GetClickPoint(void)
{
    POINT pt;
    RECT rcWork;

    if (GetCursorPos(&pt) && (pt.x != 0 || pt.y != 0))
        return pt;

    SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0);
    pt.x = rcWork.right;
    pt.y = rcWork.bottom;
    return pt;
}

static void OpenSoundsCpl(void)
{
    SHELLEXECUTEINFO sei;

    memset(&sei, 0, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_FLAG_NO_UI;
    sei.hwnd         = g_hwndMain;
    sei.lpFile       = CPL_LAUNCHER;
    sei.lpParameters = CPL_SOUNDS;
    sei.nShow        = SW_SHOW;
    ShellExecuteEx(&sei);
}

static void ShowTrayMenu(void)
{
    HMENU hMenu = LoadMenu(g_hInst, MAKEINTRESOURCE(IDM_TRAY));
    POINT pt = GetClickPoint();

    if (!hMenu)
        return;
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(GetSubMenu(hMenu, 0), TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_hwndMain, NULL);
    DestroyMenu(hMenu);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_TRAYNOTIFY:
        if (wParam != TRAY_ID)
            break;
        switch (lParam) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(g_hdlg)) {
                HidePopup();
            } else if (GetTickCount() - g_dwHiddenAt > 300) {
                // A click on the icon while the popup is open first takes
                // the focus away and closes it; do not reopen it at once.
                ShowPopup(GetClickPoint().x);
            }
            break;
        case WM_RBUTTONUP:
            HidePopup();
            ShowTrayMenu();
            break;
        }
        return 0;

    case WM_TIMER:
        // The taskbar was not ready yet: try to add the icon again.
        if (wParam == TIMER_TRAY_RETRY) {
            g_fTrayAdded = TrayIcon(NIM_ADD, NIF_MESSAGE | NIF_ICON | NIF_TIP);
            g_nTrayRetries++;
            if (g_fTrayAdded || g_nTrayRetries >= 30)   // give up after a minute
                KillTimer(hwnd, TIMER_TRAY_RETRY);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            OpenSoundsCpl();
            break;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        HidePopup();
        if (g_fTrayAdded)
            TrayIcon(NIM_DELETE, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASS wc;
    HANDLE hMutex;
    MSG msg;

    // One instance only.
    hMutex = CreateMutex(NULL, FALSE, MUTEX_NAME);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    g_hInst = hInstance;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    g_hIconOn   = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_TRAY), IMAGE_ICON, 16, 16, 0);
    g_hIconMute = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_TRAY_MUTE), IMAGE_ICON, 16, 16, 0);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClass(&wc))
        return 0;

    g_hwndMain = CreateWindow(CLASS_NAME, L"Volume", WS_POPUP, 0, 0, 0, 0,
                              NULL, NULL, hInstance, NULL);
    g_hdlg = CreateDialog(hInstance, MAKEINTRESOURCE(IDD_VOLUME), g_hwndMain, VolumeDlgProc);
    if (!g_hwndMain || !g_hdlg)
        return 0;

    LoadState();
    ApplyVolume();

    g_fTrayAdded = TrayIcon(NIM_ADD, NIF_MESSAGE | NIF_ICON | NIF_TIP);
    if (!g_fTrayAdded)
        SetTimer(g_hwndMain, TIMER_TRAY_RETRY, 2000, NULL);

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (!IsDialogMessage(g_hdlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if (g_hIconOn)
        DestroyIcon(g_hIconOn);
    if (g_hIconMute)
        DestroyIcon(g_hIconMute);
    if (hMutex)
        CloseHandle(hMutex);
    return 0;
}

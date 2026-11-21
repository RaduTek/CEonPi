//
// poweroff.cpp - Shut Down dialog (src\apps\poweroff, poweroff.exe)
//
// Shows a dialog (resource IDD_POWEROFF) with Restart, Power off and Cancel.
//
//   Restart   -> SetSystemPowerState(POWER_STATE_RESET). The power manager
//                calls IOCTL_HAL_REBOOT, which resets the SoC through the
//                PM watchdog (src\oal\oallib\power.c, OALIoCtlHalReboot).
//                If the power manager refuses, fall back to calling
//                IOCTL_HAL_REBOOT directly.
//   Power off -> SetSystemPowerState(POWER_STATE_SUSPEND). The power manager
//                powers the drivers down, notifies the file systems and the
//                kernel calls OEMPowerOff, which on this BSP halts the board
//                through the firmware. Power must be cycled to start again.
//   Cancel    -> close.
//
// Optional image component: set BSP_POWEROFF=1 (catalog item "Shut Down
// dialog (poweroff.exe)") to put poweroff.exe in \Windows and a "Shut Down"
// shortcut on the desktop.
//

#include <windows.h>
#include <pm.h>
#include <pkfuncs.h>
#include "resource.h"

#define POWEROFF_CAPTION    TEXT("Shut Down")

static HINSTANCE g_hInst = NULL;

// ---------------------------------------------------------------------------
// ReportFailure: tell the user the request did not go through.
//
static void ReportFailure(HWND hDlg, LPCTSTR pszWhat, DWORD dwErr)
{
    TCHAR sz[128];

    wsprintf(sz, TEXT("%s failed (error %u)."), pszWhat, dwErr);
    MessageBox(hDlg, sz, POWEROFF_CAPTION, MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// DoRestart
//
static void DoRestart(HWND hDlg)
{
    DWORD dwErr;

    dwErr = SetSystemPowerState(NULL, POWER_STATE_RESET, POWER_FORCE);
    if (dwErr == ERROR_SUCCESS)
        return;     // normally not reached: the board resets

    // Power manager refused or has no reset state: ask the OAL directly.
    if (!KernelIoControl(IOCTL_HAL_REBOOT, NULL, 0, NULL, 0, NULL))
        ReportFailure(hDlg, TEXT("Restart"), dwErr);
}

// ---------------------------------------------------------------------------
// DoPowerOff
//
static void DoPowerOff(HWND hDlg)
{
    DWORD dwErr;

    dwErr = SetSystemPowerState(NULL, POWER_STATE_SUSPEND, POWER_FORCE);
    if (dwErr == ERROR_SUCCESS)
    {
        // Normally not reached: OEMPowerOff halts the board. If the system
        // came back (OAL could not halt), just close.
        EndDialog(hDlg, IDC_POWEROFF);
        return;
    }
    ReportFailure(hDlg, TEXT("Power off"), dwErr);
}

// ---------------------------------------------------------------------------
// DlgProc
//
static BOOL CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HICON hIcon;

    switch (msg)
    {
    case WM_INITDIALOG:
        hIcon = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_POWEROFF),
                                 IMAGE_ICON, 16, 16, 0);
        if (hIcon)
            SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        SetForegroundWindow(hDlg);
        return TRUE;    // focus goes to the default button (Cancel)

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_RESET:
            DoRestart(hDlg);
            return TRUE;

        case IDC_POWEROFF:
            DoPowerOff(hDlg);
            return TRUE;

        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// WinMain
//
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR pszCmdLine, int nCmdShow)
{
    HWND hWnd;

    // One instance only: bring the open dialog to the front.
    hWnd = FindWindow(NULL, POWEROFF_CAPTION);
    if (hWnd)
    {
        SetForegroundWindow(hWnd);
        return 0;
    }

    g_hInst = hInstance;
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_POWEROFF), NULL, DlgProc);
    return 0;
}

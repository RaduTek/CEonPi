//
// idlemon.cpp - CPU idle monitor demo (src\apps\idlemon, idlemon.exe)
//
// Small window, centered on the work area, that samples GetIdleTime() and
// GetTickCount() once per second and shows:
//   - CPU load over the last interval  (100% - idle%)
//   - idle percentage over the last interval
//   - the raw GetIdleTime() and GetTickCount() values (ms)
//
// GetIdleTime() is fed by OEMIdle (src\oal\oallib\power.c), which counts the
// time the core spends in WFI in 1 us system timer ticks; the kernel divides
// by idleconv (1000 counts per ms). Before OEMIdle was implemented it stayed
// at 0, so this window would show 100% load.
//
// The monitor itself wakes once per second to redraw, so its own cost is
// included in the load figure (negligible).
//
// Optional image component: set BSP_IDLEMON=1 (catalog item
// "CPU Idle Monitor (demo)") to put idlemon.exe in \Windows and a shortcut
// on the desktop.
//

#include <windows.h>

#define IDLEMON_CLASS   TEXT("RaspiIdleMon")
#define IDLEMON_TITLE   TEXT("CPU Idle Monitor")
#define IDT_REFRESH     1
#define REFRESH_MS      1000

#define CLIENT_W        220
#define TEXT_LINES      4
#define MARGIN          8
#define BAR_H           14

static BOOL  g_fHaveSample = FALSE;
static BOOL  g_fIdleSupported = TRUE;
static DWORD g_dwLastTick = 0;
static DWORD g_dwLastIdle = 0;
static DWORD g_dwNowTick = 0;
static DWORD g_dwNowIdle = 0;
static DWORD g_dwIdleTenths = 0;    // idle over last interval, 0..1000
static DWORD g_dwLoadTenths = 0;    // load over last interval, 0..1000
static DWORD g_dwIntervalMs = 0;
static int   g_cyLine = 16;

// ---------------------------------------------------------------------------
// Sample: read the counters and compute the load over the last interval.
//
static void Sample(void)
{
    DWORD dwTick = GetTickCount();
    DWORD dwIdle = GetIdleTime();

    if (dwIdle == MAXDWORD)
    {
        // OAL does not support idle time accounting
        g_fIdleSupported = FALSE;
        g_dwNowTick = dwTick;
        return;
    }
    g_fIdleSupported = TRUE;

    if (g_fHaveSample)
    {
        DWORD dT = dwTick - g_dwLastTick;
        DWORD dI = dwIdle - g_dwLastIdle;

        if (dT > 0)
        {
            DWORD dwIdleTenths = (dI * 1000 + dT / 2) / dT;
            if (dwIdleTenths > 1000)
                dwIdleTenths = 1000;
            g_dwIdleTenths = dwIdleTenths;
            g_dwLoadTenths = 1000 - dwIdleTenths;
            g_dwIntervalMs = dT;
        }
    }

    g_dwLastTick = dwTick;
    g_dwLastIdle = dwIdle;
    g_dwNowTick = dwTick;
    g_dwNowIdle = dwIdle;
    g_fHaveSample = TRUE;
}

// ---------------------------------------------------------------------------
// DrawLine: one left-aligned line of text at row 'row'.
//
static void DrawLine(HDC hdc, const RECT *prcClient, int row, LPCTSTR pszText)
{
    RECT rc;

    rc.left = prcClient->left + MARGIN;
    rc.right = prcClient->right - MARGIN;
    rc.top = prcClient->top + MARGIN + row * g_cyLine;
    rc.bottom = rc.top + g_cyLine;
    DrawText(hdc, pszText, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

// ---------------------------------------------------------------------------
// OnPaint
//
static void OnPaint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdc;
    RECT rcClient, rcBar, rcFill;
    TCHAR sz[80];
    HBRUSH hbrFill, hbrOld;
    HPEN hpenOld;

    hdc = BeginPaint(hWnd, &ps);
    GetClientRect(hWnd, &rcClient);
    SetBkMode(hdc, TRANSPARENT);

    if (!g_fIdleSupported)
    {
        DrawLine(hdc, &rcClient, 0, TEXT("GetIdleTime() not supported"));
        DrawLine(hdc, &rcClient, 1, TEXT("by this OAL."));
        EndPaint(hWnd, &ps);
        return;
    }

    if (g_dwIntervalMs == 0)
    {
        DrawLine(hdc, &rcClient, 0, TEXT("CPU load:  (measuring...)"));
        DrawLine(hdc, &rcClient, 1, TEXT("Idle:      (measuring...)"));
    }
    else
    {
        wsprintf(sz, TEXT("CPU load:  %u.%u %%"), g_dwLoadTenths / 10, g_dwLoadTenths % 10);
        DrawLine(hdc, &rcClient, 0, sz);
        wsprintf(sz, TEXT("Idle:      %u.%u %%"), g_dwIdleTenths / 10, g_dwIdleTenths % 10);
        DrawLine(hdc, &rcClient, 1, sz);
    }

    wsprintf(sz, TEXT("GetIdleTime():  %u ms"), g_dwNowIdle);
    DrawLine(hdc, &rcClient, 2, sz);
    wsprintf(sz, TEXT("GetTickCount(): %u ms"), g_dwNowTick);
    DrawLine(hdc, &rcClient, 3, sz);

    // Load bar under the text
    rcBar.left = rcClient.left + MARGIN;
    rcBar.right = rcClient.right - MARGIN;
    rcBar.top = rcClient.top + MARGIN + TEXT_LINES * g_cyLine + 4;
    rcBar.bottom = rcBar.top + BAR_H;

    // Outline: CE has no FrameRect, so draw a hollow rectangle with the
    // black stock pen (Rectangle excludes right/bottom, like FillRect).
    hpenOld = (HPEN)SelectObject(hdc, GetStockObject(BLACK_PEN));
    hbrOld = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rcBar.left, rcBar.top, rcBar.right, rcBar.bottom);
    SelectObject(hdc, hbrOld);
    SelectObject(hdc, hpenOld);

    rcFill = rcBar;
    InflateRect(&rcFill, -1, -1);
    rcFill.right = rcFill.left + (int)(((rcFill.right - rcFill.left) * (LONG)g_dwLoadTenths) / 1000);
    if (rcFill.right > rcFill.left)
    {
        hbrFill = CreateSolidBrush(RGB(0, 128, 0));
        if (hbrFill)
        {
            FillRect(hdc, &rcFill, hbrFill);
            DeleteObject(hbrFill);
        }
    }

    EndPaint(hWnd, &ps);
}

// ---------------------------------------------------------------------------
// WndProc
//
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        Sample();
        SetTimer(hWnd, IDT_REFRESH, REFRESH_MS, NULL);
        return 0;

    case WM_TIMER:
        if (wParam == IDT_REFRESH)
        {
            Sample();
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;

    case WM_PAINT:
        OnPaint(hWnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_REFRESH);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WinMain
//
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR pszCmdLine, int nCmdShow)
{
    WNDCLASS wc = {0};
    HWND hWnd;
    MSG msg;
    HDC hdc;
    TEXTMETRIC tm;
    RECT rcWin, rcWork;
    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    DWORD dwExStyle = 0;
    int cxWin, cyWin, x, y;

    // One instance only: bring the existing window to the front.
    hWnd = FindWindow(IDLEMON_CLASS, NULL);
    if (hWnd)
    {
        SetForegroundWindow(hWnd);
        return 0;
    }

    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = NULL;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = IDLEMON_CLASS;
    if (!RegisterClass(&wc))
        return 1;

    // Line height from the system font
    hdc = GetDC(NULL);
    if (hdc)
    {
        if (GetTextMetrics(hdc, &tm))
            g_cyLine = tm.tmHeight + tm.tmExternalLeading + 2;
        ReleaseDC(NULL, hdc);
    }

    // Window size from the wanted client size
    rcWin.left = 0;
    rcWin.top = 0;
    rcWin.right = CLIENT_W;
    rcWin.bottom = 2 * MARGIN + TEXT_LINES * g_cyLine + 4 + BAR_H;
    AdjustWindowRectEx(&rcWin, dwStyle, FALSE, dwExStyle);
    cxWin = rcWin.right - rcWin.left;
    cyWin = rcWin.bottom - rcWin.top;

    // Center on the work area (screen minus taskbar)
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0))
    {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    x = rcWork.left + ((rcWork.right - rcWork.left) - cxWin) / 2;
    y = rcWork.top + ((rcWork.bottom - rcWork.top) - cyWin) / 2;

    hWnd = CreateWindowEx(dwExStyle, IDLEMON_CLASS, IDLEMON_TITLE, dwStyle,
                          x, y, cxWin, cyWin, NULL, NULL, hInstance, NULL);
    if (!hWnd)
        return 1;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

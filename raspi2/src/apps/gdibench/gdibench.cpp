//
// gdibench.cpp - GDI drawing benchmark (src\apps\gdibench, gdibench.exe)
//
// Opens a window covering the whole screen and times the display driver
// paths GDI uses most, each for about TEST_MS milliseconds:
//
//   Fill      FillRect of 256x256 solid rectangles (write only)
//   Invert    PatBlt DSTINVERT on 256x256 (read-modify-write of the screen)
//   Scroll    BitBlt of the whole window 8 lines up (screen to screen)
//   Text      ExtTextOut of one 60 character line
//   Upload    BitBlt 256x256 from a memory DC to the screen
//   Readback  BitBlt 256x256 from the screen to a memory DC
//
// For each test it shows the number of operations per second and the pixel
// rate, in the window and in the debug output (KITL), so runs with
// different drivers or settings can be compared. Click or press a key to
// close the window when the results are shown.
//
// GDI calls on CE are synchronous (the driver has finished when the call
// returns), so GetTickCount around a loop measures the driver.
//
// Optional image component: set BSP_GDIBENCH=1 (catalog item "GDI benchmark
// (gdibench.exe)") to put gdibench.exe in \Windows and a shortcut on the
// desktop.
//

#include <windows.h>

#define GDIBENCH_CLASS  TEXT("RaspiGdiBench")
#define GDIBENCH_TITLE  TEXT("GDI Benchmark")
#define WM_RUNTESTS     (WM_USER + 1)

#define TEST_MS         2000
#define BLOCK           256
#define SCROLL_LINES    8
#define NUM_TESTS       6
#define RESULT_LEN      96

static TCHAR g_szResults[NUM_TESTS + 2][RESULT_LEN];
static int   g_nResults = 0;
static BOOL  g_fDone = FALSE;
static DWORD g_dwSeed = 12345;

static const TCHAR g_szLine[] =
    TEXT("The quick brown fox jumps over the lazy dog 0123456789 ABCDEF");

// ---------------------------------------------------------------------------
// Rand - small LCG, so every run draws the same pattern.
//
static DWORD Rand(int nRange)
{
    if (nRange <= 0) return 0;
    g_dwSeed = g_dwSeed * 1103515245 + 12345;
    return ((g_dwSeed >> 16) & 0x7fff) % (DWORD)nRange;
}

// ---------------------------------------------------------------------------
// AddResult - records one result line and sends it to the debug output.
//
static void AddResult(LPCTSTR pszName, DWORD dwOps, DWORD dwMs, DWORD dwPixPerOp)
{
    DWORD dwOpsPerSec, dwKPixPerSec;

    if (dwMs == 0) dwMs = 1;
    dwOpsPerSec = (dwOps * 1000) / dwMs;
    // kilopixels per second, computed in steps that do not overflow 32 bits
    dwKPixPerSec = (DWORD)(((unsigned __int64)dwOps * dwPixPerOp) / dwMs);

    if (g_nResults < NUM_TESTS + 2)
    {
        wsprintf(g_szResults[g_nResults], TEXT("%s: %u ops/s, %u kpix/s (%u ops in %u ms)"),
                 pszName, dwOpsPerSec, dwKPixPerSec, dwOps, dwMs);
        RETAILMSG(1, (TEXT("gdibench: %s\r\n"), g_szResults[g_nResults]));
        g_nResults++;
    }
}

// ---------------------------------------------------------------------------
// RunTests - runs every test on the window DC. Each loop runs until TEST_MS
// has passed.
//
static void RunTests(HWND hWnd)
{
    HDC hdc, hdcMem;
    HBITMAP hbmMem, hbmOld;
    HBRUSH hbr;
    RECT rc, rcClient;
    DWORD dwStart, dwMs, dwOps;
    int cx, cy, nLen, i;
    SIZE sz;

    GetClientRect(hWnd, &rcClient);
    cx = rcClient.right;
    cy = rcClient.bottom;
    nLen = lstrlen(g_szLine);

    hdc = GetDC(hWnd);
    hdcMem = CreateCompatibleDC(hdc);
    hbmMem = CreateCompatibleBitmap(hdc, BLOCK, BLOCK);
    hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

    wsprintf(g_szResults[g_nResults++], TEXT("Screen %dx%d, %d bpp, %d ms per test"),
             GetDeviceCaps(hdc, HORZRES), GetDeviceCaps(hdc, VERTRES),
             GetDeviceCaps(hdc, BITSPIXEL), TEST_MS);
    RETAILMSG(1, (TEXT("gdibench: %s\r\n"), g_szResults[0]));

    // Something to upload: a pattern in the memory bitmap
    for (i = 0; i < 16; i++)
    {
        hbr = CreateSolidBrush(RGB(i * 16, 255 - i * 16, (i * 64) & 0xff));
        SetRect(&rc, 0, i * (BLOCK / 16), BLOCK, (i + 1) * (BLOCK / 16));
        FillRect(hdcMem, &rc, hbr);
        DeleteObject(hbr);
    }

    // Fill
    hbr = CreateSolidBrush(RGB(40, 90, 160));
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        rc.left = Rand(cx - BLOCK);
        rc.top = Rand(cy - BLOCK);
        rc.right = rc.left + BLOCK;
        rc.bottom = rc.top + BLOCK;
        FillRect(hdc, &rc, hbr);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    DeleteObject(hbr);
    AddResult(TEXT("Fill"), dwOps, dwMs, BLOCK * BLOCK);

    // Invert
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        PatBlt(hdc, Rand(cx - BLOCK), Rand(cy - BLOCK), BLOCK, BLOCK, DSTINVERT);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    AddResult(TEXT("Invert"), dwOps, dwMs, BLOCK * BLOCK);

    // Scroll
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        BitBlt(hdc, 0, 0, cx, cy - SCROLL_LINES, hdc, 0, SCROLL_LINES, SRCCOPY);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    AddResult(TEXT("Scroll"), dwOps, dwMs, cx * (cy - SCROLL_LINES));

    // Text
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkColor(hdc, RGB(0, 0, 0));
    SetBkMode(hdc, OPAQUE);
    GetTextExtentPoint32(hdc, g_szLine, nLen, &sz);
    if (sz.cx >= cx) sz.cx = cx - 1;
    if (sz.cy <= 0) sz.cy = 1;
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        ExtTextOut(hdc, Rand(cx - sz.cx), Rand(cy - sz.cy), 0, NULL,
                   g_szLine, nLen, NULL);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    AddResult(TEXT("Text"), dwOps, dwMs, sz.cx * sz.cy);

    // Upload
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        BitBlt(hdc, Rand(cx - BLOCK), Rand(cy - BLOCK), BLOCK, BLOCK,
               hdcMem, 0, 0, SRCCOPY);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    AddResult(TEXT("Upload"), dwOps, dwMs, BLOCK * BLOCK);

    // Readback
    dwOps = 0;
    dwStart = GetTickCount();
    do
    {
        BitBlt(hdcMem, 0, 0, BLOCK, BLOCK,
               hdc, Rand(cx - BLOCK), Rand(cy - BLOCK), SRCCOPY);
        dwOps++;
        dwMs = GetTickCount() - dwStart;
    } while (dwMs < TEST_MS);
    AddResult(TEXT("Readback"), dwOps, dwMs, BLOCK * BLOCK);

    lstrcpy(g_szResults[g_nResults++], TEXT("Done. Click or press a key to close."));

    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    ReleaseDC(hWnd, hdc);

    g_fDone = TRUE;
    InvalidateRect(hWnd, NULL, TRUE);
}

// ---------------------------------------------------------------------------
// WndProc
//
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;
    TEXTMETRIC tm;
    int i, y;

    switch (msg)
    {
    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        GetTextMetrics(hdc, &tm);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        y = 10;
        if (!g_fDone)
        {
            ExtTextOut(hdc, 10, y, 0, NULL, TEXT("Running..."), 10, NULL);
        }
        else
        {
            for (i = 0; i < g_nResults; i++)
            {
                ExtTextOut(hdc, 10, y, 0, NULL, g_szResults[i],
                           lstrlen(g_szResults[i]), NULL);
                y += tm.tmHeight + 2;
            }
        }
        EndPaint(hWnd, &ps);
        return 0;

    case WM_RUNTESTS:
        RunTests(hWnd);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_KEYDOWN:
        if (g_fDone) DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// WinMain
//
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    HWND hWnd;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = GDIBENCH_CLASS;
    if (!RegisterClass(&wc)) return 1;

    // Whole screen, on top of the taskbar
    hWnd = CreateWindowEx(WS_EX_TOPMOST, GDIBENCH_CLASS, GDIBENCH_TITLE,
                          WS_POPUP | WS_VISIBLE, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN),
                          GetSystemMetrics(SM_CYSCREEN),
                          NULL, NULL, hInstance, NULL);
    if (hWnd == NULL) return 1;

    UpdateWindow(hWnd);
    PostMessage(hWnd, WM_RUNTESTS, 0, 0);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

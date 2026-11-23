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
//   Upload    BitBlt 256x256 to the screen from a screen-compatible bitmap
//   Readback  BitBlt 256x256 from the screen to a screen-compatible bitmap
//   Up565     BitBlt 256x256 from a 16 bpp RGB565 DIB section
//   Up24      BitBlt 256x256 from a 24 bpp DIB section (format conversion)
//   Up32      BitBlt 256x256 from an opaque 32 bpp DIB section (conversion)
//   Stretch   StretchBlt 256x256 -> 384x384 from a compatible bitmap
//   Transp    TransparentImage 256x256 from a compatible bitmap (colour key)
//   AlphaC    AlphaBlend 256x256, constant alpha 128, compatible bitmap
//   AlphaP    AlphaBlend 256x256, per-pixel alpha, 32 bpp premultiplied DIB
//
// When the tests are done, the window also shows the 16, 24 and 32 bpp
// source bitmaps side by side, copied to the screen through the same paths
// as Up565, Up24 and Up32. They hold the same gradient, so the three tiles
// must look identical; a difference means a conversion routine has a
// channel or byte order wrong.
//
// The pixel rate counts destination pixels. For each test it shows the
// number of operations per second and the pixel rate, in the window and in
// the debug output (KITL), so runs with different drivers or settings can be
// compared. Click or press a key to close the window when the results are
// shown.
//
// AlphaBlend only exists when the OS design has the alpha blend component
// (GWES_MGALPHABLEND), so it is looked up at run time and the two Alpha
// tests are skipped without it.
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
#define STRETCH_TO      384
#define SCROLL_LINES    8
#define RESULT_LEN      96

enum
{
    T_FILL, T_INVERT, T_SCROLL, T_TEXT, T_UPLOAD, T_READBACK,
    T_UP565, T_UP24, T_UP32, T_STRETCH, T_TRANSP, T_ALPHAC, T_ALPHAP,
    NUM_TESTS
};

static const LPCTSTR g_apszNames[NUM_TESTS] =
{
    TEXT("Fill"), TEXT("Invert"), TEXT("Scroll"), TEXT("Text"),
    TEXT("Upload"), TEXT("Readback"), TEXT("Up565"), TEXT("Up24"),
    TEXT("Up32"), TEXT("Stretch"), TEXT("Transp"), TEXT("AlphaC"),
    TEXT("AlphaP")
};

typedef BOOL (WINAPI *PFN_ALPHABLEND)(HDC, int, int, int, int,
                                      HDC, int, int, int, int, BLENDFUNCTION);

// Everything one operation needs
typedef struct
{
    HDC hdc;                // window
    int cx, cy;             // window size
    HDC hdcCompat;          // screen-compatible bitmap
    HDC hdc565;             // 16 bpp RGB565 DIB section
    HDC hdc24;              // 24 bpp DIB section
    HDC hdc32;              // 32 bpp DIB section, opaque (alpha 255)
    HDC hdc32a;             // 32 bpp DIB section, premultiplied alpha ramp
    HBRUSH hbrFill;
    int nTextLen;
    SIZE szText;
    COLORREF crKey;         // TransparentImage colour key
    PFN_ALPHABLEND pfnAlphaBlend;
} BENCHCTX;

// Source bitmaps: kept until the window closes, for the visual check
static BENCHCTX g_c;
static HBITMAP g_hbmCompat, g_hbmCompatOld, g_hbm565, g_hbm565Old;
static HBITMAP g_hbm24, g_hbm24Old, g_hbm32, g_hbm32Old, g_hbm32a, g_hbm32aOld;

static TCHAR g_szResults[NUM_TESTS + 2][RESULT_LEN];
static int   g_nResults = 0;
static BOOL  g_fDone = FALSE;
static DWORD g_dwSeed = 12345;

static const TCHAR g_szLine[] =
    TEXT("The quick brown fox jumps over the lazy dog 0123456789 ABCDEF");

// ---------------------------------------------------------------------------
// Rand - small LCG, so every run draws the same pattern. Returns 0..nRange-1.
//
static DWORD Rand(int nRange)
{
    if (nRange <= 0) return 0;
    g_dwSeed = g_dwSeed * 1103515245 + 12345;
    return ((g_dwSeed >> 16) & 0x7fff) % (DWORD)nRange;
}

// ---------------------------------------------------------------------------
// AddLine - records one line of output and sends it to the debug output.
//
static void AddLine(LPCTSTR pszLine)
{
    if (g_nResults < NUM_TESTS + 2)
    {
        int i;
        for (i = 0; i < RESULT_LEN - 1 && pszLine[i] != 0; i++)
            g_szResults[g_nResults][i] = pszLine[i];
        g_szResults[g_nResults][i] = 0;
        RETAILMSG(1, (TEXT("gdibench: %s\r\n"), g_szResults[g_nResults]));
        g_nResults++;
    }
}

// ---------------------------------------------------------------------------
// AddResult - formats one test result.
//
static void AddResult(LPCTSTR pszName, DWORD dwOps, DWORD dwMs, DWORD dwPixPerOp)
{
    TCHAR szLine[RESULT_LEN];
    DWORD dwOpsPerSec, dwKPixPerSec;

    if (dwMs == 0) dwMs = 1;
    dwOpsPerSec = (dwOps * 1000) / dwMs;
    // kilopixels per second, without overflowing 32 bits
    dwKPixPerSec = (DWORD)(((unsigned __int64)dwOps * dwPixPerOp) / dwMs);

    wsprintf(szLine, TEXT("%s: %u ops/s, %u kpix/s (%u ops in %u ms)"),
             pszName, dwOpsPerSec, dwKPixPerSec, dwOps, dwMs);
    AddLine(szLine);
}

// ---------------------------------------------------------------------------
// CreateDib - creates a BLOCK x BLOCK top-down DIB section of nBpp (16 =
// RGB565 bit fields, 24, 32) filled with a colour gradient, and selects it
// into a new memory DC. For 32 bpp with fPremul the alpha channel runs
// 0..255 across the bitmap and the colours are premultiplied, as AlphaBlend
// AC_SRC_ALPHA expects; without it alpha is 255. Returns the DC (the bitmap
// is returned through phbm), or NULL.
//
static HDC CreateDib(HDC hdc, int nBpp, BOOL fPremul, HBITMAP *phbm,
                     HBITMAP *phbmOld)
{
    struct
    {
        BITMAPINFOHEADER bmih;
        DWORD adwMasks[3];
    } bi;
    HDC hdcMem;
    HBITMAP hbm;
    BYTE *pBits = NULL;
    int x, y, nStride;
    DWORD r, g, b, a;

    memset(&bi, 0, sizeof(bi));
    bi.bmih.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmih.biWidth = BLOCK;
    bi.bmih.biHeight = -BLOCK;          // top-down
    bi.bmih.biPlanes = 1;
    bi.bmih.biBitCount = (WORD)nBpp;
    if (nBpp == 16)
    {
        bi.bmih.biCompression = BI_BITFIELDS;
        bi.adwMasks[0] = 0xF800;
        bi.adwMasks[1] = 0x07E0;
        bi.adwMasks[2] = 0x001F;
    }
    else
    {
        bi.bmih.biCompression = BI_RGB;
    }

    hbm = CreateDIBSection(hdc, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
                           (VOID **)&pBits, NULL, 0);
    if (hbm == NULL || pBits == NULL)
    {
        if (hbm) DeleteObject(hbm);
        return NULL;
    }

    // Rows are DWORD aligned
    nStride = ((BLOCK * nBpp + 31) / 32) * 4;
    for (y = 0; y < BLOCK; y++)
    {
        BYTE *pRow = pBits + y * nStride;
        for (x = 0; x < BLOCK; x++)
        {
            r = x;
            g = y;
            b = (x + y) / 2;
            if (nBpp == 16)
            {
                ((WORD *)pRow)[x] = (WORD)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            else if (nBpp == 24)
            {
                pRow[x * 3 + 0] = (BYTE)b;
                pRow[x * 3 + 1] = (BYTE)g;
                pRow[x * 3 + 2] = (BYTE)r;
            }
            else
            {
                a = fPremul ? x : 255;
                ((DWORD *)pRow)[x] = (a << 24) | ((r * a / 255) << 16) |
                                     ((g * a / 255) << 8) | (b * a / 255);
            }
        }
    }

    hdcMem = CreateCompatibleDC(hdc);
    if (hdcMem == NULL)
    {
        DeleteObject(hbm);
        return NULL;
    }
    *phbmOld = (HBITMAP)SelectObject(hdcMem, hbm);
    *phbm = hbm;
    return hdcMem;
}

// ---------------------------------------------------------------------------
// FreeDc - undoes CreateDib or the compatible bitmap setup.
//
static void FreeDc(HDC hdcMem, HBITMAP hbm, HBITMAP hbmOld)
{
    if (hdcMem == NULL) return;
    SelectObject(hdcMem, hbmOld);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
}

// ---------------------------------------------------------------------------
// DoOp - one operation of test nTest at a random place. Returns FALSE if
// the call failed (the test is then reported as failed).
//
static BOOL DoOp(BENCHCTX *pc, int nTest)
{
    RECT rc;
    BLENDFUNCTION bf;
    int x = Rand(pc->cx - BLOCK);
    int y = Rand(pc->cy - BLOCK);

    switch (nTest)
    {
    case T_FILL:
        SetRect(&rc, x, y, x + BLOCK, y + BLOCK);
        return FillRect(pc->hdc, &rc, pc->hbrFill) != 0;

    case T_INVERT:
        return PatBlt(pc->hdc, x, y, BLOCK, BLOCK, DSTINVERT);

    case T_SCROLL:
        return BitBlt(pc->hdc, 0, 0, pc->cx, pc->cy - SCROLL_LINES,
                      pc->hdc, 0, SCROLL_LINES, SRCCOPY);

    case T_TEXT:
        return ExtTextOut(pc->hdc, Rand(pc->cx - pc->szText.cx),
                          Rand(pc->cy - pc->szText.cy), 0, NULL,
                          g_szLine, pc->nTextLen, NULL);

    case T_UPLOAD:
        return BitBlt(pc->hdc, x, y, BLOCK, BLOCK, pc->hdcCompat, 0, 0, SRCCOPY);

    case T_READBACK:
        return BitBlt(pc->hdcCompat, 0, 0, BLOCK, BLOCK, pc->hdc, x, y, SRCCOPY);

    case T_UP565:
        return BitBlt(pc->hdc, x, y, BLOCK, BLOCK, pc->hdc565, 0, 0, SRCCOPY);

    case T_UP24:
        return BitBlt(pc->hdc, x, y, BLOCK, BLOCK, pc->hdc24, 0, 0, SRCCOPY);

    case T_UP32:
        return BitBlt(pc->hdc, x, y, BLOCK, BLOCK, pc->hdc32, 0, 0, SRCCOPY);

    case T_STRETCH:
        return StretchBlt(pc->hdc, Rand(pc->cx - STRETCH_TO),
                          Rand(pc->cy - STRETCH_TO), STRETCH_TO, STRETCH_TO,
                          pc->hdcCompat, 0, 0, BLOCK, BLOCK, SRCCOPY);

    case T_TRANSP:
        return TransparentImage(pc->hdc, x, y, BLOCK, BLOCK, pc->hdcCompat,
                                0, 0, BLOCK, BLOCK, pc->crKey);

    case T_ALPHAC:
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 128;
        bf.AlphaFormat = 0;
        return pc->pfnAlphaBlend(pc->hdc, x, y, BLOCK, BLOCK,
                                 pc->hdcCompat, 0, 0, BLOCK, BLOCK, bf);

    case T_ALPHAP:
        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;
        return pc->pfnAlphaBlend(pc->hdc, x, y, BLOCK, BLOCK,
                                 pc->hdc32a, 0, 0, BLOCK, BLOCK, bf);
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// PixPerOp - destination pixels one operation of nTest writes.
//
static DWORD PixPerOp(BENCHCTX *pc, int nTest)
{
    switch (nTest)
    {
    case T_SCROLL:  return pc->cx * (pc->cy - SCROLL_LINES);
    case T_TEXT:    return pc->szText.cx * pc->szText.cy;
    case T_STRETCH: return STRETCH_TO * STRETCH_TO;
    }
    return BLOCK * BLOCK;
}

// ---------------------------------------------------------------------------
// RunTests - runs every test on the window DC, each until TEST_MS has
// passed.
//
static void RunTests(HWND hWnd)
{
    BENCHCTX &c = g_c;
    HBRUSH hbr;
    RECT rc, rcClient;
    TCHAR szLine[RESULT_LEN];
    DWORD dwStart, dwMs, dwOps;
    BOOL fOk;
    int i, t;

    memset(&c, 0, sizeof(c));
    GetClientRect(hWnd, &rcClient);
    c.cx = rcClient.right;
    c.cy = rcClient.bottom;
    c.hdc = GetDC(hWnd);
    c.nTextLen = lstrlen(g_szLine);
    c.pfnAlphaBlend = (PFN_ALPHABLEND)GetProcAddress(
        GetModuleHandle(TEXT("coredll.dll")), TEXT("AlphaBlend"));

    wsprintf(szLine, TEXT("Screen %dx%d, %d bpp, %d ms per test"),
             GetDeviceCaps(c.hdc, HORZRES), GetDeviceCaps(c.hdc, VERTRES),
             GetDeviceCaps(c.hdc, BITSPIXEL), TEST_MS);
    AddLine(szLine);

    // Screen-compatible bitmap with 16 colour stripes; the first stripe's
    // colour is the TransparentImage key
    c.hdcCompat = CreateCompatibleDC(c.hdc);
    g_hbmCompat = CreateCompatibleBitmap(c.hdc, BLOCK, BLOCK);
    g_hbmCompatOld = (HBITMAP)SelectObject(c.hdcCompat, g_hbmCompat);
    for (i = 0; i < 16; i++)
    {
        hbr = CreateSolidBrush(RGB(i * 16, 255 - i * 16, (i * 64) & 0xff));
        SetRect(&rc, 0, i * (BLOCK / 16), BLOCK, (i + 1) * (BLOCK / 16));
        FillRect(c.hdcCompat, &rc, hbr);
        DeleteObject(hbr);
    }
    c.crKey = GetPixel(c.hdcCompat, 0, 0);

    c.hdc565 = CreateDib(c.hdc, 16, FALSE, &g_hbm565, &g_hbm565Old);
    c.hdc24 = CreateDib(c.hdc, 24, FALSE, &g_hbm24, &g_hbm24Old);
    c.hdc32 = CreateDib(c.hdc, 32, FALSE, &g_hbm32, &g_hbm32Old);
    c.hdc32a = CreateDib(c.hdc, 32, TRUE, &g_hbm32a, &g_hbm32aOld);

    c.hbrFill = CreateSolidBrush(RGB(40, 90, 160));
    SetTextColor(c.hdc, RGB(255, 255, 255));
    SetBkColor(c.hdc, RGB(0, 0, 0));
    SetBkMode(c.hdc, OPAQUE);
    GetTextExtentPoint32(c.hdc, g_szLine, c.nTextLen, &c.szText);
    if (c.szText.cx >= c.cx) c.szText.cx = c.cx - 1;
    if (c.szText.cy <= 0) c.szText.cy = 1;

    for (t = 0; t < NUM_TESTS; t++)
    {
        // Skip tests whose source or API is missing
        if ((t == T_UP565 && c.hdc565 == NULL) ||
            (t == T_UP24 && c.hdc24 == NULL) ||
            (t == T_UP32 && c.hdc32 == NULL) ||
            (t == T_ALPHAP && c.hdc32a == NULL))
        {
            wsprintf(szLine, TEXT("%s: skipped (CreateDIBSection failed)"),
                     g_apszNames[t]);
            AddLine(szLine);
            continue;
        }
        if ((t == T_ALPHAC || t == T_ALPHAP) && c.pfnAlphaBlend == NULL)
        {
            wsprintf(szLine, TEXT("%s: skipped (no AlphaBlend in this image)"),
                     g_apszNames[t]);
            AddLine(szLine);
            continue;
        }

        dwOps = 0;
        fOk = TRUE;
        dwStart = GetTickCount();
        do
        {
            if (!DoOp(&c, t))
            {
                fOk = FALSE;
                break;
            }
            dwOps++;
            dwMs = GetTickCount() - dwStart;
        } while (dwMs < TEST_MS);

        if (fOk)
        {
            AddResult(g_apszNames[t], dwOps, dwMs, PixPerOp(&c, t));
        }
        else
        {
            wsprintf(szLine, TEXT("%s: failed after %u ops (error %u)"),
                     g_apszNames[t], dwOps, GetLastError());
            AddLine(szLine);
        }
    }

    AddLine(TEXT("Done. Click or press a key to close."));

    // The source bitmaps stay for the visual check (FreeSources)
    DeleteObject(c.hbrFill);
    c.hbrFill = NULL;
    ReleaseDC(hWnd, c.hdc);
    c.hdc = NULL;

    g_fDone = TRUE;
    InvalidateRect(hWnd, NULL, TRUE);
}

// ---------------------------------------------------------------------------
// FreeSources - frees the source bitmaps RunTests created.
//
static void FreeSources(void)
{
    FreeDc(g_c.hdc32a, g_hbm32a, g_hbm32aOld);
    FreeDc(g_c.hdc32, g_hbm32, g_hbm32Old);
    FreeDc(g_c.hdc24, g_hbm24, g_hbm24Old);
    FreeDc(g_c.hdc565, g_hbm565, g_hbm565Old);
    FreeDc(g_c.hdcCompat, g_hbmCompat, g_hbmCompatOld);
    memset(&g_c, 0, sizeof(g_c));
}

// ---------------------------------------------------------------------------
// WndProc
//
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static const LPCTSTR s_apszTiles[3] =
        { TEXT("16 bpp (565)"), TEXT("24 bpp"), TEXT("32 bpp") };
    PAINTSTRUCT ps;
    HDC hdc, hdcTile;
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

            // Visual check: the three source formats must look the same
            y += tm.tmHeight;
            ExtTextOut(hdc, 10, y, 0, NULL,
                       TEXT("These three tiles must look identical:"), 38, NULL);
            y += tm.tmHeight + 4;
            for (i = 0; i < 3; i++)
            {
                hdcTile = (i == 0) ? g_c.hdc565 : (i == 1) ? g_c.hdc24 : g_c.hdc32;
                if (hdcTile != NULL)
                {
                    BitBlt(hdc, 10 + i * (BLOCK + 20), y, BLOCK, BLOCK,
                           hdcTile, 0, 0, SRCCOPY);
                }
                ExtTextOut(hdc, 10 + i * (BLOCK + 20), y + BLOCK + 4, 0, NULL,
                           s_apszTiles[i], lstrlen(s_apszTiles[i]), NULL);
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
        FreeSources();
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

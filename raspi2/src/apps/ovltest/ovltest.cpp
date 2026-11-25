//
// ovltest.cpp - video overlay test (src\apps\ovltest, ovltest.exe)
//
// Exercises vc4disp's hardware video overlay through its private escape
// (src\INC\vc4ovl.h): a 640x360 YUV image on a VideoCore HVS plane, scaled
// and colour-converted by the hardware. It renders an animated pattern into
// the overlay buffers and flips them, in six phases of PHASE_MS each:
//
//   1  YV12, 3 buffers, as fast as possible, full screen
//   2  YV12, 3 buffers, 30 fps, full screen
//   3  YV12, 2 buffers, 30 fps, full screen (tearing check)
//   4  YV12, 3 buffers, 30 fps, 960x540 window moving around
//   5  YV12, 3 buffers, 30 fps, clipped (top-left, then bottom-right)
//   6  I420, 3 buffers, 30 fps, full screen
//
// (The overlay takes planar 4:2:0 only: packed YUY2/UYVY planes were
// accepted by the firmware but never shown.)
//
// The pattern: a grey ramp scrolling left, a white vertical bar moving
// right (tearing shows as a broken bar), a coloured horizontal band moving
// down whose colour changes every second (yellow, cyan, green, magenta,
// red, blue: wrong colours mean U and V are swapped), and in the top-left
// corner one white square per phase number, so what is on screen can be
// matched with the log.
//
// With N buffers the next frame is drawn into the buffer that left the
// screen N-1 flips ago. Each flip waits until the firmware has answered
// (VC4OVL_FLAG_WAIT).
//
// For each phase it reports frames per second, render time, flip wait and
// idle percentage (GetIdleTime) in the window and the debug output (lines
// starting with "ovltest:"). Esc aborts. Click or press a key to close the
// window when the results are shown.
//
// Optional image component: set BSP_OVLTEST=1 (catalog item "Video overlay
// test (ovltest.exe)").
//

#include <windows.h>
#include <vc4ovl.h>

#define OVLTEST_CLASS   TEXT("RaspiOvlTest")
#define OVLTEST_TITLE   TEXT("Overlay Test")
#define WM_RUNTESTS     (WM_USER + 1)

#define PHASE_MS        6000
#define SRC_W           640
#define SRC_H           360
#define WIN_W           960
#define WIN_H           540
#define BAR_W           16
#define BAND_H          32
#define MARK_SIZE       16
#define NUM_PHASES      6
#define RESULT_LEN      160

enum { MODE_MAX, MODE_FULL, MODE_MOVE, MODE_CLIP };

typedef struct {
    LPCTSTR pszName;
    DWORD   dwFourCC;
    DWORD   dwBuffers;
    int     mode;
} PHASE;

static const PHASE g_phases[NUM_PHASES] = {
    { TEXT("YV12 x3, max rate, full screen"),   VC4OVL_FOURCC_YV12, 3, MODE_MAX  },
    { TEXT("YV12 x3, 30 fps, full screen"),     VC4OVL_FOURCC_YV12, 3, MODE_FULL },
    { TEXT("YV12 x2, 30 fps, full screen"),     VC4OVL_FOURCC_YV12, 2, MODE_FULL },
    { TEXT("YV12 x3, 30 fps, moving window"),   VC4OVL_FOURCC_YV12, 3, MODE_MOVE },
    { TEXT("YV12 x3, 30 fps, clipped"),         VC4OVL_FOURCC_YV12, 3, MODE_CLIP },
    { TEXT("I420 x3, 30 fps, full screen"),     VC4OVL_FOURCC_I420, 3, MODE_FULL },
};

// Band colours (BT.601 limited range Y, U, V)
static const BYTE g_colours[6][3] = {
    { 210,  16, 146 },  // yellow
    { 170, 166,  16 },  // cyan
    { 145,  54,  34 },  // green
    { 106, 202, 222 },  // magenta
    {  81,  90, 240 },  // red
    {  41, 240, 110 },  // blue
};

static HWND  g_hwnd;
static HDC   g_hdc;
static TCHAR g_results[NUM_PHASES + 2][RESULT_LEN];
static int   g_nResults;
static BOOL  g_fDone;

// Row buffers (cached), copied into the uncached overlay rows
static BYTE  g_yRow[SRC_W], g_yBand[SRC_W];
static BYTE  g_uRow[SRC_W / 2], g_vRow[SRC_W / 2];
static BYTE  g_uBand[SRC_W / 2], g_vBand[SRC_W / 2];

//------------------------------------------------------------------------------

static void Log(LPCTSTR pszFormat, ...)
{
    TCHAR sz[RESULT_LEN + 32];
    va_list args;

    va_start(args, pszFormat);
    wvsprintf(sz, pszFormat, args);
    va_end(args);
    OutputDebugString(sz);
    OutputDebugString(TEXT("\r\n"));
}

static BOOL Ovl(DWORD dwCommand, VC4OVL_REQUEST *pReq, VC4OVL_INFO *pInfo)
{
    pReq->dwSize = sizeof(*pReq);
    pReq->dwCommand = dwCommand;
    pInfo->dwSize = sizeof(*pInfo);
    return ExtEscape(g_hdc, VC4OVL_ESCAPE, sizeof(*pReq), (LPCSTR)pReq,
        sizeof(*pInfo), (LPSTR)pInfo) > 0;
}

//------------------------------------------------------------------------------
//
//  Function:  BuildRows
//
//  Builds this frame's rows: the normal row (ramp + bar, grey chroma), the
//  band row (band colour + bar).
//
static void BuildRows(DWORD frame)
{
    const BYTE *pc = g_colours[(frame / 30) % 6];
    DWORD x, barX = (frame * 8) % (SRC_W - BAR_W);

    for (x = 0; x < SRC_W; x++) {
        DWORD r = (x + frame * 4) % SRC_W;
        g_yRow[x] = (BYTE)(16 + (r * 219) / (SRC_W - 1));
        g_yBand[x] = pc[0];
        if (x >= barX && x < barX + BAR_W) {
            g_yRow[x] = 235;
            g_yBand[x] = 235;
        }
    }
    for (x = 0; x < SRC_W / 2; x++) {
        g_uRow[x] = 128;
        g_vRow[x] = 128;
        g_uBand[x] = pc[1];
        g_vBand[x] = pc[2];
    }
}

//------------------------------------------------------------------------------
//
//  Function:  RenderFrame
//
static void RenderFrame(const VC4OVL_INFO *pi, BYTE *pFrame, DWORD frame,
    DWORD phase)
{
    DWORD y, bandTop, m;

    BuildRows(frame);
    bandTop = ((frame * 2) % (SRC_H - BAND_H)) & ~1;

    {
        BYTE *pY = pFrame + pi->dwOffset[0];
        BYTE *pU, *pV;

        // Memory order: I420 Y U V, YV12 Y V U
        if (pi->dwFourCC == VC4OVL_FOURCC_I420) {
            pU = pFrame + pi->dwOffset[1];
            pV = pFrame + pi->dwOffset[2];
        } else {
            pV = pFrame + pi->dwOffset[1];
            pU = pFrame + pi->dwOffset[2];
        }

        for (y = 0; y < SRC_H; y++) {
            BOOL fBand = (y >= bandTop && y < bandTop + BAND_H);
            memcpy(pY + y * pi->dwPitch, fBand ? g_yBand : g_yRow, SRC_W);
        }
        for (y = 0; y < SRC_H / 2; y++) {
            BOOL fBand = (y * 2 >= bandTop && y * 2 < bandTop + BAND_H);
            memcpy(pU + y * pi->dwChromaPitch, fBand ? g_uBand : g_uRow, SRC_W / 2);
            memcpy(pV + y * pi->dwChromaPitch, fBand ? g_vBand : g_vRow, SRC_W / 2);
        }

        // Phase marks: white squares (luma only; chroma grey there unless
        // the band passes over them)
        for (m = 0; m <= phase; m++) {
            for (y = 8; y < 8 + MARK_SIZE; y++) {
                memset(pY + y * pi->dwPitch + 8 + m * (MARK_SIZE + 8), 235, MARK_SIZE);
            }
        }
    }
}

//------------------------------------------------------------------------------
//
//  Function:  DestRect
//
//  Where the overlay goes in this phase at this time.
//
static void DestRect(int mode, DWORD frame, DWORD elapsed, LONG scrW, LONG scrH,
    RECT *prc)
{
    LONG x, y, rangeX, rangeY;

    switch (mode) {
    case MODE_MOVE:
        // Bounce around the screen, 6 px per frame
        rangeX = scrW - WIN_W;
        rangeY = scrH - WIN_H;
        if (rangeX < 1) rangeX = 1;
        if (rangeY < 1) rangeY = 1;
        x = (LONG)((frame * 6) % (2 * rangeX));
        y = (LONG)((frame * 4) % (2 * rangeY));
        if (x > rangeX) x = 2 * rangeX - x;
        if (y > rangeY) y = 2 * rangeY - y;
        SetRect(prc, x, y, x + WIN_W, y + WIN_H);
        break;

    case MODE_CLIP:
        // Screen-sized, a quarter off the top-left, then the bottom-right
        if (elapsed < PHASE_MS / 2) {
            SetRect(prc, -scrW / 4, -scrH / 4, scrW - scrW / 4, scrH - scrH / 4);
        } else {
            SetRect(prc, scrW / 4, scrH / 4, scrW + scrW / 4, scrH + scrH / 4);
        }
        break;

    default:
        SetRect(prc, 0, 0, scrW, scrH);
        break;
    }
}

//------------------------------------------------------------------------------
//
//  Function:  RunPhase
//
//  Returns FALSE when the overlay is missing or Esc was pressed.
//
static BOOL RunPhase(DWORD phase)
{
    const PHASE *pp = &g_phases[phase];
    VC4OVL_REQUEST req;
    VC4OVL_INFO info;
    BYTE *frames[VC4OVL_MAX_BUFFERS];
    DWORD frame = 0, dwStart, dwNow, dwIdle0, dwElapsed, dwT;
    DWORD dwRenderMs = 0, dwFlipMs = 0, dwFlipMax = 0, nFlipFail = 0;
    DWORD shown, next, i, dwIdle;
    RECT rcDst, rcLast;
    LONG scrW, scrH;
    BOOL fOk = TRUE;

    memset(&req, 0, sizeof(req));
    req.dwFourCC = pp->dwFourCC;
    req.dwWidth = SRC_W;
    req.dwHeight = SRC_H;
    req.dwBuffers = pp->dwBuffers;
    if (!Ovl(VC4OVL_CMD_CREATE, &req, &info)) {
        wsprintf(g_results[g_nResults], TEXT("%u %s: CREATE failed (error %u)"),
            phase + 1, pp->pszName, GetLastError());
        Log(TEXT("ovltest: %s"), g_results[g_nResults]);
        g_nResults++;
        return FALSE;
    }
    for (i = 0; i < info.dwBuffers; i++) frames[i] = info.pFrame[i];
    scrW = (LONG)info.dwScreenWidth;
    scrH = (LONG)info.dwScreenHeight;
    Log(TEXT("ovltest: phase %u/%u: %s (pitch %u/%u, frame %u bytes, screen %ux%u)"),
        phase + 1, NUM_PHASES, pp->pszName, info.dwPitch, info.dwChromaPitch,
        info.dwFrameBytes, scrW, scrH);

    // First frame into buffer 0, then show it
    RenderFrame(&info, frames[0], 0, phase);
    shown = 0;
    DestRect(pp->mode, 0, 0, scrW, scrH, &rcDst);
    memset(&req, 0, sizeof(req));
    SetRect(&req.rcSrc, 0, 0, SRC_W, SRC_H);
    req.rcDst = rcDst;
    req.fShow = TRUE;
    req.dwFlags = VC4OVL_FLAG_WAIT;
    if (!Ovl(VC4OVL_CMD_SHOW, &req, &info)) {
        Log(TEXT("ovltest: SHOW failed (error %u)"), GetLastError());
    }
    rcLast = rcDst;

    dwStart = GetTickCount();
    dwIdle0 = GetIdleTime();
    for (;;) {
        dwNow = GetTickCount();
        dwElapsed = dwNow - dwStart;
        if (dwElapsed >= PHASE_MS) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            fOk = FALSE;
            break;
        }

        // Paced phases: wait for this frame's slot (30 fps)
        if (pp->mode != MODE_MAX) {
            DWORD due = ((frame + 1) * 1000) / 30;
            if (due > dwElapsed) Sleep(due - dwElapsed);
        }
        frame++;

        // Draw into the buffer that left the screen longest ago
        next = (shown + 1) % info.dwBuffers;
        dwT = GetTickCount();
        RenderFrame(&info, frames[next], frame, phase);
        dwRenderMs += GetTickCount() - dwT;

        // Move the window if needed (no wait: the flip below waits)
        DestRect(pp->mode, frame, GetTickCount() - dwStart, scrW, scrH, &rcDst);
        if (!EqualRect(&rcDst, &rcLast)) {
            memset(&req, 0, sizeof(req));
            SetRect(&req.rcSrc, 0, 0, SRC_W, SRC_H);
            req.rcDst = rcDst;
            req.fShow = TRUE;
            Ovl(VC4OVL_CMD_SHOW, &req, &info);
            rcLast = rcDst;
        }

        memset(&req, 0, sizeof(req));
        req.dwBuffer = next;
        req.dwFlags = VC4OVL_FLAG_WAIT;
        dwT = GetTickCount();
        if (!Ovl(VC4OVL_CMD_FLIP, &req, &info)) nFlipFail++;
        dwT = GetTickCount() - dwT;
        dwFlipMs += dwT;
        if (dwT > dwFlipMax) dwFlipMax = dwT;
        shown = next;
    }
    dwElapsed = GetTickCount() - dwStart;
    dwIdle = GetIdleTime() - dwIdle0;

    memset(&req, 0, sizeof(req));
    Ovl(VC4OVL_CMD_DESTROY, &req, &info);

    if (frame == 0) frame = 1;
    if (dwElapsed == 0) dwElapsed = 1;
    wsprintf(g_results[g_nResults],
        TEXT("%u %s: %u.%u fps, render %u ms, flip avg %u ms max %u ms, %u failed, idle %u%%"),
        phase + 1, pp->pszName,
        (frame * 1000) / dwElapsed, ((frame * 10000) / dwElapsed) % 10,
        dwRenderMs / frame, dwFlipMs / frame, dwFlipMax, nFlipFail,
        (dwIdle * 100) / dwElapsed);
    Log(TEXT("ovltest: %s"), g_results[g_nResults]);
    g_nResults++;
    return fOk;
}

//------------------------------------------------------------------------------

static void RunTests()
{
    DWORD phase, dwEsc = VC4OVL_ESCAPE;

    g_hdc = GetDC(NULL);
    if (ExtEscape(g_hdc, QUERYESCSUPPORT, sizeof(dwEsc), (LPCSTR)&dwEsc, 0, NULL) <= 0) {
        lstrcpy(g_results[g_nResults++],
            TEXT("The display driver has no video overlay (vc4disp with Overlay=1 needed)."));
        Log(TEXT("ovltest: %s"), g_results[0]);
    } else {
        for (phase = 0; phase < NUM_PHASES; phase++) {
            if (!RunPhase(phase)) break;
        }
    }
    ReleaseDC(NULL, g_hdc);
    g_hdc = NULL;

    lstrcpy(g_results[g_nResults++], TEXT("Click or press a key to close."));
    g_fDone = TRUE;
}

//------------------------------------------------------------------------------

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_RUNTESTS:
        RunTests();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            int i, lineH = 20;

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            SetTextColor(hdc, RGB(255, 255, 255));
            SetBkMode(hdc, TRANSPARENT);
            if (g_nResults == 0) {
                ExtTextOut(hdc, 16, 16, 0, NULL, TEXT("Overlay test running..."), 23, NULL);
            }
            for (i = 0; i < g_nResults; i++) {
                ExtTextOut(hdc, 16, 16 + i * lineH, 0, NULL,
                    g_results[i], lstrlen(g_results[i]), NULL);
            }
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_KEYUP:
        if (g_fDone) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR pszCmdLine, int nShow)
{
    WNDCLASS wc;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = OVLTEST_CLASS;
    if (!RegisterClass(&wc)) return 1;

    g_hwnd = CreateWindowEx(0, OVLTEST_CLASS, OVLTEST_TITLE, WS_POPUP | WS_VISIBLE,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        NULL, NULL, hInst, NULL);
    if (g_hwnd == NULL) return 1;
    UpdateWindow(g_hwnd);
    PostMessage(g_hwnd, WM_RUNTESTS, 0, 0);

    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

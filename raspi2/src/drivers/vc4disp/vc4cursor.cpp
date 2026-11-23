//
// vc4cursor.cpp - VideoCore hardware cursor for vc4disp (CEonPi)
//
// The firmware can overlay a cursor image on the display (property tags
// "Set cursor info" 0x00008010 and "Set cursor state" 0x00008011, sent
// through the OAL's IOCTL_HAL_MY_MAILBOX_PROPERTY). With it the driver no
// longer draws the pointer into the framebuffer, so there is no save/restore
// around every blit that touches it and no flicker.
//
// Tag layouts (firmware wiki "Mailbox property interface"):
//   0x00008010 request: width, height, unused, pointer to pixels, hotspot x,
//              hotspot y; response: 0 = valid, 1 = invalid.
//              Pixels are 32 bpp ARGB. The wiki's size rule is garbled
//              ("width and height >= 16, width * height <= 64"); the default
//              firmware cursor is 64x64, so 16..64 per side is assumed here.
//   0x00008011 request: enable (1 = visible), x, y, flags (bit 0 set =
//              framebuffer coordinates); response: 0 = valid.
//
// Unverified assumptions (check on hardware): the pixel pointer is a
// VideoCore bus address (we pass the uncached alias 0xC0000000 | pa), and
// (x, y) is where the hotspot goes. If any call is rejected, the driver
// logs it and falls back to its software cursor.
//
// CE pointer shapes are monochrome AND/XOR masks. AND=1 XOR=0 is
// transparent, AND=0 gives black (XOR=0) or white (XOR=1). AND=1 XOR=1
// means "invert the screen", which an overlay cannot do; it is drawn black
// (the I-beam over text, usually on a light background).
//
// Cursor moves: the OAL busy-waits for each mailbox reply, and GWES calls
// MovePointer for every mouse report (125-1000 per second) from its input
// thread, which starved other threads (video froze while the mouse moved).
// So MovePointer only stores the newest position; a cursor thread sends it,
// at most once per HwCursorInterval ms, merging the moves in between. It
// also times the firmware calls (1 MHz system timer) and logs a summary
// every VC4_CURSOR_LOG_CALLS calls.
//
// The OAL still busy-waited ~2.2 ms per call inside KernelIoControl, so the
// cursor thread now uses its asynchronous mailbox IOCTL: it posts the
// message from its own uncached buffer and polls for the reply with
// Sleep(1) in between, leaving the CPU to other threads. Without that IOCTL
// (an older OAL), or after a timeout, it falls back to the synchronous call.
//
// Registry value HwCursor=0 keeps the software cursor.
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <image_cfg.h>
#include "vc4disp.h"

#define TAG_SET_CURSOR_INFO     0x00008010
#define TAG_SET_CURSOR_STATE    0x00008011
#define TAG_RESPONSE_OK         0x80000000

#define CURSOR_MIN              16
#define CURSOR_MAX              64
#define CURSOR_BUF_BYTES        (CURSOR_MAX * CURSOR_MAX * 4)

// The asynchronous property message lives after the image (16-byte aligned)
#define CURSOR_MSG_BYTES        64
#define CURSOR_ALLOC_BYTES      (CURSOR_BUF_BYTES + CURSOR_MSG_BYTES)

// Longest wait for an asynchronous reply, in 1 ms polls
#define VC4_CURSOR_ASYNC_POLLS  100
#define PROPERTY_RESPONSE_OK    0x80000000

#define BUS_UNCACHED_ALIAS      0xC0000000

// Cursor thread priority: above applications, below GWES's input threads
#define VC4_CURSOR_THREAD_PRIO  110

// Calls between two statistics lines (each KITL line costs about 30 ms)
#define VC4_CURSOR_LOG_CALLS    512

// BCM2835 system timer: counter low word at +4, 1 MHz
#define SYSTIMER_PA_BASE        0x20003000
#define SYSTIMER_CLO_DWORD      1

#define ARGB_TRANSPARENT        0x00000000
#define ARGB_BLACK              0xFF000000
#define ARGB_WHITE              0xFFFFFFFF

//------------------------------------------------------------------------------
//
//  Method:  MailboxTags
//
//  Sends a tag list through the OAL and checks that every tag was answered.
//  pTags is overwritten with the response.
//
BOOL Vc4GPE::MailboxTags(DWORD *pTags, DWORD cbTags)
{
    DWORD cbOut = 0, iWord, nWords = cbTags / sizeof(DWORD);

    if (!KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, pTags, cbTags, pTags, cbTags, &cbOut
    )) {
        return FALSE;
    }

    // Every tag must carry the response bit
    for (iWord = 0; iWord + 3 <= nWords; iWord += 3 + pTags[iWord + 1] / 4) {
        if ((pTags[iWord + 2] & TAG_RESPONSE_OK) == 0) return FALSE;
    }
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  HwCursorInit
//
//  Allocates the cursor image buffer (physically contiguous, uncached so the
//  VideoCore sees our writes without cache maintenance). Called once.
//
BOOL Vc4GPE::HwCursorInit()
{
    ULONG pa = 0;

    if (m_pHwCursorBuf != NULL) return TRUE;

    m_pHwCursorBuf = (DWORD*)AllocPhysMem(
        CURSOR_ALLOC_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
    );
    if (m_pHwCursorBuf == NULL) {
        RETAILMSG(1, (L"vc4disp: hardware cursor buffer allocation failed\r\n"));
        return FALSE;
    }
    m_dwHwCursorBus = BUS_UNCACHED_ALIAS | pa;
    m_pHwCursorMsg = (DWORD*)((BYTE*)m_pHwCursorBuf + CURSOR_BUF_BYTES);
    m_dwHwCursorMsgBus = m_dwHwCursorBus + CURSOR_BUF_BYTES;

    // System timer for the call statistics (optional)
    {
        PHYSICAL_ADDRESS phys;
        phys.QuadPart = SYSTIMER_PA_BASE;
        m_pSysTimer = (volatile DWORD*)MmMapIoSpace(phys, PAGE_SIZE, FALSE);
    }

    // Cursor thread
    m_hCursorEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hCursorEvent != NULL) {
        m_hCursorThread = CreateThread(NULL, 0, CursorThreadProc, this, 0, NULL);
    }
    if (m_hCursorThread == NULL) {
        RETAILMSG(1, (L"vc4disp: cursor thread creation failed\r\n"));
        HwCursorDeinit();
        FreePhysMem(m_pHwCursorBuf);
        m_pHwCursorBuf = NULL;
        return FALSE;
    }
    CeSetThreadPriority(m_hCursorThread, VC4_CURSOR_THREAD_PRIO);
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  HwCursorDeinit
//
//  Stops the cursor thread and releases what HwCursorInit set up (except the
//  image buffer, which the destructor frees).
//
VOID Vc4GPE::HwCursorDeinit()
{
    if (m_hCursorThread != NULL) {
        m_fCursorExit = TRUE;
        SetEvent(m_hCursorEvent);
        WaitForSingleObject(m_hCursorThread, 1000);
        CloseHandle(m_hCursorThread);
        m_hCursorThread = NULL;
    }
    if (m_hCursorEvent != NULL) {
        CloseHandle(m_hCursorEvent);
        m_hCursorEvent = NULL;
    }
    if (m_pSysTimer != NULL) {
        MmUnmapIoSpace((PVOID)m_pSysTimer, PAGE_SIZE);
        m_pSysTimer = NULL;
    }
}

//------------------------------------------------------------------------------
//
//  Method:  HwQueuePointerState
//
//  Records the newest cursor state for the cursor thread and wakes it.
//  Returns at once.
//
VOID Vc4GPE::HwQueuePointerState(BOOL fVisible, int x, int y)
{
    EnterCriticalSection(&m_csCursor);
    m_fCursorVisible = fVisible;
    m_xCursor = x;
    m_yCursor = y;
    m_fCursorPending = TRUE;
    m_nCursorMoves++;
    LeaveCriticalSection(&m_csCursor);

    if (m_hCursorEvent != NULL) SetEvent(m_hCursorEvent);
}

//------------------------------------------------------------------------------
//
//  Method:  CursorThreadProc / CursorThread
//
//  Sends the newest queued state, then waits HwCursorInterval ms before the
//  next one, so a fast mouse costs at most one firmware call per interval.
//
DWORD WINAPI Vc4GPE::CursorThreadProc(LPVOID pContext)
{
    ((Vc4GPE*)pContext)->CursorThread();
    return 0;
}

VOID Vc4GPE::CursorThread()
{
    BOOL fVisible, fOk;
    int x, y;
    DWORD dwStart, dwUs, nMoves;

    while (!m_fCursorExit) {
        WaitForSingleObject(m_hCursorEvent, INFINITE);

        while (!m_fCursorExit && !m_fHwCursorFailed) {
            EnterCriticalSection(&m_csCursor);
            if (!m_fCursorPending) {
                LeaveCriticalSection(&m_csCursor);
                break;
            }
            fVisible = m_fCursorVisible;
            x = m_xCursor;
            y = m_yCursor;
            m_fCursorPending = FALSE;
            LeaveCriticalSection(&m_csCursor);

            dwStart = (m_pSysTimer != NULL) ? m_pSysTimer[SYSTIMER_CLO_DWORD] : 0;
            fOk = HwSendPointerState(fVisible, x, y);
            dwUs = (m_pSysTimer != NULL) ? m_pSysTimer[SYSTIMER_CLO_DWORD] - dwStart : 0;

            if (!fOk) {
                // MovePointer switches to the software cursor
                m_fHwCursorFailed = TRUE;
                break;
            }

            m_nCursorCalls++;
            m_dwCursorUsTotal += dwUs;
            if (dwUs > m_dwCursorUsMax) m_dwCursorUsMax = dwUs;
            if (m_nCursorCalls >= VC4_CURSOR_LOG_CALLS) {
                EnterCriticalSection(&m_csCursor);
                nMoves = m_nCursorMoves;
                m_nCursorMoves = 0;
                LeaveCriticalSection(&m_csCursor);
                RETAILMSG(1, (L"vc4disp: cursor: %d firmware calls for %d moves, "
                    L"call avg %d us, max %d us, interval %d ms, %s\r\n",
                    m_nCursorCalls, nMoves, m_dwCursorUsTotal / m_nCursorCalls,
                    m_dwCursorUsMax, m_dwCursorIntervalMs,
                    m_fMailboxAsync ? L"async" : L"sync"
                ));
                m_nCursorCalls = 0;
                m_dwCursorUsTotal = 0;
                m_dwCursorUsMax = 0;
            }

            if (m_dwCursorIntervalMs != 0) Sleep(m_dwCursorIntervalMs);
        }
    }
}

//------------------------------------------------------------------------------
//
//  Method:  HwSetPointerShape
//
//  Converts the AND/XOR masks to ARGB and hands the image to the firmware.
//  Returns FALSE if the hardware cursor cannot show this shape.
//
BOOL Vc4GPE::HwSetPointerShape(
    GPESurf *pMask, int xHotspot, int yHotspot, int xSize, int ySize
) {
    DWORD tags[9];
    UCHAR *pAnd, *pXor, andBits, xorBits, mask;
    DWORD *pRow;
    int row, col, stride;

    if (xSize < CURSOR_MIN || ySize < CURSOR_MIN ||
        xSize > CURSOR_MAX || ySize > CURSOR_MAX
    ) {
        return FALSE;
    }
    if (!HwCursorInit()) return FALSE;

    // The mask surface holds the AND mask followed by the XOR mask
    stride = pMask->Stride();
    pAnd = (UCHAR*)pMask->Buffer();
    pXor = (UCHAR*)pMask->Buffer() + ySize * stride;

    for (row = 0; row < ySize; row++) {
        pRow = m_pHwCursorBuf + row * xSize;
        for (col = 0; col < xSize; col++) {
            andBits = pAnd[row * stride + (col >> 3)];
            xorBits = pXor[row * stride + (col >> 3)];
            mask = 0x80 >> (col & 7);
            if (andBits & mask) {
                pRow[col] = (xorBits & mask) ? ARGB_BLACK : ARGB_TRANSPARENT;
            } else {
                pRow[col] = (xorBits & mask) ? ARGB_WHITE : ARGB_BLACK;
            }
        }
    }

    // Make sure the uncached writes have left the write buffer
    CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);

    tags[0] = TAG_SET_CURSOR_INFO;
    tags[1] = 6 * sizeof(DWORD);
    tags[2] = 0;
    tags[3] = xSize;
    tags[4] = ySize;
    tags[5] = 0;
    tags[6] = m_dwHwCursorBus;
    tags[7] = xHotspot;
    tags[8] = yHotspot;
    if (!MailboxTags(tags, sizeof(tags)) || tags[3] != 0) {
        RETAILMSG(1, (L"vc4disp: Set cursor info %dx%d rejected (resp 0x%08x "
            L"value %d), using the software cursor\r\n",
            xSize, ySize, tags[2], tags[3]
        ));
        return FALSE;
    }

    if (!m_fHwCursorLogged) {
        RETAILMSG(1, (L"vc4disp: hardware cursor on (%dx%d, hotspot %d,%d, "
            L"image at bus 0x%08x)\r\n",
            xSize, ySize, xHotspot, yHotspot, m_dwHwCursorBus
        ));
        m_fHwCursorLogged = TRUE;
    }
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  HwSetPointerState
//
//  Shows the cursor with its hotspot at (x, y), framebuffer coordinates, or
//  hides it.
//
BOOL Vc4GPE::HwSetPointerState(BOOL fVisible, int x, int y)
{
    DWORD tags[7];

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    tags[0] = TAG_SET_CURSOR_STATE;
    tags[1] = 4 * sizeof(DWORD);
    tags[2] = 0;
    tags[3] = fVisible ? 1 : 0;
    tags[4] = x;
    tags[5] = y;
    tags[6] = 1;                        // framebuffer coordinates
    if (!MailboxTags(tags, sizeof(tags)) || tags[3] != 0) {
        RETAILMSG(1, (L"vc4disp: Set cursor state rejected (resp 0x%08x "
            L"value %d)\r\n", tags[2], tags[3]
        ));
        return FALSE;
    }
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  HwSendPointerState
//
//  Cursor thread: sends a cursor state through the OAL's asynchronous
//  mailbox IOCTL and sleeps while the VideoCore works on it. Falls back to
//  the synchronous HwSetPointerState when the asynchronous path is missing
//  or misbehaves.
//
BOOL Vc4GPE::HwSendPointerState(BOOL fVisible, int x, int y)
{
    RASPI_MAILBOX_ASYNC req;
    DWORD *pMsg = m_pHwCursorMsg;
    DWORD dwDone = 0, i;

    if (!m_fMailboxAsync || pMsg == NULL) {
        return HwSetPointerState(fVisible, x, y);
    }

    if (x < 0) x = 0;
    if (y < 0) y = 0;

    // Complete property message: size, request, one tag, end tag
    pMsg[0] = 10 * sizeof(DWORD);
    pMsg[1] = 0;
    pMsg[2] = TAG_SET_CURSOR_STATE;
    pMsg[3] = 4 * sizeof(DWORD);
    pMsg[4] = 0;
    pMsg[5] = fVisible ? 1 : 0;
    pMsg[6] = x;
    pMsg[7] = y;
    pMsg[8] = 1;                        // framebuffer coordinates
    pMsg[9] = 0;

    // The buffer is uncached: make sure the writes have left the write buffer
    CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);

    req.dwCommand = RASPI_MAILBOX_ASYNC_SEND;
    req.dwBusAddr = m_dwHwCursorMsgBus;
    req.pvMessage = pMsg;
    req.cbMessage = 10 * sizeof(DWORD);
    if (!KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req), NULL, 0, NULL)) {
        RETAILMSG(1, (L"vc4disp: asynchronous mailbox unavailable (error %d), "
            L"cursor uses synchronous calls\r\n", GetLastError()
        ));
        m_fMailboxAsync = FALSE;
        return HwSetPointerState(fVisible, x, y);
    }

    req.dwCommand = RASPI_MAILBOX_ASYNC_POLL;
    for (i = 0; i < VC4_CURSOR_ASYNC_POLLS; i++) {
        Sleep(1);
        if (!KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req),
                &dwDone, sizeof(dwDone), NULL)) {
            break;
        }
        if (dwDone) {
            if (pMsg[1] != PROPERTY_RESPONSE_OK ||
                (pMsg[4] & PROPERTY_RESPONSE_OK) == 0 || pMsg[5] != 0
            ) {
                RETAILMSG(1, (L"vc4disp: Set cursor state rejected (resp 0x%08x "
                    L"0x%08x value %d)\r\n", pMsg[1], pMsg[4], pMsg[5]
                ));
                return FALSE;
            }
            return TRUE;
        }
    }

    // No reply: forget the message and stay synchronous from now on, so a
    // late reply can never be taken for a new message's
    req.dwCommand = RASPI_MAILBOX_ASYNC_ABANDON;
    KernelIoControl(IOCTL_HAL_MY_MAILBOX_ASYNC, &req, sizeof(req), NULL, 0, NULL);
    RETAILMSG(1, (L"vc4disp: asynchronous cursor message got no reply, "
        L"cursor uses synchronous calls\r\n"
    ));
    m_fMailboxAsync = FALSE;
    return HwSetPointerState(fVisible, x, y);
}

//
// vc4cursor.cpp - VideoCore hardware cursor for vc4disp (CEonPi)
//
// The firmware can overlay a cursor image on the display (property tags
// "Set cursor info" 0x00008010 and "Set cursor state" 0x00008011, sent
// through the OAL's mailbox IOCTLs). With it the driver no longer draws the
// pointer into the framebuffer, so there is no save/restore around every
// blit that touches it and no flicker.
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
// Verified on a Pi 1 Model B: the pixel pointer is a VideoCore bus address
// (the uncached alias 0xC0000000 | pa), and (x, y) is where the hotspot
// goes. If any call is rejected, the driver logs it and falls back to its
// software cursor.
//
// CE pointer shapes are monochrome AND/XOR masks. AND=1 XOR=0 is
// transparent, AND=0 gives black (XOR=0) or white (XOR=1). AND=1 XOR=1
// means "invert the screen", which an overlay cannot do; it is drawn black
// (the I-beam over text, usually on a light background).
//
// Cursor moves: GWES calls MovePointer for every mouse report (125-1000 per
// second) from its input thread, and a firmware call takes ~2 ms. So
// MovePointer only stores the newest position; the mailbox thread
// (vc4mbox.cpp) sends it at most once per HwCursorInterval ms, merging the
// moves in between, together with any pending overlay plane update.
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

#define CURSOR_MIN              16
#define CURSOR_MAX              64
#define CURSOR_BUF_BYTES        (CURSOR_MAX * CURSOR_MAX * 4)

#define BUS_UNCACHED_ALIAS      0xC0000000

#define ARGB_TRANSPARENT        0x00000000
#define ARGB_BLACK              0xFF000000
#define ARGB_WHITE              0xFFFFFFFF

//------------------------------------------------------------------------------
//
//  Method:  HwCursorInit
//
//  Allocates the cursor image buffer (physically contiguous, uncached so the
//  VideoCore sees our writes without cache maintenance) and starts the
//  mailbox thread. Called on every shape change; does the work once.
//
BOOL Vc4GPE::HwCursorInit()
{
    ULONG pa = 0;

    if (m_pHwCursorBuf == NULL) {
        m_pHwCursorBuf = (DWORD*)AllocPhysMem(
            CURSOR_BUF_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
        );
        if (m_pHwCursorBuf == NULL) {
            RETAILMSG(1, (L"vc4disp: hardware cursor buffer allocation failed\r\n"));
            return FALSE;
        }
        m_dwHwCursorBus = BUS_UNCACHED_ALIAS | pa;
    }

    return MboxInit();
}

//------------------------------------------------------------------------------
//
//  Method:  HwQueuePointerState
//
//  Records the newest cursor state for the mailbox thread and wakes it.
//  Returns at once.
//
VOID Vc4GPE::HwQueuePointerState(BOOL fVisible, int x, int y)
{
    EnterCriticalSection(&m_csMbox);
    m_fCursorVisible = fVisible;
    m_xCursor = x;
    m_yCursor = y;
    m_fCursorPending = TRUE;
    m_nCursorMoves++;
    LeaveCriticalSection(&m_csMbox);

    MboxWake();
}

//------------------------------------------------------------------------------
//
//  Method:  HwBuildCursorTag
//
//  Writes a "Set cursor state" tag at pTag; returns its size in DWORDs.
//  The reply's value word (pTag[3]) must be 0.
//
DWORD Vc4GPE::HwBuildCursorTag(DWORD *pTag, BOOL fVisible, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    pTag[0] = TAG_SET_CURSOR_STATE;
    pTag[1] = 4 * sizeof(DWORD);
    pTag[2] = 0;
    pTag[3] = fVisible ? 1 : 0;
    pTag[4] = x;
    pTag[5] = y;
    pTag[6] = 1;                        // framebuffer coordinates
    return 7;
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

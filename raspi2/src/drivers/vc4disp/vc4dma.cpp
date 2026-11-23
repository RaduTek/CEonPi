//
// vc4dma.cpp - DMA blits for vc4disp (CEonPi)
//
// Screen-to-screen copies (scrolling, window moves) and solid fills on the
// screen are handed to a BCM2835 DMA channel (DISP_DMA_CHANNEL, bsp_cfg.h)
// instead of the CPU. Only the framebuffer qualifies: it is physically
// contiguous, while GDI's system-memory bitmaps are not.
//
// Design:
//  - One control block per row, chained, no 2D mode: no doubt about the 2D
//    row count field, and nothing is ever written outside the rectangle.
//    Rows run bottom-up when the copy moves down (yPositive == 0); copies
//    that need a right-to-left row (xPositive == 0) stay on the CPU.
//  - 128-bit reads and writes with a burst length (registry DmaBurst). The
//    BCM2835 DMA packs misaligned byte streams itself.
//  - Fills read a colour pattern without incrementing the source address.
//    Within a burst the engine still reads consecutive addresses (burst 2
//    drew 8 pixels of colour, 8 of black), so the pattern fills
//    VC4_DMA_FILL_BYTES: one 16-byte beat for each beat of the longest burst.
//  - The CPU draws through the cached framebuffer view (CachedFrameBuffer),
//    so before each transfer the whole L1 D-cache is written back and
//    invalidated (16 KB, cheaper than walking a large range); nothing
//    touches those lines until the transfer ends, so nothing goes stale.
//  - The last block raises SYSINTR_DISPDMA; the blit waits on an event, so
//    the CPU is free for other threads meanwhile. A timeout or a DMA error
//    aborts the channel, logs once, turns DMA off and redoes the blit with
//    GPE's emulation.
//  - Bus address = physical + DmaBusOffset (default 0x40000000, as the USB
//    and audio drivers use).
//
// At load the driver asks the firmware which DMA channels the ARM may use
// (property tag 0x00060001) and only uses its channel if it is in the mask.
//
// Burst length matters (Pi 1 Model B, 2026-11-23): with 128-bit transfers
// and DmaBurst 8 the HDMI signal was lost as soon as the desktop was filled
// (bus offset 0x40000000), or the whole system hung on the first fill
// (0xC0000000). Single transfers (DmaBurst 0) with 0x40000000 work: fills
// land in the right place with the right colours. The first
// VC4_DMA_LOG_OPS operations are logged.
//
// DmaBurst 2 then hung the whole system on the 6th fill, just as the audio
// driver started its own DMA channel. Bursts are not safe here: keep 0.
// Copies (DmaCopy) are off by default: with single transfers they are slower
// than the CPU (52 vs 81 Mpix/s for a full-screen scroll), while fills are
// faster (68 vs 45 Mpix/s).
//
// Registry (Drivers\Display\vc4disp\Config): DmaBlt (1), DmaCopy (0),
// DmaMinPixels (16384), DmaBurst (0), DmaBusOffset (0x40000000).
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <nkintr.h>
#include <winddi.h>
#include <gpe.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <image_cfg.h>
#include <bsp_cfg.h>
#include "vc4disp.h"

// DMA controller (BCM2835 ARM Peripherals, chapter 4)
#define DMA_PA_BASE             0x20007000
#define DMA_PA_SIZE             0x1000
#define DMA_CHAN_STRIDE_DWORDS  (0x100 / sizeof(DWORD))
#define DMA_ENABLE_DWORD        (0xFF0 / sizeof(DWORD))

// Channel register offsets (in DWORDs)
#define DMA_CS                  0
#define DMA_CONBLK_AD           1
#define DMA_DEBUG               8

// CS bits
#define CS_ACTIVE               (1 << 0)
#define CS_END                  (1 << 1)
#define CS_INT                  (1 << 2)
#define CS_ERROR                (1 << 8)
#define CS_WAIT_WRITES          (1 << 28)
#define CS_ABORT                (1 << 30)
#define CS_RESET                (1 << 31)

// TI bits
#define TI_INTEN                (1 << 0)
#define TI_WAIT_RESP            (1 << 3)
#define TI_DEST_INC             (1 << 4)
#define TI_DEST_WIDTH           (1 << 5)
#define TI_SRC_INC              (1 << 8)
#define TI_SRC_WIDTH            (1 << 9)
#define TI_BURST(n)             (((n) & 0xF) << 12)

// Firmware property tag: mask of DMA channels the ARM may use
#define TAG_GET_DMA_CHANNELS    0x00060001

// A transfer that takes longer than this is considered hung
#define DMA_TIMEOUT_MS          500

// Fill pattern: 16 bytes per beat x 16 beats (DmaBurst 15)
#define VC4_DMA_FILL_BYTES      256
#define VC4_DMA_FILL_DWORDS     (VC4_DMA_FILL_BYTES / sizeof(DWORD))

// The first operations are logged (each KITL line costs about 30 ms)
#define VC4_DMA_LOG_OPS         8

// Static kernel view: physical address of a static virtual address
#define STATIC_VA_TO_PA(va)     ((DWORD)(va) & 0x1FFFFFFF)

//------------------------------------------------------------------------------
//
//  Method:  DmaInit
//
//  Sets up the channel: checks the firmware's channel mask, maps the DMA
//  controller, allocates the control blocks and the fill pattern, hooks the
//  interrupt and resets the channel. On any failure DMA stays off.
//
VOID Vc4GPE::DmaInit()
{
    DWORD tags[4], dwMask, cbAlloc;
    ULONG pa = 0;
    PHYSICAL_ADDRESS phys;
    BYTE *pAlloc;

    if (m_pPrimarySurface == NULL || m_gpeMode.Bpp != 16) {
        RETAILMSG(1, (L"vc4disp: DMA blits need a 16 bpp screen, off\r\n"));
        m_fDmaEnabled = FALSE;
        return;
    }

    // Which channels may the ARM use?
    tags[0] = TAG_GET_DMA_CHANNELS;
    tags[1] = sizeof(DWORD);
    tags[2] = 0;
    tags[3] = 0;
    if (!MailboxTags(tags, sizeof(tags))) {
        RETAILMSG(1, (L"vc4disp: firmware DMA channel query failed, DMA off\r\n"));
        m_fDmaEnabled = FALSE;
        return;
    }
    dwMask = tags[3];
    if ((dwMask & (1 << DISP_DMA_CHANNEL)) == 0) {
        RETAILMSG(1, (L"vc4disp: DMA channel %d not in the firmware mask 0x%04x, "
            L"DMA off\r\n", DISP_DMA_CHANNEL, dwMask
        ));
        m_fDmaEnabled = FALSE;
        return;
    }

    // Controller registers
    phys.QuadPart = DMA_PA_BASE;
    m_pDmaRegs = (volatile DWORD*)MmMapIoSpace(phys, DMA_PA_SIZE, FALSE);
    if (m_pDmaRegs == NULL) {
        DmaDisable(L"MmMapIoSpace failed", 0, 0);
        return;
    }
    m_pDmaChan = m_pDmaRegs + DISP_DMA_CHANNEL * DMA_CHAN_STRIDE_DWORDS;

    // One control block per screen row, then the fill pattern.
    // AllocPhysMem returns page-aligned memory, so the blocks are 32-byte
    // aligned and the pattern (after a whole number of blocks) 16-byte.
    m_nDmaCbMax = m_gpeMode.height;
    cbAlloc = m_nDmaCbMax * sizeof(VC4_DMA_CB) + VC4_DMA_FILL_BYTES;
    pAlloc = (BYTE*)AllocPhysMem(
        cbAlloc, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
    );
    if (pAlloc == NULL) {
        DmaDisable(L"AllocPhysMem failed", 0, 0);
        return;
    }
    m_pDmaCb = (VC4_DMA_CB*)pAlloc;
    m_dwDmaCbBus = pa + m_dwDmaBusOffset;
    m_pDmaFill = (volatile DWORD*)(pAlloc + m_nDmaCbMax * sizeof(VC4_DMA_CB));
    m_dwDmaFillBus = m_dwDmaCbBus + m_nDmaCbMax * sizeof(VC4_DMA_CB);

    // Completion interrupt
    m_hDmaEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hDmaEvent == NULL ||
        !InterruptInitialize(SYSINTR_DISPDMA, m_hDmaEvent, NULL, 0)
    ) {
        DmaDisable(L"interrupt setup failed", 0, 0);
        return;
    }
    m_fDmaIntr = TRUE;

    // Framebuffer bus address (the surface may use the cached or uncached
    // static view; both map to the same physical address)
    m_dwFbBus = STATIC_VA_TO_PA(m_pPrimarySurface->Buffer()) + m_dwDmaBusOffset;

    // Enable and reset our channel
    m_pDmaRegs[DMA_ENABLE_DWORD] |= (1 << DISP_DMA_CHANNEL);
    m_pDmaChan[DMA_CS] = CS_RESET;
    Sleep(1);
    m_pDmaChan[DMA_CS] = CS_INT | CS_END;

    m_fDmaReady = TRUE;
    RETAILMSG(1, (L"vc4disp: DMA blits on, channel %d (firmware mask 0x%04x), "
        L"fills%s, min %d pixels, burst %d, fb bus 0x%08x\r\n",
        DISP_DMA_CHANNEL, dwMask, m_fDmaCopy ? L" and copies" : L"",
        m_dwDmaMinPixels, m_dwDmaBurst, m_dwFbBus
    ));
}

//------------------------------------------------------------------------------
//
//  Method:  DmaDeinit
//
VOID Vc4GPE::DmaDeinit()
{
    m_fDmaReady = FALSE;
    if (m_pDmaChan != NULL) {
        m_pDmaChan[DMA_CS] = CS_RESET;
    }
    if (m_fDmaIntr) {
        InterruptDisable(SYSINTR_DISPDMA);
        m_fDmaIntr = FALSE;
    }
    if (m_hDmaEvent != NULL) {
        CloseHandle(m_hDmaEvent);
        m_hDmaEvent = NULL;
    }
    if (m_pDmaCb != NULL) {
        FreePhysMem(m_pDmaCb);
        m_pDmaCb = NULL;
        m_pDmaFill = NULL;
    }
    if (m_pDmaRegs != NULL) {
        MmUnmapIoSpace((PVOID)m_pDmaRegs, DMA_PA_SIZE);
        m_pDmaRegs = NULL;
        m_pDmaChan = NULL;
    }
}

//------------------------------------------------------------------------------
//
//  Method:  DmaDisable
//
//  Logs why DMA blits stop and turns them off for good.
//
VOID Vc4GPE::DmaDisable(LPCWSTR pszReason, DWORD dwCs, DWORD dwDebug)
{
    RETAILMSG(1, (L"vc4disp: DMA blits off: %s (CS 0x%08x DEBUG 0x%08x)\r\n",
        pszReason, dwCs, dwDebug
    ));
    m_fDmaEnabled = FALSE;
    DmaDeinit();
}

//------------------------------------------------------------------------------
//
//  Method:  DmaRun
//
//  Runs the first nCb control blocks (already chained, the last one with
//  TI_INTEN) and waits for the end. Returns FALSE on a timeout or an error,
//  after turning DMA off.
//
BOOL Vc4GPE::DmaRun(DWORD nCb)
{
    DWORD dwWait, dwCs;

    // Write back and invalidate the D-cache: the DMA reads what the CPU
    // drew, and the CPU must not keep stale copies of what the DMA writes.
    // This also drains the write buffer (control blocks, fill pattern).
    CacheRangeFlush(NULL, 0, CACHE_SYNC_DISCARD);

    m_pDmaChan[DMA_CS] = CS_INT | CS_END;
    m_pDmaChan[DMA_CONBLK_AD] = m_dwDmaCbBus;
    m_pDmaChan[DMA_CS] = CS_WAIT_WRITES | CS_ACTIVE;

    dwWait = WaitForSingleObject(m_hDmaEvent, DMA_TIMEOUT_MS);
    dwCs = m_pDmaChan[DMA_CS];

    if (dwWait != WAIT_OBJECT_0 || (dwCs & CS_ERROR) || (dwCs & CS_ACTIVE)) {
        DWORD dwDebug = m_pDmaChan[DMA_DEBUG];
        m_pDmaChan[DMA_CS] = CS_ABORT;
        m_pDmaChan[DMA_CS] = CS_RESET;
        if (dwWait == WAIT_OBJECT_0) InterruptDone(SYSINTR_DISPDMA);
        DmaDisable((dwWait != WAIT_OBJECT_0) ? L"transfer timed out" :
            L"transfer error", dwCs, dwDebug);
        return FALSE;
    }

    // Clear the interrupt in the channel, then let the OAL unmask it
    m_pDmaChan[DMA_CS] = CS_INT | CS_END;
    InterruptDone(SYSINTR_DISPDMA);
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  DmaSelect
//
//  Called first from SelectDriverBlt. Points pBlt at a DMA routine for large
//  copies within the screen and large solid fills on the screen.
//
BOOL Vc4GPE::DmaSelect(GPEBltParms *pParms)
{
    const RECTL *prc = pParms->prclDst;
    LONG cx, cy;

    if (!m_fDmaReady ||
        pParms->pDst != m_pPrimarySurface ||
        (pParms->bltFlags & (BLT_TRANSPARENT | BLT_STRETCH | BLT_ALPHABLEND)) ||
        pParms->pMask != NULL ||
        prc == NULL || prc->left > prc->right || prc->top > prc->bottom
    ) {
        return FALSE;
    }
    cx = prc->right - prc->left;
    cy = prc->bottom - prc->top;
    if ((DWORD)(cx * cy) < m_dwDmaMinPixels) return FALSE;

    switch (pParms->rop4) {
    case 0x0000:    // BLACKNESS
    case 0xFFFF:    // WHITENESS
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltDmaFill;
        return TRUE;

    case 0xF0F0:    // PATCOPY with a solid brush
        if (pParms->solidColor == (COLOR)-1) return FALSE;
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltDmaFill;
        return TRUE;

    case 0xCCCC:    // SRCCOPY within the screen, no conversion
        if (!m_fDmaCopy ||
            pParms->pSrc != m_pPrimarySurface ||
            pParms->pLookup != NULL || pParms->pConvert != NULL ||
            !pParms->xPositive ||
            pParms->prclSrc == NULL
        ) {
            return FALSE;
        }
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltDmaCopy;
        return TRUE;
    }
    return FALSE;
}

//------------------------------------------------------------------------------
//
//  Method:  BltDmaCopy
//
//  Screen-to-screen copy of the clipped rectangle, one control block per row.
//
SCODE Vc4GPE::BltDmaCopy(GPEBltParms *pParms)
{
    const RECTL *prcDst = pParms->prclDst;
    const RECTL *prcSrc = pParms->prclSrc;
    LONG cx = prcDst->right - prcDst->left;
    LONG cy = prcDst->bottom - prcDst->top;
    LONG stride = m_pPrimarySurface->Stride();
    DWORD ti, i, row;

    // Small pieces of a clipped blit, or a DMA failure, go to the CPU
    if (!m_fDmaReady || cx <= 0 || cy <= 0 || (DWORD)cy > m_nDmaCbMax ||
        stride <= 0 || (DWORD)(cx * cy) < m_dwDmaMinPixels
    ) {
        return EmulatedBlt(pParms);
    }

    ti = TI_SRC_INC | TI_SRC_WIDTH | TI_DEST_INC | TI_DEST_WIDTH |
        TI_WAIT_RESP | TI_BURST(m_dwDmaBurst);

    for (i = 0; i < (DWORD)cy; i++) {
        // Moving down: copy the bottom row first
        row = pParms->yPositive ? i : (DWORD)cy - 1 - i;
        m_pDmaCb[i].ti = ti;
        m_pDmaCb[i].src = m_dwFbBus + (prcSrc->top + row) * stride +
            prcSrc->left * 2;
        m_pDmaCb[i].dst = m_dwFbBus + (prcDst->top + row) * stride +
            prcDst->left * 2;
        m_pDmaCb[i].len = cx * 2;
        m_pDmaCb[i].stride = 0;
        m_pDmaCb[i].next = m_dwDmaCbBus + (i + 1) * sizeof(VC4_DMA_CB);
    }
    m_pDmaCb[cy - 1].ti |= TI_INTEN;
    m_pDmaCb[cy - 1].next = 0;

    if (m_nDmaLogged < VC4_DMA_LOG_OPS) {
        m_nDmaLogged++;
        RETAILMSG(1, (L"vc4disp: DMA copy (%d,%d)-(%d,%d) from (%d,%d), %s\r\n",
            prcDst->left, prcDst->top, prcDst->right, prcDst->bottom,
            prcSrc->left, prcSrc->top, pParms->yPositive ? L"top-down" : L"bottom-up"
        ));
    }

    if (!DmaRun(cy)) return EmulatedBlt(pParms);
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  BltDmaFill
//
//  Solid fill of the clipped rectangle from the colour pattern.
//
SCODE Vc4GPE::BltDmaFill(GPEBltParms *pParms)
{
    const RECTL *prcDst = pParms->prclDst;
    LONG cx = prcDst->right - prcDst->left;
    LONG cy = prcDst->bottom - prcDst->top;
    LONG stride = m_pPrimarySurface->Stride();
    DWORD ti, i, dwColor, dwPair;

    if (!m_fDmaReady || cx <= 0 || cy <= 0 || (DWORD)cy > m_nDmaCbMax ||
        stride <= 0 || (DWORD)(cx * cy) < m_dwDmaMinPixels
    ) {
        return EmulatedBlt(pParms);
    }

    // 16 bpp pixel value: BLACKNESS, WHITENESS or the solid brush colour
    // (GPE passes it already in the destination format)
    if (pParms->rop4 == 0x0000) {
        dwColor = 0;
    } else if (pParms->rop4 == 0xFFFF) {
        dwColor = 0xFFFF;
    } else {
        dwColor = pParms->solidColor & 0xFFFF;
    }
    dwPair = dwColor | (dwColor << 16);
    for (i = 0; i < VC4_DMA_FILL_DWORDS; i++) {
        m_pDmaFill[i] = dwPair;
    }

    // Source does not increment between bursts; within one it walks the
    // pattern, which holds the colour for the longest burst
    ti = TI_SRC_WIDTH | TI_DEST_INC | TI_DEST_WIDTH | TI_WAIT_RESP |
        TI_BURST(m_dwDmaBurst);

    for (i = 0; i < (DWORD)cy; i++) {
        m_pDmaCb[i].ti = ti;
        m_pDmaCb[i].src = m_dwDmaFillBus;
        m_pDmaCb[i].dst = m_dwFbBus + (prcDst->top + i) * stride +
            prcDst->left * 2;
        m_pDmaCb[i].len = cx * 2;
        m_pDmaCb[i].stride = 0;
        m_pDmaCb[i].next = m_dwDmaCbBus + (i + 1) * sizeof(VC4_DMA_CB);
    }
    m_pDmaCb[cy - 1].ti |= TI_INTEN;
    m_pDmaCb[cy - 1].next = 0;

    if (m_nDmaLogged < VC4_DMA_LOG_OPS) {
        m_nDmaLogged++;
        RETAILMSG(1, (L"vc4disp: DMA fill (%d,%d)-(%d,%d) colour 0x%04x\r\n",
            prcDst->left, prcDst->top, prcDst->right, prcDst->bottom, dwColor
        ));
    }

    if (!DmaRun(cy)) return EmulatedBlt(pParms);
    return S_OK;
}

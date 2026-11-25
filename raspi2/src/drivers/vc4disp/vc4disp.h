//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// Use of this sample source code is subject to the terms of the Microsoft
// license agreement under which you licensed this sample source code. If
// you did not accept the terms of the license agreement, you are not
// authorized to use this sample source code. For the terms of the license,
// please see the license agreement between you and Microsoft or, if applicable,
// see the LICENSE.RTF on your install media or the root of your tools installation.
// THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR INDEMNITIES.
//
//------------------------------------------------------------------------------
//
//  File:  vc4disp.h
//
//  VideoCore IV display driver for the BCM2835 (CEonPi). Derived from the
//  Microsoft HALFlat sample through raspiBasic_ddi. The framebuffer and its
//  single mode still come from the OAL through IOCTL_HAL_DDI. D1: GDI draws
//  through the cached view of the framebuffer and the driver writes the
//  touched rows back after every operation (registry CachedFrameBuffer).
//
#ifndef __VC4DISP_H
#define __VC4DISP_H

#include <ddhal.h>

#define GPE_ZONE_PDD        DEBUGZONE(14)

// Registry key used when GWES passes no instance key to DisplayInit
#define VC4DISP_REG_KEY             L"Drivers\\Display\\vc4disp\\Config"

// DWORD: 1 (default) draw through the cached framebuffer view, 0 uncached
#define VC4DISP_REG_CACHEDFB        L"CachedFrameBuffer"

// DWORD: 1 (default) use gpe.lib's optimized FastBlt routines, 0 do not
#define VC4DISP_REG_FASTBLT         L"FastBlt"

// DWORD: 1 (default) use vc4disp's own blits (vc4blt.cpp), 0 do not
#define VC4DISP_REG_DRIVERBLT       L"DriverBlt"

// DWORD: 1 (default) use the VideoCore hardware cursor, 0 software cursor
#define VC4DISP_REG_HWCURSOR        L"HwCursor"

// DWORD: shortest time between two hardware cursor moves in ms (default 8);
// moves in between are merged
#define VC4DISP_REG_HWCURSORINTERVAL L"HwCursorInterval"

// DMA blits (vc4dma.cpp): DmaBlt 1 (default) on / 0 off; DmaMinPixels
// smallest rectangle (pixels) worth a DMA transfer; DmaBurst DMA burst
// length (0..15); DmaBusOffset physical -> VideoCore bus address offset
#define VC4DISP_REG_DMABLT          L"DmaBlt"
#define VC4DISP_REG_DMAMINPIXELS    L"DmaMinPixels"
#define VC4DISP_REG_DMABURST        L"DmaBurst"
#define VC4DISP_REG_DMABUSOFFSET    L"DmaBusOffset"

// DWORD: 1 = screen-to-screen copies by DMA too, 0 (default) fills only
// (with single transfers DMA copies were slower than the CPU)
#define VC4DISP_REG_DMACOPY         L"DmaCopy"

// RawFrameBuffer escape (vc4escape.cpp): RawFrameBuffer 1 (default) on / 0
// off; RawFbWriteCombine 1 (default) map write-combining, 0 uncached
#define VC4DISP_REG_RAWFB           L"RawFrameBuffer"
#define VC4DISP_REG_RAWFBWC         L"RawFbWriteCombine"

// DWORD: 1 = run the HVS overlay plane probe (vc4overlay.cpp) after the
// first SetMode, 0 (default) do not
#define VC4DISP_REG_OVERLAYPROBE    L"OverlayProbe"

// Driver settings, read from the registry by DisplayInit
typedef struct {
    BOOL  fCachedFb;
    BOOL  fDriverBlt;
    BOOL  fHwCursor;
    DWORD dwHwCursorInterval;
    BOOL  fDmaBlt;
    BOOL  fDmaCopy;
    BOOL  fRawFb;
    BOOL  fRawFbWriteCombine;
    BOOL  fOverlayProbe;
    DWORD dwDmaMinPixels;
    DWORD dwDmaBurst;
    DWORD dwDmaBusOffset;
} VC4DISP_SETTINGS;

// Windows Mobile RawFrameBufferInfo (GETRAWFRAMEBUFFER escape output)
typedef struct {
    WORD  wFormat;
    WORD  wBPP;
    VOID *pFramePointer;
    int   cxStride;
    int   cyStride;
    int   cxPixels;
    int   cyPixels;
} VC4_RAWFRAMEBUFFERINFO;

// One BCM2835 DMA control block (32-byte aligned)
typedef struct {
    DWORD ti;           // transfer information
    DWORD src;          // source bus address
    DWORD dst;          // destination bus address
    DWORD len;          // transfer length in bytes
    DWORD stride;       // 2D strides (unused: one block per row)
    DWORD next;         // bus address of the next block, 0 = last
    DWORD reserved[2];
} VC4_DMA_CB;

// CE ARM static mappings: uncached view = cached view + 0x20000000
#define VC4DISP_STATIC_CACHED       0x80000000
#define VC4DISP_STATIC_UNCACHED     0xA0000000
#define VC4DISP_STATIC_MASK         0xE0000000
#define VC4DISP_STATIC_UNCACHED_OFS 0x20000000

//------------------------------------------------------------------------------

class Vc4GPE : public GPE
{

public:
    Vc4GPE(const VC4DISP_SETTINGS *pSettings);
    virtual ~Vc4GPE();

    virtual int InVBlank();
    virtual int NumModes();

    virtual SCODE SetPalette(const PALETTEENTRY*, WORD, WORD);

    virtual SCODE AllocSurface(GPESurf**, int, int, EGPEFormat, int);
    virtual SCODE Line(GPELineParms*, EGPEPhase);
    virtual SCODE BltPrepare(GPEBltParms*);
    virtual SCODE BltComplete(GPEBltParms*);

    virtual SCODE GetModeInfo(GPEMode*, int);
    virtual SCODE SetMode(int, HPALETTE*);
    virtual SCODE SetPointerShape(GPESurf*, GPESurf*, int, int, int, int);
    virtual SCODE MovePointer(int, int);

    virtual VOID PowerHandler(BOOL bOff);

    virtual ULONG DrvEscape(SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn,
        ULONG cjOut, PVOID pvOut);

private:

    // Actual mode info (width, height, bpp, frequency, and format)
    GPEMode m_gpeMode;

    // Framebuffer: TRUE when drawing goes through the cached view, so every
    // write to the primary surface must be followed by FlushRect
    BOOL   m_fCachedFb;

    // TRUE: BltPrepare may route blits to the routines in vc4blt.cpp
    BOOL   m_fDriverBlt;

    // Software cursor
    BOOL   m_cursorDisabled;
    BOOL   m_cursorVisible;
    BOOL   m_cursorForcedOff;
    RECTL  m_cursorRect;
    POINTL m_cursorSize;
    POINTL m_cursorHotspot;
    UCHAR* m_cursorStore;
    UCHAR* m_cursorXor;
    UCHAR* m_cursorAnd;

    // Hardware cursor (vc4cursor.cpp): enabled by the registry, active while
    // the firmware shows the current shape (the software cursor is then off)
    BOOL   m_fHwCursorEnabled;
    BOOL   m_fHwCursorActive;
    BOOL   m_fHwCursorLogged;
    DWORD* m_pHwCursorBuf;
    DWORD  m_dwHwCursorBus;

    // Cursor thread: MovePointer only queues the newest position, the thread
    // sends it to the firmware (the OAL busy-waits for the reply), at most
    // once per m_dwCursorIntervalMs
    CRITICAL_SECTION m_csCursor;
    HANDLE m_hCursorThread;
    HANDLE m_hCursorEvent;
    BOOL   m_fCursorExit;
    BOOL   m_fCursorPending;
    BOOL   m_fCursorVisible;
    int    m_xCursor;
    int    m_yCursor;
    BOOL   m_fHwCursorFailed;
    DWORD  m_dwCursorIntervalMs;
    volatile DWORD *m_pSysTimer;    // BCM2835 system timer (1 MHz)
    DWORD  m_nCursorMoves;          // statistics since the last log line
    DWORD  m_nCursorCalls;
    DWORD  m_dwCursorUsTotal;
    DWORD  m_dwCursorUsMax;
    DWORD* m_pHwCursorMsg;          // asynchronous property message, uncached
    DWORD  m_dwHwCursorMsgBus;
    BOOL   m_fMailboxAsync;         // OAL asynchronous mailbox path usable

    // DMA blits (vc4dma.cpp): ready once DmaInit has set up the channel
    BOOL   m_fDmaEnabled;
    BOOL   m_fDmaReady;
    BOOL   m_fDmaCopy;
    DWORD  m_dwDmaMinPixels;
    DWORD  m_dwDmaBurst;
    DWORD  m_dwDmaBusOffset;
    volatile DWORD *m_pDmaRegs;     // DMA controller page (0x20007000)
    volatile DWORD *m_pDmaChan;     // our channel's registers
    VC4_DMA_CB *m_pDmaCb;           // control blocks, uncached
    DWORD  m_dwDmaCbBus;
    DWORD  m_nDmaCbMax;
    volatile DWORD *m_pDmaFill;     // fill pattern (VC4_DMA_FILL_BYTES), uncached
    DWORD  m_dwDmaFillBus;
    DWORD  m_dwFbBus;               // framebuffer bus address
    HANDLE m_hDmaEvent;
    BOOL   m_fDmaIntr;
    DWORD  m_nDmaLogged;            // DMA operations logged so far

    // RawFrameBuffer escape (vc4escape.cpp). m_fRawFbGiven: a process has a
    // direct pointer, so FlushRect must also invalidate
    BOOL   m_fRawFbEnabled;
    BOOL   m_fRawFbWriteCombine;
    BOOL   m_fRawFbGiven;

    // HVS overlay plane probe (vc4overlay.cpp)
    BOOL   m_fOverlayProbe;
    BOOL   m_fOverlayProbeStarted;

    // Helper functions
    UINT8* QueryModeInfo(int modeNumber);
    VOID FlushRect(const RECTL *pRect);

    // vc4blt.cpp
    VOID SelectDriverBlt(GPEBltParms *pParms);
    SCODE BltCopy32To565(GPEBltParms *pParms);
    SCODE BltCopy24To565(GPEBltParms *pParms);
    SCODE BltInvert16(GPEBltParms *pParms);

    // vc4cursor.cpp
    BOOL MailboxTags(DWORD *pTags, DWORD cbTags);
    BOOL HwCursorInit();
    BOOL HwSetPointerShape(GPESurf *pMask, int xHotspot, int yHotspot,
        int xSize, int ySize);
    BOOL HwSetPointerState(BOOL fVisible, int x, int y);
    VOID HwQueuePointerState(BOOL fVisible, int x, int y);
    BOOL HwSendPointerState(BOOL fVisible, int x, int y);
    VOID HwCursorDeinit();
    VOID CursorThread();
    static DWORD WINAPI CursorThreadProc(LPVOID pContext);

    // vc4dma.cpp
    VOID DmaInit();
    VOID DmaDeinit();
    BOOL DmaRun(DWORD nCb);
    VOID DmaDisable(LPCWSTR pszReason, DWORD dwCs, DWORD dwDebug);
    BOOL DmaSelect(GPEBltParms *pParms);
    SCODE BltDmaCopy(GPEBltParms *pParms);
    SCODE BltDmaFill(GPEBltParms *pParms);

    // vc4escape.cpp
    BOOL GetRawFrameBuffer(VC4_RAWFRAMEBUFFERINFO *pInfo);
    VOID CursorOn();
    VOID CursorOff();
    SCODE WrappedEmulatedLine(GPELineParms*);

    // vc4overlay.cpp
    VOID OverlayProbeStart();
    VOID OverlayProbe();
    static DWORD WINAPI OverlayProbeThreadProc(LPVOID pContext);
};

//------------------------------------------------------------------------------

#endif

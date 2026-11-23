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

// CE ARM static mappings: uncached view = cached view + 0x20000000
#define VC4DISP_STATIC_CACHED       0x80000000
#define VC4DISP_STATIC_UNCACHED     0xA0000000
#define VC4DISP_STATIC_MASK         0xE0000000
#define VC4DISP_STATIC_UNCACHED_OFS 0x20000000

//------------------------------------------------------------------------------

class Vc4GPE : public GPE
{

public:
    Vc4GPE(BOOL fCachedFb, BOOL fDriverBlt);
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

    // Helper functions
    UINT8* QueryModeInfo(int modeNumber);
    VOID FlushRect(const RECTL *pRect);

    // vc4blt.cpp
    VOID SelectDriverBlt(GPEBltParms *pParms);
    SCODE BltCopy32To565(GPEBltParms *pParms);
    SCODE BltCopy24To565(GPEBltParms *pParms);
    SCODE BltInvert16(GPEBltParms *pParms);
    VOID CursorOn();
    VOID CursorOff();
    SCODE WrappedEmulatedLine(GPELineParms*);
};

//------------------------------------------------------------------------------

#endif

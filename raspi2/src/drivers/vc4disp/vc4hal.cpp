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
//  File:  vc4hal.cpp
//
//  DirectDraw HAL of vc4disp (CEonPi), V2a: modelled on the Microsoft
//  vgaflat sample (public\common\oak\drivers\display\vgaflat\halcaps.cpp).
//  The driver is a DDGPE (ddgpe.lib supplies HALInit, which calls
//  buildDDHALInfo below, and the DDGPExxx callbacks). DirectDraw gets the
//  framebuffer as its primary surface and nothing else yet: no off-screen
//  video memory, no overlay (V2b).
//
//  Video memory: the DirectDraw runtime maps the blocks listed by
//  GUID_GetDriverInfo_VidMemList into the client, and Lock pointers inside a
//  block are translated to the client's mapping. Every shipping CE 7 DDGPE
//  driver (OMAP35xx, SMDK6410, NE1TB, iMX27) lists a view it made itself
//  with VirtualAlloc + VirtualCopy(PAGE_PHYSICAL). The first try listed the
//  kernel's static cached view of the framebuffer (0x8xxxxxxx, what GDI
//  draws through) and the client's ddraw.dll raised exception 0x57
//  (ERROR_INVALID_PARAMETER) right after HALInit. So DirectDraw now gets its
//  own uncached VirtualCopy view (m_pDdFbView, made in SetMode), and HalLock
//  moves primary-surface pointers from GDI's view into it. GDI keeps the
//  cached static view (fast). The framebuffer is VideoCore memory outside
//  CE's RAM, so PAGE_PHYSICAL takes no RAM page reference here (unlike the
//  trap in vc4overlay.cpp).
//
//  Once DirectDraw asks for the list, a client may write the framebuffer
//  behind GDI's cache, so the driver from then on cleans and invalidates
//  after each drawing operation (as it does for the RawFrameBuffer escape).
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <ddrawi.h>
#include <ddgpe.h>
#include <ddhfuncs.h>
#include "vc4disp.h"

// GetDriverInfo queries logged (diagnostics; each KITL line costs ~30 ms)
#define VC4HAL_LOG_QUERIES      16

//------------------------------------------------------------------------------
// Callbacks from the DirectDraw object

static DDHAL_DDCALLBACKS g_ddCallbacks = {
    sizeof(DDHAL_DDCALLBACKS),
    DDHAL_CB32_CREATESURFACE |
    DDHAL_CB32_CANCREATESURFACE |
    0,
    DDGPECreateSurface,         // CreateSurface
    NULL,                       // WaitForVerticalBlank
    DDGPECanCreateSurface,      // CanCreateSurface
    NULL,                       // CreatePalette
    NULL                        // GetScanLine
};

//------------------------------------------------------------------------------
// Callbacks from the DirectDrawSurface object

static DDHAL_DDSURFACECALLBACKS g_ddSurfaceCallbacks = {
    sizeof(DDHAL_DDSURFACECALLBACKS),
    DDHAL_SURFCB32_DESTROYSURFACE |
    DDHAL_SURFCB32_LOCK |
    DDHAL_SURFCB32_UNLOCK |
    DDHAL_SURFCB32_SETCOLORKEY |
    DDHAL_SURFCB32_GETBLTSTATUS |
    0,
    DDGPEDestroySurface,        // DestroySurface
    NULL,                       // Flip
    HalLock,                    // Lock
    DDGPEUnlock,                // Unlock
    DDGPESetColorKey,           // SetColorKey
    HalGetBltStatus,            // GetBltStatus
    NULL,                       // GetFlipStatus
    NULL,                       // UpdateOverlay
    NULL,                       // SetOverlayPosition
    NULL                        // SetPalette
};

//------------------------------------------------------------------------------
//
//  Method:  DdFbViewInit
//
//  Called by SetMode: makes DirectDraw's uncached view of the framebuffer
//  (dwFbPhys, cb bytes), once.
//
VOID Vc4GPE::DdFbViewInit(DWORD dwFbPhys, DWORD cb)
{
    VOID *pView;

    if (m_pDdFbView != NULL) return;

    cb = (cb + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    pView = VirtualAlloc(NULL, cb, MEM_RESERVE, PAGE_NOACCESS);
    if (pView == NULL) {
        RETAILMSG(1, (L"vc4disp: DirectDraw framebuffer view: VirtualAlloc "
            L"failed (%d)\r\n", GetLastError()));
        return;
    }
    if (!VirtualCopy(pView, (VOID*)(dwFbPhys >> 8), cb,
            PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL)) {
        RETAILMSG(1, (L"vc4disp: DirectDraw framebuffer view: VirtualCopy "
            L"failed (%d)\r\n", GetLastError()));
        VirtualFree(pView, 0, MEM_RELEASE);
        return;
    }
    m_pDdFbView = (BYTE*)pView;
    m_cbDdFbView = cb;
}

//------------------------------------------------------------------------------
//
//  Method:  GetVirtualVideoMemory
//
//  DirectDraw's video memory: its view of the framebuffer.
//
VOID Vc4GPE::GetVirtualVideoMemory(ULONG *pVirtualBase, ULONG *pSize)
{
    *pVirtualBase = (ULONG)m_pDdFbView;
    *pSize = (m_pDdFbView != NULL) ? m_cbDdFbView : 0;
}

//------------------------------------------------------------------------------
//
//  Method:  DdLockPointer
//
//  A Lock pointer for DirectDraw: pointers into the primary surface move
//  from GDI's view to DirectDraw's; anything else is returned unchanged.
//
VOID *Vc4GPE::DdLockPointer(DDGPESurf *pSurf, VOID *p)
{
    BYTE *pGdiBase;

    if (pSurf == NULL || m_pDdFbView == NULL ||
        pSurf != (DDGPESurf*)m_pPrimarySurface) {
        return p;
    }
    pGdiBase = (BYTE*)m_pPrimarySurface->Buffer();
    return m_pDdFbView + ((BYTE*)p - pGdiBase);
}

//------------------------------------------------------------------------------
//
//  Method:  DirectDrawOpened
//
//  A DirectDraw client is about to get the framebuffer mapped: from now on
//  FlushRect also invalidates, so GDI never reads stale cached pixels.
//
VOID Vc4GPE::DirectDrawOpened()
{
    if (!m_fRawFbGiven) {
        m_fRawFbGiven = TRUE;
        CacheRangeFlush(NULL, 0, CACHE_SYNC_DISCARD);
        RETAILMSG(1, (L"vc4disp: DirectDraw maps the framebuffer\r\n"));
    }
}

//------------------------------------------------------------------------------
//
//  Function:  HalLock
//
DWORD WINAPI HalLock(LPDDHAL_LOCKDATA pd)
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    DWORD rc;

    rc = DDGPELock(pd);
    if (pd->ddRVal == DD_OK) {
        pd->lpSurfData = pGpe->DdLockPointer(
            DDGPESurf::GetDDGPESurf(pd->lpDDSurface), pd->lpSurfData);
    }
    return rc;
}

//------------------------------------------------------------------------------
//
//  Function:  HalGetDriverInfo
//
DWORD WINAPI HalGetDriverInfo(LPDDHAL_GETDRIVERINFODATA lpInput)
{
    static DWORD s_nLogged = 0;
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    ULONG base = 0, size = 0;

    DEBUGENTER(HalGetDriverInfo);

    pGpe->GetVirtualVideoMemory(&base, &size);

    if (lpInput->guidInfo == GUID_GetDriverInfo_VidMemList) {
        lpInput->dwActualSize = sizeof(DDHAL_DDVIDMEMLIST);
        if (lpInput->dwExpectedSize < lpInput->dwActualSize) {
            lpInput->ddRVal = DDERR_MOREDATA;
        } else if (base == 0) {
            lpInput->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        } else {
            LPDDHAL_DDVIDMEMLIST pList = (LPDDHAL_DDVIDMEMLIST)lpInput->lpvData;
            pGpe->DirectDrawOpened();
            pList->cVidMemBlocks = 1;
            pList->rgVidMem[0].pVidMemBase = (void*)base;
            pList->rgVidMem[0].dwVidMemSize = size;
            lpInput->ddRVal = DD_OK;
        }
    } else if (lpInput->guidInfo == GUID_GetDriverInfo_VidMemBase) {
        if (base == 0) {
            lpInput->ddRVal = DDERR_CURRENTLYNOTAVAIL;
        } else {
            pGpe->DirectDrawOpened();
            *(DWORD*)lpInput->lpvData = base;
            lpInput->dwActualSize = sizeof(DWORD);
            lpInput->ddRVal = DD_OK;
        }
    } else {
        lpInput->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    }

    if (s_nLogged < VC4HAL_LOG_QUERIES) {
        s_nLogged++;
        RETAILMSG(1, (L"vc4disp: DirectDraw GetDriverInfo {%08x-%04x} expected %u "
            L"-> 0x%08x (base 0x%08x size %u)\r\n",
            lpInput->guidInfo.Data1, lpInput->guidInfo.Data2,
            lpInput->dwExpectedSize, lpInput->ddRVal, base, size
        ));
    }

    DEBUGLEAVE(HalGetDriverInfo);
    return DDHAL_DRIVER_HANDLED;
}

//------------------------------------------------------------------------------
//
//  Function:  HalGetBltStatus
//
//  Blits are synchronous (CPU, or DMA waited for), so always done.
//
DWORD WINAPI HalGetBltStatus(LPDDHAL_GETBLTSTATUSDATA pd)
{
    pd->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

//------------------------------------------------------------------------------
//
//  Function:  buildDDHALInfo
//
//  Called by HALInit (ddgpe.lib) when a process first uses DirectDraw.
//
EXTERN_C void buildDDHALInfo(LPDDHALINFO lpddhi, DWORD modeidx)
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    ULONG base = 0, size = 0;

    UNREFERENCED_PARAMETER(modeidx);

    pGpe->GetVirtualVideoMemory(&base, &size);

    memset(lpddhi, 0, sizeof(DDHALINFO));
    lpddhi->dwSize = sizeof(DDHALINFO);
    lpddhi->lpDDCallbacks = &g_ddCallbacks;
    lpddhi->lpDDSurfaceCallbacks = &g_ddSurfaceCallbacks;
    lpddhi->GetDriverInfo = HalGetDriverInfo;
    lpddhi->lpdwFourCC = NULL;

    lpddhi->ddCaps.dwSize = sizeof(DDCAPS);

    // The framebuffer is the only video memory, and the primary fills it
    lpddhi->ddCaps.dwVidMemTotal = size;
    lpddhi->ddCaps.dwVidMemFree = 0;
    lpddhi->ddCaps.dwVidMemStride = 0;

    lpddhi->ddCaps.ddsCaps.dwCaps =
        DDSCAPS_PRIMARYSURFACE |
        DDSCAPS_FRONTBUFFER |
        DDSCAPS_SYSTEMMEMORY |
        DDSCAPS_OWNDC |
        0;

    lpddhi->ddCaps.dwNumFourCCCodes = 0;
    lpddhi->ddCaps.dwPalCaps = 0;
    lpddhi->ddCaps.dwMiscCaps = 0;

    // Hardware blitting: none advertised (GDI's software paths still run)
    lpddhi->ddCaps.dwBltCaps = 0;
    lpddhi->ddCaps.dwCKeyCaps = 0;
    lpddhi->ddCaps.dwAlphaCaps = 0;

    // Overlay: none yet (V2b)
    lpddhi->ddCaps.dwOverlayCaps = 0;
    lpddhi->ddCaps.dwMaxVisibleOverlays = 0;
    lpddhi->ddCaps.dwCurrVisibleOverlays = 0;
    lpddhi->ddCaps.dwMinOverlayStretch = 0;
    lpddhi->ddCaps.dwMaxOverlayStretch = 0;

    RETAILMSG(1, (L"vc4disp: DirectDraw HAL: primary = framebuffer, video "
        L"memory view 0x%08x (%u bytes), no overlay\r\n", base, size
    ));
}

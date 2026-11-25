//
// vc4ddovl.cpp - DirectDraw overlay of vc4disp (CEonPi), video overlay V2b
//
// DirectDraw YV12 overlay surfaces shown on the VideoCore HVS plane, through
// the same engine as the VC4OVL_ESCAPE overlay (vc4overlay.cpp: the plane
// state is a set of Y/U/V bus addresses, a source and a destination
// rectangle; the mailbox thread sends it with the firmware's Set plane tag).
// The VideoCore scales and colour-converts; no colour key (the plane is
// above the 16 bpp desktop, which has no alpha), so windows over the video
// area are hidden by it. One plane: either the escape or DirectDraw owns it.
//
// Surfaces (Vc4OvlSurf): AllocSurface turns every YV12 request into one.
// Each has its own physically contiguous uncached buffer (Y, then V, then
// U; luma pitch a multiple of 32 bytes, chroma pitch half of it, which is
// what DirectDraw clients such as TCPMP assume and what the firmware wants)
// mapped into the creating process with VirtualAllocCopyEx, so Buffer() and
// Lock pointers are that process's addresses. VirtualAllocCopyEx takes a
// reference on every page, so the client's mapping going away (unmap, or
// the process exiting) never frees the pages under the driver (the
// VirtualCopyEx PAGE_PHYSICAL trap, see vc4overlay.cpp). This is the
// pattern of SMDK6410's PACSurf; DirectDraw sees system-memory surfaces.
//
// HAL callbacks: UpdateOverlay (show / hide / new rectangles; while shown
// the buffer on screen stays the last one flipped to), SetOverlayPosition,
// Flip (an overlay flip shows the target's buffer and waits until the
// firmware has answered) and GetFlipStatus. Primary flips go to DDGPEFlip.
//
// Unverified: whether the firmware's answer means the HVS has stopped
// reading the previous buffer (V1: at 30 fps two buffers showed no
// tearing, at 330 flips/s heavy tearing).
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

#define BUS_UNCACHED_ALIAS      0xC0000000

// Longest wait for the firmware to answer a plane update
#define DDOVL_WAIT_MS           100
#define DDOVL_GONE_WAIT_MS      300

// Largest surface side, largest destination side (dst_w/h are 16 bit)
#define DDOVL_MAX_SIDE          2048
#define DDOVL_MAX_DST           0x7FFF

// Black: Y 16, U and V 128 (BT.601 limited range)
#define DDOVL_BLACK_Y           0x10101010
#define DDOVL_BLACK_C           0x80808080

//------------------------------------------------------------------------------
//
//  Function:  FillDwords
//
static VOID FillDwords(BYTE *p, DWORD cb, DWORD dwValue)
{
    DWORD *pd = (DWORD*)p, n = cb / sizeof(DWORD);

    while (n-- != 0) *pd++ = dwValue;
}

//------------------------------------------------------------------------------
//
//  Function:  ValidRects
//
static BOOL ValidRects(Vc4OvlSurf *pSurf, const RECT *prcSrc,
    const RECT *prcDst)
{
    return prcSrc->left >= 0 && prcSrc->top >= 0 &&
        prcSrc->right <= pSurf->Width() &&
        prcSrc->bottom <= pSurf->Height() &&
        prcSrc->right > prcSrc->left && prcSrc->bottom > prcSrc->top &&
        prcDst->right > prcDst->left && prcDst->bottom > prcDst->top &&
        prcDst->right - prcDst->left <= DDOVL_MAX_DST &&
        prcDst->bottom - prcDst->top <= DDOVL_MAX_DST;
}

//==============================================================================
//
//  Vc4OvlSurf
//

Vc4OvlSurf::Vc4OvlSurf(int width, int height, BYTE *pBits, int stride,
    EDDGPEPixelFormat pixelFormat)
    : DDGPESurf(width, height, pBits, stride,
        EDDGPEPixelFormatToEGPEFormat[pixelFormat], pixelFormat)
{
    m_fInVideoMemory = FALSE;
    m_nOffsetInVideoMemory = 0;
    m_pKernel = NULL;
    m_cbAlloc = 0;
    m_dwOwner = 0;
    m_pOwnerVa = NULL;
    m_dwBusY = 0;
    m_dwBusU = 0;
    m_dwBusV = 0;
    m_dwPitch = stride;
}

//------------------------------------------------------------------------------
//
//  Method:  Create (static)
//
//  Allocates a YV12 surface, cleared to black, mapped into the caller.
//
SCODE Vc4OvlSurf::Create(int width, int height, EDDGPEPixelFormat pixelFormat,
    DDGPESurf **ppSurf)
{
    DWORD pitch, cbFrame, cb, pa = 0, dwCaller;
    BYTE *pKernel, *pUser;
    Vc4OvlSurf *pSurf;

    *ppSurf = NULL;
    if (width < 2 || height < 2 || (width & 1) != 0 || (height & 1) != 0 ||
        width > DDOVL_MAX_SIDE || height > DDOVL_MAX_SIDE) {
        RETAILMSG(1, (L"vc4disp: DirectDraw overlay: %dx%d YV12 not supported\r\n",
            width, height));
        return DDERR_INVALIDPARAMS;
    }

    pitch = (width + 31) & ~31;
    cbFrame = pitch * height + 2 * (pitch / 2) * (height / 2);
    cb = (cbFrame + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    pKernel = (BYTE*)AllocPhysMem(cb, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa);
    if (pKernel == NULL) {
        RETAILMSG(1, (L"vc4disp: DirectDraw overlay: cannot allocate %u bytes\r\n", cb));
        return DDERR_OUTOFVIDEOMEMORY;
    }
    FillDwords(pKernel, pitch * height, DDOVL_BLACK_Y);
    FillDwords(pKernel + pitch * height, cb - pitch * height, DDOVL_BLACK_C);

    // Map it into the caller (the DirectDraw client) unless that is us
    dwCaller = (DWORD)GetCallerVMProcessId();
    pUser = pKernel;
    if (dwCaller != GetCurrentProcessId()) {
        pUser = (BYTE*)VirtualAllocCopyEx(GetCurrentProcess(), (HANDLE)dwCaller,
            pKernel, cb, PAGE_READWRITE | PAGE_NOCACHE);
        if (pUser == NULL) {
            RETAILMSG(1, (L"vc4disp: DirectDraw overlay: VirtualAllocCopyEx "
                L"failed (%d)\r\n", GetLastError()));
            FreePhysMem(pKernel);
            return DDERR_OUTOFMEMORY;
        }
    } else {
        dwCaller = 0;
    }

    pSurf = new Vc4OvlSurf(width, height, pUser, pitch, pixelFormat);
    if (pSurf == NULL) {
        if (dwCaller != 0) {
            VirtualFreeEx((HANDLE)dwCaller, pUser - ((DWORD)pKernel & 0xFFFF),
                0, MEM_RELEASE);
        }
        FreePhysMem(pKernel);
        return E_OUTOFMEMORY;
    }
    pSurf->m_pKernel = pKernel;
    pSurf->m_cbAlloc = cb;
    pSurf->m_dwOwner = dwCaller;
    pSurf->m_pOwnerVa = pUser;
    pSurf->m_dwBusY = BUS_UNCACHED_ALIAS | pa;
    pSurf->m_dwBusV = pSurf->m_dwBusY + pitch * height;
    pSurf->m_dwBusU = pSurf->m_dwBusV + (pitch / 2) * (height / 2);
    pSurf->m_dwSurfaceSize = cbFrame;

    RETAILMSG(1, (L"vc4disp: DirectDraw overlay surface %dx%d YV12 pitch %u, "
        L"bus 0x%08x, process 0x%08x va 0x%08x\r\n",
        width, height, pitch, pSurf->m_dwBusY, dwCaller, (DWORD)pUser));

    *ppSurf = pSurf;
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Destructor
//
//  Takes the surface off the plane first (waiting for the firmware), then
//  unmaps it from its process and frees it. If the hide is not confirmed
//  the memory is leaked rather than freed under the HVS.
//
Vc4OvlSurf::~Vc4OvlSurf()
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    BOOL fSafe = TRUE;

    if (pGpe != NULL) fSafe = pGpe->DdOverlaySurfaceGone(this);

    // The process's mapping (already gone if the process has exited)
    if (m_dwOwner != 0 && m_pOwnerVa != NULL) {
        VirtualFreeEx((HANDLE)m_dwOwner,
            m_pOwnerVa - ((DWORD)m_pKernel & 0xFFFF), 0, MEM_RELEASE);
    }

    if (m_pKernel != NULL) {
        if (fSafe) {
            FreePhysMem(m_pKernel);
        } else {
            RETAILMSG(1, (L"vc4disp: DirectDraw overlay: hide not confirmed, "
                L"%u bytes at bus 0x%08x left allocated\r\n", m_cbAlloc, m_dwBusY));
        }
    }
    m_pKernel = NULL;
}

//==============================================================================
//
//  Vc4GPE: DirectDraw overlay operations. m_csOvl serializes them with the
//  escape; plane state changes under m_csMbox, then the mailbox thread is
//  woken and (for show, hide and flip) the firmware's answer awaited.
//

//------------------------------------------------------------------------------
//
//  Function:  SetPlaneImage
//
//  m_csMbox held: pSurf becomes the plane's image.
//
static VOID SetPlaneImage(Vc4OvlSurf *pSurf, DWORD *pBusY, DWORD *pBusU,
    DWORD *pBusV, DWORD *pWidth, DWORD *pHeight, DWORD *pPitch)
{
    *pBusY = pSurf->m_dwBusY;
    *pBusU = pSurf->m_dwBusU;
    *pBusV = pSurf->m_dwBusV;
    *pWidth = pSurf->Width();
    *pHeight = pSurf->Height();
    *pPitch = pSurf->m_dwPitch;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlayShow
//
HRESULT Vc4GPE::DdOverlayShow(Vc4OvlSurf *pSurf, const RECT *prcSrc,
    const RECT *prcDst)
{
    DWORD dwSeq;
    BOOL fWasShown;

    if (!ValidRects(pSurf, prcSrc, prcDst)) return DDERR_INVALIDRECT;

    EnterCriticalSection(&m_csOvl);

    // Take the plane from the escape only if its owner has gone
    if (m_dwOvlUser == OVL_USER_ESCAPE) {
        if (Vc4IsProcessAlive(m_dwOvlOwner)) {
            LeaveCriticalSection(&m_csOvl);
            return DDERR_CURRENTLYNOTAVAIL;
        }
        OverlayDestroy();
    }
    OverlayDisplayInit();
    if (m_dwOvlUser == OVL_USER_ESCAPE || !MboxInit()) {
        LeaveCriticalSection(&m_csOvl);
        return DDERR_CURRENTLYNOTAVAIL;
    }

    EnterCriticalSection(&m_csMbox);
    fWasShown = (m_dwOvlUser == OVL_USER_DDRAW && m_fOvlShow && m_pOvlDdSurf != NULL);
    if (!fWasShown) {
        // Newly shown: this surface. While shown, the buffer on screen stays
        // the last one flipped to (the runtime may pass the chain's front)
        SetPlaneImage(pSurf, &m_dwOvlBusY, &m_dwOvlBusU, &m_dwOvlBusV,
            &m_dwPlaneWidth, &m_dwPlaneHeight, &m_dwPlanePitch);
        m_pOvlDdSurf = pSurf;
    }
    m_dwOvlUser = OVL_USER_DDRAW;
    m_fOvlShow = TRUE;
    m_rcOvlSrc = *prcSrc;
    m_rcOvlDst = *prcDst;
    dwSeq = OverlayQueueLocked();
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);
    MboxWake();

    if (!fWasShown) {
        RETAILMSG(1, (L"vc4disp: DirectDraw overlay shown: %dx%d src (%d,%d)-(%d,%d) "
            L"dst (%d,%d)-(%d,%d)\r\n", pSurf->Width(), pSurf->Height(),
            prcSrc->left, prcSrc->top, prcSrc->right, prcSrc->bottom,
            prcDst->left, prcDst->top, prcDst->right, prcDst->bottom));
    }
    OverlayWait(dwSeq, DDOVL_WAIT_MS);
    return DD_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlayUpdate
//
//  UpdateOverlay without show or hide: new rectangles if shown.
//
HRESULT Vc4GPE::DdOverlayUpdate(Vc4OvlSurf *pSurf, const RECT *prcSrc,
    const RECT *prcDst)
{
    if (!ValidRects(pSurf, prcSrc, prcDst)) return DDERR_INVALIDRECT;

    EnterCriticalSection(&m_csOvl);
    EnterCriticalSection(&m_csMbox);
    if (m_dwOvlUser == OVL_USER_DDRAW && m_fOvlShow) {
        m_rcOvlSrc = *prcSrc;
        m_rcOvlDst = *prcDst;
        OverlayQueueLocked();
    }
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);
    MboxWake();
    return DD_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlayHide
//
HRESULT Vc4GPE::DdOverlayHide()
{
    DWORD dwSeq = 0;
    BOOL fHide = FALSE;

    EnterCriticalSection(&m_csOvl);
    EnterCriticalSection(&m_csMbox);
    if (m_dwOvlUser == OVL_USER_DDRAW) {
        m_fOvlShow = FALSE;
        m_dwOvlUser = OVL_USER_NONE;
        m_pOvlDdSurf = NULL;
        dwSeq = OverlayQueueLocked();
        fHide = TRUE;
    }
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);

    if (fHide) {
        MboxWake();
        OverlayWait(dwSeq, DDOVL_WAIT_MS);
        RETAILMSG(1, (L"vc4disp: DirectDraw overlay hidden\r\n"));
    }
    return DD_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlayFlip
//
//  Shows pTarget's buffer and waits until the firmware has answered.
//
HRESULT Vc4GPE::DdOverlayFlip(Vc4OvlSurf *pTarget)
{
    DWORD dwSeq = 0;
    BOOL fQueued = FALSE;

    EnterCriticalSection(&m_csOvl);
    EnterCriticalSection(&m_csMbox);
    if (m_dwOvlUser == OVL_USER_DDRAW && m_fOvlShow) {
        SetPlaneImage(pTarget, &m_dwOvlBusY, &m_dwOvlBusU, &m_dwOvlBusV,
            &m_dwPlaneWidth, &m_dwPlaneHeight, &m_dwPlanePitch);
        m_pOvlDdSurf = pTarget;
        dwSeq = OverlayQueueLocked();
        fQueued = TRUE;
    }
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);

    if (fQueued) {
        MboxWake();
        OverlayWait(dwSeq, DDOVL_WAIT_MS);
    }
    return DD_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlayMove
//
//  SetOverlayPosition: the destination's top-left corner, same size.
//
HRESULT Vc4GPE::DdOverlayMove(LONG x, LONG y)
{
    EnterCriticalSection(&m_csOvl);
    EnterCriticalSection(&m_csMbox);
    if (m_dwOvlUser == OVL_USER_DDRAW && m_fOvlShow) {
        OffsetRect(&m_rcOvlDst, x - m_rcOvlDst.left, y - m_rcOvlDst.top);
        OverlayQueueLocked();
    }
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);
    MboxWake();
    return DD_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  DdOverlaySurfaceGone
//
//  A Vc4OvlSurf is being destroyed: if it is on the plane, hide the plane
//  and wait for the firmware. Returns FALSE if its memory may still be in
//  use (the hide was not confirmed).
//
BOOL Vc4GPE::DdOverlaySurfaceGone(Vc4OvlSurf *pSurf)
{
    DWORD dwSeq = 0;
    BOOL fHide = FALSE, fOk;

    EnterCriticalSection(&m_csOvl);
    EnterCriticalSection(&m_csMbox);
    if (m_dwOvlUser == OVL_USER_DDRAW && m_pOvlDdSurf == pSurf) {
        m_fOvlShow = FALSE;
        m_dwOvlUser = OVL_USER_NONE;
        m_pOvlDdSurf = NULL;
        dwSeq = OverlayQueueLocked();
        fHide = TRUE;
    }
    LeaveCriticalSection(&m_csMbox);
    LeaveCriticalSection(&m_csOvl);

    if (!fHide) return TRUE;

    MboxWake();
    fOk = OverlayWait(dwSeq, DDOVL_GONE_WAIT_MS);
    RETAILMSG(1, (L"vc4disp: DirectDraw overlay surface destroyed while shown: "
        L"plane hidden%s\r\n", fOk ? L"" : L" (not confirmed)"));
    return fOk;
}

//==============================================================================
//
//  HAL callbacks
//

//------------------------------------------------------------------------------
//
//  Function:  OvlSurf
//
//  The Vc4OvlSurf behind a DirectDraw surface, or NULL. Every YV12 surface
//  is one (AllocSurface).
//
static Vc4OvlSurf *OvlSurf(LPDDRAWI_DDRAWSURFACE_LCL lpSurf)
{
    DDGPESurf *pSurf;

    if (lpSurf == NULL) return NULL;
    pSurf = DDGPESurf::GetDDGPESurf(lpSurf);
    if (pSurf == NULL || pSurf->PixelFormat() != ddgpePixelFormat_YV12) return NULL;
    return static_cast<Vc4OvlSurf*>(pSurf);
}

//------------------------------------------------------------------------------
//
//  Function:  HalUpdateOverlay
//
DWORD WINAPI HalUpdateOverlay(LPDDHAL_UPDATEOVERLAYDATA pd)
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    Vc4OvlSurf *pSurf = OvlSurf(pd->lpDDSrcSurface);
    RECT rcSrc = pd->rSrc;

    if (pSurf == NULL) {
        pd->ddRVal = DDERR_NOTAOVERLAYSURFACE;
        return DDHAL_DRIVER_HANDLED;
    }
    // An empty source means the whole surface
    if (IsRectEmpty(&rcSrc)) SetRect(&rcSrc, 0, 0, pSurf->Width(), pSurf->Height());

    if (pd->dwFlags & DDOVER_HIDE) {
        pd->ddRVal = pGpe->DdOverlayHide();
    } else if (pd->dwFlags & DDOVER_SHOW) {
        pd->ddRVal = pGpe->DdOverlayShow(pSurf, &rcSrc, &pd->rDest);
    } else {
        pd->ddRVal = pGpe->DdOverlayUpdate(pSurf, &rcSrc, &pd->rDest);
    }
    return DDHAL_DRIVER_HANDLED;
}

//------------------------------------------------------------------------------
//
//  Function:  HalSetOverlayPosition
//
DWORD WINAPI HalSetOverlayPosition(LPDDHAL_SETOVERLAYPOSITIONDATA pd)
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());

    pd->ddRVal = pGpe->DdOverlayMove(pd->lXPos, pd->lYPos);
    return DDHAL_DRIVER_HANDLED;
}

//------------------------------------------------------------------------------
//
//  Function:  HalFlip
//
//  Overlay flips show the target's buffer; anything else is DDGPE's.
//
DWORD WINAPI HalFlip(LPDDHAL_FLIPDATA pd)
{
    Vc4GPE *pGpe = static_cast<Vc4GPE*>(GetDDGPE());
    Vc4OvlSurf *pTarget;

    if (OvlSurf(pd->lpSurfCurr) == NULL) return DDGPEFlip(pd);

    pTarget = OvlSurf(pd->lpSurfTarg);
    pd->ddRVal = (pTarget != NULL) ? pGpe->DdOverlayFlip(pTarget) : DDERR_INVALIDOBJECT;
    return DDHAL_DRIVER_HANDLED;
}

//------------------------------------------------------------------------------
//
//  Function:  HalGetFlipStatus
//
//  Flips complete (firmware answered) before HalFlip returns.
//
DWORD WINAPI HalGetFlipStatus(LPDDHAL_GETFLIPSTATUSDATA pd)
{
    pd->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

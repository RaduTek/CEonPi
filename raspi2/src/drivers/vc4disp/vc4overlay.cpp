//
// vc4overlay.cpp - VideoCore HVS video overlay for vc4disp (CEonPi)
//
// The firmware's "Set plane" property tag (0x00048015, used by the Linux
// fkms driver) puts an image on an HVS plane, scaled and colour-converted by
// the hardware, without VCHIQ. Verified on a Pi 1 Model B (2026-11-25, V0
// probe below): it works alongside the legacy mailbox framebuffer, on the
// display id GET_DISPLAY_ID returns for index 0 (2 = HDMI; display 0 shows
// nothing), one layer above the framebuffer's (-127). The firmware leaves
// the tag's response bit clear even when it works.
//
// Set plane message (Linux vc4_firmware_kms.c struct set_plane, 60 bytes,
// little endian; built word by word here to avoid packing questions):
//   w0  display | plane_id << 8 | vc_image_type << 16 | (u8)layer << 24
//   w1  width | height << 16
//   w2  pitch (bytes, luma) | vpitch << 16 (0)
//   w3..w6  src_x, src_y, src_w, src_h (16.16 fixed point)
//   w7  (u16)dst_x | (u16)dst_y << 16
//   w8  dst_w | dst_h << 16
//   w9  alpha | num_planes << 8 | is_vu << 16 | color_encoding << 24
//   w10..w13  bus address of each plane (Y, U, V, 0)
//   w14 transform (0)
// Disabling a plane: same tag, only display and plane_id set.
// The firmware assumes chroma pitch = luma pitch / 2 for 4:2:0.
//
// V1 overlay engine: one overlay, reached through the private escape in
// vc4ovl.h (the DirectDraw overlay will sit on the same engine). CREATE
// allocates 1..3 frame buffers in one physically contiguous uncached block
// and maps it into the calling process; SHOW sets the source and
// destination rectangles (the destination is clipped to the screen here,
// the source adjusted in 16.16); FLIP picks the buffer on screen. SHOW and
// FLIP only queue the new plane state (sequence number m_dwOvlSeqQueued);
// the mailbox thread (vc4mbox.cpp) sends it and sets m_dwOvlSeqDone when the
// firmware has answered. DESTROY hides the plane and frees the buffers only
// once the firmware has answered the hide; otherwise it leaks them rather
// than let the HVS scan freed memory.
//
// Limits: if the owning process exits without DESTROY, the plane stays on
// screen until another process creates an overlay (the owner is then seen
// to be gone and its overlay destroyed). The overlay is always above the
// desktop (no colour key: the 16 bpp desktop has no alpha), so windows over
// the video area are hidden by it; the hardware cursor stays on top.
//
// Registry Overlay=0 turns the escape off.
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

#define TAG_GET_NUM_DISPLAYS    0x00040013
#define TAG_GET_DISPLAY_ID      0x00040016
#define TAG_GET_LAYER           0x0004000C
#define TAG_SET_PLANE           0x00048015
#define TAG_RESPONSE_OK         0x80000000

#define SET_PLANE_WORDS         15

// vc_image_type values (Linux vc_image_types.h)
#define VC_IMAGE_YUV420         3

// color_encoding: VC_IMAGE_YUVINFO_CSC_ITUR_BT601
#define VC_CSC_BT601            1

#define BUS_UNCACHED_ALIAS      0xC0000000

// The overlay's HVS plane (plane 0 would be fkms's primary)
#define OVL_PLANE_ID            1

// Longest wait for the firmware to answer a plane update
#define OVL_WAIT_MS             100
#define OVL_DESTROY_WAIT_MS     300

// Black: Y 16, U and V 128 (BT.601 limited range)
#define OVL_BLACK_Y             0x10101010
#define OVL_BLACK_C             0x80808080

// Test image: 4:2:0 planar, luma pitch a multiple of 32 bytes
#define PROBE_W                 320
#define PROBE_H                 240
#define PROBE_Y_BYTES           (PROBE_W * PROBE_H)
#define PROBE_C_BYTES           ((PROBE_W / 2) * (PROBE_H / 2))
#define PROBE_BYTES             (PROBE_Y_BYTES + 2 * PROBE_C_BYTES)

#define PROBE_PLANE_ID          1
#define PROBE_STAGE_MS          10000

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
//  Function:  Vc4IsProcessAlive
//
//  Also used by vc4ddovl.cpp.
//
BOOL Vc4IsProcessAlive(DWORD dwProcessId)
{
    HANDLE hProc;
    DWORD dwCode = 0;
    BOOL fAlive = FALSE;

    hProc = OpenProcess(0, FALSE, dwProcessId);
    if (hProc != NULL) {
        fAlive = GetExitCodeProcess(hProc, &dwCode) && (dwCode == STILL_ACTIVE);
        CloseHandle(hProc);
    }
    return fAlive;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayEscape
//
//  VC4OVL_ESCAPE (see vc4ovl.h). Returns 1 on success, (ULONG)-1 on failure
//  (with SetLastError), 0 when the overlay is turned off.
//
ULONG Vc4GPE::OverlayEscape(ULONG cjIn, PVOID pvIn, ULONG cjOut, PVOID pvOut)
{
    const VC4OVL_REQUEST *pReq = (const VC4OVL_REQUEST*)pvIn;
    VC4OVL_INFO *pInfo = NULL;
    DWORD dwCaller, dwSeq = 0, dwError = ERROR_SUCCESS;
    BOOL fWait = FALSE;
    BYTE *frames[VC4OVL_MAX_BUFFERS];

    if (!m_fOverlayEnabled) return 0;

    if (pReq == NULL || cjIn < sizeof(VC4OVL_REQUEST) ||
        pReq->dwSize < sizeof(VC4OVL_REQUEST)
    ) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (ULONG)-1;
    }
    if (pvOut != NULL && cjOut >= sizeof(VC4OVL_INFO)) {
        pInfo = (VC4OVL_INFO*)pvOut;
    }
    dwCaller = (DWORD)GetCallerVMProcessId();

    EnterCriticalSection(&m_csOvl);

    // Everything but CREATE and QUERY needs the caller's own overlay
    if (pReq->dwCommand != VC4OVL_CMD_CREATE &&
        pReq->dwCommand != VC4OVL_CMD_QUERY &&
        (!m_fOvlCreated || m_dwOvlOwner != dwCaller)
    ) {
        dwError = m_fOvlCreated ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE;
        goto cleanUp;
    }

    switch (pReq->dwCommand) {
    case VC4OVL_CMD_CREATE:
        memset(frames, 0, sizeof(frames));
        if (!OverlayCreate(pReq->dwFourCC, pReq->dwWidth, pReq->dwHeight,
                pReq->dwBuffers, frames)) {
            dwError = GetLastError();
            if (dwError == ERROR_SUCCESS) dwError = ERROR_GEN_FAILURE;
        }
        break;

    case VC4OVL_CMD_DESTROY:
        OverlayDestroy();
        break;

    case VC4OVL_CMD_SHOW:
        if (pReq->fShow && (
            pReq->rcSrc.left < 0 || pReq->rcSrc.top < 0 ||
            pReq->rcSrc.right > (LONG)m_dwOvlWidth ||
            pReq->rcSrc.bottom > (LONG)m_dwOvlHeight ||
            pReq->rcSrc.right <= pReq->rcSrc.left ||
            pReq->rcSrc.bottom <= pReq->rcSrc.top ||
            pReq->rcDst.right <= pReq->rcDst.left ||
            pReq->rcDst.bottom <= pReq->rcDst.top ||
            pReq->rcDst.right - pReq->rcDst.left > 0x7FFF ||
            pReq->rcDst.bottom - pReq->rcDst.top > 0x7FFF)
        ) {
            dwError = ERROR_INVALID_PARAMETER;
            break;
        }
        EnterCriticalSection(&m_csMbox);
        m_fOvlShow = (pReq->fShow != 0);
        if (m_fOvlShow) {
            m_rcOvlSrc = pReq->rcSrc;
            m_rcOvlDst = pReq->rcDst;
        }
        dwSeq = OverlayQueueLocked();
        LeaveCriticalSection(&m_csMbox);
        MboxWake();
        fWait = (pReq->dwFlags & VC4OVL_FLAG_WAIT) != 0;
        break;

    case VC4OVL_CMD_FLIP:
        if (pReq->dwBuffer >= m_dwOvlBuffers) {
            dwError = ERROR_INVALID_PARAMETER;
            break;
        }
        EnterCriticalSection(&m_csMbox);
        OverlayEscapeBufferLocked(pReq->dwBuffer);
        dwSeq = OverlayQueueLocked();
        LeaveCriticalSection(&m_csMbox);
        MboxWake();
        fWait = (pReq->dwFlags & VC4OVL_FLAG_WAIT) != 0;
        break;

    case VC4OVL_CMD_QUERY:
        break;

    default:
        dwError = ERROR_INVALID_PARAMETER;
        break;
    }

cleanUp:
    if (pInfo != NULL) {
        OverlayFillInfo(pInfo);
        if (m_fOvlCreated && m_dwOvlOwner == dwCaller) {
            DWORD i;
            for (i = 0; i < m_dwOvlBuffers; i++) {
                pInfo->pFrame[i] = m_pOvlOwnerVa + i * m_dwOvlFrameBytes;
            }
        }
    }
    LeaveCriticalSection(&m_csOvl);

    // Wait outside the lock, so another thread of the app can go on
    if (dwError == ERROR_SUCCESS && fWait) {
        if (!OverlayWait(dwSeq, OVL_WAIT_MS)) dwError = ERROR_TIMEOUT;
        if (pInfo != NULL) pInfo->dwSeqDone = m_dwOvlSeqDone;
    }

    if (dwError != ERROR_SUCCESS) {
        SetLastError(dwError);
        return (ULONG)-1;
    }
    return 1;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayCreate
//
//  Called with m_csOvl held. Allocates and clears the buffers, maps them
//  into the calling process and returns the caller's frame pointers.
//
BOOL Vc4GPE::OverlayCreate(DWORD dwFourCC, DWORD dwWidth, DWORD dwHeight,
    DWORD dwBuffers, BYTE **ppFrames)
{
    DWORD dwCaller, pitch, cpitch = 0, offset[3], frame, total, i;
    DWORD pa = 0;
    HANDLE hProc;
    BYTE *pMem, *pVa;

    dwCaller = (DWORD)GetCallerVMProcessId();

    // One overlay at a time: not while DirectDraw shows one; replace our
    // own, or a dead process's
    if (m_dwOvlUser == OVL_USER_DDRAW) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    if (m_fOvlCreated) {
        if (m_dwOvlOwner != dwCaller && Vc4IsProcessAlive(m_dwOvlOwner)) {
            SetLastError(ERROR_BUSY);
            return FALSE;
        }
        OverlayDestroy();
        if (m_fOvlCreated) {
            SetLastError(ERROR_BUSY);
            return FALSE;
        }
    }

    // Layout: planar 4:2:0 only. Packed 4:2:2 (YUY2 = vc_image_type 45,
    // UYVY = 47) was tried: the firmware accepts the plane but shows nothing
    // (Pi 1 Model B, 2026-11-25); Linux fkms does not offer it either.
    if (dwFourCC != VC4OVL_FOURCC_YV12 && dwFourCC != VC4OVL_FOURCC_I420) {
        RETAILMSG(1, (L"vc4disp: overlay: format 0x%08x not supported\r\n", dwFourCC));
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if (dwWidth < 2 || dwHeight < 2 || (dwWidth & 1) != 0 || (dwHeight & 1) != 0 ||
        dwWidth > VC4OVL_MAX_WIDTH || dwHeight > VC4OVL_MAX_HEIGHT ||
        dwBuffers < 1 || dwBuffers > VC4OVL_MAX_BUFFERS
    ) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    pitch = (dwWidth + 31) & ~31;
    cpitch = pitch / 2;
    offset[0] = 0;
    offset[1] = pitch * dwHeight;
    offset[2] = offset[1] + cpitch * (dwHeight / 2);
    frame = offset[2] + cpitch * (dwHeight / 2);
    frame = (frame + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    total = frame * dwBuffers;

    // The firmware needs the display id and the framebuffer's layer
    OverlayDisplayInit();

    if (!MboxInit()) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    pMem = (BYTE*)AllocPhysMem(total, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa);
    if (pMem == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay: cannot allocate %u bytes\r\n", total));
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    // Black
    for (i = 0; i < dwBuffers; i++) {
        BYTE *pFrame = pMem + i * frame;
        FillDwords(pFrame, offset[1], OVL_BLACK_Y);
        FillDwords(pFrame + offset[1], frame - offset[1], OVL_BLACK_C);
    }

    // Map it into the caller, uncached. Not VirtualCopyEx with PAGE_PHYSICAL
    // (as the RawFrameBuffer escape does for the firmware's framebuffer,
    // which lies outside CE's RAM): for RAM pages that maps them without a
    // page reference (kernel VMCopyPhysical, fIncRef FALSE), yet releasing
    // the mapping (VirtualFreeEx, or the process exiting) still drops one
    // (DecommitPages -> FreePhysPage), so FreePhysMem then freed every page
    // a second time ("DEBUGCHK failed in physmem.c at line 1060", then
    // "In-use page in free-list"). VirtualAllocCopyEx from our own kernel VM
    // address locks the pages and takes a reference per page (VMAllocCopy ->
    // VMSetPages -> DupPhysPage), so either release is balanced.
    hProc = (HANDLE)dwCaller;
    pVa = (BYTE*)VirtualAllocCopyEx(GetCurrentProcess(), hProc, pMem, total,
        PAGE_READWRITE | PAGE_NOCACHE);
    if (pVa == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay: VirtualAllocCopyEx failed (%d)\r\n",
            GetLastError()));
        FreePhysMem(pMem);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    EnterCriticalSection(&m_csMbox);
    m_dwOvlFourCC = dwFourCC;
    m_dwOvlWidth = dwWidth;
    m_dwOvlHeight = dwHeight;
    m_dwOvlBuffers = dwBuffers;
    m_dwOvlPitch = pitch;
    m_dwOvlChromaPitch = cpitch;
    m_dwOvlOffset[0] = offset[0];
    m_dwOvlOffset[1] = offset[1];
    m_dwOvlOffset[2] = offset[2];
    m_dwOvlFrameBytes = frame;
    m_pOvlMem = pMem;
    m_dwOvlBus = BUS_UNCACHED_ALIAS | pa;
    m_cbOvlMem = total;
    m_dwOvlOwner = dwCaller;
    m_pOvlOwnerVa = pVa;
    m_fOvlShow = FALSE;
    OverlayEscapeBufferLocked(0);
    m_fOvlCreated = TRUE;
    m_dwOvlUser = OVL_USER_ESCAPE;
    LeaveCriticalSection(&m_csMbox);

    for (i = 0; i < dwBuffers; i++) ppFrames[i] = pVa + i * frame;

    RETAILMSG(1, (L"vc4disp: overlay: created %ux%u %c%c%c%c x%u, pitch %u, "
        L"%u bytes at bus 0x%08x, display %u layer %d, process 0x%08x va 0x%08x\r\n",
        dwWidth, dwHeight,
        (WCHAR)(dwFourCC & 0xFF), (WCHAR)((dwFourCC >> 8) & 0xFF),
        (WCHAR)((dwFourCC >> 16) & 0xFF), (WCHAR)((dwFourCC >> 24) & 0xFF),
        dwBuffers, pitch, total, m_dwOvlBus, m_dwOvlDisplay, m_ovlLayer,
        dwCaller, (DWORD)pVa
    ));
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayDestroy
//
//  Hides the plane, then unmaps and frees the buffers once the firmware has
//  answered the hide. Called with m_csOvl held (or from the destructor).
//
VOID Vc4GPE::OverlayDestroy()
{
    DWORD dwSeq;
    BOOL fHidden;

    if (!m_fOvlCreated) return;

    EnterCriticalSection(&m_csMbox);
    m_fOvlShow = FALSE;
    dwSeq = OverlayQueueLocked();
    LeaveCriticalSection(&m_csMbox);
    MboxWake();
    fHidden = OverlayWait(dwSeq, OVL_DESTROY_WAIT_MS);

    EnterCriticalSection(&m_csMbox);
    m_fOvlCreated = FALSE;
    m_dwOvlUser = OVL_USER_NONE;
    m_dwOvlBusY = m_dwOvlBusU = m_dwOvlBusV = 0;
    LeaveCriticalSection(&m_csMbox);

    // The owner's mapping: only if the owner is the caller (a dead process's
    // mappings are already gone). VirtualAllocCopyEx registered the
    // reservation at the 64 KB block base, at the same block offset as our
    // own address. Each page holds a reference for the mapping, so if this
    // fails the pages simply stay allocated until the process exits.
    if (m_pOvlOwnerVa != NULL && m_dwOvlOwner == (DWORD)GetCallerVMProcessId()) {
        BYTE *pBase = m_pOvlOwnerVa - ((DWORD)m_pOvlMem & 0xFFFF);
        if (!VirtualFreeEx((HANDLE)m_dwOvlOwner, pBase, 0, MEM_RELEASE)) {
            RETAILMSG(1, (L"vc4disp: overlay: VirtualFreeEx(0x%08x) failed (%d)\r\n",
                (DWORD)pBase, GetLastError()));
        }
    }
    m_pOvlOwnerVa = NULL;

    if (fHidden) {
        FreePhysMem(m_pOvlMem);
        RETAILMSG(1, (L"vc4disp: overlay: destroyed\r\n"));
    } else {
        // The HVS may still be reading it: leak it rather than free it
        RETAILMSG(1, (L"vc4disp: overlay: hide not confirmed, %u bytes at bus "
            L"0x%08x left allocated\r\n", m_cbOvlMem, m_dwOvlBus
        ));
    }
    m_pOvlMem = NULL;
    m_dwOvlBus = 0;
    m_cbOvlMem = 0;
    m_dwOvlBuffers = 0;
    m_dwOvlOwner = 0;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayDisplayInit
//
//  Queries the firmware display id and the framebuffer's layer, once.
//
VOID Vc4GPE::OverlayDisplayInit()
{
    DWORD dwValue;

    if (m_fOvlDisplayKnown) return;

    if (!MailboxGetValue(TAG_GET_DISPLAY_ID, 0, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay: get display id failed, using 0\r\n"));
        dwValue = 0;
    }
    m_dwOvlDisplay = dwValue;
    if (!MailboxGetValue(TAG_GET_LAYER, 0, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay: get fb layer failed, assuming -127\r\n"));
        dwValue = (DWORD)-127;
    }
    m_ovlLayer = (int)dwValue + 1;
    if (m_ovlLayer > 127) m_ovlLayer = 127;
    m_fOvlDisplayKnown = TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayEscapeBufferLocked
//
//  m_csMbox held: the escape overlay's buffer dwBuffer becomes the plane's
//  image (Y/U/V bus addresses, size, pitch). YV12 memory order is Y, V, U;
//  I420 is Y, U, V.
//
VOID Vc4GPE::OverlayEscapeBufferLocked(DWORD dwBuffer)
{
    DWORD dwFrameBus = m_dwOvlBus + dwBuffer * m_dwOvlFrameBytes;

    m_dwOvlShown = dwBuffer;
    m_dwOvlBusY = dwFrameBus + m_dwOvlOffset[0];
    if (m_dwOvlFourCC == VC4OVL_FOURCC_I420) {
        m_dwOvlBusU = dwFrameBus + m_dwOvlOffset[1];
        m_dwOvlBusV = dwFrameBus + m_dwOvlOffset[2];
    } else {
        m_dwOvlBusV = dwFrameBus + m_dwOvlOffset[1];
        m_dwOvlBusU = dwFrameBus + m_dwOvlOffset[2];
    }
    m_dwPlaneWidth = m_dwOvlWidth;
    m_dwPlaneHeight = m_dwOvlHeight;
    m_dwPlanePitch = m_dwOvlPitch;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayQueueLocked
//
//  Called with m_csMbox held: marks the plane state for the mailbox thread
//  and returns its sequence number. The caller wakes the thread after
//  leaving the lock.
//
DWORD Vc4GPE::OverlayQueueLocked()
{
    m_dwOvlSeqQueued++;
    m_fOvlPending = TRUE;
    return m_dwOvlSeqQueued;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayWait
//
//  Waits until the firmware has answered plane update dwSeq.
//
BOOL Vc4GPE::OverlayWait(DWORD dwSeq, DWORD dwTimeoutMs)
{
    DWORD dwStart = GetTickCount();

    for (;;) {
        if ((LONG)(m_dwOvlSeqDone - dwSeq) >= 0) return TRUE;
        if (GetTickCount() - dwStart >= dwTimeoutMs || m_hOvlDoneEvent == NULL) {
            return FALSE;
        }
        // Short slices: the event is auto-reset and may wake another waiter
        WaitForSingleObject(m_hOvlDoneEvent, 5);
    }
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayBuildTag
//
//  Mailbox thread, m_csMbox held: writes the "Set plane" tag for the
//  current overlay state at pTag (a disable when hidden, destroyed or
//  clipped away). Returns its size in DWORDs.
//
DWORD Vc4GPE::OverlayBuildTag(DWORD *pTag)
{
    DWORD *w = &pTag[3];
    __int64 sx, sy, sw, sh, dw, dh;
    LONG dl, dt, dr, db, cut;
    LONG scrW = m_nScreenWidth, scrH = m_nScreenHeight;

    pTag[0] = TAG_SET_PLANE;
    pTag[1] = SET_PLANE_WORDS * sizeof(DWORD);
    pTag[2] = 0;
    memset(w, 0, SET_PLANE_WORDS * sizeof(DWORD));
    w[0] = (m_dwOvlDisplay & 0xFF) | (OVL_PLANE_ID << 8);

    if (!m_fOvlShow || m_dwOvlBusY == 0) return 3 + SET_PLANE_WORDS;

    // Source in 16.16, destination in screen pixels, then clip the
    // destination to the screen and cut the source by the same fraction
    sx = (__int64)m_rcOvlSrc.left << 16;
    sy = (__int64)m_rcOvlSrc.top << 16;
    sw = (__int64)(m_rcOvlSrc.right - m_rcOvlSrc.left) << 16;
    sh = (__int64)(m_rcOvlSrc.bottom - m_rcOvlSrc.top) << 16;
    dl = m_rcOvlDst.left;
    dt = m_rcOvlDst.top;
    dr = m_rcOvlDst.right;
    db = m_rcOvlDst.bottom;
    dw = dr - dl;
    dh = db - dt;

    if (dl >= scrW || dt >= scrH || dr <= 0 || db <= 0) {
        return 3 + SET_PLANE_WORDS;     // entirely off screen: disable
    }
    if (dl < 0) {
        cut = -dl;
        sx += (sw * cut) / dw;
        dl = 0;
    }
    if (dt < 0) {
        cut = -dt;
        sy += (sh * cut) / dh;
        dt = 0;
    }
    if (dr > scrW) dr = scrW;
    if (db > scrH) db = scrH;
    // Source width/height of the visible part, from the original scale
    sw = (sw * (dr - dl)) / dw;
    sh = (sh * (db - dt)) / dh;
    if (sw <= 0 || sh <= 0) return 3 + SET_PLANE_WORDS;

    // Planar 4:2:0, the Y/U/V bus addresses of the buffer on screen
    w[0] |= (VC_IMAGE_YUV420 << 16) | (((DWORD)m_ovlLayer & 0xFF) << 24);
    w[1] = m_dwPlaneWidth | (m_dwPlaneHeight << 16);
    w[2] = m_dwPlanePitch;
    w[3] = (DWORD)sx;
    w[4] = (DWORD)sy;
    w[5] = (DWORD)sw;
    w[6] = (DWORD)sh;
    w[7] = ((DWORD)dl & 0xFFFF) | (((DWORD)dt & 0xFFFF) << 16);
    w[8] = ((DWORD)(dr - dl) & 0xFFFF) | (((DWORD)(db - dt) & 0xFFFF) << 16);
    w[9] = 255 | (3 << 8) | (0 << 16) | (VC_CSC_BT601 << 24);
    w[10] = m_dwOvlBusY;
    w[11] = m_dwOvlBusU;
    w[12] = m_dwOvlBusV;
    w[13] = 0;
    w[14] = 0;                          // transform
    return 3 + SET_PLANE_WORDS;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayFillInfo
//
VOID Vc4GPE::OverlayFillInfo(VC4OVL_INFO *pInfo)
{
    memset(pInfo, 0, sizeof(*pInfo));
    pInfo->dwSize = sizeof(*pInfo);

    EnterCriticalSection(&m_csMbox);
    if (m_fOvlCreated) {
        pInfo->dwFourCC = m_dwOvlFourCC;
        pInfo->dwWidth = m_dwOvlWidth;
        pInfo->dwHeight = m_dwOvlHeight;
        pInfo->dwBuffers = m_dwOvlBuffers;
        pInfo->dwPitch = m_dwOvlPitch;
        pInfo->dwChromaPitch = m_dwOvlChromaPitch;
        pInfo->dwOffset[0] = m_dwOvlOffset[0];
        pInfo->dwOffset[1] = m_dwOvlOffset[1];
        pInfo->dwOffset[2] = m_dwOvlOffset[2];
        pInfo->dwFrameBytes = m_dwOvlFrameBytes;
        pInfo->dwShown = m_dwOvlShown;
    }
    pInfo->dwSeqQueued = m_dwOvlSeqQueued;
    pInfo->dwSeqDone = m_dwOvlSeqDone;
    pInfo->dwScreenWidth = m_nScreenWidth;
    pInfo->dwScreenHeight = m_nScreenHeight;
    LeaveCriticalSection(&m_csMbox);
}

//==============================================================================
//
//  V0 probe (registry OverlayProbe=1), verified 2026-11-25. After the first
//  SetMode a thread waits 10 s, queries and logs the number of displays, the
//  id of display 0 and the framebuffer's layer, then shows a 320x240 YUV420
//  colour-bar image on plane 1 one layer above the framebuffer: stage 1 at
//  100,100 scaled to 640x480, stage 2 at 400,200 960x540, stage 3 disabled,
//  and (display id not 0) stages 4/5 the same on display 0. Each stage logs
//  the tag's response. Do not run it together with the overlay escape: both
//  use plane 1, and the probe calls the firmware synchronously.
//
//------------------------------------------------------------------------------
//
//  Function:  ProbeTag
//
//  Sends one tag with a one-word value. Returns TRUE when the IOCTL worked;
//  *pResp gets the tag's request/response word and *pValue the value word.
//
static BOOL ProbeTag(DWORD dwTag, DWORD dwIn, DWORD *pResp, DWORD *pValue)
{
    DWORD msg[4], cbOut = 0;

    msg[0] = dwTag;
    msg[1] = sizeof(DWORD);
    msg[2] = 0;
    msg[3] = dwIn;
    *pResp = 0;
    *pValue = 0;
    if (!KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, msg, sizeof(msg), msg, sizeof(msg), &cbOut
    )) {
        return FALSE;
    }
    *pResp = msg[2];
    *pValue = msg[3];
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Function:  ProbeSetPlane
//
//  Shows (fShow) or disables the test plane. Logs the result under pszStage.
//
static VOID ProbeSetPlane(LPCWSTR pszStage, BOOL fShow, DWORD dwDisplay,
    int layer, DWORD dwBus, int x, int y, int w, int h)
{
    DWORD msg[3 + SET_PLANE_WORDS], cbOut = 0;
    BOOL fOk;

    memset(msg, 0, sizeof(msg));
    msg[0] = TAG_SET_PLANE;
    msg[1] = SET_PLANE_WORDS * sizeof(DWORD);
    msg[2] = 0;
    msg[3] = (dwDisplay & 0xFF) | (PROBE_PLANE_ID << 8);
    if (fShow) {
        msg[3] |= (VC_IMAGE_YUV420 << 16) | (((DWORD)layer & 0xFF) << 24);
        msg[4] = PROBE_W | (PROBE_H << 16);
        msg[5] = PROBE_W;                       // luma pitch, vpitch 0
        msg[6] = 0;                             // src_x
        msg[7] = 0;                             // src_y
        msg[8] = PROBE_W << 16;                 // src_w
        msg[9] = PROBE_H << 16;                 // src_h
        msg[10] = ((DWORD)x & 0xFFFF) | (((DWORD)y & 0xFFFF) << 16);
        msg[11] = ((DWORD)w & 0xFFFF) | (((DWORD)h & 0xFFFF) << 16);
        msg[12] = 255 | (3 << 8) | (0 << 16) | (VC_CSC_BT601 << 24);
        msg[13] = dwBus;                                    // Y
        msg[14] = dwBus + PROBE_Y_BYTES;                    // U
        msg[15] = dwBus + PROBE_Y_BYTES + PROBE_C_BYTES;    // V
        msg[16] = 0;
        msg[17] = 0;                            // transform
    }

    fOk = KernelIoControl(
        IOCTL_HAL_MY_MAILBOX_PROPERTY, msg, sizeof(msg), msg, sizeof(msg), &cbOut
    );
    if (fShow) {
        RETAILMSG(1, (L"vc4disp: overlay probe %s: show display %u layer %d "
            L"at %d,%d %dx%d: ioctl %s, tag resp 0x%08x, value 0x%08x\r\n",
            pszStage, dwDisplay, layer, x, y, w, h,
            fOk ? L"ok" : L"FAILED", msg[2], msg[3]
        ));
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe %s: disable display %u: "
            L"ioctl %s, tag resp 0x%08x, value 0x%08x\r\n",
            pszStage, dwDisplay, fOk ? L"ok" : L"FAILED", msg[2], msg[3]
        ));
    }
}

//------------------------------------------------------------------------------
//
//  Function:  ProbeFillImage
//
//  Colour bars (BT.601 limited range) in the top three quarters, a luma ramp
//  in the bottom quarter. Bars left to right: white, yellow, cyan, green,
//  magenta, red, blue, black. A U/V mix-up shows as wrong bar colours.
//
static VOID ProbeFillImage(BYTE *pImage)
{
    static const BYTE bars[8][3] = {
        { 235, 128, 128 },  // white
        { 210,  16, 146 },  // yellow
        { 170, 166,  16 },  // cyan
        { 145,  54,  34 },  // green
        { 106, 202, 222 },  // magenta
        {  81,  90, 240 },  // red
        {  41, 240, 110 },  // blue
        {  16, 128, 128 },  // black
    };
    BYTE *pY = pImage;
    BYTE *pU = pImage + PROBE_Y_BYTES;
    BYTE *pV = pU + PROBE_C_BYTES;
    int x, y, bar, rampTop = (PROBE_H * 3) / 4;

    for (y = 0; y < PROBE_H; y++) {
        for (x = 0; x < PROBE_W; x++) {
            if (y < rampTop) {
                bar = (x * 8) / PROBE_W;
                pY[y * PROBE_W + x] = bars[bar][0];
            } else {
                pY[y * PROBE_W + x] = (BYTE)(16 + (x * 219) / (PROBE_W - 1));
            }
        }
    }
    for (y = 0; y < PROBE_H / 2; y++) {
        for (x = 0; x < PROBE_W / 2; x++) {
            if (y * 2 < rampTop) {
                bar = (x * 2 * 8) / PROBE_W;
                pU[y * (PROBE_W / 2) + x] = bars[bar][1];
                pV[y * (PROBE_W / 2) + x] = bars[bar][2];
            } else {
                pU[y * (PROBE_W / 2) + x] = 128;
                pV[y * (PROBE_W / 2) + x] = 128;
            }
        }
    }
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbe
//
VOID Vc4GPE::OverlayProbe()
{
    DWORD dwResp, dwValue, dwDisplays = 0, dwDisplayId = 0, dwBus, pa = 0;
    int fbLayer = 0, layer;
    BOOL fHaveId = FALSE;
    BYTE *pImage;

    // Let the desktop come up first
    Sleep(PROBE_STAGE_MS);

    if (ProbeTag(TAG_GET_NUM_DISPLAYS, 0, &dwResp, &dwValue)) {
        dwDisplays = dwValue;
        RETAILMSG(1, (L"vc4disp: overlay probe: get num displays: tag resp "
            L"0x%08x, displays %u\r\n", dwResp, dwValue));
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get num displays: ioctl FAILED\r\n"));
    }
    if (ProbeTag(TAG_GET_DISPLAY_ID, 0, &dwResp, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay probe: get display id (index 0): tag "
            L"resp 0x%08x, id %u\r\n", dwResp, dwValue));
        if ((dwResp & TAG_RESPONSE_OK) != 0) {
            dwDisplayId = dwValue;
            fHaveId = TRUE;
        }
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get display id: ioctl FAILED\r\n"));
    }
    if (ProbeTag(TAG_GET_LAYER, 0, &dwResp, &dwValue)) {
        RETAILMSG(1, (L"vc4disp: overlay probe: get fb layer: tag resp 0x%08x, "
            L"layer %d\r\n", dwResp, (int)dwValue));
        if ((dwResp & TAG_RESPONSE_OK) != 0) fbLayer = (int)dwValue;
    } else {
        RETAILMSG(1, (L"vc4disp: overlay probe: get fb layer: ioctl FAILED\r\n"));
    }

    // One layer above the framebuffer (s8 in the message)
    layer = fbLayer + 1;
    if (layer > 127) layer = 127;
    if (layer < -126) layer = -126;

    pImage = (BYTE*)AllocPhysMem(
        PROBE_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &pa
    );
    if (pImage == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay probe: image allocation failed\r\n"));
        return;
    }
    dwBus = BUS_UNCACHED_ALIAS | pa;
    ProbeFillImage(pImage);
    RETAILMSG(1, (L"vc4disp: overlay probe: %ux%u YUV420 image at bus 0x%08x, "
        L"displays %u, display id %u%s, layer %d\r\n",
        PROBE_W, PROBE_H, dwBus, dwDisplays, dwDisplayId,
        fHaveId ? L"" : L" (not answered, using 0)", layer));

    ProbeSetPlane(L"stage 1", TRUE, dwDisplayId, layer, dwBus, 100, 100, 640, 480);
    Sleep(PROBE_STAGE_MS);
    ProbeSetPlane(L"stage 2", TRUE, dwDisplayId, layer, dwBus, 400, 200, 960, 540);
    Sleep(PROBE_STAGE_MS);
    ProbeSetPlane(L"stage 3", FALSE, dwDisplayId, layer, dwBus, 0, 0, 0, 0);

    if (dwDisplayId != 0) {
        Sleep(PROBE_STAGE_MS);
        ProbeSetPlane(L"stage 4", TRUE, 0, layer, dwBus, 100, 100, 640, 480);
        Sleep(PROBE_STAGE_MS);
        ProbeSetPlane(L"stage 5", FALSE, 0, layer, dwBus, 0, 0, 0, 0);
    }

    // The firmware may still scan the image if a disable did not take, so
    // the buffer is deliberately not freed (115 KB, probe builds only)
    RETAILMSG(1, (L"vc4disp: overlay probe done\r\n"));
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbeThreadProc
//
DWORD WINAPI Vc4GPE::OverlayProbeThreadProc(LPVOID pContext)
{
    ((Vc4GPE*)pContext)->OverlayProbe();
    return 0;
}

//------------------------------------------------------------------------------
//
//  Method:  OverlayProbeStart
//
//  Called from SetMode; starts the probe thread once.
//
VOID Vc4GPE::OverlayProbeStart()
{
    HANDLE hThread;

    if (!m_fOverlayProbe || m_fOverlayProbeStarted) return;
    m_fOverlayProbeStarted = TRUE;

    hThread = CreateThread(NULL, 0, OverlayProbeThreadProc, this, 0, NULL);
    if (hThread == NULL) {
        RETAILMSG(1, (L"vc4disp: overlay probe thread creation failed\r\n"));
        return;
    }
    CloseHandle(hThread);
    RETAILMSG(1, (L"vc4disp: overlay probe starts in %u s\r\n",
        PROBE_STAGE_MS / 1000));
}

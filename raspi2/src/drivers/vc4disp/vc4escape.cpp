//
// vc4escape.cpp - ExtEscape support for vc4disp (CEonPi): RawFrameBuffer
//
// GETRAWFRAMEBUFFER (0x00020001, the Windows Mobile "raw frame buffer"
// escape) gives an application a pointer to the framebuffer it can write to
// directly. Video players use it: TCPMP prefers it over GDI, and its own
// ARM colour conversion and scaling then write frames straight to the
// screen instead of into a DIB that GDI copies again (about 48 ms per
// 1920x1080 frame at 16 bpp).
//
// The application is a user-mode process, so the framebuffer's physical
// pages are mapped into it: VirtualAllocEx reserves the range in the caller,
// VirtualCopyEx (PAGE_PHYSICAL) maps it uncached, and VirtualProtectEx then
// tries to make it write-combining (registry RawFbWriteCombine), which
// suits a player that only writes. Each call makes a new mapping: a mapping
// cannot be reused safely across calls (the process may have exited and its
// id been reused), and it disappears with the process. The cost is address
// space in the caller (the framebuffer size per call); players ask once or
// a few times per run.
//
// Cache coherence: the driver draws through the cached framebuffer view
// (CachedFrameBuffer) and normally only writes cached lines back. Once a
// raw pointer exists, someone else may write the framebuffer behind the
// cache, so the whole D-cache is flushed once and FlushRect from then on
// also invalidates (m_fRawFbGiven), so GDI never reads stale cached pixels.
//
// Registry value RawFrameBuffer=0 turns the escape off.
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include "vc4disp.h"

// Escape codes (Windows Mobile SDK values; not in the CE 7 headers)
#define VC4_ESC_QUERYESCSUPPORT     8
#define VC4_ESC_GETRAWFRAMEBUFFER   0x00020001

// RawFrameBufferInfo.wFormat
#define VC4_RAWFB_FORMAT_565        1

// Static kernel view: physical address of a static virtual address
#define STATIC_VA_TO_PA(va)         ((DWORD)(va) & 0x1FFFFFFF)

//------------------------------------------------------------------------------
//
//  Method:  DrvEscape
//
//  Returns > 0 on success, 0 for escapes the driver does not support, < 0 on
//  failure (ExtEscape conventions).
//
ULONG Vc4GPE::DrvEscape(
    SURFOBJ *pso, ULONG iEsc, ULONG cjIn, PVOID pvIn, ULONG cjOut, PVOID pvOut
) {
    switch (iEsc) {
    case VC4_ESC_QUERYESCSUPPORT:
        if (pvIn != NULL && cjIn >= sizeof(DWORD) &&
            *(DWORD*)pvIn == VC4_ESC_GETRAWFRAMEBUFFER
        ) {
            return m_fRawFbEnabled ? 1 : 0;
        }
        return 0;

    case VC4_ESC_GETRAWFRAMEBUFFER:
        if (!m_fRawFbEnabled) return 0;
        if (pvOut == NULL || cjOut < sizeof(VC4_RAWFRAMEBUFFERINFO)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return (ULONG)-1;
        }
        return GetRawFrameBuffer((VC4_RAWFRAMEBUFFERINFO*)pvOut) ? 1 : (ULONG)-1;
    }

    return GPE::DrvEscape(pso, iEsc, cjIn, pvIn, cjOut, pvOut);
}

//------------------------------------------------------------------------------
//
//  Method:  GetRawFrameBuffer
//
//  Maps the framebuffer into the calling process and describes it.
//
BOOL Vc4GPE::GetRawFrameBuffer(VC4_RAWFRAMEBUFFERINFO *pInfo)
{
    HANDLE hProc;
    DWORD pa, cbMap, dwOldProtect;
    LONG stride;
    VOID *pVa;
    BOOL fWc = FALSE;

    if (m_pPrimarySurface == NULL || m_gpeMode.Bpp != 16) return FALSE;

    stride = m_pPrimarySurface->Stride();
    if (stride <= 0) return FALSE;

    // Physical pages of the framebuffer (page aligned by the firmware)
    pa = STATIC_VA_TO_PA(m_pPrimarySurface->Buffer());
    if (pa & (PAGE_SIZE - 1)) {
        RETAILMSG(1, (L"vc4disp: raw framebuffer: fb 0x%08x not page aligned\r\n", pa));
        return FALSE;
    }
    cbMap = (stride * m_gpeMode.height + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // The process that called ExtEscape
    hProc = (HANDLE)GetCallerVMProcessId();

    pVa = VirtualAllocEx(hProc, NULL, cbMap, MEM_RESERVE, PAGE_NOACCESS);
    if (pVa == NULL) {
        RETAILMSG(1, (L"vc4disp: raw framebuffer: VirtualAllocEx failed (%d)\r\n",
            GetLastError()));
        return FALSE;
    }
    if (!VirtualCopyEx(hProc, pVa, (HANDLE)GetCurrentProcessId(),
            (VOID*)(pa >> 8), cbMap, PAGE_READWRITE | PAGE_NOCACHE | PAGE_PHYSICAL)
    ) {
        RETAILMSG(1, (L"vc4disp: raw framebuffer: VirtualCopyEx failed (%d)\r\n",
            GetLastError()));
        VirtualFreeEx(hProc, pVa, 0, MEM_RELEASE);
        return FALSE;
    }
    if (m_fRawFbWriteCombine) {
        fWc = VirtualProtectEx(hProc, pVa, cbMap,
            PAGE_READWRITE | PAGE_WRITECOMBINE, &dwOldProtect);
    }

    // From now on someone may write the framebuffer behind our cache
    if (!m_fRawFbGiven) {
        m_fRawFbGiven = TRUE;
        CacheRangeFlush(NULL, 0, CACHE_SYNC_DISCARD);
    }

    pInfo->wFormat = VC4_RAWFB_FORMAT_565;
    pInfo->wBPP = 16;
    pInfo->pFramePointer = pVa;
    pInfo->cxStride = 2;
    pInfo->cyStride = stride;
    pInfo->cxPixels = m_gpeMode.width;
    pInfo->cyPixels = m_gpeMode.height;

    RETAILMSG(1, (L"vc4disp: raw framebuffer for process 0x%08x at 0x%08x, "
        L"%dx%d stride %d, %s\r\n",
        (DWORD)hProc, (DWORD)pVa, m_gpeMode.width, m_gpeMode.height, stride,
        fWc ? L"write-combining" : L"uncached"
    ));
    return TRUE;
}

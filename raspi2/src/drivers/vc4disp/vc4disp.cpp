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
//  File:  vc4disp.cpp
//
//  VideoCore IV display driver for the BCM2835 (CEonPi), GPE based.
//
//  D0: a cleaned-up copy of raspiBasic_ddi (the Microsoft HALFlat sample).
//  The OAL owns the framebuffer: it allocates it through the mailbox at boot
//  (src\Common\VidConDebugOut\video.cpp) and describes it as the single mode
//  returned by IOCTL_HAL_DDI (src\oal\oallib\ddi.c). The phFrameBase field
//  carries the kernel's uncached static virtual address (0xA0000000 | pa),
//  not a physical address. All drawing is GPE software emulation with a
//  software cursor.
//
//  D1: GDI reads the screen back for scrolling, window moves, ROPs, alpha and
//  the cursor, and uncached reads are very slow on the ARM1176. So the driver
//  draws through the cached static view (0x80000000 | pa, the OAL address
//  table maps all RAM) and cleans the D-cache over the touched rows after
//  every blit, line and cursor update, so the VideoCore scans out current
//  pixels. Nothing else writes the framebuffer once SetMode has stopped the
//  OAL console, so the cached copy never goes stale. Registry value
//  CachedFrameBuffer=0 restores the old uncached drawing (for comparison).
//
//  Changes from raspiBasic_ddi: no MmUnmapIoSpace on a buffer that was never
//  mapped, the mode-info buffer is no longer leaked by SetMode and is zeroed
//  (the colour masks were read from uninitialised memory for 24/32 bpp), and
//  the disabled DMA blit code is gone.
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <emul.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <ddhal.h>
#include "vc4disp.h"

//------------------------------------------------------------------------------
// Debug Zones

INSTANTIATE_GPE_ZONES(0x0003, "vc4disp", "PDD", "Unused")

//------------------------------------------------------------------------------
// Static variables

static GPE *g_pGPE;
static ULONG g_bitMasks[] = { 0, 0, 0 };

//------------------------------------------------------------------------------
// External functions

BOOL APIENTRY GPEEnableDriver(
   ULONG version, ULONG cj, DRVENABLEDATA *pData, ENGCALLBACKS *pCallbacks
);

//------------------------------------------------------------------------------
//
//  Function:  DisplayInit
//
//  GWES calls this once before any other call into the driver.
//
BOOL DisplayInit(LPCTSTR pszInstance, DWORD monitors)
{
    HKEY hKey;
    DWORD dwCachedFb = 1, dwType, dwSize, dwValue;

    DEBUGMSG(GPE_ZONE_PDD, (
        L"+DisplayInit(%s, %d)\r\n", pszInstance, monitors
    ));

    // Driver settings from the instance key GWES passes (the display
    // candidate), or our own key
    if (RegOpenKeyEx(
            HKEY_LOCAL_MACHINE,
            (pszInstance != NULL) ? pszInstance : VC4DISP_REG_KEY,
            0, 0, &hKey
        ) == ERROR_SUCCESS
    ) {
        dwSize = sizeof(dwValue);
        if (RegQueryValueEx(
                hKey, VC4DISP_REG_CACHEDFB, NULL, &dwType, (LPBYTE)&dwValue,
                &dwSize
            ) == ERROR_SUCCESS && dwType == REG_DWORD
        ) {
            dwCachedFb = dwValue;
        }
        RegCloseKey(hKey);
    }

    g_pGPE = new Vc4GPE(dwCachedFb != 0);

    DEBUGMSG(GPE_ZONE_PDD, (L"-DisplayInit(rc = %d)\r\n", g_pGPE != NULL));
    return (g_pGPE != NULL);
}

//------------------------------------------------------------------------------
//
//  Function:  DrvEnableDriver
//
BOOL DrvEnableDriver(
    ULONG version, ULONG cj, DRVENABLEDATA *pData, ENGCALLBACKS *pCallbacks
) {
    return GPEEnableDriver(version, cj, pData, pCallbacks);
}

//------------------------------------------------------------------------------
//
//  Function:  DrvGetMasks
//
ULONG *APIENTRY DrvGetMasks(DHPDEV dhpdev)
{
    return g_bitMasks;
}

//------------------------------------------------------------------------------
//
//  Function:  RegisterDDHALAPI
//
VOID RegisterDDHALAPI()
{
}

//------------------------------------------------------------------------------
//
//  Function:  GetGPE
//
GPE* GetGPE()
{
    return g_pGPE;
}

//------------------------------------------------------------------------------
//
//  Constructor
//
Vc4GPE::Vc4GPE(BOOL fCachedFb)
{
    m_pPrimarySurface = NULL;
    memset(&m_gpeMode, 0, sizeof(m_gpeMode));
    m_fCachedFb = fCachedFb;
    m_cursorVisible = FALSE;
    m_cursorDisabled = TRUE;
    m_cursorForcedOff = FALSE;
    memset(&m_cursorRect, 0, sizeof(m_cursorRect));
    memset(&m_cursorSize, 0, sizeof(m_cursorSize));
    memset(&m_cursorHotspot, 0, sizeof(m_cursorHotspot));
    m_cursorStore = NULL;
    m_cursorXor = NULL;
    m_cursorAnd = NULL;
}

//------------------------------------------------------------------------------
//
//  Destructor
//
Vc4GPE::~Vc4GPE()
{
    delete m_pPrimarySurface; m_pPrimarySurface = NULL;
    delete [] m_cursorStore; m_cursorStore = NULL;
    delete [] m_cursorXor; m_cursorXor = NULL;
    delete [] m_cursorAnd; m_cursorAnd = NULL;
}

//------------------------------------------------------------------------------
//
//  Method:  QueryModeInfo
//
//  Asks the OAL for a mode description: a DDHAL_MODE_INFO followed by the
//  palette or colour-mask DWORDs. The first call has no output buffer and
//  returns the size needed. Returns a zeroed buffer the caller must delete [],
//  or NULL.
//
UINT8* Vc4GPE::QueryModeInfo(int modeNumber)
{
    ULONG commands[2], size = 0;
    UINT8 *pBuffer;

    commands[0] = DDHAL_COMMAND_GET_MODE_INFO;
    commands[1] = modeNumber;
    if (KernelIoControl(
            IOCTL_HAL_DDI, commands, sizeof(commands), NULL, 0, &size
        ) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        size < sizeof(DDHAL_MODE_INFO) + 3 * sizeof(ULONG)
    ) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::QueryModeInfo: "
            L"IOCTL_HAL_DDI!GET_MODE_INFO size query failed for mode %d\r\n",
            modeNumber
        ));
        return NULL;
    }

    pBuffer = new UINT8[size];
    if (pBuffer == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::QueryModeInfo: "
            L"Failed to allocate %d bytes\r\n", size
        ));
        return NULL;
    }
    memset(pBuffer, 0, size);

    if (!KernelIoControl(
        IOCTL_HAL_DDI, commands, sizeof(commands), pBuffer, size, &size
    )) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::QueryModeInfo: "
            L"IOCTL_HAL_DDI!GET_MODE_INFO failed for mode %d\r\n", modeNumber
        ));
        delete [] pBuffer;
        return NULL;
    }

    return pBuffer;
}

//------------------------------------------------------------------------------
//
//  Method:  NumModes
//
int Vc4GPE::NumModes()
{
    ULONG command;
    int count;

    command = DDHAL_COMMAND_GET_NUM_MODES;
    if (!KernelIoControl(
        IOCTL_HAL_DDI, &command, sizeof(command), &count, sizeof(count), NULL
    )) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::NumModes: "
            L"IOCTL_HAL_DDI!GET_NUM_MODES failed\r\n"
        ));
        count = 0;
    }

    return count;
}

//------------------------------------------------------------------------------
//
//  Method:  GetModeInfo
//
SCODE Vc4GPE::GetModeInfo(GPEMode* pMode, int modeNumber)
{
    UINT8 *pBuffer;
    DDHAL_MODE_INFO *pInfo;

    pBuffer = QueryModeInfo(modeNumber);
    if (pBuffer == NULL) return E_FAIL;

    pInfo = (DDHAL_MODE_INFO*)pBuffer;
    pMode->modeId    = pInfo->modeId;
    pMode->width     = pInfo->width;
    pMode->height    = pInfo->height;
    pMode->Bpp       = pInfo->bpp;
    pMode->frequency = pInfo->frequency;
    pMode->format    = (EGPEFormat)pInfo->format;

    delete [] pBuffer;
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  SetMode
//
SCODE Vc4GPE::SetMode(int modeNumber, HPALETTE *pPalette)
{
    SCODE sc = S_OK;
    UINT8 *pBuffer = NULL;
    DDHAL_MODE_INFO *pInfo;
    ULONG commands[2], *pColors, ulFrame;

    pBuffer = QueryModeInfo(modeNumber);
    if (pBuffer == NULL) {
        sc = E_FAIL;
        goto cleanUp;
    }
    pInfo = (DDHAL_MODE_INFO*)pBuffer;

    // Ask the OAL to switch modes. Today this only stops the OAL's on-screen
    // debug console (and the boot splash) from drawing into the framebuffer.
    commands[0] = DDHAL_COMMAND_SET_MODE;
    commands[1] = modeNumber;
    if (!KernelIoControl(
        IOCTL_HAL_DDI, commands, sizeof(commands), NULL, 0, NULL
    )) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetMode: "
            L"IOCTL_HAL_DDI!SET_MODE failed for mode %d\r\n", modeNumber
        ));
        sc = E_FAIL;
        goto cleanUp;
    }

    // The framebuffer is the OAL's static mapping, nothing to unmap
    delete m_pPrimarySurface; m_pPrimarySurface = NULL;

    m_gpeMode.modeId    = pInfo->modeId;
    m_gpeMode.width     = pInfo->width;
    m_gpeMode.height    = pInfo->height;
    m_gpeMode.Bpp       = pInfo->bpp;
    m_gpeMode.frequency = pInfo->frequency;
    m_gpeMode.format    = (EGPEFormat)pInfo->format;
    m_pMode = &m_gpeMode;
    m_nScreenWidth  = m_pMode->width;
    m_nScreenHeight = m_pMode->height;

    // phFrameBase holds the OAL's uncached virtual address (see file header)
    ulFrame = (ULONG)pInfo->phFrameBase;
    if (ulFrame == 0) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetMode: no frame buffer\r\n"));
        sc = E_FAIL;
        goto cleanUp;
    }

    // Draw through the cached view of the same memory when asked to. Only
    // the static uncached range has a known cached twin.
    if (m_fCachedFb) {
        if ((ulFrame & VC4DISP_STATIC_MASK) == VC4DISP_STATIC_UNCACHED) {
            ulFrame -= VC4DISP_STATIC_UNCACHED_OFS;
            // Drop any lines a previous mode left in the cache
            CacheRangeFlush(NULL, 0, CACHE_SYNC_DISCARD);
        } else {
            RETAILMSG(1, (L"vc4disp: fb 0x%08x is not in the static uncached "
                L"range, drawing uncached\r\n", ulFrame
            ));
            m_fCachedFb = FALSE;
        }
    }

    m_pPrimarySurface = new GPESurf(
        m_pMode->width, m_pMode->height, (VOID*)ulFrame,
        pInfo->frameStride, m_pMode->format
    );
    if (m_pPrimarySurface == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetMode: "
            L"Can't allocate GPESurf for display surface\r\n"
        ));
        sc = E_OUTOFMEMORY;
        goto cleanUp;
    }

    // Palette: indexed, bit fields (16 bpp masks) or plain RGB
    pColors = (ULONG*)&pInfo[1];
    if (pInfo->paletteMode == PAL_INDEXED) {
        *pPalette = EngCreatePalette(
            pInfo->paletteMode, pInfo->entries, pColors, 0, 0, 0
        );
    } else if (pInfo->paletteMode == PAL_BITFIELDS) {
        *pPalette = EngCreatePalette(
            pInfo->paletteMode, 0, NULL, pColors[0], pColors[1], pColors[2]
        );
    } else {
        *pPalette = EngCreatePalette(pInfo->paletteMode, 0, NULL, 0, 0, 0);
    }
    if (*pPalette == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetMode: "
            L"Can't create palette (mode: %d, entries: %d)\r\n",
            pInfo->paletteMode, pInfo->entries
        ));
        sc = E_OUTOFMEMORY;
        goto cleanUp;
    }

    g_bitMasks[0] = pColors[0];
    g_bitMasks[1] = pColors[1];
    g_bitMasks[2] = pColors[2];

    RETAILMSG(1, (L"vc4disp: mode %d %dx%d %d bpp stride %d fb 0x%08x (%s)\r\n",
        modeNumber, m_gpeMode.width, m_gpeMode.height, m_gpeMode.Bpp,
        pInfo->frameStride, ulFrame, m_fCachedFb ? L"cached" : L"uncached"
    ));

cleanUp:
    delete [] pBuffer;
    if (FAILED(sc)) {
        RETAILMSG(1, (L"vc4disp: SetMode(%d) failed 0x%08x\r\n",
            modeNumber, sc
        ));
    }
    return sc;
}

//------------------------------------------------------------------------------
//
//  Method:  AllocSurface
//
//  There is no off-screen video memory, so surfaces that need it fail and
//  everything else is allocated in system memory.
//
SCODE Vc4GPE::AllocSurface(
    GPESurf **ppSurf, int width, int height, EGPEFormat format, int flags
) {
    if ((flags & GPE_REQUIRE_VIDEO_MEMORY) != 0) {
        *ppSurf = NULL;
        return E_OUTOFMEMORY;
    }

    *ppSurf = new GPESurf(width, height, format);
    if (*ppSurf == NULL || (*ppSurf)->Buffer() == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::AllocSurface: "
            L"Failed to allocate surface (width: %d, height: %d, format %d)\r\n",
            width, height, format
        ));
        delete *ppSurf; *ppSurf = NULL;
        return E_OUTOFMEMORY;
    }

    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  FlushRect
//
//  Writes the cached framebuffer rows covering pRect (exclusive right/bottom,
//  any order) back to memory, so the display shows them. NULL = whole cache.
//  The OAL cleans the whole 16 KB L1 D-cache instead of walking lines once
//  the range is larger than the cache, so big rectangles cost no more than
//  one full clean.
//
VOID Vc4GPE::FlushRect(const RECTL *pRect)
{
    LONG left, top, right, bottom, stride, bytesPerPixel;
    UCHAR *pStart, *pEnd;

    if (!m_fCachedFb || m_pPrimarySurface == NULL) return;

    if (pRect == NULL) {
        CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);
        return;
    }

    left   = min(pRect->left, pRect->right);
    right  = max(pRect->left, pRect->right);
    top    = min(pRect->top, pRect->bottom);
    bottom = max(pRect->top, pRect->bottom);

    // Clip to the screen
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m_gpeMode.width) right = m_gpeMode.width;
    if (bottom > m_gpeMode.height) bottom = m_gpeMode.height;
    if (left >= right || top >= bottom) return;

    stride = m_pPrimarySurface->Stride();
    bytesPerPixel = (m_gpeMode.Bpp + 7) >> 3;
    if (stride <= 0 || bytesPerPixel <= 0) {
        CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);
        return;
    }

    // One contiguous range from the first touched byte to the last one
    pStart = (UCHAR*)m_pPrimarySurface->Buffer() + top * stride +
        left * bytesPerPixel;
    pEnd = (UCHAR*)m_pPrimarySurface->Buffer() + (bottom - 1) * stride +
        right * bytesPerPixel;
    CacheRangeFlush(pStart, (DWORD)(pEnd - pStart), CACHE_SYNC_WRITEBACK);
}

//------------------------------------------------------------------------------
//
//  Method: WrappedEmulatedLine
//
//  Emulated line on the primary surface: hides the software cursor while the
//  line crosses it and writes the touched rows back (cached framebuffer).
//
SCODE Vc4GPE::WrappedEmulatedLine(GPELineParms *lineParameters)
{
    SCODE sc;
    RECTL bounds;
    int N_plus_1;

    // Bounding rectangle of the line
    if (lineParameters->dN) {
        // The line has a diagonal component
        N_plus_1 = 2 + (
            (lineParameters->cPels * lineParameters->dN)/lineParameters->dM
        );
    } else {
        N_plus_1 = 1;
    }

    switch (lineParameters->iDir) {
    case 0:
        bounds.left = lineParameters->xStart;
        bounds.top = lineParameters->yStart;
        bounds.right = lineParameters->xStart + lineParameters->cPels + 1;
        bounds.bottom = bounds.top + N_plus_1;
        break;
    case 1:
        bounds.left = lineParameters->xStart;
        bounds.top = lineParameters->yStart;
        bounds.bottom = lineParameters->yStart + lineParameters->cPels + 1;
        bounds.right = bounds.left + N_plus_1;
        break;
    case 2:
        bounds.right = lineParameters->xStart + 1;
        bounds.top = lineParameters->yStart;
        bounds.bottom = lineParameters->yStart + lineParameters->cPels + 1;
        bounds.left = bounds.right - N_plus_1;
        break;
    case 3:
        bounds.right = lineParameters->xStart + 1;
        bounds.top = lineParameters->yStart;
        bounds.left = lineParameters->xStart - lineParameters->cPels;
        bounds.bottom = bounds.top + N_plus_1;
        break;
    case 4:
        bounds.right = lineParameters->xStart + 1;
        bounds.bottom = lineParameters->yStart + 1;
        bounds.left = lineParameters->xStart - lineParameters->cPels;
        bounds.top = bounds.bottom - N_plus_1;
        break;
    case 5:
        bounds.right = lineParameters->xStart + 1;
        bounds.bottom = lineParameters->yStart + 1;
        bounds.top = lineParameters->yStart - lineParameters->cPels;
        bounds.left = bounds.right - N_plus_1;
        break;
    case 6:
        bounds.left = lineParameters->xStart;
        bounds.bottom = lineParameters->yStart + 1;
        bounds.top = lineParameters->yStart - lineParameters->cPels;
        bounds.right = bounds.left + N_plus_1;
        break;
    case 7:
        bounds.left = lineParameters->xStart;
        bounds.bottom = lineParameters->yStart + 1;
        bounds.right = lineParameters->xStart + lineParameters->cPels + 1;
        bounds.top = bounds.bottom - N_plus_1;
        break;
    default:
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::WrappedEmulatedLine: "
            L"Invalid direction: %d\r\n", lineParameters->iDir
        ));
        return E_INVALIDARG;
    }

    // If the line overlaps the visible cursor, turn the cursor off
    if (m_cursorVisible && !m_cursorDisabled) {
        RECTL cursorRect = m_cursorRect;
        RotateRectl(&cursorRect);

        if (cursorRect.top < bounds.bottom &&
            cursorRect.bottom > bounds.top &&
            cursorRect.left < bounds.right &&
            cursorRect.right > bounds.left
        ) {
            CursorOff();
            m_cursorForcedOff = TRUE;
        }
    }

    sc = EmulatedLine(lineParameters);

    // Write the line back, one pixel of margin against rounding in bounds
    bounds.left--; bounds.top--; bounds.right++; bounds.bottom++;
    FlushRect(&bounds);

    // If cursor was forced off turn it back on
    if (m_cursorForcedOff) {
        m_cursorForcedOff = FALSE;
        CursorOn();
    }

    return sc;
}

//------------------------------------------------------------------------------
//
//  Method: Line
//
SCODE Vc4GPE::Line(GPELineParms *pLineParms, EGPEPhase phase)
{
    if (phase == gpeSingle || phase == gpePrepare) {
        if (pLineParms->pDst != m_pPrimarySurface) {
            pLineParms->pLine = &GPE::EmulatedLine;
        } else {
            pLineParms->pLine =
                (SCODE (GPE::*)(struct GPELineParms *))&Vc4GPE::WrappedEmulatedLine;
        }
    }
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  BltPrepare
//
//  All blits are emulated. Hides the software cursor when the source or
//  destination rectangle on the primary surface overlaps it.
//
SCODE Vc4GPE::BltPrepare(GPEBltParms *pBltParms)
{
    RECTL rect;
    LONG swapTmp;

    pBltParms->pBlt = &GPE::EmulatedBlt;

    // Check if destination overlaps the cursor
    if (
        pBltParms->pDst == m_pPrimarySurface &&
        m_cursorVisible && !m_cursorDisabled
    ) {
        if (pBltParms->prclDst != NULL) {
            rect = *pBltParms->prclDst;

            // The rectangle may be flipped or mirrored
            if (rect.top > rect.bottom) {
                swapTmp = rect.top;
                rect.top = rect.bottom;
                rect.bottom = swapTmp;
            }
            if (rect.left > rect.right) {
                swapTmp    = rect.left;
                rect.left  = rect.right;
                rect.right = swapTmp;
            }
        } else {
            rect = m_cursorRect;
        }

        if (
            m_cursorRect.top <= rect.bottom &&
            m_cursorRect.bottom >= rect.top &&
            m_cursorRect.left <= rect.right &&
            m_cursorRect.right >= rect.left
        ) {
            CursorOff();
            m_cursorForcedOff = TRUE;
        }
    }

    // Check if source overlaps the cursor
    if (
        pBltParms->pSrc == m_pPrimarySurface &&
        m_cursorVisible && !m_cursorDisabled
    ) {
        if (pBltParms->prclSrc != NULL) {
            rect = *pBltParms->prclSrc;
        } else {
            rect = m_cursorRect;
        }
        if (
            m_cursorRect.top < rect.bottom && m_cursorRect.bottom > rect.top &&
            m_cursorRect.left < rect.right && m_cursorRect.right > rect.left
        ) {
            CursorOff();
            m_cursorForcedOff = TRUE;
        }
    }

    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  BltComplete
//
SCODE Vc4GPE::BltComplete(GPEBltParms *pBltParms)
{
    // Write what the blit drew back to memory (FlushRect orders the
    // rectangle; no destination rectangle means clean the whole cache)
    if (pBltParms->pDst == m_pPrimarySurface) {
        FlushRect(pBltParms->prclDst);
    }

    // If cursor was forced off turn it back on
    if (m_cursorForcedOff) {
        m_cursorForcedOff = FALSE;
        CursorOn();
    }
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  CursorOn
//
//  Saves the pixels under the cursor and draws it (AND then XOR mask).
//
VOID Vc4GPE::CursorOn()
{
    UCHAR *pFrame;
    UCHAR *pFrameLine, *pStoreLine, *pXorLine, *pAndLine, data;
    int bytesPerPixel, bytesPerLine;
    int xf, yf, xc, yc, i;

    // If cursor should not be visible or already is then exit
    if (m_cursorForcedOff || m_cursorDisabled || m_cursorVisible) return;
    if (m_cursorStore == NULL || m_pPrimarySurface == NULL) return;

    // We support only 1,2,3 and 4 bytes per pixel
    bytesPerPixel = (m_gpeMode.Bpp + 7) >> 3;
    if (bytesPerPixel <= 0 || bytesPerPixel > 4) return;

    pFrame = (UCHAR*)m_pPrimarySurface->Buffer();
    bytesPerLine = m_pPrimarySurface->Stride();

    for (yf = m_cursorRect.top, yc = 0; yf < m_cursorRect.bottom; yf++, yc++) {
        if (yf < 0) continue;
        if (yf >= m_gpeMode.height) break;

        pFrameLine = &pFrame[yf * bytesPerLine];
        pStoreLine = &m_cursorStore[yc * m_cursorSize.x * bytesPerPixel];
        pAndLine = &m_cursorAnd[yc * m_cursorSize.x * bytesPerPixel];
        pXorLine = &m_cursorXor[yc * m_cursorSize.x * bytesPerPixel];

        for (
            xf = m_cursorRect.left, xc = 0; xf < m_cursorRect.right; xf++, xc++
        ) {
            if (xf < 0) continue;
            if (xf >= m_gpeMode.width) break;

            switch (bytesPerPixel) {
            case 1:
                pStoreLine[xc] = pFrameLine[xf];
                pFrameLine[xf] &= pAndLine[xc];
                pFrameLine[xf] ^= pXorLine[xc];
                break;
            case 2:
                ((USHORT*)pStoreLine)[xc]  = ((USHORT*)pFrameLine)[xf];
                ((USHORT*)pFrameLine)[xf] &= ((USHORT*)pAndLine)[xc];
                ((USHORT*)pFrameLine)[xf] ^= ((USHORT*)pXorLine)[xc];
                break;
            case 3:
                for (i = 0; i < bytesPerPixel; i++) {
                    data = pFrameLine[xf * bytesPerPixel + i];
                    pStoreLine[xc * bytesPerPixel + i] = data;
                    data &= pAndLine[xc * bytesPerPixel + i];
                    data ^= pXorLine[xc * bytesPerPixel + i];
                    pFrameLine[xf * bytesPerPixel + i] = data;
                }
                break;
            case 4:
                ((ULONG*)pStoreLine)[xc]  = ((ULONG*)pFrameLine)[xf];
                ((ULONG*)pFrameLine)[xf] &= ((ULONG*)pAndLine)[xc];
                ((ULONG*)pFrameLine)[xf] ^= ((ULONG*)pXorLine)[xc];
                break;
            }
        }
    }

    FlushRect(&m_cursorRect);
    m_cursorVisible = TRUE;
}

//------------------------------------------------------------------------------
//
//  Method:  CursorOff
//
//  Restores the pixels saved by CursorOn.
//
VOID Vc4GPE::CursorOff()
{
    UCHAR *pFrame, *pFrameLine, *pStoreLine, data;
    int bytesPerPixel, bytesPerLine;
    int xf, yf, xc, yc, i;

    if (m_cursorForcedOff || m_cursorDisabled || !m_cursorVisible) return;
    if (m_cursorStore == NULL || m_pPrimarySurface == NULL) return;

    // We support only 1,2,3 and 4 bytes per pixel
    bytesPerPixel = (m_gpeMode.Bpp + 7) >> 3;
    if (bytesPerPixel <= 0 || bytesPerPixel > 4) return;

    pFrame = (UCHAR*)m_pPrimarySurface->Buffer();
    bytesPerLine = m_pPrimarySurface->Stride();

    for (yf = m_cursorRect.top, yc = 0; yf < m_cursorRect.bottom; yf++, yc++) {
        if (yf < 0) continue;
        if (yf >= m_gpeMode.height) break;

        pFrameLine = &pFrame[yf * bytesPerLine];
        pStoreLine = &m_cursorStore[yc * m_cursorSize.x * bytesPerPixel];

        for (
            xf = m_cursorRect.left, xc = 0; xf < m_cursorRect.right; xf++, xc++
        ) {
            if (xf < 0) continue;
            if (xf >= m_gpeMode.width) break;

            switch (bytesPerPixel) {
            case 1:
                pFrameLine[xf] = pStoreLine[xc];
                break;
            case 2:
                ((USHORT*)pFrameLine)[xf] = ((USHORT*)pStoreLine)[xc];
                break;
            case 3:
                for (i = 0; i < bytesPerPixel; i++) {
                    data = pStoreLine[xc * bytesPerPixel + i];
                    pFrameLine[xf * bytesPerPixel + i] = data;
                }
                break;
            case 4:
                ((ULONG*)pFrameLine)[xf] = ((ULONG*)pStoreLine)[xc];
                break;
            }
        }
    }

    FlushRect(&m_cursorRect);
    m_cursorVisible = FALSE;
}

//------------------------------------------------------------------------------
//
//  Method:  SetPointerShape
//
//  Sets the shape and hot spot of the pointer (monochrome AND/XOR masks).
//
SCODE Vc4GPE::SetPointerShape(
    GPESurf* pMask, GPESurf* pColorSurf, int xHotspot, int yHotspot,
    int xSize, int ySize
) {
    SCODE sc = S_OK;
    UCHAR *pAndPtr, *pXorPtr, *pAndLine, *pXorLine;
    UCHAR andPtr, xorPtr, mask;
    ULONG size;
    int bytesPerPixel;
    int row, col, i;

    CursorOff();

    delete [] m_cursorStore; m_cursorStore = NULL;
    delete [] m_cursorXor; m_cursorXor = NULL;
    delete [] m_cursorAnd; m_cursorAnd = NULL;

    if (pMask == NULL) {
        m_cursorDisabled = TRUE;
        goto cleanUp;
    }

    m_cursorDisabled = FALSE;

    if (xSize > m_nScreenWidth || ySize > m_nScreenHeight) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetPointerShape: "
            L"Invalid cursor size %d, %d\r\n", xSize, ySize
        ));
        m_cursorDisabled = TRUE;
        sc = E_FAIL;
        goto cleanUp;
    }

    bytesPerPixel = (m_gpeMode.Bpp + 7) >> 3;
    size = xSize * ySize * bytesPerPixel;

    m_cursorStore = new UCHAR[size];
    m_cursorXor   = new UCHAR[size];
    m_cursorAnd   = new UCHAR[size];
    if (m_cursorStore == NULL || m_cursorXor == NULL || m_cursorAnd == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetPointerShape: "
            L"Memory allocation for cursor buffers failed\r\n"
        ));
        delete [] m_cursorStore; m_cursorStore = NULL;
        delete [] m_cursorXor; m_cursorXor = NULL;
        delete [] m_cursorAnd; m_cursorAnd = NULL;
        m_cursorDisabled = TRUE;
        sc = E_OUTOFMEMORY;
        goto cleanUp;
    }

    m_cursorSize.x = xSize;
    m_cursorSize.y = ySize;
    m_cursorHotspot.x = xHotspot;
    m_cursorHotspot.y = yHotspot;

    // The mask surface holds the AND mask followed by the XOR mask
    pAndPtr = (UCHAR*)pMask->Buffer();
    pXorPtr = (UCHAR*)pMask->Buffer() + (ySize * pMask->Stride());

    // Expand the 1 bpp masks to the screen depth
    for (row = 0; row < ySize; row++) {
        pAndLine = &m_cursorAnd[row * xSize * bytesPerPixel];
        pXorLine = &m_cursorXor[row * xSize * bytesPerPixel];
        for (col = 0; col < xSize; col++) {
            andPtr = pAndPtr[row * pMask->Stride() + (col >> 3)];
            xorPtr = pXorPtr[row * pMask->Stride() + (col >> 3)];
            mask = 0x80 >> (col & 0x7);
            for (i = 0; i < bytesPerPixel; i++) {
                pAndLine[col * bytesPerPixel + i] = andPtr&mask?0xFF:0x00;
                pXorLine[col * bytesPerPixel + i] = xorPtr&mask?0xFF:0x00;
            }
        }
    }

cleanUp:
    return sc;
}

//------------------------------------------------------------------------------
//
//  Method:  MovePointer
//
//  Moves the cursor hot spot to (x, y), or hides the cursor (x == -1).
//
SCODE Vc4GPE::MovePointer(int x, int y)
{
    CursorOff();

    if (x != -1 || y != -1) {
        m_cursorRect.left = x - m_cursorHotspot.x;
        m_cursorRect.right = m_cursorRect.left + m_cursorSize.x;
        m_cursorRect.top = y - m_cursorHotspot.y;
        m_cursorRect.bottom = m_cursorRect.top + m_cursorSize.y;
        CursorOn();
    }

    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  InVBlank
//
//  No vertical blank information yet.
//
int Vc4GPE::InVBlank()
{
    return 1;
}

//------------------------------------------------------------------------------
//
//  Method:  SetPalette
//
SCODE Vc4GPE::SetPalette(
    const PALETTEENTRY *pSource, WORD firstEntry, WORD numEntries
) {
    SCODE sc = S_OK;
    UINT8 *pBuffer;
    ULONG command, size, paletteSize;
    DDHAL_SET_PALETTE *pInfo;

    paletteSize = numEntries * sizeof(PALETTEENTRY);
    size = sizeof(DDHAL_SET_PALETTE) + paletteSize;
    pBuffer = new UINT8[size];
    if (pBuffer == NULL) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetPalette: "
            L"Memory allocation for support buffer failed\r\n"
        ));
        return E_OUTOFMEMORY;
    }

    pInfo = (DDHAL_SET_PALETTE *)pBuffer;
    pInfo->firstEntry = firstEntry;
    pInfo->entries = numEntries;
    memcpy(&pInfo[1], pSource, paletteSize);

    command = DDHAL_COMMAND_SET_PALETTE;
    if (!KernelIoControl(
        IOCTL_HAL_DDI, &command, sizeof(command), pBuffer, size, NULL
    )) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::SetPalette: "
            L"IOCTL_HAL_DDI!SET_PALETTE failed\r\n"
        ));
        sc = E_FAIL;
    }

    delete [] pBuffer;
    return sc;
}

//------------------------------------------------------------------------------
//
//  Method:  PowerHandler
//
//  The OAL's DDHAL_COMMAND_POWER handler is still a stub.
//
VOID Vc4GPE::PowerHandler(BOOL off)
{
    ULONG commands[2];

    commands[0] = DDHAL_COMMAND_POWER;
    commands[1] = off;
    if (!KernelIoControl(
        IOCTL_HAL_DDI, commands, sizeof(commands), NULL, 0, NULL
    )) {
        DEBUGMSG(GPE_ZONE_ERROR, (L"Vc4GPE::PowerHandler: "
            L"IOCTL_HAL_DDI!POWER failed for off flag %d\r\n", off
        ));
    }
}

//------------------------------------------------------------------------------

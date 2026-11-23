//
// vc4blt.cpp - vc4disp's own software blits (CEonPi)
//
// GPE's emulation (gpe.lib) has fast routines for most 16 bpp blits once
// FastBlt is on, but three common cases still fall to its generic
// pixel-by-pixel path (about 2 Mpix/s, measured with gdibench):
//
//   32 bpp -> RGB565 SRCCOPY  (32 bpp DIBs without an alpha mask)
//   24 bpp -> RGB565 SRCCOPY  (24 bpp DIBs: images, IE page content)
//   DSTINVERT on 16 bpp       (selection highlights, caret, focus)
//
// SelectDriverBlt (called at the end of BltPrepare) points pBlt at one of
// the routines below when the blit qualifies. Like gpe.lib's routines they
// work on the clipped prclDst / prclSrc GPE passes for each clip rectangle.
//
// The 24/32 bpp routines assume the source pixel is 0x00RRGGBB (byte order
// B, G, R in memory, as in standard DIBs) and the destination is RGB565.
// Before using them, ConverterIs888To565 checks that GPE's own colour
// converter for this blit maps a set of probe colours exactly that way, so
// any other layout (BGR palettes, 555 destinations, ...) keeps the generic
// path. Conversion truncates each channel, as gpe.lib's
// Blt_NoStretch_NoTrans_32To16at565 does.
//
// Registry value DriverBlt=0 turns these routines off (for comparison).
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include "vc4disp.h"

//------------------------------------------------------------------------------
// Pixel conversions

static __inline WORD Px32To565(DWORD p)
{
    return (WORD)(((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F));
}

static __inline WORD Px24To565(const BYTE *p)
{
    // p[0] = blue, p[1] = green, p[2] = red
    return (WORD)(((p[2] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[0] >> 3));
}

//------------------------------------------------------------------------------
//
//  Function:  ConverterIs888To565
//
//  TRUE if the blit's colour converter maps 0x00RRGGBB to RGB565 by
//  truncation, checked on probe colours that pin down each channel's
//  position and width.
//
static BOOL ConverterIs888To565(GPEBltParms *pParms)
{
    static const ULONG s_in[] = {
        0x00FF0000, 0x0000FF00, 0x000000FF, 0x00800000,
        0x00008000, 0x00000080, 0x00FFFFFF, 0x00000000
    };
    static const ULONG s_out[] = {
        0xF800, 0x07E0, 0x001F, 0x8000,
        0x0400, 0x0010, 0xFFFF, 0x0000
    };
    int i;

    if (pParms->pColorConverter == NULL || pParms->pConvert == NULL) {
        return FALSE;
    }
    for (i = 0; i < (int)(sizeof(s_in) / sizeof(s_in[0])); i++) {
        if ((pParms->pColorConverter->*(pParms->pConvert))(s_in[i]) != s_out[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

//------------------------------------------------------------------------------
//
//  Function:  IsOrdered
//
static __inline BOOL IsOrdered(const RECTL *pRect)
{
    return pRect != NULL && pRect->left <= pRect->right &&
        pRect->top <= pRect->bottom;
}

//------------------------------------------------------------------------------
//
//  Method:  SelectDriverBlt
//
//  Called at the end of BltPrepare: replaces pBlt with one of our routines
//  when the blit is one they handle.
//
VOID Vc4GPE::SelectDriverBlt(GPEBltParms *pParms)
{
    // Large screen-to-screen copies and screen fills go to the DMA engine
    if (DmaSelect(pParms)) return;

    if (!m_fDriverBlt) return;

    // Common conditions: plain (no stretch, transparency, alpha or mask)
    // blit to an unrotated 16 bpp surface
    if ((pParms->bltFlags & (BLT_TRANSPARENT | BLT_STRETCH | BLT_ALPHABLEND)) ||
        pParms->pMask != NULL ||
        pParms->pDst == NULL ||
        pParms->pDst->Format() != gpe16Bpp ||
        pParms->pDst->IsRotate() ||
        !IsOrdered(pParms->prclDst)
    ) {
        return;
    }

    if (pParms->rop4 == 0x5555) {
        // DSTINVERT: no source, no brush
        pParms->pBlt = (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltInvert16;
        return;
    }

    if (pParms->rop4 == 0xCCCC &&
        pParms->pSrc != NULL &&
        pParms->pSrc != pParms->pDst &&
        !pParms->pSrc->IsRotate() &&
        pParms->pBrush == NULL &&
        pParms->pLookup == NULL &&
        IsOrdered(pParms->prclSrc) &&
        (pParms->pSrc->Format() == gpe32Bpp ||
         pParms->pSrc->Format() == gpe24Bpp) &&
        ConverterIs888To565(pParms)
    ) {
        if (pParms->pSrc->Format() == gpe32Bpp) {
            pParms->pBlt =
                (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltCopy32To565;
        } else {
            pParms->pBlt =
                (SCODE (GPE::*)(GPEBltParms *))&Vc4GPE::BltCopy24To565;
        }
    }
}

//------------------------------------------------------------------------------
//
//  Method:  BltCopy32To565
//
//  SRCCOPY from 0x00RRGGBB (alpha byte ignored) to RGB565. Writes two
//  pixels per 32-bit store where the destination is aligned.
//
SCODE Vc4GPE::BltCopy32To565(GPEBltParms *pParms)
{
    const RECTL *prcDst = pParms->prclDst;
    const RECTL *prcSrc = pParms->prclSrc;
    int width = prcDst->right - prcDst->left;
    int height = prcDst->bottom - prcDst->top;
    int dstStride = pParms->pDst->Stride();
    int srcStride = pParms->pSrc->Stride();
    BYTE *pDstRow = (BYTE*)pParms->pDst->Buffer() + prcDst->top * dstStride +
        prcDst->left * 2;
    const BYTE *pSrcRow = (const BYTE*)pParms->pSrc->Buffer() +
        prcSrc->top * srcStride + prcSrc->left * 4;
    int x, y;

    for (y = 0; y < height; y++) {
        WORD *pD = (WORD*)pDstRow;
        const DWORD *pS = (const DWORD*)pSrcRow;

        x = 0;
        if (((ULONG)pD & 2) && x < width) {
            *pD++ = Px32To565(*pS++);
            x++;
        }
        for (; x + 1 < width; x += 2) {
            *(DWORD*)pD = (DWORD)Px32To565(pS[0]) | ((DWORD)Px32To565(pS[1]) << 16);
            pD += 2;
            pS += 2;
        }
        if (x < width) {
            *pD = Px32To565(*pS);
        }

        pDstRow += dstStride;
        pSrcRow += srcStride;
    }
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  BltCopy24To565
//
//  SRCCOPY from 24 bpp (bytes B, G, R) to RGB565.
//
SCODE Vc4GPE::BltCopy24To565(GPEBltParms *pParms)
{
    const RECTL *prcDst = pParms->prclDst;
    const RECTL *prcSrc = pParms->prclSrc;
    int width = prcDst->right - prcDst->left;
    int height = prcDst->bottom - prcDst->top;
    int dstStride = pParms->pDst->Stride();
    int srcStride = pParms->pSrc->Stride();
    BYTE *pDstRow = (BYTE*)pParms->pDst->Buffer() + prcDst->top * dstStride +
        prcDst->left * 2;
    const BYTE *pSrcRow = (const BYTE*)pParms->pSrc->Buffer() +
        prcSrc->top * srcStride + prcSrc->left * 3;
    int x, y;

    for (y = 0; y < height; y++) {
        WORD *pD = (WORD*)pDstRow;
        const BYTE *pS = pSrcRow;

        x = 0;
        if (((ULONG)pD & 2) && x < width) {
            *pD++ = Px24To565(pS);
            pS += 3;
            x++;
        }
        for (; x + 1 < width; x += 2) {
            *(DWORD*)pD = (DWORD)Px24To565(pS) | ((DWORD)Px24To565(pS + 3) << 16);
            pD += 2;
            pS += 6;
        }
        if (x < width) {
            *pD = Px24To565(pS);
        }

        pDstRow += dstStride;
        pSrcRow += srcStride;
    }
    return S_OK;
}

//------------------------------------------------------------------------------
//
//  Method:  BltInvert16
//
//  DSTINVERT on a 16 bpp surface: every bit of every pixel flips, whatever
//  the colour masks are.
//
SCODE Vc4GPE::BltInvert16(GPEBltParms *pParms)
{
    const RECTL *prcDst = pParms->prclDst;
    int width = prcDst->right - prcDst->left;
    int height = prcDst->bottom - prcDst->top;
    int dstStride = pParms->pDst->Stride();
    BYTE *pDstRow = (BYTE*)pParms->pDst->Buffer() + prcDst->top * dstStride +
        prcDst->left * 2;
    int x, y;

    for (y = 0; y < height; y++) {
        WORD *pD = (WORD*)pDstRow;

        x = 0;
        if (((ULONG)pD & 2) && x < width) {
            *pD++ ^= 0xFFFF;
            x++;
        }
        for (; x + 1 < width; x += 2) {
            *(DWORD*)pD ^= 0xFFFFFFFF;
            pD += 2;
        }
        if (x < width) {
            *pD ^= 0xFFFF;
        }

        pDstRow += dstStride;
    }
    return S_OK;
}

//
// Copyright (c) CEonPi contributors.
//
// bootsplash.cpp - Boot splash screen drawn by the OAL into the VideoCore
// frame buffer until the display driver takes over (ddi.c SetMode ->
// VidCON_DisableDebugSerial -> BootSplash_Stop).
//
// Normal mode: black screen, logo centred, an XP style "marquee" loading bar
// centred below it. The fill block slides through the bar's inner area,
// driven by BootSplash_Tick from the scheduler timer ISR (intr.c).
// Log mode (boot log shown on screen): logo in the top-left corner only, the
// text console scrolls below it. No bar, no animation.
//
// The images are FILES entries in platform.bib (flag U, uncompressed), found
// through the ROM table of contents with BinFindFileData:
//   splash.bmp  logo, 8 or 24 bpp
//   ldbar.bmp   empty bar with a SPLASH_BAR_MARGIN pixel border, 8 or 24 bpp
//   ldfill.bmp  moving block, 8 or 24 bpp
//
// File data in the image is not aligned, so all BMP header fields are read
// byte by byte.
//

#include <windows.h>
#include <nkintr.h>
#include <pehdr.h>
#include <romldr.h>
#include "binfilesystem.h"
#include <image_cfg.h>
#include "Video.h"
#include "bootsplash.h"
#include "Debugfuncs.h"

extern "C" ROMHDR * volatile const pTOC;

#define SPLASH_LOGO_FILE    "splash.bmp"
#define SPLASH_LOGO_LOG_FILE "splashsm.bmp"    // log mode: smaller (less tall) logo
#define SPLASH_BAR_FILE     "ldbar.bmp"
#define SPLASH_FILL_FILE    "ldfill.bmp"

#define SPLASH_BAR_MARGIN   3       // bar border on every side, in pixels
#define SPLASH_BAR_GAP      32      // logo bottom -> bar top, normal mode
#define SPLASH_TEXT_GAP     8       // logo bottom -> console top, log mode

#define SPLASH_STEP_MS      25      // marquee step period
#define SPLASH_STEP_PX      2       // marquee step size

// Pre-converted (frame buffer format) copies of the bar and the fill, so the
// ISR only copies pixels.
#define SPLASH_BAR_MAXW     192
#define SPLASH_BAR_MAXH     24
#define SPLASH_FILL_MAXW    48
#define SPLASH_FILL_MAXH    24

typedef struct {
	int nWidth;
	int nHeight;
	int nBpp;
	BOOL fBottomUp;
	DWORD dwStride;
	const BYTE *pBits;
	const BYTE *pPal;
	DWORD nPalEntries;
} SPLASHBMP, *PSPLASHBMP;

static PSURFINFO s_pSurf = 0;

static DWORD s_aBar[SPLASH_BAR_MAXH][SPLASH_BAR_MAXW];
static DWORD s_aFill[SPLASH_FILL_MAXH][SPLASH_FILL_MAXW];
static int s_nBarX, s_nBarY;            // bar position on screen
static int s_nInnerW, s_nInnerH;        // bar inner area size
static int s_nFillW, s_nFillH;
static int s_nFillTop;                  // fill row offset inside the inner area
static int s_nPos;                      // fill left edge, inner-area columns
static DWORD s_dwLastMs;
static BOOL s_fNeedTime;
static volatile BOOL s_fAnim = FALSE;

//----------------------------------------------------------------------------------
// Unaligned little-endian reads
//
static DWORD ReadLE16(const BYTE *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8);
}

static DWORD ReadLE32(const BYTE *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

//----------------------------------------------------------------------------------
// LoadBmp - Finds a BMP in the image and validates it. 8 bpp (palette) and
// 24 bpp, uncompressed, bottom-up or top-down.
//
static int LoadBmp(const char *szName, PSPLASHBMP pBmp)
{
	const unsigned char *pFile;
	DWORD dwSize;
	const BYTE *pInfo;
	DWORD dwOffBits, dwInfoSize, dwBpp, dwComp, dwClrUsed;
	LONG lWidth, lHeight;

	if (BinFindFileData((DWORD)pTOC, (char *)szName, &pFile, &dwSize) != 0)
	{
		Debug (TEXT("BootSplash: %S not found in the image\r\n"), szName);
		return -1;
	}
	if ((dwSize < 54) || (pFile[0] != 'B') || (pFile[1] != 'M'))
	{
		Debug (TEXT("BootSplash: %S is not a BMP file\r\n"), szName);
		return -1;
	}

	dwOffBits  = ReadLE32(pFile + 10);
	pInfo      = pFile + 14;
	dwInfoSize = ReadLE32(pInfo + 0);
	lWidth     = (LONG)ReadLE32(pInfo + 4);
	lHeight    = (LONG)ReadLE32(pInfo + 8);
	dwBpp      = ReadLE16(pInfo + 14);
	dwComp     = ReadLE32(pInfo + 16);
	dwClrUsed  = ReadLE32(pInfo + 32);

	if ((dwComp != 0) || ((dwBpp != 8) && (dwBpp != 24)) || (lWidth <= 0) || (lHeight == 0) ||
		(lWidth > 4096) || (lHeight > 4096) || (lHeight < -4096))
	{
		Debug (TEXT("BootSplash: %S unsupported (%dx%d, %d bpp, compression %d)\r\n"),
		       szName, lWidth, lHeight, dwBpp, dwComp);
		return -1;
	}

	pBmp->nWidth = (int)lWidth;
	pBmp->fBottomUp = (lHeight > 0);
	pBmp->nHeight = (int)((lHeight > 0) ? lHeight : -lHeight);
	pBmp->nBpp = (int)dwBpp;
	pBmp->dwStride = ((pBmp->nWidth * dwBpp + 31) / 32) * 4;
	pBmp->pBits = pFile + dwOffBits;
	pBmp->pPal = pInfo + dwInfoSize;
	pBmp->nPalEntries = 0;

	if (dwOffBits + (pBmp->dwStride * pBmp->nHeight) > dwSize)
	{
		Debug (TEXT("BootSplash: %S is truncated\r\n"), szName);
		return -1;
	}

	if (dwBpp == 8)
	{
		pBmp->nPalEntries = (dwClrUsed != 0) ? dwClrUsed : 256;
		if ((pBmp->nPalEntries > 256) ||
			(pBmp->pPal + (pBmp->nPalEntries * 4) > pFile + dwOffBits))
		{
			Debug (TEXT("BootSplash: %S has a bad palette\r\n"), szName);
			return -1;
		}
	}
	return 0;
}

//----------------------------------------------------------------------------------
// BmpPixel - Returns the pixel at (x, y), y = 0 at the top, in the frame
// buffer's native format.
//
static DWORD NativeColor(BYTE r, BYTE g, BYTE b)
{
	if (s_pSurf->nBitsPerPixel == 16)
	{
		// RGB565, red in the high bits (GetVideoSettings masks)
		return (((DWORD)r >> 3) << 11) | (((DWORD)g >> 2) << 5) | ((DWORD)b >> 3);
	}
	// 24 bpp: byte 0 = r, byte 1 = g, byte 2 = b (VidXBlt_i order; unverified)
	return (DWORD)r | ((DWORD)g << 8) | ((DWORD)b << 16);
}

static DWORD BmpPixel(PSPLASHBMP pBmp, int x, int y)
{
	const BYTE *pRow;
	const BYTE *p;
	DWORD dwIdx;
	int nRow = pBmp->fBottomUp ? (pBmp->nHeight - 1 - y) : y;

	pRow = pBmp->pBits + ((DWORD)nRow * pBmp->dwStride);
	if (pBmp->nBpp == 24)
	{
		p = pRow + (x * 3);
		return NativeColor(p[2], p[1], p[0]);
	}

	dwIdx = pRow[x];
	if (dwIdx >= pBmp->nPalEntries)
		dwIdx = 0;
	p = pBmp->pPal + (dwIdx * 4);       // RGBQUAD: blue, green, red, reserved
	return NativeColor(p[2], p[1], p[0]);
}

//----------------------------------------------------------------------------------
// PutPixel - Writes one native pixel to the frame buffer, clipped.
//
static void PutPixel(int x, int y, DWORD dwColor)
{
	PBYTE p;

	if ((x < 0) || (y < 0) || (x >= s_pSurf->nWidth) || (y >= s_pSurf->nHeight))
		return;

	if (s_pSurf->nBitsPerPixel == 16)
	{
		p = s_pSurf->pBuffer + ((DWORD)y * s_pSurf->dwStride) + (x * 2);
		*(WORD *)p = (WORD)dwColor;
	}
	else if (s_pSurf->nBitsPerPixel == 24)
	{
		p = s_pSurf->pBuffer + ((DWORD)y * s_pSurf->dwStride) + (x * 3);
		p[0] = (BYTE)dwColor;
		p[1] = (BYTE)(dwColor >> 8);
		p[2] = (BYTE)(dwColor >> 16);
	}
}

//----------------------------------------------------------------------------------
// DrawBmp - Draws a whole BMP at (nX, nY).
//
static void DrawBmp(PSPLASHBMP pBmp, int nX, int nY)
{
	int x, y;

	for (y = 0; y < pBmp->nHeight; y++)
		for (x = 0; x < pBmp->nWidth; x++)
			PutPixel(nX + x, nY + y, BmpPixel(pBmp, x, y));
}

//----------------------------------------------------------------------------------
// DrawInnerCols - Redraws inner-area columns [c0, c1) of the bar: fill pixels
// where the fill is, bar pixels elsewhere.
//
static void DrawInnerCols(int c0, int c1)
{
	int row, col, fc, fr;
	DWORD dwColor;

	if (c0 < 0)
		c0 = 0;
	if (c1 > s_nInnerW)
		c1 = s_nInnerW;

	for (row = 0; row < s_nInnerH; row++)
	{
		fr = row - s_nFillTop;
		for (col = c0; col < c1; col++)
		{
			fc = col - s_nPos;
			if ((fc >= 0) && (fc < s_nFillW) && (fr >= 0) && (fr < s_nFillH))
				dwColor = s_aFill[fr][fc];
			else
				dwColor = s_aBar[row + SPLASH_BAR_MARGIN][col + SPLASH_BAR_MARGIN];
			PutPixel(s_nBarX + SPLASH_BAR_MARGIN + col, s_nBarY + SPLASH_BAR_MARGIN + row, dwColor);
		}
	}
}

//----------------------------------------------------------------------------------
// InitBar - Loads and pre-converts the bar and the fill, draws the empty bar.
// nX/nY: bar position. Returns 0 if the animation can run.
//
static int InitBar(PSPLASHBMP pBar, PSPLASHBMP pFill, int nX, int nY)
{
	int x, y;

	if ((pBar->nWidth > SPLASH_BAR_MAXW) || (pBar->nHeight > SPLASH_BAR_MAXH) ||
		(pFill->nWidth > SPLASH_FILL_MAXW) || (pFill->nHeight > SPLASH_FILL_MAXH))
	{
		Debug (TEXT("BootSplash: bar %dx%d or fill %dx%d too large\r\n"),
		       pBar->nWidth, pBar->nHeight, pFill->nWidth, pFill->nHeight);
		return -1;
	}

	s_nInnerW = pBar->nWidth - (2 * SPLASH_BAR_MARGIN);
	s_nInnerH = pBar->nHeight - (2 * SPLASH_BAR_MARGIN);
	if ((s_nInnerW < pFill->nWidth) || (s_nInnerH < pFill->nHeight))
	{
		Debug (TEXT("BootSplash: fill %dx%d does not fit the bar\r\n"), pFill->nWidth, pFill->nHeight);
		return -1;
	}

	for (y = 0; y < pBar->nHeight; y++)
		for (x = 0; x < pBar->nWidth; x++)
			s_aBar[y][x] = BmpPixel(pBar, x, y);

	for (y = 0; y < pFill->nHeight; y++)
		for (x = 0; x < pFill->nWidth; x++)
			s_aFill[y][x] = BmpPixel(pFill, x, y);

	s_nBarX = nX;
	s_nBarY = nY;
	s_nFillW = pFill->nWidth;
	s_nFillH = pFill->nHeight;
	s_nFillTop = (s_nInnerH - s_nFillH) / 2;
	s_nPos = -s_nFillW;                 // starts just left of the inner area

	// Empty bar
	for (y = 0; y < pBar->nHeight; y++)
		for (x = 0; x < pBar->nWidth; x++)
			PutPixel(nX + x, nY + y, s_aBar[y][x]);

	return 0;
}

//----------------------------------------------------------------------------------
// BootSplash_Init
//
extern "C" int BootSplash_Init(DWORD hFrame, BOOL fLogMode, DWORD *pdwTextTop)
{
	SPLASHBMP bmLogo, bmBar, bmFill;
	BOOL fLogo, fBar;
	int nW, nH, nLogoX, nLogoY, nTotalH, nBarX, nBarY;

	s_fAnim = FALSE;
	if (pdwTextTop)
		*pdwTextTop = 0;

	s_pSurf = (PSURFINFO)hFrame;
	if ((s_pSurf == 0) || (s_pSurf->pBuffer == 0) ||
		((s_pSurf->nBitsPerPixel != 16) && (s_pSurf->nBitsPerPixel != 24)))
	{
		Debug (TEXT("BootSplash: no usable frame buffer\r\n"));
		return -1;
	}
	nW = s_pSurf->nWidth;
	nH = s_pSurf->nHeight;

	// Log mode uses the smaller logo, so more rows are left for text.
	fLogo = (LoadBmp(fLogMode ? SPLASH_LOGO_LOG_FILE : SPLASH_LOGO_FILE, &bmLogo) == 0);

	if (fLogMode)
	{
		// Logo in the top-left corner, console below it.
		if (!fLogo)
			return -1;
		if (bmLogo.nHeight + SPLASH_TEXT_GAP + 16 > nH)
			return -1;                  // no room left for text
		DrawBmp(&bmLogo, 0, 0);
		if (pdwTextTop)
			*pdwTextTop = bmLogo.nHeight + SPLASH_TEXT_GAP;
		return 0;
	}

	fBar = (LoadBmp(SPLASH_BAR_FILE, &bmBar) == 0) && (LoadBmp(SPLASH_FILL_FILE, &bmFill) == 0);

	// Centre logo + gap + bar as one block.
	nTotalH = 0;
	if (fLogo)
		nTotalH += bmLogo.nHeight;
	if (fBar)
		nTotalH += (fLogo ? SPLASH_BAR_GAP : 0) + bmBar.nHeight;

	nLogoY = (nH - nTotalH) / 2;
	if (nLogoY < 0)
		nLogoY = 0;

	nBarY = nLogoY;
	if (fLogo)
	{
		nLogoX = (nW - bmLogo.nWidth) / 2;
		if (nLogoX < 0)
			nLogoX = 0;
		DrawBmp(&bmLogo, nLogoX, nLogoY);
		nBarY = nLogoY + bmLogo.nHeight + SPLASH_BAR_GAP;
	}

	if (fBar)
	{
		nBarX = (nW - bmBar.nWidth) / 2;
		if (nBarX < 0)
			nBarX = 0;
		if (InitBar(&bmBar, &bmFill, nBarX, nBarY) == 0)
		{
			s_fNeedTime = TRUE;
			s_fAnim = TRUE;
		}
	}

	Debug (TEXT("BootSplash: %dx%d, logo %d, bar %d\r\n"), nW, nH, fLogo, (int)s_fAnim);
	return (fLogo || s_fAnim) ? 0 : -1;
}

//----------------------------------------------------------------------------------
// BootSplash_Tick - Runs in the timer ISR. No logging here.
//
extern "C" void BootSplash_Tick(DWORD dwMSec)
{
	int nOld, nNew;

	if (!s_fAnim)
		return;

	if (s_fNeedTime)
	{
		s_dwLastMs = dwMSec;
		s_fNeedTime = FALSE;
		return;
	}
	if ((DWORD)(dwMSec - s_dwLastMs) < SPLASH_STEP_MS)
		return;
	s_dwLastMs = dwMSec;

	nOld = s_nPos;
	nNew = nOld + SPLASH_STEP_PX;
	if (nNew >= s_nInnerW)
	{
		// Left the bar on the right: wrap to just left of the inner area.
		nNew = -s_nFillW;
		s_nPos = nNew;
		DrawInnerCols(nOld, nOld + s_nFillW);
	}
	else
	{
		s_nPos = nNew;
		DrawInnerCols(nOld, nNew + s_nFillW);
	}
}

//----------------------------------------------------------------------------------
// BootSplash_Stop
//
extern "C" void BootSplash_Stop(void)
{
	s_fAnim = FALSE;
}

//
// Copyright (c) CEonPi contributors.
//
// bootsplash.h - Boot splash screen (logo + marquee loading bar) drawn by the
// OAL into the VideoCore framebuffer until the display driver takes over.
// Implementation: src\Common\VidConDebugOut\bootsplash.cpp (VidConDebug.lib).
//

#ifndef __BOOTSPLASH_H
#define __BOOTSPLASH_H

#if __cplusplus
extern "C" {
#endif

//----------------------------------------------------------------------------------
// BootSplash_Init - Draws the splash on the frame buffer surface hFrame (an
// HSURF from InitVideoSystem, passed as a DWORD).
//   fLogMode = FALSE: logo centred, loading bar below it, bar animation armed.
//   fLogMode = TRUE:  logo in the top-left corner only (no bar, no animation);
//                     *pdwTextTop receives the first pixel row free for the
//                     text console.
// Returns 0 on success. On failure *pdwTextTop (if given) is 0.
//
int BootSplash_Init(DWORD hFrame, BOOL fLogMode, DWORD *pdwTextTop);

//----------------------------------------------------------------------------------
// BootSplash_Tick - Called from the scheduler timer ISR every tick with the
// current millisecond count. Advances the marquee. Does nothing unless the
// animation is running.
//
void BootSplash_Tick(DWORD dwMSec);

//----------------------------------------------------------------------------------
// BootSplash_Stop - Stops the animation (the display driver owns the screen).
//
void BootSplash_Stop(void);

#if __cplusplus
}
#endif

#endif //__BOOTSPLASH_H

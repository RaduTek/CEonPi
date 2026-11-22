//
// pwmhw.h
//
// BCM2835 PWM audio render engine (3.5 mm jack on the Pi 1).
//
// The two PWM channels are fed from their shared FIFO by one DMA channel
// (AUDIO_DMA_CHANNEL, DREQ-paced by the PWM). The DMA loops forever over a
// ring of PWMHW_RING_FRAMES frames, split into PWMHW_BLOCKS blocks with one
// control block each; every finished block raises SYSINTR_AUDIO.
//
// The ring is used as a write-cursor buffer, not as a set of buffers that
// are filled whole: when a block has finished playing, PwmHwAck clears it to
// silence, so anything nobody wrote plays as silence. The owner writes audio
// frames at absolute frame positions between PwmHwReadPos (+ a margin) and
// PwmHwWindowEnd. Positions are absolute frame counts since PwmHwStart
// (DWORD, wrap-safe when compared as differences).
//
// With 16 blocks of 256 frames the ring holds ~93 ms at 44.1 kHz: the IST
// can be that late before the DMA laps into blocks that have not been
// cleared, and an interrupt arrives every ~5.8 ms.
//
// A frame is two DWORDs in FIFO order: channel 1 (PWM0, GPIO40, LEFT) then
// channel 2 (PWM1, GPIO45, RIGHT) - verified by ear on a Pi 1 Model B. Each
// word is a duty value in 0..range-1; range/2 is silence.
//
// Rate (verified on a Pi 1 Model B, spectrum analyser and timing): the PWM
// takes one FIFO word per period, alternating between the channels, so a
// stereo frame lasts two periods: frame rate = clock / (2 * range).
//
#ifndef __PWMHW_H
#define __PWMHW_H

#include <windows.h>

#define PWMHW_BLOCKS        16              // blocks (control blocks) in the ring
#define PWMHW_BLOCK_FRAMES  256             // frames per block (~5.8 ms at 44.1 kHz)
#define PWMHW_BLOCK_WORDS   (PWMHW_BLOCK_FRAMES * 2)
#define PWMHW_RING_FRAMES   (PWMHW_BLOCKS * PWMHW_BLOCK_FRAMES)

// PWM clock = PLLD (500 MHz) / 2 = 250 MHz: range 2834 at 44.1 kHz, ~11.5
// bits (verified on a Pi 1 Model B). PLLD / 1 (500 MHz) does NOT work: tried
// on a Pi 1 Model B, the PWM never drained its FIFO (STA=0x601, FIFO full,
// both channels "transmitting") and the jack buzzed.
#define PWMHW_CLOCK_DIV     2
#define PWMHW_CLOCK_HZ      (500000000 / PWMHW_CLOCK_DIV)
#define PWMHW_DEFAULT_RATE  44100

// Frames kept between the DMA read address and the first frame written:
// the DMA reads a few words ahead into the PWM FIFO.
#define PWMHW_WRITE_MARGIN  64

typedef struct _PWMHW {
    // Mapped registers
    volatile DWORD *pPwm;                   // 0x2020C000
    volatile DWORD *pCm;                    // 0x20101000
    volatile DWORD *pGpio;                  // 0x20200000
    volatile DWORD *pDmaBlock;              // DMA controller block (0x20007000)
    volatile DWORD *pDma;                   // this channel's register block
    volatile DWORD *pDmaEnable;             // global DMA enable register

    // DMA memory (uncached, physically contiguous)
    PBYTE  pDmaMem;
    DWORD  dwDmaMemPa;
    DWORD  dwDmaMemSize;
    DWORD  dwBusOffset;                     // PA -> VideoCore bus address
    DWORD  dwCbBus[PWMHW_BLOCKS];           // bus address of each control block
    DWORD  dwRingBus;                       // bus address of the first frame
    DWORD *pRing;                           // the ring, PWMHW_RING_FRAMES frames

    // Ring state
    UINT   nPlaying;                        // block the DMA is playing
    DWORD  dwPlayed;                        // absolute frame where nPlaying starts
    DWORD  dwLate;                          // acks that found 2+ finished blocks

    DWORD  dwRate;                          // sample rate in Hz
    DWORD  dwRange;                         // PWM range = clock / (2 * rate)
    BOOL   fClockOn;                        // PWM clock started by us
    BOOL   fPwmOn;                          // PWM channels running
    BOOL   fRunning;                        // DMA running
} PWMHW, *PPWMHW;

// Maps the registers, allocates the DMA buffer, starts the PWM clock, routes
// GPIO40/45 to the PWM, and ramps both outputs to mid-level (silence).
BOOL  PwmHwInit(PPWMHW p, DWORD dwBusOffset);

// Stops the DMA, disables the PWM channels and stops the PWM clock, then
// unmaps and frees everything. Also the failure path of PwmHwInit.
void  PwmHwDeinit(PPWMHW p);

// Sets the sample rate (PWM range). Only while stopped.
BOOL  PwmHwSetRate(PPWMHW p, DWORD dwRate);

// Converts a signed 16-bit sample to a PWM word for the current range.
__inline DWORD PwmHwSample(const PWMHW *p, int s)
{
    if (s > 32767)  s = 32767;
    if (s < -32768) s = -32768;
    return (DWORD)(((DWORD)(s + 32768) * p->dwRange) >> 16);
}

// Fills nWords words of a buffer with silence (range/2).
void  PwmHwFillSilence(const PWMHW *p, DWORD *pWords, UINT nWords);

// While stopped: clears the whole ring to silence and sets the positions to
// 0, so frames 0..PWMHW_RING_FRAMES-1 can be written before PwmHwStart.
void  PwmHwResetRing(PPWMHW p);

// Starts the DMA at frame 0 (block 0).
BOOL  PwmHwStart(PPWMHW p);

// Stops the DMA at once and holds both outputs at mid-level.
void  PwmHwStop(PPWMHW p);

// Called from the IST for SYSINTR_AUDIO, before InterruptDone. Clears the
// channel's interrupt, clears every block that has finished playing to
// silence and advances the play position. Returns the number of finished
// blocks (normally 1), or -1 if the DMA is not running or reports an error.
int   PwmHwAck(PPWMHW p);

// Absolute frame the DMA is reading now (dwPlayed while stopped).
DWORD PwmHwReadPos(PPWMHW p);

// First absolute frame that may NOT be written: one ring ahead of the start
// of the playing block (the blocks after it are cleared or written).
__inline DWORD PwmHwWindowEnd(const PWMHW *p)
{
    return p->dwPlayed + PWMHW_RING_FRAMES;
}

// Pointer to the two words of absolute frame dwFrame in the ring. The ring
// is contiguous, so up to PwmHwContiguous(dwFrame) frames follow it.
__inline DWORD *PwmHwFramePtr(const PWMHW *p, DWORD dwFrame)
{
    return p->pRing + 2 * (dwFrame % PWMHW_RING_FRAMES);
}
__inline DWORD PwmHwContiguous(DWORD dwFrame)
{
    return PWMHW_RING_FRAMES - (dwFrame % PWMHW_RING_FRAMES);
}

// Returns the PWM status register and clears its sticky error/gap flags.
DWORD PwmHwReadClearStatus(PPWMHW p);

// Logs the clock, PWM and DMA registers (for bring-up).
void  PwmHwDump(const PWMHW *p, LPCWSTR pszWhen);

#endif // __PWMHW_H

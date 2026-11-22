//
// pwmhw.cpp
//
// BCM2835 PWM audio render engine. See pwmhw.h.
//
// Register values follow the BCM2835 ARM Peripherals document; the clock and
// PWM set-up sequence follows the Circle and FreeBSD drivers (used as
// documentation). Bring-up status: see docs\audio.md.
//
#include <windows.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <image_cfg.h>
#include <BCM2835.h>
#include <bsp_cfg.h>
#include "pwmhw.h"

#define REG(base, off)  ((base)[(off) / 4])

#define GPFSEL4_OFFSET_PWM  0x0010          // GPIO40..49 function select
#define GPIO_FSEL_ALT0      4

#define DMA_REG_CS          0x00
#define DMA_REG_CONBLK_AD   0x04
#define DMA_REG_TI          0x08
#define DMA_REG_SOURCE_AD   0x0C
#define DMA_REG_DEST_AD     0x10
#define DMA_REG_TXFR_LEN    0x14
#define DMA_REG_NEXTCONBK   0x1C
#define DMA_REG_DEBUG       0x20
#define DMA_ENABLE_OFFSET   0x0FF0          // global enable, from the DMA block base

// DMA memory layout: the 32-byte control blocks, then the sample blocks
// back to back (so the ring is contiguous).
#define CB_WORDS            8
#define BLOCK_OFFSET        512
#define BLOCK_BYTES         (PWMHW_BLOCK_WORDS * sizeof(DWORD))
#define DMA_MEM_BYTES       (BLOCK_OFFSET + PWMHW_BLOCKS * BLOCK_BYTES)

#if (PWMHW_BLOCKS * CB_WORDS * 4) > BLOCK_OFFSET
#error control blocks overlap the sample blocks
#endif

#define DMA_MEM_ALLOC       (((DMA_MEM_BYTES) + 4095) & ~4095)

#define BUS_ADDR_MASK       0x3FFFFFFF      // strips the VideoCore cache alias bits

#define RAMP_FRAMES         1024            // 0 -> mid-level ramp at start-up (~23 ms)

static volatile DWORD *MapRegs(DWORD dwPa, DWORD dwLen)
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = dwPa;
    return (volatile DWORD *)MmMapIoSpace(pa, dwLen, FALSE);
}

// Waits up to about dwMs for (reg & mask) == want.
static BOOL WaitReg(volatile DWORD *pReg, DWORD mask, DWORD want, DWORD dwMs)
{
    DWORD dwStart = GetTickCount();
    for (;;) {
        if ((*pReg & mask) == want)
            return TRUE;
        if (GetTickCount() - dwStart > dwMs)
            return FALSE;
        Sleep(1);
    }
}

// Stops the PWM clock gracefully (clear ENAB, keep the source), then waits
// for BUSY to drop. KILL only if it will not stop: it can glitch.
static BOOL ClockStop(PPWMHW p)
{
    volatile DWORD *pCtl = &REG(p->pCm, CM_PWMCTL_OFFSET);

    *pCtl = CM_PASSWORD | (*pCtl & 0x0F);
    if (!WaitReg(pCtl, CM_CTL_BUSY, 0, 20)) {
        RETAILMSG(1, (L"PWMAUD: clock busy, killing it (ctl=0x%08x)\r\n", *pCtl));
        *pCtl = CM_PASSWORD | CM_CTL_KILL;
        if (!WaitReg(pCtl, CM_CTL_BUSY, 0, 20)) {
            RETAILMSG(1, (L"PWMAUD: ERROR clock will not stop (ctl=0x%08x)\r\n", *pCtl));
            return FALSE;
        }
    }
    return TRUE;
}

// Starts the PWM clock at PWMHW_CLOCK_HZ: PLLD (500 MHz) / PWMHW_CLOCK_DIV.
static BOOL ClockStart(PPWMHW p)
{
    volatile DWORD *pCtl = &REG(p->pCm, CM_PWMCTL_OFFSET);
    volatile DWORD *pDiv = &REG(p->pCm, CM_PWMDIV_OFFSET);

    if (!ClockStop(p))
        return FALSE;

    p->fClockOn = TRUE;                     // from here on, Deinit stops it
    *pDiv = CM_PASSWORD | CM_DIV_DIVI(PWMHW_CLOCK_DIV);
    *pCtl = CM_PASSWORD | CM_CTL_SRC_PLLD;
    *pCtl = CM_PASSWORD | CM_CTL_SRC_PLLD | CM_CTL_ENAB;

    if (!WaitReg(pCtl, CM_CTL_BUSY, CM_CTL_BUSY, 20)) {
        RETAILMSG(1, (L"PWMAUD: ERROR clock did not start (ctl=0x%08x div=0x%08x)\r\n",
                      *pCtl, *pDiv));
        return FALSE;
    }
    return TRUE;
}

// Routes GPIO40 (PWM0) and GPIO45 (PWM1) to the PWM block (ALT0).
static void GpioToPwm(PPWMHW p)
{
    DWORD v = REG(p->pGpio, GPFSEL4_OFFSET_PWM);
    v &= ~((7 << 0) | (7 << 15));
    v |= (GPIO_FSEL_ALT0 << 0) | (GPIO_FSEL_ALT0 << 15);
    REG(p->pGpio, GPFSEL4_OFFSET_PWM) = v;
}

// Writes one frame (both channels) to the PWM FIFO by hand, waiting while it
// is full. Used only while the DMA is stopped.
static BOOL FifoWriteFrame(PPWMHW p, DWORD dwRight, DWORD dwLeft)
{
    DWORD n = 0;
    while (REG(p->pPwm, PWM_STA_OFFSET) & PWM_STA_FULL1) {
        if (++n > 100000)
            return FALSE;
    }
    REG(p->pPwm, PWM_FIF1_OFFSET) = dwRight;
    n = 0;
    while (REG(p->pPwm, PWM_STA_OFFSET) & PWM_STA_FULL1) {
        if (++n > 100000)
            return FALSE;
    }
    REG(p->pPwm, PWM_FIF1_OFFSET) = dwLeft;
    return TRUE;
}

static void PwmSetRange(PPWMHW p, DWORD dwRate)
{
    p->dwRate  = dwRate;
    // Two PWM periods per stereo frame (see pwmhw.h): 2834 at 44100 Hz.
    // PwmHwSample stays within 32 bits down to 4000 Hz (65535 * 31250).
    p->dwRange = (PWMHW_CLOCK_HZ + dwRate) / (2 * dwRate);
    REG(p->pPwm, PWM_RNG1_OFFSET) = p->dwRange;
    REG(p->pPwm, PWM_RNG2_OFFSET) = p->dwRange;
}

// Enables both channels in PWM (balanced) mode, fed from the FIFO, repeating
// the last word when the FIFO runs empty, then ramps from 0 to mid-level so
// the speaker sees a slow step instead of a click.
static BOOL PwmStart(PPWMHW p)
{
    UINT i;

    REG(p->pPwm, PWM_DMAC_OFFSET) = 0;
    REG(p->pPwm, PWM_CTL_OFFSET) = 0;
    Sleep(1);
    REG(p->pPwm, PWM_STA_OFFSET) = PWM_STA_ERRORS;
    PwmSetRange(p, PWMHW_DEFAULT_RATE);

    REG(p->pPwm, PWM_CTL_OFFSET) = PWM_CTL_CLRF1;
    Sleep(1);
    REG(p->pPwm, PWM_CTL_OFFSET) =
        PWM_CTL_PWEN1 | PWM_CTL_USEF1 | PWM_CTL_RPTL1 |
        PWM_CTL_PWEN2 | PWM_CTL_USEF2 | PWM_CTL_RPTL2;

    for (i = 0; i < RAMP_FRAMES; i++) {
        DWORD v = (p->dwRange / 2) * i / RAMP_FRAMES;
        if (!FifoWriteFrame(p, v, v)) {
            RETAILMSG(1, (L"PWMAUD: ERROR PWM FIFO stays full (sta=0x%08x) - no PWM clock?\r\n",
                          REG(p->pPwm, PWM_STA_OFFSET)));
            return FALSE;
        }
    }
    FifoWriteFrame(p, p->dwRange / 2, p->dwRange / 2);

    // DREQ to the DMA engine when the FIFO has room.
    REG(p->pPwm, PWM_DMAC_OFFSET) = PWM_DMAC_ENAB | PWM_DMAC_PANIC(7) | PWM_DMAC_DREQ(3);
    return TRUE;
}

// Builds the ring: CB i plays block i and links to CB i+1; the last links
// back to CB 0. Every block raises an interrupt on completion.
static void BuildControlBlocks(PPWMHW p)
{
    int i;
    for (i = 0; i < PWMHW_BLOCKS; i++) {
        DWORD *pCb = (DWORD *)(p->pDmaMem + i * CB_WORDS * sizeof(DWORD));
        pCb[0] = DMA_TI_INTEN | DMA_TI_WAIT_RESP | DMA_TI_DEST_DREQ | DMA_TI_SRC_INC |
                 DMA_TI_PERMAP(DMA_DREQ_PWM);
        pCb[1] = p->dwDmaMemPa + p->dwBusOffset + BLOCK_OFFSET + i * BLOCK_BYTES;
        pCb[2] = PWM_BUS_BASE + PWM_FIF1_OFFSET;
        pCb[3] = BLOCK_BYTES;
        pCb[4] = 0;
        pCb[5] = p->dwCbBus[(i + 1) % PWMHW_BLOCKS];
        pCb[6] = 0;
        pCb[7] = 0;
    }
}

BOOL PwmHwInit(PPWMHW p, DWORD dwBusOffset)
{
    DWORD dwDmaBase = DMA_CONTROLLER_PA_BASE;
    int i;

    memset(p, 0, sizeof(*p));
    p->dwBusOffset = dwBusOffset;

    p->pPwm  = MapRegs(PWM_PA_BASE, 0x100);
    p->pCm   = MapRegs(CM_PA_BASE, 0x100);
    p->pGpio = MapRegs(GPIO_PA_BASE, 0x100);
    p->pDmaBlock = MapRegs(dwDmaBase, 0x1000);
    if (!p->pPwm || !p->pCm || !p->pGpio || !p->pDmaBlock) {
        RETAILMSG(1, (L"PWMAUD: ERROR MmMapIoSpace failed\r\n"));
        goto Fail;
    }
    p->pDmaEnable = &REG(p->pDmaBlock, DMA_ENABLE_OFFSET);
    p->pDma = p->pDmaBlock + (AUDIO_DMA_CHANNEL * 0x100) / 4;

    p->dwDmaMemSize = DMA_MEM_ALLOC;
    p->pDmaMem = (PBYTE)AllocPhysMem(p->dwDmaMemSize, PAGE_READWRITE | PAGE_NOCACHE,
                                     0, 0, (ULONG *)&p->dwDmaMemPa);
    if (!p->pDmaMem) {
        RETAILMSG(1, (L"PWMAUD: ERROR AllocPhysMem(%u) failed\r\n", p->dwDmaMemSize));
        goto Fail;
    }
    for (i = 0; i < PWMHW_BLOCKS; i++)
        p->dwCbBus[i] = p->dwDmaMemPa + dwBusOffset + i * CB_WORDS * sizeof(DWORD);
    p->pRing     = (DWORD *)(p->pDmaMem + BLOCK_OFFSET);
    p->dwRingBus = p->dwDmaMemPa + dwBusOffset + BLOCK_OFFSET;

    // Make sure our channel is idle before touching the PWM.
    REG(p->pDma, DMA_REG_CS) = DMA_CS_RESET;
    Sleep(1);

    if (!ClockStart(p))
        goto Fail;
    GpioToPwm(p);
    p->fPwmOn = TRUE;                       // from here on, Deinit disables it
    if (!PwmStart(p))
        goto Fail;

    BuildControlBlocks(p);
    PwmHwResetRing(p);

    RETAILMSG(1, (L"PWMAUD: init ok, DMA ch%u, buffer PA 0x%08x bus 0x%08x, %u blocks of %u frames, PWM clock %u Hz, rate %u range %u\r\n",
                  AUDIO_DMA_CHANNEL, p->dwDmaMemPa, p->dwCbBus[0],
                  PWMHW_BLOCKS, PWMHW_BLOCK_FRAMES, PWMHW_CLOCK_HZ,
                  p->dwRate, p->dwRange));
    return TRUE;

Fail:
    RETAILMSG(1, (L"PWMAUD: init failed, PWM and its clock switched off\r\n"));
    PwmHwDeinit(p);
    return FALSE;
}

void PwmHwDeinit(PPWMHW p)
{
    if (p->pDma)
        PwmHwStop(p);

    // Leave nothing driving the jack: a PWM left enabled (for example
    // stalled with a full FIFO after a failed start) keeps buzzing.
    if (p->fPwmOn) {
        REG(p->pPwm, PWM_DMAC_OFFSET) = 0;
        REG(p->pPwm, PWM_CTL_OFFSET)  = 0;
        REG(p->pPwm, PWM_STA_OFFSET)  = PWM_STA_ERRORS;
        p->fPwmOn = FALSE;
    }
    if (p->fClockOn) {
        ClockStop(p);
        p->fClockOn = FALSE;
    }

    if (p->pDmaBlock)
        MmUnmapIoSpace((PVOID)p->pDmaBlock, 0x1000);
    if (p->pPwm)
        MmUnmapIoSpace((PVOID)p->pPwm, 0x100);
    if (p->pCm)
        MmUnmapIoSpace((PVOID)p->pCm, 0x100);
    if (p->pGpio)
        MmUnmapIoSpace((PVOID)p->pGpio, 0x100);
    if (p->pDmaMem)
        FreePhysMem(p->pDmaMem);
    memset(p, 0, sizeof(*p));
}

BOOL PwmHwSetRate(PPWMHW p, DWORD dwRate)
{
    if (p->fRunning || dwRate < 4000 || dwRate > 96000)
        return FALSE;
    PwmSetRange(p, dwRate);
    BuildControlBlocks(p);
    return TRUE;
}

void PwmHwFillSilence(const PWMHW *p, DWORD *pWords, UINT nWords)
{
    DWORD dwMid = p->dwRange / 2;
    UINT i;
    for (i = 0; i < nWords; i++)
        pWords[i] = dwMid;
}

void PwmHwResetRing(PPWMHW p)
{
    if (p->fRunning)
        return;
    PwmHwFillSilence(p, p->pRing, PWMHW_RING_FRAMES * 2);
    p->nPlaying = 0;
    p->dwPlayed = 0;
}

BOOL PwmHwStart(PPWMHW p)
{
    if (p->fRunning)
        return TRUE;

    REG(p->pDma, DMA_REG_CS) = DMA_CS_RESET;
    REG(p->pDma, DMA_REG_CS) = DMA_CS_INT | DMA_CS_END;
    *p->pDmaEnable |= (1 << AUDIO_DMA_CHANNEL);
    REG(p->pPwm, PWM_STA_OFFSET) = PWM_STA_ERRORS;

    // The DMA starts at block 0, absolute frame 0 (see PwmHwResetRing).
    p->nPlaying = 0;
    p->dwPlayed = 0;
    p->dwLate   = 0;

    REG(p->pDma, DMA_REG_CONBLK_AD) = p->dwCbBus[0];
    REG(p->pDma, DMA_REG_CS) = DMA_CS_ACTIVE;
    p->fRunning = TRUE;
    return TRUE;
}

void PwmHwStop(PPWMHW p)
{
    DWORD dwMid = p->dwRange / 2;

    // Pause, then reset the channel (drops the control block chain).
    REG(p->pDma, DMA_REG_CS) = 0;
    REG(p->pDma, DMA_REG_CS) = DMA_CS_RESET;
    REG(p->pDma, DMA_REG_CS) = DMA_CS_INT | DMA_CS_END;
    p->fRunning = FALSE;

    // Drop whatever is left in the FIFO and hold mid-level (RPTL repeats it).
    if (p->fPwmOn) {
        REG(p->pPwm, PWM_CTL_OFFSET) |= PWM_CTL_CLRF1;
        FifoWriteFrame(p, dwMid, dwMid);
    }
}

int PwmHwAck(PPWMHW p)
{
    DWORD dwCs, dwCur;
    UINT i, nNew, nDone;

    if (!p->fRunning)
        return -1;

    dwCs = REG(p->pDma, DMA_REG_CS);
    if (dwCs & DMA_CS_ERROR) {
        RETAILMSG(1, (L"PWMAUD: ERROR DMA cs=0x%08x debug=0x%08x\r\n",
                      dwCs, REG(p->pDma, DMA_REG_DEBUG)));
        PwmHwStop(p);
        return -1;
    }

    // Clear INT and END. ACTIVE must be written as 1: writing 0 pauses.
    REG(p->pDma, DMA_REG_CS) = DMA_CS_ACTIVE | DMA_CS_INT | DMA_CS_END;

    // CONBLK_AD holds the control block now playing. Every block from the
    // previous nPlaying up to (not including) it has finished: clear it to
    // silence so that nothing stale plays when the DMA comes round again.
    dwCur = REG(p->pDma, DMA_REG_CONBLK_AD) & BUS_ADDR_MASK;
    nNew = p->nPlaying;
    for (i = 0; i < PWMHW_BLOCKS; i++) {
        if (dwCur == (p->dwCbBus[i] & BUS_ADDR_MASK)) {
            nNew = i;
            break;
        }
    }

    nDone = (nNew + PWMHW_BLOCKS - p->nPlaying) % PWMHW_BLOCKS;
    for (i = 0; i < nDone; i++) {
        UINT nBlock = (p->nPlaying + i) % PWMHW_BLOCKS;
        PwmHwFillSilence(p, p->pRing + nBlock * PWMHW_BLOCK_WORDS, PWMHW_BLOCK_WORDS);
    }
    p->nPlaying  = nNew;
    p->dwPlayed += nDone * PWMHW_BLOCK_FRAMES;

    if (nDone >= 2)
        p->dwLate++;                        // an interrupt was served late
    return (int)nDone;
}

DWORD PwmHwReadPos(PPWMHW p)
{
    DWORD dwSrc, dwIdx, dwStart;

    if (!p->fRunning)
        return p->dwPlayed;

    // SOURCE_AD is the next ring address the DMA reads. Express it as a
    // distance (mod ring) from the start of the block we last saw playing;
    // the DMA may have moved on by a block or two since the last ack.
    dwSrc = (REG(p->pDma, DMA_REG_SOURCE_AD) & BUS_ADDR_MASK) -
            (p->dwRingBus & BUS_ADDR_MASK);
    if (dwSrc >= PWMHW_RING_FRAMES * 2 * sizeof(DWORD)) {
        // Between control blocks: assume the playing block is done.
        return p->dwPlayed + PWMHW_BLOCK_FRAMES;
    }
    dwIdx   = dwSrc / (2 * sizeof(DWORD));
    dwStart = p->nPlaying * PWMHW_BLOCK_FRAMES;
    return p->dwPlayed + ((dwIdx + PWMHW_RING_FRAMES - dwStart) % PWMHW_RING_FRAMES);
}

DWORD PwmHwReadClearStatus(PPWMHW p)
{
    DWORD dwSta = REG(p->pPwm, PWM_STA_OFFSET);
    REG(p->pPwm, PWM_STA_OFFSET) = PWM_STA_ERRORS;
    return dwSta;
}

void PwmHwDump(const PWMHW *p, LPCWSTR pszWhen)
{
    RETAILMSG(1, (L"PWMAUD: [%s] CM ctl=0x%08x div=0x%08x  PWM ctl=0x%08x sta=0x%08x dmac=0x%08x rng=%u/%u\r\n",
                  pszWhen,
                  REG(p->pCm, CM_PWMCTL_OFFSET), REG(p->pCm, CM_PWMDIV_OFFSET),
                  REG(p->pPwm, PWM_CTL_OFFSET), REG(p->pPwm, PWM_STA_OFFSET),
                  REG(p->pPwm, PWM_DMAC_OFFSET),
                  REG(p->pPwm, PWM_RNG1_OFFSET), REG(p->pPwm, PWM_RNG2_OFFSET)));
    RETAILMSG(1, (L"PWMAUD: [%s] DMA cs=0x%08x cb=0x%08x ti=0x%08x src=0x%08x len=%u debug=0x%08x enable=0x%08x gpfsel4=0x%08x\r\n",
                  pszWhen,
                  REG(p->pDma, DMA_REG_CS), REG(p->pDma, DMA_REG_CONBLK_AD),
                  REG(p->pDma, DMA_REG_TI), REG(p->pDma, DMA_REG_SOURCE_AD),
                  REG(p->pDma, DMA_REG_TXFR_LEN), REG(p->pDma, DMA_REG_DEBUG),
                  *p->pDmaEnable, REG(p->pGpio, GPFSEL4_OFFSET_PWM)));
}

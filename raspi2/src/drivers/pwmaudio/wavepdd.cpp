//
// wavepdd.cpp
//
// PDD for the Microsoft wave MDD (wavemdd.lib, linked from the public tree)
// driving the Pi 1 analog audio jack through pwmhw.
//
// Playback: one output stream at a time (the MDD keeps a single stream
// state); no input device. Accepted formats: PCM, 8 or 16 bit, mono or
// stereo, 4000..48000 Hz. The PWM range is set from the stream's sample rate,
// so there is no resampling. In practice CE 7's waveapi software mixer
// (swmixer, HKLM\Audio\SoftwareMixer) sits in front of the driver, mixes all
// applications' streams and opens the device at 44100 Hz 16-bit stereo; it
// recycles a few small buffers, sending the next one when the driver
// returns one (WOM_DONE).
//
// Write cursor: pwmhw's DMA loops over a ~93 ms ring that it clears to
// silence behind itself. Audio from the queued WAVEHDRs is copied into the
// ring at an absolute frame position (g_out.dwWrite) that stays ahead of the
// DMA read address, and each header's dwBytesRecorded is advanced as its
// bytes are copied. The MDD treats a header whose dwBytesRecorded reached
// dwBufferLength as finished and returns it on the next interrupt (every
// ~5.8 ms), so a producer gets its buffers back quickly and the ring is never
// padded with silence while data exists. Silence plays only when no data is
// queued (counted as an underrun when data then arrives late).
//
// Consequences: waveOutGetPosition counts bytes copied, which is ahead of
// the sound by up to the ring length (~93 ms); waveOutPause drops whatever
// was copied but not yet played (up to ~93 ms). At the end of the data the
// MDD sends WPDM_ENDOFDATA as soon as the last header is copied: the DMA
// keeps running until the copied audio has played ("draining"), and
// WPDM_CLOSE waits for that, so the end of a sound is not cut off.
//
// With TestTone=1 (registry) the driver plays the A1 bring-up tone at load:
// 0.5 s of 440 Hz LEFT, then 0.5 s of 880 Hz RIGHT.
//
#include <windows.h>
#include <types.h>
#include <nkintr.h>
#include <ceddk.h>
#include <devload.h>
#include <math.h>
#include <wavedbg.h>
#include <waveddsi.h>
#include <image_cfg.h>
#include <BCM2835.h>
#include <bsp_cfg.h>
#include "pwmhw.h"

#define REG_DMA_BUS_OFFSET  L"DmaBusOffset"
#define REG_SYSINTR         L"SysIntr"
#define REG_TEST_TONE       L"TestTone"
#define REG_PRIORITY        L"Priority256"   // also read by the MDD for its IST

#define DEFAULT_BUS_OFFSET  0x40000000      // L2-coherent alias, as dwchci
#define DEFAULT_PRIORITY    110             // above Ethernet (130), below USB

// Requantisation from 16-bit samples to the PWM range (registry "Dither"):
#define REG_DITHER          L"Dither"
#define DITHER_OFF          0               // truncate
#define DITHER_TPDF         1               // TPDF dither, +-1 PWM step
#define DITHER_SHAPED       2               // TPDF dither + 1st-order noise shaping (default)

// The MDD's interrupt thread waits on this SYSINTR (waveddsi.h).
DWORD gIntrAudio = SYSINTR_AUDIO;

static PWMHW            g_hw;
static CRITICAL_SECTION g_cs;               // protects g_hw and g_out
static BOOL             g_fInit;
static ULONG            g_ulVolume = 0xFFFFFFFF;   // low word left, high word right

// Requantiser state (used under g_cs)
static int              g_nDither = DITHER_SHAPED;
static DWORD            g_dwSeed  = 0x2545F491;    // dither random generator
static int              g_aErr[2];                 // shaping error per channel, Q16

// Output stream state
static struct {
    BOOL  fOpen;                            // a stream is open (one at a time)
    BOOL  fPlaying;                         // DMA running for it
    BOOL  fDraining;                        // all data copied, playing it out
    DWORD dwWrite;                          // absolute ring frame for the next data
    DWORD dwRate;
    WORD  wChannels;
    WORD  wBits;
    WORD  wBlockAlign;                      // bytes per source frame
    DWORD dwUnderruns;                      // data arrived after the DMA passed
    DWORD dwFaults;                         // app buffer access faults
} g_out;

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
static DWORD RegReadDword(HKEY hKey, LPCWSTR pszName, DWORD dwDefault)
{
    DWORD dwValue, dwType, dwSize = sizeof(dwValue);
    if (hKey && RegQueryValueEx(hKey, pszName, NULL, &dwType, (LPBYTE)&dwValue,
                                &dwSize) == ERROR_SUCCESS &&
        dwType == REG_DWORD)
        return dwValue;
    return dwDefault;
}

// ---------------------------------------------------------------------------
// Sample conversion and the write cursor
// ---------------------------------------------------------------------------

// 0..0xFFFF volume word to a 0..0x10000 gain (0xFFFF is unity).
static __inline int VolumeGain(WORD w)
{
    return (int)w + (w >> 15);
}

// Triangular (TPDF) dither in Q16 PWM steps: the sum of two uniform 16-bit
// values minus 65536, so -65536..65534 (+-1 PWM step), mean 0. The two
// values come from the top bits of a 32-bit LCG (its low bits are poor).
static __inline int TpdfDither(void)
{
    DWORD a, b;
    g_dwSeed = g_dwSeed * 1664525 + 1013904223;
    a = g_dwSeed >> 16;
    g_dwSeed = g_dwSeed * 1664525 + 1013904223;
    b = g_dwSeed >> 16;
    return (int)(a + b) - 65536;
}

// Requantises one signed 16-bit sample to a PWM word for channel nCh.
//
// x = (s + 32768) * range is the exact PWM value in Q16 (16 fraction bits).
// It stays below 2^31 for every supported rate: range <= 31250 at 4000 Hz,
// and 65535 * 31250 + the dither and feedback terms < 2^31.
//
//   DITHER_OFF:    truncate (the plain PwmHwSample conversion).
//   DITHER_TPDF:   add TPDF dither, round. The error no longer follows the
//                  signal (no distortion on quiet sounds and fade-outs); the
//                  noise floor rises by a few dB but is steady.
//   DITHER_SHAPED: also subtract the previous total quantisation error
//                  (dither included) before quantising: error feedback with
//                  noise transfer 1 - z^-1, which moves the noise from the
//                  low and mid frequencies towards fs/2, where the jack's RC
//                  filter and hearing attenuate it.
static __inline DWORD Requantize(int s, int nCh)
{
    int nRange = (int)g_hw.dwRange;
    int x, v, q, e;

    if (s > 32767)
        s = 32767;
    else if (s < -32768)
        s = -32768;
    x = (s + 32768) * nRange;

    if (g_nDither == DITHER_OFF)
        return (DWORD)(x >> 16);

    v = x;
    if (g_nDither == DITHER_SHAPED)
        v -= g_aErr[nCh];

    q = (v + TpdfDither() + 32768) >> 16;   // round to the nearest step
    if (q < 0)
        q = 0;
    else if (q > nRange - 1)
        q = nRange - 1;

    if (g_nDither == DITHER_SHAPED) {
        // Total error of this sample. Clamp it so that clipping at full
        // scale cannot wind the feedback up.
        e = (q << 16) - v;
        if (e > 2 * 65536)
            e = 2 * 65536;
        else if (e < -2 * 65536)
            e = -2 * 65536;
        g_aErr[nCh] = e;
    }
    return (DWORD)q;
}

// Converts nFrames source frames at pSrc to PWM words (left, right) at pOut.
static void ConvertFrames(const BYTE *pSrc, DWORD *pOut, UINT nFrames)
{
    int gainL = VolumeGain(LOWORD(g_ulVolume));
    int gainR = VolumeGain(HIWORD(g_ulVolume));
    UINT i;

    for (i = 0; i < nFrames; i++) {
        int l, r;
        if (g_out.wBits == 16) {
            // Byte reads: the application buffer need not be 2-byte aligned.
            l = (short)(pSrc[0] | (pSrc[1] << 8));
            r = (g_out.wChannels == 2) ? (short)(pSrc[2] | (pSrc[3] << 8)) : l;
        } else {
            l = ((int)pSrc[0] - 128) << 8;
            r = (g_out.wChannels == 2) ? (((int)pSrc[1] - 128) << 8) : l;
        }
        pSrc += g_out.wBlockAlign;

        // FIFO order: channel 1 (GPIO40, left) then channel 2 (GPIO45, right)
        pOut[2 * i]     = Requantize((l * gainL) >> 16, 0);
        pOut[2 * i + 1] = Requantize((r * gainR) >> 16, 1);
    }
}

// TRUE if a header in the chain still has bytes to copy.
static BOOL HaveData(PWAVEHDR pwh)
{
    for (; pwh != NULL; pwh = pwh->lpNext) {
        if (pwh->dwBytesRecorded < pwh->dwBufferLength)
            return TRUE;
    }
    return FALSE;
}

// Copies queued audio into the ring at the write cursor, as far as the ring
// window allows, advancing each header's dwBytesRecorded. A partial frame at
// the end of a header (length not a multiple of nBlockAlign) is skipped but
// counted, so the header still completes. Called with g_cs held.
static void WriteFromHeaders(PWAVEHDR pwh)
{
    DWORD dwEnd = PwmHwWindowEnd(&g_hw);

    if (!HaveData(pwh))
        return;

    // Never write behind the DMA: if the cursor fell behind (no data for a
    // while), restart it just ahead of the read address.
    if (g_hw.fRunning) {
        DWORD dwMin = PwmHwReadPos(&g_hw) + PWMHW_WRITE_MARGIN;
        if ((int)(g_out.dwWrite - dwMin) < 0) {
            g_out.dwUnderruns++;
            g_out.dwWrite = dwMin;
        }
    }

    while (pwh != NULL && (int)(dwEnd - g_out.dwWrite) > 0) {
        DWORD dwAvail = (pwh->dwBytesRecorded < pwh->dwBufferLength) ?
                        pwh->dwBufferLength - pwh->dwBytesRecorded : 0;
        DWORD dwFrames, dwRoom;

        if (dwAvail < g_out.wBlockAlign) {
            pwh->dwBytesRecorded = pwh->dwBufferLength;
            pwh = pwh->lpNext;
            continue;
        }

        dwFrames = dwAvail / g_out.wBlockAlign;
        dwRoom   = dwEnd - g_out.dwWrite;
        if (dwFrames > dwRoom)
            dwFrames = dwRoom;
        dwRoom = PwmHwContiguous(g_out.dwWrite);
        if (dwFrames > dwRoom)
            dwFrames = dwRoom;

        __try {
            ConvertFrames((const BYTE *)pwh->lpData + pwh->dwBytesRecorded,
                          PwmHwFramePtr(&g_hw, g_out.dwWrite), dwFrames);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // The application's buffer went away: play silence for it.
            PwmHwFillSilence(&g_hw, PwmHwFramePtr(&g_hw, g_out.dwWrite), 2 * dwFrames);
            g_out.dwFaults++;
        }

        pwh->dwBytesRecorded += dwFrames * g_out.wBlockAlign;
        g_out.dwWrite        += dwFrames;
    }
}

// TRUE once the DMA has played everything written (called with g_cs held).
static BOOL DrainDone(void)
{
    return (int)(PwmHwReadPos(&g_hw) - g_out.dwWrite) >= 0;
}

// ---------------------------------------------------------------------------
// Output control (called with the MDD's output lock held, takes g_cs)
// ---------------------------------------------------------------------------
static void OutStart(PWAVEHDR pwh)
{
    EnterCriticalSection(&g_cs);

    g_out.fDraining = FALSE;
    if (g_out.fPlaying) {
        // New data while the previous data is still playing out: append.
        WriteFromHeaders(pwh);
    } else {
        PwmHwSetRate(&g_hw, g_out.dwRate);
        PwmHwResetRing(&g_hw);
        g_out.dwWrite = 0;
        g_aErr[0] = g_aErr[1] = 0;          // new stream: no shaping history
        WriteFromHeaders(pwh);
        PwmHwStart(&g_hw);
        g_out.fPlaying = TRUE;
    }

    LeaveCriticalSection(&g_cs);
}

static void OutContinue(PWAVEHDR pwh)
{
    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying)
        WriteFromHeaders(pwh);
    LeaveCriticalSection(&g_cs);
}

static void OutStop(void)
{
    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying) {
        PwmHwStop(&g_hw);
        g_out.fPlaying  = FALSE;
        g_out.fDraining = FALSE;
    }
    LeaveCriticalSection(&g_cs);
}

static void OutEndOfData(void)
{
    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying)
        g_out.fDraining = TRUE;             // GetInterruptType stops the DMA
    LeaveCriticalSection(&g_cs);
}

static MMRESULT OutOpen(LPWAVEFORMATEX pwfx, BOOL fQueryOnly)
{
    MMRESULT mmRet = MMSYSERR_NOERROR;

    if (pwfx == NULL)
        return WAVERR_BADFORMAT;

    if (pwfx->wFormatTag != WAVE_FORMAT_PCM ||
        (pwfx->nChannels != 1 && pwfx->nChannels != 2) ||
        (pwfx->wBitsPerSample != 8 && pwfx->wBitsPerSample != 16) ||
        pwfx->nSamplesPerSec < 4000 || pwfx->nSamplesPerSec > 48000 ||
        pwfx->nBlockAlign != pwfx->nChannels * pwfx->wBitsPerSample / 8)
        return WAVERR_BADFORMAT;

    if (fQueryOnly)
        return MMSYSERR_NOERROR;

    EnterCriticalSection(&g_cs);
    if (g_out.fOpen) {
        // The MDD keeps one stream state and does not check this itself.
        mmRet = MMSYSERR_ALLOCATED;
    } else {
        g_out.fOpen       = TRUE;
        g_out.dwRate      = pwfx->nSamplesPerSec;
        g_out.wChannels   = pwfx->nChannels;
        g_out.wBits       = pwfx->wBitsPerSample;
        g_out.wBlockAlign = pwfx->nBlockAlign;
        g_out.dwUnderruns = 0;
        g_out.dwFaults    = 0;
    }
    LeaveCriticalSection(&g_cs);

    if (mmRet == MMSYSERR_NOERROR)
        RETAILMSG(1, (L"PWMAUD: open %u Hz %u-bit %s\r\n", pwfx->nSamplesPerSec,
                      pwfx->wBitsPerSample, (pwfx->nChannels == 2) ? L"stereo" : L"mono"));
    return mmRet;
}

static void OutClose(void)
{
    DWORD dwStart = GetTickCount();

    // Let the copied audio play out (at most one ring, ~93 ms at 44.1 kHz),
    // so the end of a sound is not cut off.
    for (;;) {
        BOOL fWait;
        EnterCriticalSection(&g_cs);
        fWait = g_out.fPlaying && !DrainDone();
        LeaveCriticalSection(&g_cs);
        if (!fWait || GetTickCount() - dwStart > 500)
            break;
        Sleep(5);
    }

    OutStop();
    EnterCriticalSection(&g_cs);
    if (g_out.dwUnderruns || g_out.dwFaults)
        RETAILMSG(1, (L"PWMAUD: close, %u underruns, %u application buffer faults\r\n",
                      g_out.dwUnderruns, g_out.dwFaults));
    g_out.fOpen = FALSE;
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------------------
// Bring-up test tone (A1)
// ---------------------------------------------------------------------------
#define TONE_AMPLITUDE  16384.0             // half of full scale
#define TONE_PI2        6.283185307179586

// Writes the tone from the write cursor up to the window end: 440 Hz LEFT
// until dwLeftEnd, 880 Hz RIGHT until dwRightEnd, nothing after (the ring
// is already silent there).
static void ToneWrite(DWORD dwLeftEnd, DWORD dwRightEnd)
{
    DWORD dwRate = g_hw.dwRate;
    DWORD dwMid  = g_hw.dwRange / 2;
    DWORD dwEnd  = PwmHwWindowEnd(&g_hw);

    while ((int)(dwEnd - g_out.dwWrite) > 0 && g_out.dwWrite < dwRightEnd) {
        DWORD *pw = PwmHwFramePtr(&g_hw, g_out.dwWrite);
        DWORD f = g_out.dwWrite;
        DWORD dwLeft = dwMid, dwRight = dwMid;
        if (f < dwLeftEnd) {
            double s = sin(TONE_PI2 * 440.0 * f / dwRate) * TONE_AMPLITUDE;
            dwLeft = PwmHwSample(&g_hw, (int)s);
        } else {
            double s = sin(TONE_PI2 * 880.0 * f / dwRate) * TONE_AMPLITUDE;
            dwRight = PwmHwSample(&g_hw, (int)s);
        }
        // FIFO order: channel 1 (GPIO40, left) then channel 2 (GPIO45, right)
        pw[0] = dwLeft;
        pw[1] = dwRight;
        g_out.dwWrite++;
    }
}

static void RunTestTone(DWORD dwPriority)
{
    HANDLE hEvent;
    DWORD dwStart, dwElapsed, dwInts = 0, dwTimeouts = 0, dwSta;
    DWORD dwLeftEnd, dwRightEnd;
    int nOldPriority;

    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!hEvent)
        return;
    if (!InterruptInitialize(gIntrAudio, hEvent, NULL, 0)) {
        RETAILMSG(1, (L"PWMAUD: ERROR InterruptInitialize(%u) failed\r\n", gIntrAudio));
        CloseHandle(hEvent);
        return;
    }

    // Refill at the audio IST priority (the MDD's IST uses the same value),
    // above the boot and NDIS threads whose KITL output starved it at 251.
    nOldPriority = CeGetThreadPriority(GetCurrentThread());
    CeSetThreadPriority(GetCurrentThread(), dwPriority);

    PwmHwSetRate(&g_hw, PWMHW_DEFAULT_RATE);
    PwmHwResetRing(&g_hw);
    dwLeftEnd  = g_hw.dwRate / 2;
    dwRightEnd = g_hw.dwRate;
    g_out.dwWrite = 0;
    ToneWrite(dwLeftEnd, dwRightEnd);

    RETAILMSG(1, (L"PWMAUD: test tone: 0.5 s 440 Hz LEFT, then 0.5 s 880 Hz RIGHT (priority %u)\r\n",
                  dwPriority));
    dwStart = GetTickCount();
    PwmHwStart(&g_hw);
    Sleep(5);
    PwmHwDump(&g_hw, L"tone");

    // The FIFO was empty until the DMA started, so gap flags are expected
    // here. Clear them: any gap seen at the end happened during playback.
    PwmHwReadClearStatus(&g_hw);

    // Keep writing until the whole tone has been played.
    while ((int)(PwmHwReadPos(&g_hw) - dwRightEnd) < 0) {
        int nDone;

        if (WaitForSingleObject(hEvent, 500) != WAIT_OBJECT_0) {
            RETAILMSG(1, (L"PWMAUD: ERROR no DMA interrupt within 500 ms\r\n"));
            PwmHwDump(&g_hw, L"timeout");
            if (++dwTimeouts >= 2)
                break;
            continue;
        }
        dwInts++;
        nDone = PwmHwAck(&g_hw);
        InterruptDone(gIntrAudio);
        if (nDone < 0)
            break;
        ToneWrite(dwLeftEnd, dwRightEnd);
    }

    dwElapsed = GetTickCount() - dwStart;
    dwSta = PwmHwReadClearStatus(&g_hw);
    PwmHwDump(&g_hw, L"end");
    RETAILMSG(1, (L"PWMAUD: PWM status during playback 0x%08x: %s\r\n", dwSta,
                  (dwSta & (PWM_STA_GAPO1 | PWM_STA_GAPO2)) ?
                      L"FIFO ran empty (gap) - DMA did not keep up" :
                  (dwSta & PWM_STA_ERRORS) ? L"FIFO/bus error" : L"no gaps or errors"));
    PwmHwStop(&g_hw);

    CeSetThreadPriority(GetCurrentThread(), nOldPriority);
    InterruptDisable(gIntrAudio);
    CloseHandle(hEvent);

    // One interrupt per block of PWMHW_BLOCK_FRAMES frames; 1 s of tone.
    RETAILMSG(1, (L"PWMAUD: test tone done: %u DMA interrupts in %u ms (expect ~%u in ~%u ms), %u late\r\n",
                  dwInts, dwElapsed,
                  dwRightEnd / PWMHW_BLOCK_FRAMES,
                  dwRightEnd * 1000 / g_hw.dwRate,
                  g_hw.dwLate));
}

// ---------------------------------------------------------------------------
// PDD entry points (waveddsi.h)
// ---------------------------------------------------------------------------
BOOL PDD_AudioInitialize(DWORD dwIndex)
{
    HKEY  hKey;
    DWORD dwBusOffset, dwTestTone, dwPriority;

    // The MDD passes the Device Manager active key path as dwIndex.
    hKey = OpenDeviceKey((LPCWSTR)dwIndex);
    dwBusOffset = RegReadDword(hKey, REG_DMA_BUS_OFFSET, DEFAULT_BUS_OFFSET);
    gIntrAudio  = RegReadDword(hKey, REG_SYSINTR, SYSINTR_AUDIO);
    dwTestTone  = RegReadDword(hKey, REG_TEST_TONE, 0);
    dwPriority  = RegReadDword(hKey, REG_PRIORITY, DEFAULT_PRIORITY);
    g_nDither   = (int)RegReadDword(hKey, REG_DITHER, DITHER_SHAPED);
    if (g_nDither > DITHER_SHAPED)
        g_nDither = DITHER_SHAPED;
    if (hKey)
        RegCloseKey(hKey);

    RETAILMSG(1, (L"PWMAUD: PDD_AudioInitialize, SysIntr=%u DmaBusOffset=0x%08x TestTone=%u Priority256=%u Dither=%d (%s)\r\n",
                  gIntrAudio, dwBusOffset, dwTestTone, dwPriority, g_nDither,
                  (g_nDither == DITHER_OFF) ? L"off" :
                  (g_nDither == DITHER_TPDF) ? L"TPDF" : L"TPDF + noise shaping"));

    InitializeCriticalSection(&g_cs);
    memset(&g_out, 0, sizeof(g_out));
    if (!PwmHwInit(&g_hw, dwBusOffset)) {
        DeleteCriticalSection(&g_cs);
        return FALSE;
    }
    g_fInit = TRUE;
    PwmHwDump(&g_hw, L"init");

    if (dwTestTone)
        RunTestTone(dwPriority);

    return TRUE;
}

VOID PDD_AudioDeinitialize(VOID)
{
    if (!g_fInit)
        return;
    OutStop();
    PwmHwDeinit(&g_hw);
    DeleteCriticalSection(&g_cs);
    g_fInit = FALSE;
}

VOID PDD_AudioPowerHandler(BOOL power_down)
{
    // Called by the MDD's power handler (no blocking calls allowed). On this
    // BSP suspend is a halt, so only the DMA is stopped on power-down; the
    // MDD marks the queued headers done and restarts the stream on the next
    // write after power-up.
    if (power_down && g_out.fPlaying) {
        PwmHwStop(&g_hw);
        g_out.fPlaying  = FALSE;
        g_out.fDraining = FALSE;
    }
}

AUDIO_STATE PDD_AudioGetInterruptType(VOID)
{
    AUDIO_STATE state = AUDIO_STATE_IGNORE;
    int nDone;

    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying) {
        nDone = PwmHwAck(&g_hw);
        if (nDone < 0) {
            // DMA error (logged by PwmHwAck, DMA stopped): end the stream.
            g_out.fPlaying  = FALSE;
            g_out.fDraining = FALSE;
            state = AUDIO_STATE_OUT_STOPPED;
        } else if (g_out.fDraining) {
            // The MDD has no more data. Stop once the ring has played out;
            // until then there is nothing for the MDD to do.
            if (DrainDone()) {
                PwmHwStop(&g_hw);
                g_out.fPlaying  = FALSE;
                g_out.fDraining = FALSE;
            }
        } else if (nDone > 0) {
            // Return copied headers and copy more (WPDM_CONTINUE).
            state = AUDIO_STATE_OUT_PLAYING;
        }
    }
    LeaveCriticalSection(&g_cs);
    return state;
}

DWORD PDD_AudioMessage(UINT uMsg, DWORD dwParam1, DWORD dwParam2)
{
    return MMSYSERR_NOTSUPPORTED;
}

static MMRESULT GetOutCaps(PWAVEOUTCAPS pCaps)
{
    if (pCaps == NULL)
        return MMSYSERR_NOERROR;            // "is there an output device"

    memset(pCaps, 0, sizeof(*pCaps));
    pCaps->wMid = MM_MICROSOFT;
    pCaps->wPid = 24;                       // generic wave out
    pCaps->vDriverVersion = 0x0001;
    wcsncpy(pCaps->szPname, L"Raspberry Pi analog audio", MAXPNAMELEN - 1);
    pCaps->dwFormats = WAVE_FORMAT_1M08 | WAVE_FORMAT_1M16 |
                       WAVE_FORMAT_1S08 | WAVE_FORMAT_1S16 |
                       WAVE_FORMAT_2M08 | WAVE_FORMAT_2M16 |
                       WAVE_FORMAT_2S08 | WAVE_FORMAT_2S16 |
                       WAVE_FORMAT_4M08 | WAVE_FORMAT_4M16 |
                       WAVE_FORMAT_4S08 | WAVE_FORMAT_4S16;
    pCaps->wChannels = 2;
    pCaps->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

MMRESULT PDD_WaveProc(WAPI_INOUT apidir, DWORD dwCode, DWORD dwParam1, DWORD dwParam2)
{
    // No input: GETDEVCAPS(NULL) failing makes the MDD report 0 input devices.
    if (apidir != WAPI_OUT)
        return MMSYSERR_NOTSUPPORTED;

    switch (dwCode) {
    case WPDM_GETDEVCAPS:
        return GetOutCaps((PWAVEOUTCAPS)dwParam1);

    case WPDM_OPEN:
        return OutOpen((LPWAVEFORMATEX)dwParam1, (BOOL)dwParam2);

    case WPDM_CLOSE:
        OutClose();
        return MMSYSERR_NOERROR;

    case WPDM_START:
    case WPDM_RESTART:
        if (dwParam1)
            OutStart((PWAVEHDR)dwParam1);
        return MMSYSERR_NOERROR;

    case WPDM_CONTINUE:
        if (dwParam1)
            OutContinue((PWAVEHDR)dwParam1);
        return MMSYSERR_NOERROR;

    case WPDM_ENDOFDATA:                    // every queued byte has been copied
        OutEndOfData();
        return MMSYSERR_NOERROR;

    case WPDM_PAUSE:                        // copied but unplayed audio is dropped
    case WPDM_STOP:                         // waveOutReset
        OutStop();
        return MMSYSERR_NOERROR;

    case WPDM_STANDBY:
        return MMSYSERR_NOERROR;

    case WPDM_GETVOLUME:
        if (dwParam1)
            *(PULONG)dwParam1 = g_ulVolume;
        return MMSYSERR_NOERROR;

    case WPDM_SETVOLUME:
        g_ulVolume = dwParam1;
        return MMSYSERR_NOERROR;

    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}

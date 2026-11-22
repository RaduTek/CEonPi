//
// audiotest.cpp
//
// waveOut test for the Pi 1 analog audio driver (pwmaudio.dll).
//
//   audiotest              plays the built-in tests (below), then shows a
//                          summary; each line also goes to the debug output
//   audiotest <file.wav>   plays the file with PlaySound
//
// Built-in tests (each tone is split over 4 wave headers so the driver has
// to chain them):
//   1. 44100 Hz 16-bit stereo, 440 Hz LEFT only, 1 s
//   2. 44100 Hz 16-bit stereo, 880 Hz RIGHT only, 1 s
//   3. 22050 Hz 16-bit mono, 660 Hz, 1 s
//   4. 11025 Hz 8-bit mono, 330 Hz, 1 s
//   5. 44100 Hz 16-bit stereo, 523 Hz, 1 s with a 0.5 s pause in the middle
//   6. 44100 Hz 16-bit stereo, 440 Hz, 5 s fading from -20 dB to -80 dB
//      (listen for distortion and hiss; compare the driver's Dither modes)
// For each test: the elapsed time (expect the tone length, plus the pause in
// test 5, plus up to ~100 ms of start-up and DMA latency) and the final
// waveOutGetPosition against the bytes written (expect equal).
//
#include <windows.h>
#include <mmsystem.h>
#include <math.h>

#define NUM_HEADERS     4
#define AMPLITUDE       0.4
#define TWO_PI          6.283185307179586

static WCHAR g_szSummary[2048];

static void Report(LPCWSTR pszFormat, ...)
{
    WCHAR sz[256];
    va_list args;

    va_start(args, pszFormat);
    wvsprintf(sz, pszFormat, args);
    va_end(args);

    OutputDebugString(L"AUDIOTEST: ");
    OutputDebugString(sz);
    OutputDebugString(L"\r\n");
    if (wcslen(g_szSummary) + wcslen(sz) + 3 < sizeof(g_szSummary) / sizeof(WCHAR)) {
        wcscat(g_szSummary, sz);
        wcscat(g_szSummary, L"\r\n");
    }
}

// Writes one sample value (-1.0..1.0) at p in the given sample size.
static BYTE *PutSample(BYTE *p, int nBits, double v)
{
    if (nBits == 16) {
        short s = (short)(v * 32767.0);
        p[0] = (BYTE)(s & 0xFF);
        p[1] = (BYTE)((s >> 8) & 0xFF);
        return p + 2;
    }
    p[0] = (BYTE)(128 + (int)(v * 127.0));
    return p + 1;
}

// Plays dwMs of a tone (freqL on the left, freqR on the right; mono uses
// freqL). With dwPauseAtMs != 0 the stream is paused for 500 ms at that
// point. With fFade the level falls exponentially from -20 dB to -80 dB
// (full scale = 0 dB) over the tone, which makes requantisation distortion
// and hiss audible. Returns FALSE if the device fails.
static BOOL PlayTone(LPCWSTR pszName, DWORD nRate, WORD nBits, WORD nChannels,
                     double freqL, double freqR, DWORD dwMs, DWORD dwPauseAtMs,
                     BOOL fFade)
{
    WAVEFORMATEX wfx;
    HWAVEOUT hwo = NULL;
    WAVEHDR whdr[NUM_HEADERS];
    MMTIME mmt;
    BYTE *pData = NULL, *p;
    DWORD dwFrames, dwBytes, dwPart, dwStart, dwElapsed, dwExpect, i;
    MMRESULT mmr;
    BOOL fPaused = FALSE, fOk = FALSE;
    int h;

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = nChannels;
    wfx.nSamplesPerSec  = nRate;
    wfx.wBitsPerSample  = nBits;
    wfx.nBlockAlign     = (WORD)(nChannels * nBits / 8);
    wfx.nAvgBytesPerSec = nRate * wfx.nBlockAlign;

    dwFrames = nRate * dwMs / 1000;
    dwFrames -= dwFrames % NUM_HEADERS;
    dwBytes  = dwFrames * wfx.nBlockAlign;
    dwPart   = dwBytes / NUM_HEADERS;

    pData = (BYTE *)LocalAlloc(LMEM_FIXED, dwBytes);
    if (!pData) {
        Report(L"%s: out of memory", pszName);
        return FALSE;
    }
    p = pData;
    for (i = 0; i < dwFrames; i++) {
        // -20 dB (0.1) down to -80 dB (0.0001): 0.1 * 0.001^(i / dwFrames)
        double amp = fFade ? 0.1 * pow(0.001, (double)i / dwFrames) : AMPLITUDE;
        double l = (freqL > 0.0) ? amp * sin(TWO_PI * freqL * i / nRate) : 0.0;
        p = PutSample(p, nBits, l);
        if (nChannels == 2) {
            double r = (freqR > 0.0) ? amp * sin(TWO_PI * freqR * i / nRate) : 0.0;
            p = PutSample(p, nBits, r);
        }
    }

    mmr = waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mmr != MMSYSERR_NOERROR) {
        Report(L"%s: waveOutOpen failed (%u)", pszName, mmr);
        LocalFree(pData);
        return FALSE;
    }

    memset(whdr, 0, sizeof(whdr));
    for (h = 0; h < NUM_HEADERS; h++) {
        whdr[h].lpData         = (LPSTR)(pData + h * dwPart);
        whdr[h].dwBufferLength = dwPart;
        waveOutPrepareHeader(hwo, &whdr[h], sizeof(WAVEHDR));
    }

    dwStart = GetTickCount();
    for (h = 0; h < NUM_HEADERS; h++)
        waveOutWrite(hwo, &whdr[h], sizeof(WAVEHDR));

    // Wait for the last header, with a generous timeout.
    for (;;) {
        dwElapsed = GetTickCount() - dwStart;
        if (whdr[NUM_HEADERS - 1].dwFlags & WHDR_DONE)
            break;
        if (dwElapsed > 2 * dwMs + 2000) {
            Report(L"%s: TIMEOUT after %u ms", pszName, dwElapsed);
            break;
        }
        if (dwPauseAtMs && !fPaused && dwElapsed >= dwPauseAtMs) {
            waveOutPause(hwo);
            Sleep(500);
            waveOutRestart(hwo);
            fPaused = TRUE;
        }
        Sleep(5);
    }

    mmt.wType = TIME_BYTES;
    mmt.u.cb  = 0;
    waveOutGetPosition(hwo, &mmt, sizeof(mmt));

    dwExpect = dwMs + (dwPauseAtMs ? 500 : 0);
    Report(L"%s: %u ms (expect ~%u), position %u of %u bytes%s", pszName,
           dwElapsed, dwExpect, mmt.u.cb, dwBytes,
           (mmt.u.cb == dwBytes) ? L"" : L"  <-- MISMATCH");
    fOk = TRUE;

    waveOutReset(hwo);
    for (h = 0; h < NUM_HEADERS; h++)
        waveOutUnprepareHeader(hwo, &whdr[h], sizeof(WAVEHDR));
    waveOutClose(hwo);
    LocalFree(pData);
    return fOk;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    g_szSummary[0] = 0;

    if (lpCmdLine && lpCmdLine[0]) {
        DWORD dwStart = GetTickCount();
        BOOL fOk = PlaySound(lpCmdLine, NULL, SND_FILENAME | SND_SYNC | SND_NODEFAULT);
        Report(L"PlaySound(%s): %s, %u ms", lpCmdLine, fOk ? L"ok" : L"FAILED",
               GetTickCount() - dwStart);
        MessageBox(NULL, g_szSummary, L"Audio test", MB_OK);
        return 0;
    }

    Report(L"%u output device(s)", waveOutGetNumDevs());
    PlayTone(L"1: 44.1k 16-bit stereo, 440 Hz LEFT",  44100, 16, 2, 440.0, 0.0,   1000, 0, FALSE);
    PlayTone(L"2: 44.1k 16-bit stereo, 880 Hz RIGHT", 44100, 16, 2, 0.0,   880.0, 1000, 0, FALSE);
    PlayTone(L"3: 22.05k 16-bit mono, 660 Hz",        22050, 16, 1, 660.0, 0.0,   1000, 0, FALSE);
    PlayTone(L"4: 11.025k 8-bit mono, 330 Hz",        11025,  8, 1, 330.0, 0.0,   1000, 0, FALSE);
    PlayTone(L"5: 44.1k stereo 523 Hz, paused 0.5 s", 44100, 16, 2, 523.0, 523.0, 1000, 500, FALSE);
    PlayTone(L"6: 44.1k stereo 440 Hz, fade -20 to -80 dB", 44100, 16, 2, 440.0, 440.0, 5000, 0, TRUE);

    MessageBox(NULL, g_szSummary, L"Audio test", MB_OK);
    return 0;
}

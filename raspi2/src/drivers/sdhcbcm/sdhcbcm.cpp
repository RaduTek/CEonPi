//
// sdhcbcm.cpp
//
// BCM2835 SD host controller driver for Windows Embedded Compact 7.
//
// MILESTONE M0 - probe & identify only.
//
// This build is a self-contained probe: at driver load it maps the Arasan
// eMMC / SDHCI register block (0x20300000) and the GPIO block, dumps the
// SDHCI capability / version / present-state registers and the GPIO 48-53
// function-select fields through KITL debug output, then stays resident.
//
// It deliberately does NOT link the CE SD host MDD (SDHCBase.lib) yet, so it
// builds without any SD catalog items selected. Later milestones replace the
// probe body with a real PDD (CSDHCBcm2835 : public CSDHCBase) that plugs
// into the CE SD stack. See ~/.claude/plans/typed-weaving-flame.md.
//
// Nothing here is asserted about which controller the firmware wired to the
// card slot or which ALT function each GPIO carries; the raw values are
// logged for analysis on real hardware.
//
#include <windows.h>
#include <nkintr.h>
#include <ceddk.h>
#include "bcm2835.h"

// Size of the register window we map for the probe (one 4K page is plenty;
// the SDHCI register file is well under 0x100 bytes).
#define SDHC_PROBE_WINDOW       0x1000

// Standard SD Host Controller (SDHCI) register offsets used by the probe.
// These match the CE MDD's SDHCRegs.h and the SD Host Controller Standard
// Specification. The BCM2835 Arasan core requires 32-bit register accesses,
// so every read below is a DWORD read.
#define SDHC_REG_PRESENT_STATE      0x24    // 32-bit
#define SDHC_REG_CAPABILITIES       0x40    // 32-bit (caps low)
#define SDHC_REG_CAPABILITIES_HI    0x44    // 32-bit (caps high)
#define SDHC_REG_SLOTINT_VERSION    0xFC    // 32-bit: [15:0]=SlotIntStatus, [31:16]=Host Version

// Present State bits (SDHCI spec; identical to the MDD's STATE_* defines).
#define SDHC_STATE_CARD_INSERTED        0x00010000
#define SDHC_STATE_CARD_STATE_STABLE    0x00020000
#define SDHC_STATE_CARD_DETECT_SIGNAL   0x00040000
#define SDHC_STATE_WRITE_PROTECT        0x00080000

// Read a 32-bit SDHCI register.
static DWORD RdReg(volatile BYTE *pBase, DWORD dwOffset)
{
    return *(volatile DWORD *)(pBase + dwOffset);
}

// ---------------------------------------------------------------------------
// DoProbe - the M0 work. Maps and dumps the eMMC and GPIO state.
//
static void DoProbe(void)
{
    PHYSICAL_ADDRESS pa;
    volatile BYTE *pEmmc;
    volatile BYTE *pGpio;
    DWORD dwCapsLo, dwCapsHi, dwState, dwVerReg;
    WORD  wVer;

    RETAILMSG(1, (TEXT("SDPROBE: ++ BCM2835 Arasan eMMC probe (M0)\r\n")));

    // Map the Arasan eMMC / SDHCI register block at 0x20300000.
    pa.QuadPart = EX_MASSMEDIA_PA_BASE;
    pEmmc = (volatile BYTE *) MmMapIoSpace(pa, SDHC_PROBE_WINDOW, FALSE);
    if (pEmmc == NULL) {
        RETAILMSG(1, (TEXT("SDPROBE: MmMapIoSpace(eMMC 0x%08x) FAILED\r\n"),
                      (DWORD)EX_MASSMEDIA_PA_BASE));
        return;
    }
    RETAILMSG(1, (TEXT("SDPROBE: eMMC regs mapped at 0x%08x (phys 0x%08x)\r\n"),
                  (DWORD)pEmmc, (DWORD)EX_MASSMEDIA_PA_BASE));

    dwVerReg = RdReg(pEmmc, SDHC_REG_SLOTINT_VERSION);
    wVer     = (WORD)(dwVerReg >> 16);
    dwCapsLo = RdReg(pEmmc, SDHC_REG_CAPABILITIES);
    dwCapsHi = RdReg(pEmmc, SDHC_REG_CAPABILITIES_HI);
    dwState  = RdReg(pEmmc, SDHC_REG_PRESENT_STATE);

    RETAILMSG(1, (TEXT("SDPROBE: HOST_VERSION = 0x%04x (spec ver %d, vendor 0x%02x)\r\n"),
                  wVer, (wVer & 0xFF), (wVer >> 8) & 0xFF));
    RETAILMSG(1, (TEXT("SDPROBE: CAPABILITIES = 0x%08x : 0x%08x (hi:lo)\r\n"),
                  dwCapsHi, dwCapsLo));

    // Base clock for the SD clock: SDHCI v3 uses caps[15:8] (MHz), v1/v2 uses
    // caps[13:8]. The BCM2835 Arasan often reports 0 here, which means the
    // real base clock must come from the VideoCore mailbox (done in M1).
    RETAILMSG(1, (TEXT("SDPROBE:   base clock field = %d MHz (v3) / %d MHz (v1-2)")
                  TEXT(" - 0 => query mailbox in M1\r\n"),
                  (dwCapsLo >> 8) & 0xFF, (dwCapsLo >> 8) & 0x3F));
    RETAILMSG(1, (TEXT("SDPROBE:   timeout clock = %d %s\r\n"),
                  (dwCapsLo & 0x3F),
                  ((dwCapsLo >> 7) & 1) ? TEXT("MHz") : TEXT("KHz")));

    RETAILMSG(1, (TEXT("SDPROBE: PRESENT_STATE = 0x%08x\r\n"), dwState));
    RETAILMSG(1, (TEXT("SDPROBE:   CardInserted=%d StateStable=%d DetectSignal=%d WriteProt=%d\r\n"),
                  (dwState & SDHC_STATE_CARD_INSERTED)      ? 1 : 0,
                  (dwState & SDHC_STATE_CARD_STATE_STABLE)  ? 1 : 0,
                  (dwState & SDHC_STATE_CARD_DETECT_SIGNAL) ? 1 : 0,
                  (dwState & SDHC_STATE_WRITE_PROTECT)      ? 1 : 0));

    // Map the GPIO block and dump the function-select fields for GPIO 48-53
    // (the SD card bus pins). GPIO 40-49 are in GPFSEL4, 50-59 in GPFSEL5;
    // each pin is 3 bits. ALT function encoding: 0=input,1=output,
    // 4=ALT0,5=ALT1,6=ALT2,7=ALT3,3=ALT4,2=ALT5. We log raw values only and
    // draw no conclusion here about which controller owns the slot.
    pa.QuadPart = GPIO_PA_BASE;
    pGpio = (volatile BYTE *) MmMapIoSpace(pa, 0x1000, FALSE);
    if (pGpio != NULL) {
        DWORD dwFsel4 = RdReg(pGpio, GPFSEL4_OFFSET);   // GPIO 40-49
        DWORD dwFsel5 = RdReg(pGpio, GPFSEL5_OFFSET);   // GPIO 50-59
        RETAILMSG(1, (TEXT("SDPROBE: GPFSEL4=0x%08x GPFSEL5=0x%08x\r\n"),
                      dwFsel4, dwFsel5));
        RETAILMSG(1, (TEXT("SDPROBE:   FSEL G48=%d G49=%d G50=%d G51=%d G52=%d G53=%d\r\n"),
                      (dwFsel4 >> 24) & 7, (dwFsel4 >> 27) & 7,
                      (dwFsel5 >>  0) & 7, (dwFsel5 >>  3) & 7,
                      (dwFsel5 >>  6) & 7, (dwFsel5 >>  9) & 7));
    } else {
        RETAILMSG(1, (TEXT("SDPROBE: MmMapIoSpace(GPIO 0x%08x) FAILED\r\n"),
                      (DWORD)GPIO_PA_BASE));
    }

    RETAILMSG(1, (TEXT("SDPROBE: -- probe done\r\n")));
}

// ---------------------------------------------------------------------------
// DLL entry.
//
extern "C" BOOL WINAPI DllMain(HANDLE hInst, DWORD dwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hInst);
    UNREFERENCED_PARAMETER(lpvReserved);
    if (dwReason == DLL_PROCESS_ATTACH) {
        // No per-process work; the probe runs from SDP_Init so it executes in
        // the Device Manager context after KITL is up.
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Stream interface entry points (prefix "SDP"). The Device Manager calls
// SDP_Init when it loads the BuiltIn driver; that is where the probe runs.
// The rest are minimal stubs - this build exposes no real device I/O.
//
extern "C" DWORD SDP_Init(DWORD dwContext)
{
    UNREFERENCED_PARAMETER(dwContext);
    DoProbe();
    // Return a non-zero device context so the Device Manager keeps us loaded.
    return (DWORD) 1;
}

extern "C" BOOL SDP_Deinit(DWORD hDeviceContext)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
    return TRUE;
}

extern "C" DWORD SDP_Open(DWORD hDeviceContext, DWORD AccessCode, DWORD ShareMode)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
    UNREFERENCED_PARAMETER(AccessCode);
    UNREFERENCED_PARAMETER(ShareMode);
    return (DWORD) 1;
}

extern "C" BOOL SDP_Close(DWORD hOpenContext)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    return TRUE;
}

extern "C" DWORD SDP_Read(DWORD hOpenContext, LPVOID pBuffer, DWORD Count)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(Count);
    return 0;
}

extern "C" DWORD SDP_Write(DWORD hOpenContext, LPCVOID pBuffer, DWORD Count)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(Count);
    return 0;
}

extern "C" DWORD SDP_Seek(DWORD hOpenContext, long Amount, DWORD Type)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(Amount);
    UNREFERENCED_PARAMETER(Type);
    return (DWORD) -1;
}

extern "C" BOOL SDP_IOControl(DWORD hOpenContext, DWORD dwCode,
                              PBYTE pBufIn, DWORD dwLenIn,
                              PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(dwCode);
    UNREFERENCED_PARAMETER(pBufIn);
    UNREFERENCED_PARAMETER(dwLenIn);
    UNREFERENCED_PARAMETER(pBufOut);
    UNREFERENCED_PARAMETER(dwLenOut);
    UNREFERENCED_PARAMETER(pdwActualOut);
    return FALSE;
}

extern "C" void SDP_PowerUp(DWORD hDeviceContext)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
}

extern "C" void SDP_PowerDown(DWORD hDeviceContext)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
}

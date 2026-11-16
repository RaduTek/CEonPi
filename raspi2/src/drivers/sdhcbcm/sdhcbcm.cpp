//
// sdhcbcm.cpp
//
// BCM2835 PDD for the CE standard SD host controller MDD. See sdhcbcm.h.
//
// The MDD (SDHCBase.lib) provides the SHC_* stream entry points, the DllEntry,
// and the CSDHCBase / CSDHCSlotBase engine. This file provides only the
// platform specifics: the synthesized capabilities and the create/destroy
// factory. Register window (0x20300000), SysIntr (SYSINTR_SDHC) and the base
// clock (the mailbox-reported 200 MHz) all arrive through the device registry
// key; see files\platform.reg.
//
#include "sdhcbcm.h"


// ---------------------------------------------------------------------------
// CSDHCSlotBcm2835::GetCapabilities
//
// The Arasan eMMC CAPABILITIES register reads 0 on the BCM2835 (verified at
// M0), so synthesize the fields the MDD relies on. ClkFreq / TOFreq are left 0
// because DetermineMaxClockRate / DetermineTimeoutControl take them from the
// registry (BaseClockFrequency / TimeoutClockFrequency) when non-zero, which
// lets us express the real 200 MHz base clock (the 6-bit ClkFreq field maxes
// at 63 MHz). ADMA2 is left 0 so the MDD selects SDMA when DMA is enabled.
//
SSDHC_CAPABILITIES
CSDHCSlotBcm2835::GetCapabilities()
{
    SSDHC_CAPABILITIES caps;

    caps.dw = 0;
    caps.bits.MaxBlockLen = 0;      // 0 => 512-byte maximum block length
    caps.bits.HighSpeed   = 1;      // core supports high speed
    caps.bits.SDMA        = 1;      // single-operation DMA (used in M2+)
    caps.bits.Voltage33   = 1;      // 3.3 V only

    return caps;
}


// ---------------------------------------------------------------------------
// Create/destroy factory. CreateSDHCBcmObject is exported and named by the
// registry "ObjectCreationProc" value.
//
extern "C"
PCSDHCBase
CreateSDHCBcmObject()
{
    return new CSDHCBcm2835;
}

VOID
CSDHCBcm2835::DestroySDHCBcmObject(
    PCSDHCBase pSDHC
    )
{
    if (pSDHC) {
        delete pSDHC;
    }
}

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

    // Registry "DisableHighSpeed"=1: do not advertise high speed, so sdbus
    // never switches the card to 50 MHz and it stays at 25 MHz.
    if (m_pregDevice &&
        m_pregDevice->ValueDW(SDHCBCM_DISABLE_HIGH_SPEED_KEY, 0) != 0) {
        caps.bits.HighSpeed = 0;
    }

    return caps;
}


// ---------------------------------------------------------------------------
// CSDHCSlotBcm2835::SetInterface
//
// The MDD sets HOSTCTL_HIGH_SPEED in Host Control when sdbus has switched the
// card to high speed. On the BCM2835 Arasan that bit must stay clear
// (FreeBSD bcm2835_sdhci.c: SDHCI_QUIRK_DONT_SET_HISPD_BIT); the 50 MHz clock
// is still used. Observed: two older 2 GB / 512 MB cards (SD 1.x, no CMD8
// response) failed the first command after the switch (CMD16, "Timeout
// waiting for CMD Inhibit") with the bit set, while a newer card worked.
//
VOID
CSDHCSlotBcm2835::SetInterface(
    PSD_CARD_INTERFACE_EX pInterface
    )
{
    if (pInterface == NULL) {
        CSDHCSlotBase::SetInterface(pInterface);
        return;
    }

    // Clear the bit only for the MDD call; sdbus keeps its own view.
    DWORD dwHighSpeed = pInterface->InterfaceModeEx.bit.sdHighSpeed;
    pInterface->InterfaceModeEx.bit.sdHighSpeed = 0;
    CSDHCSlotBase::SetInterface(pInterface);
    pInterface->InterfaceModeEx.bit.sdHighSpeed = dwHighSpeed;
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

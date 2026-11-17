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
// CSDHCSlotBcm2835::HandleInsertion
//
// The MDD creates its SDMA object (CSDHCSlotBaseSDMA) here on card insertion.
// Let that run, read the DMA bus-address offset from the registry, then swap in
// CSDHCSlotBcmSDMA so DMA uses VideoCore bus addresses. Insertion runs before
// the bus driver issues any transfer (card init is driven asynchronously after
// this returns), so replacing the object here is safe. When DMA is disabled
// (DisableDMA=1) the MDD leaves m_SlotDma NULL and we stay in PIO.
//
VOID
CSDHCSlotBcm2835::HandleInsertion()
{
    CSDHCSlotBase::HandleInsertion();

    m_dwDmaBusOffset = (m_pregDevice != NULL)
        ? m_pregDevice->ValueDW(SDHC_DMA_BUS_OFFSET_KEY, SDHC_DMA_BUS_OFFSET_DEFAULT)
        : SDHC_DMA_BUS_OFFSET_DEFAULT;

    if (m_SlotDma != NULL) {
        // We advertise SDMA only (GetCapabilities: ADMA2=0), so the MDD always
        // builds a CSDHCSlotBaseSDMA here; replace it with our translating one.
        delete m_SlotDma;
        m_SlotDma = new CSDHCSlotBcmSDMA(*this);
        if (m_SlotDma != NULL && !m_SlotDma->Init()) {
            delete m_SlotDma;
            m_SlotDma = NULL;       // fall back to PIO
        }
    }
}


// ---------------------------------------------------------------------------
// CSDHCSlotBcmSDMA::ArmDMA
//
// Mirrors CSDHCSlotBaseSDMA::ArmDMA, but translates the page physical addresses
// to VideoCore bus addresses before they reach the SDMA System Address
// register. GetDMABuffer() fills m_pDmaBufferList with CE physical addresses and
// sets up the cache-align bounce buffers; we then add the (page-aligned) bus
// offset to every entry. Because the offset is a multiple of PAGE_SIZE, the
// MDD's page-boundary arithmetic (GetDMALengthBit, and the re-arm writes in the
// base DMANotifyEvent that read this same list) is unaffected and also emits
// bus addresses - so only ArmDMA needs overriding.
//
BOOL
CSDHCSlotBcmSDMA::ArmDMA(SD_BUS_REQUEST& Request, BOOL fToDevice)
{
    BOOL fResult = GetDMABuffer(Request, fToDevice);
    if (fResult) {
        CSDHCSlotBcm2835& bcm = (CSDHCSlotBcm2835&) m_SDHCSloteBase;
        DWORD dwBusOffset = bcm.DmaBusOffset();

        for (DWORD i = 0; i < m_dwNumOfAvailabe; i++) {
            m_pDmaBufferList[i].physicalAddress.QuadPart += dwBusOffset;
        }

        m_fDMAProcessing = TRUE;
        m_dwDMACompletionCode = ERROR_IO_PENDING;
        m_dwCurDMAListPos = 0;
        m_dwNextOffset = GetDMALengthBit(
            m_pDmaBufferList[0].physicalAddress.LowPart,
            m_pDmaBufferList[0].dwLength);
        bcm.WriteSdmaSystemAddress(m_pDmaBufferList[0].physicalAddress.LowPart);
    }
    return fResult;
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

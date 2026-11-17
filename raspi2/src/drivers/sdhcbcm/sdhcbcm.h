//
// sdhcbcm.h
//
// BCM2835 platform-dependent driver (PDD) for the Windows CE standard SD host
// controller MDD (SDHCBase.lib / CSDHCBase + CSDHCSlotBase). The MDD does the
// SD protocol, clock, interrupt and DMA work against the Arasan eMMC core; this
// PDD only supplies the things the BCM2835 silicon makes non-standard:
//
//   * the capabilities register reads 0, so CSDHCSlotBcm2835 synthesizes it;
//   * the SDMA System Address register needs a VideoCore *bus* address, not a
//     CE physical address, because the Arasan masters the VideoCore bus. The
//     MDD writes CE physical addresses, so CSDHCSlotBcmSDMA translates them by
//     adding the DMA bus-address offset (M2);
//   * the controller object factory the MDD looks up through the registry
//     "ObjectCreationProc" value.
//
// Everything else (register window, SysIntr, base clock) comes from the device
// registry key - see files\platform.reg.
//
#ifndef _SDHCBCM_H_
#define _SDHCBCM_H_

#include <SDHC.h>       // CSDHCBase, CSDHCSlotBase, CSDHCSlotBaseSDMA, SDHC regs

// VideoCore DMA bus-address offset (registry value + default). The Arasan is a
// VideoCore bus master, so a CE physical address P is reached at bus address
// P + 0x40000000 (the L2-coherent alias, matching the USB driver's verified
// DmaBusOffset). 0xC0000000 (the uncached alias) is the fallback if cache
// coherency problems appear.
#define SDHC_DMA_BUS_OFFSET_KEY       _T("DmaBusOffset")
#define SDHC_DMA_BUS_OFFSET_DEFAULT   0x40000000


// Forward declaration: the SDMA object casts its slot reference back to this.
class CSDHCSlotBcm2835;


// ---------------------------------------------------------------------------
// SDMA object that writes VideoCore bus addresses. The stock CSDHCSlotBaseSDMA
// programs the SDMA System Address register with CE physical addresses; on the
// BCM2835 the Arasan masters the VideoCore bus and needs bus addresses. We
// override only ArmDMA: after the base GetDMABuffer() builds the page list, we
// bias every page physical address by the (page-aligned) bus offset, so the
// first write here and every re-arm write in the base DMANotifyEvent() both
// carry bus addresses, while the MDD's page-offset arithmetic stays unchanged.
//
class CSDHCSlotBcmSDMA : public CSDHCSlotBaseSDMA {
public:
    CSDHCSlotBcmSDMA(CSDHCSlotBase& slot) : CSDHCSlotBaseSDMA(slot) {}
    virtual BOOL ArmDMA(SD_BUS_REQUEST& Request, BOOL fToDevice);
};


// ---------------------------------------------------------------------------
// Slot subclass. Overrides:
//   * GetCapabilities(): the Arasan CAPABILITIES register reads 0;
//   * HandleInsertion(): swap the stock SDMA object for CSDHCSlotBcmSDMA.
//
class CSDHCSlotBcm2835 : public CSDHCSlotBase {
public:
    CSDHCSlotBcm2835()
        : CSDHCSlotBase(), m_dwDmaBusOffset(SDHC_DMA_BUS_OFFSET_DEFAULT) {}
    virtual ~CSDHCSlotBcm2835() {}

    // The DMA bus-address offset the SDMA object adds to CE physical addresses.
    DWORD DmaBusOffset() const { return m_dwDmaBusOffset; }

    // Program the SDMA System Address register. Exposed so CSDHCSlotBcmSDMA can
    // write it: the MDD's WriteDword is protected and non-virtual, so the DMA
    // object (not a CSDHCSlotBase subclass) cannot reach it directly.
    VOID WriteSdmaSystemAddress(DWORD dwBusAddr) {
        WriteDword(SDHC_SYSTEMADDRESS_LO, dwBusAddr);
    }

protected:
    // Return a synthesized capabilities value (the register reads 0): advertise
    // SDMA, high speed, 3.3 V and a 512-byte maximum block length. Base and
    // timeout clock still come from the registry.
    virtual SSDHC_CAPABILITIES GetCapabilities();

    // Let the MDD create the stock SDMA object on insertion, then replace it
    // with the bus-address-translating CSDHCSlotBcmSDMA.
    virtual VOID HandleInsertion();

    DWORD m_dwDmaBusOffset;     // VideoCore DMA bus-address offset
};


// ---------------------------------------------------------------------------
// Controller subclass. Supplies one slot of our slot type and the destruction
// proc the MDD calls on teardown.
//
class CSDHCBcm2835 : public CSDHCBase {
public:
    CSDHCBcm2835() : CSDHCBase() {}
    virtual ~CSDHCBcm2835() {}

    virtual LPSDHC_DESTRUCTION_PROC GetDestructionProc() {
        return &DestroySDHCBcmObject;
    }

    static VOID DestroySDHCBcmObject(PCSDHCBase pSDHC);

protected:
    // Use our slot subclass instead of the plain CSDHCSlotBase.
    virtual PCSDHCSlotBase AllocateSlotObjects(DWORD cSlots) {
        return new CSDHCSlotBcm2835[cSlots];
    }
};


// Creation proc named by the registry "ObjectCreationProc" value and exported
// from the DLL; the MDD's CSDHCBase::CreateSDHCControllerObject GetProcAddress'es
// it and calls it from SHC_Init.
extern "C" PCSDHCBase CreateSDHCBcmObject();

#endif // _SDHCBCM_H_

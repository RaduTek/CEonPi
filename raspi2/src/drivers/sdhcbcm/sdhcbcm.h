//
// sdhcbcm.h
//
// BCM2835 platform-dependent driver (PDD) for the Windows CE standard SD host
// controller MDD (SDHCBase.lib / CSDHCBase + CSDHCSlotBase). The MDD does the
// SD protocol, clock, interrupt and DMA work against the Arasan eMMC core; this
// PDD only supplies the two things the BCM2835 silicon makes non-standard:
//
//   * the capabilities register reads 0, so CSDHCSlotBcm2835 synthesizes it;
//   * the controller object factory the MDD looks up through the registry
//     "ObjectCreationProc" value.
//
// Everything else (register window, SysIntr, base clock) comes from the device
// registry key - see files\platform.reg.
//
#ifndef _SDHCBCM_H_
#define _SDHCBCM_H_

#include <SDHC.h>       // CSDHCBase, from the SDIOStdHC\base MDD

// ---------------------------------------------------------------------------
// Slot subclass. The only override needed for bring-up is GetCapabilities():
// the Arasan CAPABILITIES register reads 0, so the MDD would otherwise think
// the controller supports no DMA, no voltage and a 0 Hz clock.
//
class CSDHCSlotBcm2835 : public CSDHCSlotBase {
public:
    CSDHCSlotBcm2835() : CSDHCSlotBase() {}
    virtual ~CSDHCSlotBcm2835() {}

protected:
    // Return a synthesized capabilities value. The base clock and timeout
    // clock still come from the registry (BaseClockFrequency /
    // TimeoutClockFrequency); here we only advertise SDMA, high speed, 3.3 V
    // and a 512-byte maximum block length.
    virtual SSDHC_CAPABILITIES GetCapabilities();
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

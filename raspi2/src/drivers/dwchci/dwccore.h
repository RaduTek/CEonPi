//
// dwccore.h
//
// CDwcCore: direct hardware access to the DWC2 OTG controller in host mode.
// This class knows nothing about the CE USB stack.
//
#ifndef __DWCCORE_H__
#define __DWCCORE_H__

#include <windows.h>
#include "dwc2reg.h"

// PHY clock seen by the core. The BCM2835 has an 8 bit UTMI+ PHY (GHWCFG4
// utmiwidth=0, read from hardware), which runs the core at 60 MHz.
#define DWC_PHY_CLOCK_MHZ       60

// FIFO sizes in 32 bit words. The RX size is the value Linux uses for the
// BCM2835; the two TX sizes are the reset values read from hardware.
#define DWC_RX_FIFO_WORDS       774
#define DWC_NPTX_FIFO_WORDS     256
#define DWC_PTX_FIFO_WORDS      512

class CDwcCore
{
public:
    CDwcCore();
    ~CDwcCore();

    // Map / unmap the register block
    BOOL Map(void);
    void Unmap(void);

    // Read the identification and configuration registers and log them.
    // Returns FALSE if the core ID is not a DWC2 OTG core or a register
    // read faulted. Read only: it does not change any hardware state.
    BOOL DumpIdentity(void);

    // Reset the core and bring it up as a host with the root port powered.
    // Interrupts stay globally disabled until EnableInterrupts().
    BOOL HostInit(void);

    // Global interrupt gate (GAHBCFG) and mask everything on shutdown
    void EnableInterrupts(void);
    void DisableInterrupts(void);

    // Root port
    DWORD ReadPort(void) const      { return Rd(HPRT0); }
    void  AckPortChanges(DWORD dwHprt);     // clear the change bits set in dwHprt
    void  PortReset(DWORD dwMilliseconds);  // drive reset, blocks for the duration
    void  PortEnabled(DWORD dwHprt);        // program the frame interval for the speed

    DWORD Channels(void) const      { return m_dwChannels; }

    DWORD Rd(DWORD dwOffset) const
    {
        return *(volatile DWORD *)(m_pRegs + dwOffset);
    }
    void Wr(DWORD dwOffset, DWORD dwValue)
    {
        *(volatile DWORD *)(m_pRegs + dwOffset) = dwValue;
    }

private:
    BOOL WaitBits(DWORD dwOffset, DWORD dwMask, DWORD dwWanted, DWORD dwTimeoutMs);
    BOOL CoreReset(void);
    BOOL ForceHostMode(void);
    BOOL FlushTxFifos(void);
    BOOL FlushRxFifo(void);
    void HaltAllChannels(void);

    PUCHAR m_pRegs;         // virtual address of the register block
    DWORD  m_dwChannels;    // number of host channels (from GHWCFG2)
};

#endif // __DWCCORE_H__

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

    DWORD Rd(DWORD dwOffset) const
    {
        return *(volatile DWORD *)(m_pRegs + dwOffset);
    }
    void Wr(DWORD dwOffset, DWORD dwValue)
    {
        *(volatile DWORD *)(m_pRegs + dwOffset) = dwValue;
    }

private:
    PUCHAR m_pRegs;         // virtual address of the register block
    DWORD  m_dwChannels;    // number of host channels (from GHWCFG2)
};

#endif // __DWCCORE_H__

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

// Limits for one transfer attempt on a channel (values Linux uses for
// the BCM2835; the hardware fields are 19 and 10 bits wide).
#define DWC_MAX_XFER_BYTES      65535
#define DWC_MAX_XFER_PACKETS    511

// Everything needed to start one transfer attempt on a channel
typedef struct _DWC_CHANNEL_SETUP
{
    DWORD dwDevAddr;        // USB device address
    DWORD dwEpNum;          // endpoint number 0..15
    BOOL  fIn;              // direction of this attempt
    BOOL  fLowSpeed;        // low speed device
    DWORD dwEpType;         // 0 control, 1 isochronous, 2 bulk, 3 interrupt
    DWORD dwMaxPacket;      // endpoint max packet size
    DWORD dwPid;            // HCTSIZ_PID_xxx of the first packet
    DWORD dwBytes;          // transfer size programmed into the channel
    DWORD dwPackets;        // packet count programmed into the channel
    DWORD dwDmaAddr;        // bus address of the buffer
    BOOL  fOddFrame;        // periodic endpoints: send in an odd (micro)frame
} DWC_CHANNEL_SETUP;

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

    // Start-of-frame interrupt. Enabled only while an interrupt pipe is
    // waiting for its scheduled frame (see the interrupt scheduler).
    void EnableSof(void)    { Wr(GINTMSK, Rd(GINTMSK) | GINTSTS_SOF); }
    void DisableSof(void)   { Wr(GINTMSK, Rd(GINTMSK) & ~GINTSTS_SOF); Wr(GINTSTS, GINTSTS_SOF); }

    // The host frame counter (HFNUM). At high speed it counts microframes,
    // otherwise frames. It wraps at HFNUM_MAX_FRNUM.
    DWORD CurFrame(void) const { return Rd(HFNUM) & HFNUM_MAX_FRNUM; }
    // TRUE once the counter has reached dwFrame (wrap-safe over half the range)
    BOOL  FrameReached(DWORD dwFrame) const
    {
        return (((CurFrame() - dwFrame) & HFNUM_MAX_FRNUM) < ((HFNUM_MAX_FRNUM + 1) / 2)) ? TRUE : FALSE;
    }

    // Root port
    DWORD ReadPort(void) const      { return Rd(HPRT0); }
    void  AckPortChanges(DWORD dwHprt);     // clear the change bits set in dwHprt
    void  PortResetBegin(void);             // start driving reset
    void  PortResetEnd(void);               // release reset
    void  PortReset(DWORD dwMilliseconds);  // drive reset, blocks for the duration
    void  PortDisable(void);                // disable the port (device stays connected)
    void  PortPower(void);                  // make sure the port is powered
    void  PortEnabled(DWORD dwHprt);        // program the frame interval for the speed

    DWORD Channels(void) const      { return m_dwChannels; }

    // Host channels. One channel carries one transfer attempt at a time.
    void  ChannelStart(DWORD dwCh, const DWC_CHANNEL_SETUP *pSetup);
    BOOL  ChannelRequestHalt(DWORD dwCh);   // FALSE: channel is not running
    void  ChannelRelease(DWORD dwCh);       // mask the channel's interrupts
    void  ChannelForceOff(DWORD dwCh);      // disable without waiting for the halt
    DWORD ChannelReadAndClearInt(DWORD dwCh);
    DWORD ChannelBytesLeft(DWORD dwCh) const
    {
        return Rd(HCTSIZ(dwCh)) & HCTSIZ_XFERSIZE_MASK;
    }
    DWORD ChannelPacketsLeft(DWORD dwCh) const
    {
        return (Rd(HCTSIZ(dwCh)) & HCTSIZ_PKTCNT_MASK) >> HCTSIZ_PKTCNT_SHIFT;
    }
    DWORD ChannelNextPid(DWORD dwCh) const
    {
        return (Rd(HCTSIZ(dwCh)) & HCTSIZ_PID_MASK) >> HCTSIZ_PID_SHIFT;
    }
    void  DisableAllChannels(void);         // after a disconnect

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

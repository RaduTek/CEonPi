//
// dwccore.cpp
//
// CDwcCore: direct hardware access to the DWC2 OTG controller in host mode.
//
// Milestone 1: map the registers and dump the identification registers.
// Milestone 2: core reset, host initialisation, root port control.
//
#include <windows.h>
#include <ceddk.h>
#include "dwccore.h"

#define DWCMSG(x)   RETAILMSG(1, x)

// Bounded wait for a channel to clear CHENA after a halt request. On a live
// bus a halt takes microseconds; the cap only guards a channel that refuses
// to halt (e.g. the bus is already gone) so it cannot stall the caller.
#define DWC_CH_HALT_MS          20

CDwcCore::CDwcCore()
{
    m_pRegs = NULL;
    m_dwChannels = 0;
}

CDwcCore::~CDwcCore()
{
    Unmap();
}

BOOL CDwcCore::Map(void)
{
    PHYSICAL_ADDRESS pa;

    if (m_pRegs != NULL)
        return TRUE;

    pa.QuadPart = DWC_PA_BASE;
    m_pRegs = (PUCHAR)MmMapIoSpace(pa, DWC_MAP_SIZE, FALSE);

    DWCMSG((TEXT("DWCHCI: MmMapIoSpace(0x%08x, 0x%x) = 0x%08x\r\n"),
            DWC_PA_BASE, DWC_MAP_SIZE, m_pRegs));

    return (m_pRegs != NULL);
}

void CDwcCore::Unmap(void)
{
    if (m_pRegs != NULL)
    {
        MmUnmapIoSpace(m_pRegs, DWC_MAP_SIZE);
        m_pRegs = NULL;
    }
}

BOOL CDwcCore::DumpIdentity(void)
{
    BOOL fOk = FALSE;
    DWORD dwId, dwCfg2, dwCfg3, dwCfg4, dwPrt;

    if (m_pRegs == NULL)
        return FALSE;

    __try
    {
        dwId = Rd(GSNPSID);
        DWCMSG((TEXT("DWCHCI: GSNPSID  = 0x%08x  GUID = 0x%08x\r\n"), dwId, Rd(GUID)));

        dwCfg2 = Rd(GHWCFG2);
        dwCfg3 = Rd(GHWCFG3);
        dwCfg4 = Rd(GHWCFG4);
        DWCMSG((TEXT("DWCHCI: GHWCFG1  = 0x%08x\r\n"), Rd(GHWCFG1)));
        DWCMSG((TEXT("DWCHCI: GHWCFG2  = 0x%08x\r\n"), dwCfg2));
        DWCMSG((TEXT("DWCHCI: GHWCFG3  = 0x%08x\r\n"), dwCfg3));
        DWCMSG((TEXT("DWCHCI: GHWCFG4  = 0x%08x\r\n"), dwCfg4));

        m_dwChannels = ((dwCfg2 & GHWCFG2_NUM_HOST_CHAN_MASK) >> GHWCFG2_NUM_HOST_CHAN_SHIFT) + 1;

        DWCMSG((TEXT("DWCHCI:   opmode=%d arch=%d (2=internal DMA) hsphy=%d (1=UTMI) fsphy=%d\r\n"),
                dwCfg2 & GHWCFG2_OP_MODE_MASK,
                (dwCfg2 & GHWCFG2_ARCH_MASK) >> GHWCFG2_ARCH_SHIFT,
                (dwCfg2 & GHWCFG2_HS_PHY_MASK) >> GHWCFG2_HS_PHY_SHIFT,
                (dwCfg2 & GHWCFG2_FS_PHY_MASK) >> GHWCFG2_FS_PHY_SHIFT));
        DWCMSG((TEXT("DWCHCI:   channels=%d dynfifo=%d periodic=%d\r\n"),
                m_dwChannels,
                (dwCfg2 & GHWCFG2_DYNAMIC_FIFO) ? 1 : 0,
                (dwCfg2 & GHWCFG2_PERIO_EP) ? 1 : 0));
        DWCMSG((TEXT("DWCHCI:   fifodepth=%d xfersizew=%d pktsizew=%d\r\n"),
                dwCfg3 >> GHWCFG3_DFIFO_DEPTH_SHIFT,
                dwCfg3 & GHWCFG3_XFER_SIZE_W_MASK,
                (dwCfg3 & GHWCFG3_PKT_SIZE_W_MASK) >> GHWCFG3_PKT_SIZE_W_SHIFT));
        DWCMSG((TEXT("DWCHCI:   utmiwidth=%d (0=8bit 1=16bit 2=either) descdma=%d dedfifo=%d\r\n"),
                (dwCfg4 & GHWCFG4_UTMI_WIDTH_MASK) >> GHWCFG4_UTMI_WIDTH_SHIFT,
                (dwCfg4 & GHWCFG4_DESC_DMA) ? 1 : 0,
                (dwCfg4 & GHWCFG4_DED_FIFO_EN) ? 1 : 0));

        DWCMSG((TEXT("DWCHCI: GOTGCTL  = 0x%08x\r\n"), Rd(GOTGCTL)));
        DWCMSG((TEXT("DWCHCI: GAHBCFG  = 0x%08x\r\n"), Rd(GAHBCFG)));
        DWCMSG((TEXT("DWCHCI: GUSBCFG  = 0x%08x\r\n"), Rd(GUSBCFG)));
        DWCMSG((TEXT("DWCHCI: GRSTCTL  = 0x%08x\r\n"), Rd(GRSTCTL)));
        DWCMSG((TEXT("DWCHCI: GINTSTS  = 0x%08x (bit0: 1=host mode)\r\n"), Rd(GINTSTS)));
        DWCMSG((TEXT("DWCHCI: GINTMSK  = 0x%08x\r\n"), Rd(GINTMSK)));
        DWCMSG((TEXT("DWCHCI: GRXFSIZ  = 0x%08x\r\n"), Rd(GRXFSIZ)));
        DWCMSG((TEXT("DWCHCI: GNPTXFSIZ= 0x%08x\r\n"), Rd(GNPTXFSIZ)));
        DWCMSG((TEXT("DWCHCI: HPTXFSIZ = 0x%08x\r\n"), Rd(HPTXFSIZ)));
        DWCMSG((TEXT("DWCHCI: HCFG     = 0x%08x\r\n"), Rd(HCFG)));
        DWCMSG((TEXT("DWCHCI: HFIR     = 0x%08x\r\n"), Rd(HFIR)));

        dwPrt = Rd(HPRT0);
        DWCMSG((TEXT("DWCHCI: HPRT0    = 0x%08x (conn=%d ena=%d pwr=%d spd=%d)\r\n"),
                dwPrt,
                (dwPrt & HPRT0_CONNSTS) ? 1 : 0,
                (dwPrt & HPRT0_ENA) ? 1 : 0,
                (dwPrt & HPRT0_PWR) ? 1 : 0,
                (dwPrt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT));
        DWCMSG((TEXT("DWCHCI: PCGCTL   = 0x%08x\r\n"), Rd(PCGCTL)));

        if ((dwId & GSNPSID_ID_MASK) == GSNPSID_ID_OTG2)
        {
            fOk = TRUE;
        }
        else
        {
            DWCMSG((TEXT("DWCHCI: ***ERROR*** GSNPSID is not 0x4F542xxx. Wrong mapping or USB not powered.\r\n")));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** exception 0x%08x while reading registers\r\n"),
                GetExceptionCode()));
        fOk = FALSE;
    }

    return fOk;
}

//-----------------------------------------------------------------------
// Milestone 2: core reset, host initialisation and root port control.
// The sequence follows the Linux dwc2 driver (dwc2_core_reset,
// dwc2_core_init, dwc2_core_host_init), reduced to what the BCM2835 needs.
//

// Poll a register until (value & dwMask) == dwWanted. Sleep(1) between
// polls, so the granularity is about one millisecond.
BOOL CDwcCore::WaitBits(DWORD dwOffset, DWORD dwMask, DWORD dwWanted, DWORD dwTimeoutMs)
{
    DWORD dwStart = GetTickCount();

    for (;;)
    {
        if ((Rd(dwOffset) & dwMask) == dwWanted)
            return TRUE;
        if ((GetTickCount() - dwStart) > dwTimeoutMs)
            break;
        Sleep(1);
    }
    return ((Rd(dwOffset) & dwMask) == dwWanted);
}

// Core soft reset. This core is version 2.80a, so reset completion is
// signalled by CSFTRST clearing itself (cores from 4.20a use another bit).
BOOL CDwcCore::CoreReset(void)
{
    if (!WaitBits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** AHB not idle before reset, GRSTCTL=0x%08x\r\n"), Rd(GRSTCTL)));
        return FALSE;
    }

    Wr(GRSTCTL, Rd(GRSTCTL) | GRSTCTL_CSFTRST);

    if (!WaitBits(GRSTCTL, GRSTCTL_CSFTRST, 0, 100))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** core reset did not finish, GRSTCTL=0x%08x\r\n"), Rd(GRSTCTL)));
        return FALSE;
    }
    if (!WaitBits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** AHB not idle after reset, GRSTCTL=0x%08x\r\n"), Rd(GRSTCTL)));
        return FALSE;
    }
    return TRUE;
}

// Force host mode so the driver does not depend on the OTG ID pin.
BOOL CDwcCore::ForceHostMode(void)
{
    DWORD dwCfg = Rd(GUSBCFG);

    dwCfg &= ~GUSBCFG_FORCEDEVMODE;
    dwCfg |= GUSBCFG_FORCEHOSTMODE;
    Wr(GUSBCFG, dwCfg);

    if (!WaitBits(GINTSTS, GINTSTS_CURMODE_HOST, GINTSTS_CURMODE_HOST, 200))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** core did not enter host mode, GINTSTS=0x%08x\r\n"), Rd(GINTSTS)));
        return FALSE;
    }
    return TRUE;
}

BOOL CDwcCore::FlushTxFifos(void)
{
    WaitBits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100);
    Wr(GRSTCTL, GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL);
    if (!WaitBits(GRSTCTL, GRSTCTL_TXFFLSH, 0, 100))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** TX FIFO flush timeout\r\n")));
        return FALSE;
    }
    return TRUE;
}

BOOL CDwcCore::FlushRxFifo(void)
{
    WaitBits(GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 100);
    Wr(GRSTCTL, GRSTCTL_RXFFLSH);
    if (!WaitBits(GRSTCTL, GRSTCTL_RXFFLSH, 0, 100))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** RX FIFO flush timeout\r\n")));
        return FALSE;
    }
    return TRUE;
}

// Halt every channel that is still enabled (left over from the firmware).
void CDwcCore::HaltAllChannels(void)
{
    DWORD i, dwChar;

    for (i = 0; i < m_dwChannels; i++)
    {
        dwChar = Rd(HCCHAR(i));
        if (dwChar & HCCHAR_CHENA)
        {
            dwChar &= ~HCCHAR_EPDIR_IN;
            dwChar |= HCCHAR_CHENA | HCCHAR_CHDIS;
            Wr(HCCHAR(i), dwChar);
            if (!WaitBits(HCCHAR(i), HCCHAR_CHENA, 0, 50))
            {
                DWCMSG((TEXT("DWCHCI: channel %d did not halt, HCCHAR=0x%08x\r\n"), i, Rd(HCCHAR(i))));
            }
        }
        Wr(HCINTMSK(i), 0);
        Wr(HCINT(i), HCINT_ALL);
    }
}

BOOL CDwcCore::HostInit(void)
{
    DWORD dwCfg, dwNew, dwDepth, dwRx, dwNp, dwP, dwHprt;

    if (m_pRegs == NULL)
        return FALSE;

    if (m_dwChannels == 0)
        m_dwChannels = ((Rd(GHWCFG2) & GHWCFG2_NUM_HOST_CHAN_MASK) >> GHWCFG2_NUM_HOST_CHAN_SHIFT) + 1;

    // No interrupts while the core is set up
    DisableInterrupts();

    dwCfg = Rd(GUSBCFG);
    dwCfg &= ~(GUSBCFG_ULPI_EXT_VBUS_DRV | GUSBCFG_TERMSELDLPULSE);
    Wr(GUSBCFG, dwCfg);

    if (!CoreReset())
        return FALSE;
    if (!ForceHostMode())
        return FALSE;

    // PHY: UTMI+ with an 8 bit interface. A change of PHY selection only
    // takes effect after another core reset.
    dwCfg = Rd(GUSBCFG);
    dwNew = dwCfg & ~(GUSBCFG_ULPI_UTMI_SEL | GUSBCFG_PHYIF16);
    if (dwNew != dwCfg)
    {
        DWCMSG((TEXT("DWCHCI: PHY select changed, GUSBCFG 0x%08x -> 0x%08x, resetting again\r\n"), dwCfg, dwNew));
        Wr(GUSBCFG, dwNew);
        if (!CoreReset())
            return FALSE;
        if (!ForceHostMode())
            return FALSE;
    }

    // Host only: no ULPI low power clocking, no HNP or SRP
    dwCfg = Rd(GUSBCFG);
    dwCfg &= ~(GUSBCFG_ULPI_FS_LS | GUSBCFG_ULPI_CLK_SUSP_M);
    dwCfg &= ~(GUSBCFG_HNPCAP | GUSBCFG_SRPCAP);
    Wr(GUSBCFG, dwCfg);

    // Internal DMA on. Bit 4 is the BCM2835 specific "wait for all AXI
    // writes" bit that Linux sets for this SoC. Global interrupt stays off.
    Wr(GAHBCFG, GAHBCFG_DMA_EN | GAHBCFG_BCM_WAIT_AXI_WR);

    // Clear anything pending
    Wr(GOTGINT, 0xFFFFFFFF);
    Wr(GINTSTS, 0xFFFFFFFF);

    // Restart the PHY clock
    Wr(PCGCTL, 0);

    // 30/60 MHz PHY clock for all speeds, high speed allowed
    dwCfg = Rd(HCFG);
    dwCfg &= ~(HCFG_FSLSPCLKSEL_MASK | HCFG_FSLSSUPP);
    Wr(HCFG, dwCfg);

    // FIFO layout: RX, then non-periodic TX, then periodic TX
    dwDepth = Rd(GHWCFG3) >> GHWCFG3_DFIFO_DEPTH_SHIFT;
    dwRx = DWC_RX_FIFO_WORDS;
    dwNp = DWC_NPTX_FIFO_WORDS;
    dwP = DWC_PTX_FIFO_WORDS;
    if ((dwRx + dwNp + dwP) > dwDepth)
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** FIFOs (%d words) do not fit in %d\r\n"), dwRx + dwNp + dwP, dwDepth));
        return FALSE;
    }
    Wr(GRXFSIZ, dwRx);
    Wr(GNPTXFSIZ, (dwNp << 16) | dwRx);
    Wr(HPTXFSIZ, (dwP << 16) | (dwRx + dwNp));

    Wr(GOTGCTL, Rd(GOTGCTL) & ~GOTGCTL_HSTSETHNPEN);

    if (!FlushTxFifos())
        return FALSE;
    if (!FlushRxFifo())
        return FALSE;

    HaltAllChannels();

    // Power the root port. On the Pi Zero VBUS is wired to 5V permanently,
    // but the core only reports a connection when the port is powered.
    dwHprt = Rd(HPRT0) & ~HPRT0_W1C_MASK;
    if (!(dwHprt & HPRT0_PWR))
        Wr(HPRT0, dwHprt | HPRT0_PWR);

    // Port, disconnect and channel interrupts. Channels are unmasked
    // one by one in HAINTMSK when they are used.
    Wr(HAINTMSK, 0);
    Wr(GINTMSK, GINTSTS_PRTINT | GINTSTS_DISCONNINT | GINTSTS_HCHINT);

    DWCMSG((TEXT("DWCHCI: host init done. GUSBCFG=0x%08x GAHBCFG=0x%08x HCFG=0x%08x\r\n"),
            Rd(GUSBCFG), Rd(GAHBCFG), Rd(HCFG)));
    DWCMSG((TEXT("DWCHCI:   GRXFSIZ=0x%08x GNPTXFSIZ=0x%08x HPTXFSIZ=0x%08x\r\n"),
            Rd(GRXFSIZ), Rd(GNPTXFSIZ), Rd(HPTXFSIZ)));
    DWCMSG((TEXT("DWCHCI:   GINTSTS=0x%08x GOTGCTL=0x%08x HPRT0=0x%08x\r\n"),
            Rd(GINTSTS), Rd(GOTGCTL), Rd(HPRT0)));
    return TRUE;
}

void CDwcCore::EnableInterrupts(void)
{
    Wr(GAHBCFG, Rd(GAHBCFG) | GAHBCFG_GLBL_INTR_EN);
}

void CDwcCore::DisableInterrupts(void)
{
    if (m_pRegs == NULL)
        return;
    Wr(GAHBCFG, Rd(GAHBCFG) & ~GAHBCFG_GLBL_INTR_EN);
    Wr(GINTMSK, 0);
}

// Clear the port change bits that are set in dwHprt (a value read from
// HPRT0). The enable bit is forced to 0: writing 1 to it disables the port.
void CDwcCore::AckPortChanges(DWORD dwHprt)
{
    DWORD dwAck = dwHprt & (HPRT0_CONNDET | HPRT0_ENACHG | HPRT0_OVRCURRCHG);

    Wr(HPRT0, (dwHprt & ~HPRT0_W1C_MASK) | dwAck);
}

// Drive a bus reset on the root port. USB requires at least 50 ms for a
// root port. The port enables itself after the reset if a device answers.
void CDwcCore::PortReset(DWORD dwMilliseconds)
{
    PortResetBegin();
    Sleep(dwMilliseconds);
    PortResetEnd();
}

void CDwcCore::PortResetBegin(void)
{
    DWORD dwHprt;

    Wr(PCGCTL, 0);

    dwHprt = Rd(HPRT0) & ~HPRT0_W1C_MASK;
    dwHprt &= ~HPRT0_SUSP;
    Wr(HPRT0, dwHprt | HPRT0_PWR | HPRT0_RST);
}

void CDwcCore::PortResetEnd(void)
{
    DWORD dwHprt = Rd(HPRT0) & ~HPRT0_W1C_MASK;

    Wr(HPRT0, dwHprt & ~HPRT0_RST);
}

// Writing 1 to the enable bit disables the port
void CDwcCore::PortDisable(void)
{
    DWORD dwHprt = Rd(HPRT0) & ~HPRT0_W1C_MASK;

    Wr(HPRT0, dwHprt | HPRT0_ENA);
}

void CDwcCore::PortPower(void)
{
    DWORD dwHprt = Rd(HPRT0) & ~HPRT0_W1C_MASK;

    if (!(dwHprt & HPRT0_PWR))
        Wr(HPRT0, dwHprt | HPRT0_PWR);
}

// Called when the port has become enabled. The frame interval depends on
// the speed of the attached device: 125 us for high speed, 1 ms otherwise.
void CDwcCore::PortEnabled(DWORD dwHprt)
{
    DWORD dwSpeed = (dwHprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT;
    DWORD dwFrInt, dwHfir;

    if (dwSpeed == HPRT0_SPD_HIGH)
        dwFrInt = 125 * DWC_PHY_CLOCK_MHZ - 1;
    else
        dwFrInt = 1000 * DWC_PHY_CLOCK_MHZ - 1;

    dwHfir = Rd(HFIR);
    dwHfir &= ~HFIR_FRINT_MASK;
    dwHfir |= dwFrInt;
    Wr(HFIR, dwHfir);
}

//-----------------------------------------------------------------------
// Milestone 3: host channels (buffer DMA mode, no split transactions).
// Follows Linux dwc2_hc_init and dwc2_hc_start_transfer.
//

// Program a channel and enable it. In DMA mode the core runs the whole
// attempt by itself and raises "channel halted" when it stops, whatever
// the reason. The reason is in the other HCINT bits.
void CDwcCore::ChannelStart(DWORD dwCh, const DWC_CHANNEL_SETUP *pSetup)
{
    DWORD dwChar;

    // Clear old events, enable only halted and bus error
    Wr(HCINT(dwCh), HCINT_ALL);
    Wr(HCINTMSK(dwCh), HCINT_CHHLTD | HCINT_AHBERR);
    Wr(HAINTMSK, Rd(HAINTMSK) | DWC_BIT(dwCh));

    dwChar = (pSetup->dwMaxPacket & HCCHAR_MPS_MASK)
           | ((pSetup->dwEpNum & 0xF) << HCCHAR_EPNUM_SHIFT)
           | ((pSetup->dwEpType & 0x3) << HCCHAR_EPTYPE_SHIFT)
           | (1UL << HCCHAR_MULTICNT_SHIFT)
           | ((pSetup->dwDevAddr & 0x7F) << HCCHAR_DEVADDR_SHIFT);
    if (pSetup->fIn)
        dwChar |= HCCHAR_EPDIR_IN;
    if (pSetup->fLowSpeed)
        dwChar |= HCCHAR_LSPDDEV;
    if (pSetup->fOddFrame)
        dwChar |= HCCHAR_ODDFRM;

    Wr(HCCHAR(dwCh), dwChar);
    Wr(HCSPLT(dwCh), 0);
    Wr(HCTSIZ(dwCh), (pSetup->dwBytes & HCTSIZ_XFERSIZE_MASK)
                   | ((pSetup->dwPackets << HCTSIZ_PKTCNT_SHIFT) & HCTSIZ_PKTCNT_MASK)
                   | ((pSetup->dwPid << HCTSIZ_PID_SHIFT) & HCTSIZ_PID_MASK));
    Wr(HCDMA(dwCh), pSetup->dwDmaAddr);

    // Enable last
    Wr(HCCHAR(dwCh), dwChar | HCCHAR_CHENA);
}

// Ask the core to stop a running channel. The core answers with a
// "channel halted" interrupt. Returns FALSE if the channel is not enabled,
// in which case no interrupt will come.
BOOL CDwcCore::ChannelRequestHalt(DWORD dwCh)
{
    DWORD dwChar = Rd(HCCHAR(dwCh));

    if (!(dwChar & HCCHAR_CHENA))
        return FALSE;

    Wr(HCINTMSK(dwCh), HCINT_CHHLTD);
    Wr(HCCHAR(dwCh), dwChar | HCCHAR_CHENA | HCCHAR_CHDIS);
    return TRUE;
}

void CDwcCore::ChannelRelease(DWORD dwCh)
{
    Wr(HAINTMSK, Rd(HAINTMSK) & ~DWC_BIT(dwCh));
    Wr(HCINTMSK(dwCh), 0);
    Wr(HCINT(dwCh), HCINT_ALL);
}

// Last resort when a halt request is not answered (port disabled)
void CDwcCore::ChannelForceOff(DWORD dwCh)
{
    DWORD dwChar = Rd(HCCHAR(dwCh));

    if (dwChar & HCCHAR_CHENA)
    {
        dwChar &= ~HCCHAR_CHENA;
        dwChar |= HCCHAR_CHDIS;
        Wr(HCCHAR(dwCh), dwChar);
    }
    ChannelRelease(dwCh);
}

DWORD CDwcCore::ChannelReadAndClearInt(DWORD dwCh)
{
    DWORD dwInt = Rd(HCINT(dwCh));

    Wr(HCINT(dwCh), dwInt);
    return dwInt;
}

// The device is gone and the port is disabled, so running channels will
// never finish by themselves. Halt each one, then drop whatever is left in
// the FIFOs.
//
// The halt must be done the same way as HaltAllChannels(): the core only
// acts on CHDIS when CHENA is written with it, and an IN channel must first
// be turned around to OUT (EPDIR = 0) or the core keeps waiting for the
// in-flight IN token, which never completes once the device has gone. A
// plain "clear CHENA, set CHDIS" leaves a busy IN channel wedged. That is
// harmless for a single idle HID device but corrupts the FIFOs for the next
// enumeration when several interrupt-IN channels are active at once (as on a
// composite device such as a wireless keyboard/mouse receiver), which then
// shows up as a device that will not re-enumerate.
void CDwcCore::DisableAllChannels(void)
{
    DWORD i, dwChar, dwActive = 0, dwStuck = 0;

    for (i = 0; i < m_dwChannels; i++)
    {
        dwChar = Rd(HCCHAR(i));
        if (dwChar & HCCHAR_CHENA)
        {
            dwActive++;
            dwChar &= ~HCCHAR_EPDIR_IN;
            dwChar |= HCCHAR_CHENA | HCCHAR_CHDIS;
            Wr(HCCHAR(i), dwChar);
            // The channel halts almost at once on a dead bus; bound the wait
            // so a channel that will not halt cannot stall the disconnect.
            if (!WaitBits(HCCHAR(i), HCCHAR_CHENA, 0, DWC_CH_HALT_MS))
                dwStuck++;
        }
        ChannelRelease(i);
    }
    FlushTxFifos();
    FlushRxFifo();

    // One line on disconnect (rare), so the log shows how many channels were
    // still busy and whether any refused to halt.
    if (dwActive != 0)
        DWCMSG((TEXT("DWCHCI: disconnect halted %d active channel(s), %d stuck\r\n"),
                dwActive, dwStuck));
}

//
// dwccore.cpp
//
// CDwcCore: direct hardware access to the DWC2 OTG controller in host mode.
//
// Milestone 1: map the registers and dump the identification registers.
// Nothing is written to the hardware yet.
//
#include <windows.h>
#include <ceddk.h>
#include "dwccore.h"

#define DWCMSG(x)   RETAILMSG(1, x)

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

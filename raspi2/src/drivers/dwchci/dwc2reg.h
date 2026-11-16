//
// dwc2reg.h
//
// Register offsets and bit masks for the Synopsys DesignWare Hi-Speed USB 2.0
// OTG controller (DWC2) as used in the BCM2835, host mode only.
//
// Offsets and bit positions follow the Linux dwc2 driver (hw.h, which is
// dual licensed GPL-2.0+ OR BSD-3-Clause). Nothing here has been verified
// against hardware yet unless a comment says so.
//
// Plain masks are used on purpose instead of bitfield structs: several
// registers (HPRT0 above all) have write-1-to-clear bits, and a bitfield
// assignment compiles to a read-modify-write that would clear them.
//
#ifndef __DWC2REG_H__
#define __DWC2REG_H__

// Physical base and size of the register block
#define DWC_PA_BASE             0x20980000
#define DWC_MAP_SIZE            0x1000

#define DWC_BIT(n)              (1UL << (n))

//-----------------------------------------------------------------------
// Core global registers
//
#define GOTGCTL                 0x000
#define GOTGCTL_HSTSETHNPEN     DWC_BIT(10)
#define GOTGCTL_CONID_B         DWC_BIT(16)
#define GOTGCTL_VBVALOVAL       DWC_BIT(3)
#define GOTGCTL_VBVALOEN        DWC_BIT(2)

#define GOTGINT                 0x004

#define GAHBCFG                 0x008
#define GAHBCFG_GLBL_INTR_EN    DWC_BIT(0)
#define GAHBCFG_BCM_WAIT_AXI_WR DWC_BIT(4)      // BCM2835 specific meaning
#define GAHBCFG_DMA_EN          DWC_BIT(5)

#define GUSBCFG                 0x00C
#define GUSBCFG_TOUTCAL_MASK    0x00000007
#define GUSBCFG_PHYIF16         DWC_BIT(3)
#define GUSBCFG_ULPI_UTMI_SEL   DWC_BIT(4)      // 1 = ULPI, 0 = UTMI+
#define GUSBCFG_PHYSEL          DWC_BIT(6)
#define GUSBCFG_SRPCAP          DWC_BIT(8)
#define GUSBCFG_HNPCAP          DWC_BIT(9)
#define GUSBCFG_USBTRDTIM_SHIFT 10              // USB turnaround time, in PHY clocks
#define GUSBCFG_USBTRDTIM_MASK  (0xFUL << 10)
#define GUSBCFG_USBTRDTIM_8BIT  9               // for an 8 bit UTMI+ PHY (16 bit = 5)
#define GUSBCFG_ULPI_FS_LS      DWC_BIT(17)
#define GUSBCFG_ULPI_CLK_SUSP_M DWC_BIT(19)
#define GUSBCFG_ULPI_EXT_VBUS_DRV DWC_BIT(20)
#define GUSBCFG_TERMSELDLPULSE  DWC_BIT(22)
#define GUSBCFG_FORCEHOSTMODE   DWC_BIT(29)
#define GUSBCFG_FORCEDEVMODE    DWC_BIT(30)

#define GRSTCTL                 0x010
#define GRSTCTL_CSFTRST         DWC_BIT(0)
#define GRSTCTL_RXFFLSH         DWC_BIT(4)
#define GRSTCTL_TXFFLSH         DWC_BIT(5)
#define GRSTCTL_TXFNUM_SHIFT    6
#define GRSTCTL_TXFNUM_ALL      (0x10UL << GRSTCTL_TXFNUM_SHIFT)
#define GRSTCTL_CSFTRST_DONE    DWC_BIT(29)
#define GRSTCTL_AHBIDLE         DWC_BIT(31)

#define GINTSTS                 0x014
#define GINTMSK                 0x018
#define GINTSTS_CURMODE_HOST    DWC_BIT(0)
#define GINTSTS_MODEMIS         DWC_BIT(1)
#define GINTSTS_OTGINT          DWC_BIT(2)
#define GINTSTS_SOF             DWC_BIT(3)
#define GINTSTS_RXFLVL          DWC_BIT(4)
#define GINTSTS_NPTXFEMP        DWC_BIT(5)
#define GINTSTS_PRTINT          DWC_BIT(24)
#define GINTSTS_HCHINT          DWC_BIT(25)
#define GINTSTS_PTXFEMP         DWC_BIT(26)
#define GINTSTS_CONIDSTSCHNG    DWC_BIT(28)
#define GINTSTS_DISCONNINT      DWC_BIT(29)
#define GINTSTS_SESSREQINT      DWC_BIT(30)
#define GINTSTS_WKUPINT         DWC_BIT(31)

#define GRXSTSR                 0x01C
#define GRXSTSP                 0x020           // reading this one pops the FIFO
#define GRXFSIZ                 0x024
#define GNPTXFSIZ               0x028           // depth 31:16, start address 15:0
#define GNPTXSTS                0x02C
#define GUID                    0x03C
#define GSNPSID                 0x040
#define GSNPSID_ID_MASK         0xFFFFF000
#define GSNPSID_ID_OTG2         0x4F542000      // "OT2"

#define GHWCFG1                 0x044
#define GHWCFG2                 0x048
#define GHWCFG2_OP_MODE_MASK        0x00000007
#define GHWCFG2_ARCH_SHIFT          3
#define GHWCFG2_ARCH_MASK           (0x3UL << 3)
#define GHWCFG2_ARCH_INT_DMA        2
#define GHWCFG2_HS_PHY_SHIFT        6
#define GHWCFG2_HS_PHY_MASK         (0x3UL << 6)    // 1 = UTMI+, 2 = ULPI, 3 = both
#define GHWCFG2_FS_PHY_SHIFT        8
#define GHWCFG2_FS_PHY_MASK         (0x3UL << 8)
#define GHWCFG2_NUM_HOST_CHAN_SHIFT 14
#define GHWCFG2_NUM_HOST_CHAN_MASK  (0xFUL << 14)   // value + 1 channels
#define GHWCFG2_PERIO_EP            DWC_BIT(18)
#define GHWCFG2_DYNAMIC_FIFO        DWC_BIT(19)

#define GHWCFG3                 0x04C
#define GHWCFG3_XFER_SIZE_W_MASK    0x0000000F
#define GHWCFG3_PKT_SIZE_W_SHIFT    4
#define GHWCFG3_PKT_SIZE_W_MASK     (0x7UL << 4)
#define GHWCFG3_DFIFO_DEPTH_SHIFT   16

#define GHWCFG4                 0x050
#define GHWCFG4_UTMI_WIDTH_SHIFT    14
#define GHWCFG4_UTMI_WIDTH_MASK     (0x3UL << 14)   // 0 = 8 bit, 1 = 16 bit, 2 = 8 or 16
#define GHWCFG4_DED_FIFO_EN         DWC_BIT(25)
#define GHWCFG4_DESC_DMA            DWC_BIT(30)

#define HPTXFSIZ                0x100           // depth 31:16, start address 15:0

//-----------------------------------------------------------------------
// Host mode registers
//
#define HCFG                    0x400
#define HCFG_FSLSPCLKSEL_MASK   0x00000003      // 0 = 30/60 MHz, 1 = 48 MHz, 2 = 6 MHz
#define HCFG_FSLSSUPP           DWC_BIT(2)

#define HFIR                    0x404
#define HFIR_FRINT_MASK         0x0000FFFF
#define HFIR_RLDCTRL            DWC_BIT(16)

#define HFNUM                   0x408
#define HFNUM_FRNUM_MASK        0x0000FFFF
#define HFNUM_MAX_FRNUM         0x3FFF

#define HPTXSTS                 0x410
#define HAINT                   0x414
#define HAINTMSK                0x418

#define HPRT0                   0x440
#define HPRT0_CONNSTS           DWC_BIT(0)
#define HPRT0_CONNDET           DWC_BIT(1)      // write 1 to clear
#define HPRT0_ENA               DWC_BIT(2)      // write 1 to clear: DISABLES the port
#define HPRT0_ENACHG            DWC_BIT(3)      // write 1 to clear
#define HPRT0_OVRCURRACT        DWC_BIT(4)
#define HPRT0_OVRCURRCHG        DWC_BIT(5)      // write 1 to clear
#define HPRT0_RES               DWC_BIT(6)
#define HPRT0_SUSP              DWC_BIT(7)
#define HPRT0_RST               DWC_BIT(8)
#define HPRT0_LNSTS_SHIFT       10
#define HPRT0_LNSTS_MASK        (0x3UL << 10)
#define HPRT0_PWR               DWC_BIT(12)
#define HPRT0_SPD_SHIFT         17
#define HPRT0_SPD_MASK          (0x3UL << 17)
#define HPRT0_SPD_HIGH          0
#define HPRT0_SPD_FULL          1
#define HPRT0_SPD_LOW           2
// Bits that must be written as 0 in any read-modify-write of HPRT0
#define HPRT0_W1C_MASK          (HPRT0_CONNDET | HPRT0_ENA | HPRT0_ENACHG | HPRT0_OVRCURRCHG)

// Host channel registers, n = 0 .. channels-1
#define HCCHAR(n)               (0x500 + 0x20 * (n))
#define HCCHAR_MPS_MASK         0x000007FF
#define HCCHAR_EPNUM_SHIFT      11
#define HCCHAR_EPDIR_IN         DWC_BIT(15)
#define HCCHAR_LSPDDEV          DWC_BIT(17)
#define HCCHAR_EPTYPE_SHIFT     18              // 0 ctrl, 1 isoc, 2 bulk, 3 intr
#define HCCHAR_MULTICNT_SHIFT   20
#define HCCHAR_DEVADDR_SHIFT    22
#define HCCHAR_ODDFRM           DWC_BIT(29)
#define HCCHAR_CHDIS            DWC_BIT(30)
#define HCCHAR_CHENA            DWC_BIT(31)

#define HCSPLT(n)               (0x504 + 0x20 * (n))
// Split-transaction control for a full/low speed device behind a high speed
// hub's transaction translator (TT). XACTPOS encoding is not numeric order.
#define HCSPLT_SPLTENA          DWC_BIT(31)
#define HCSPLT_COMPSPLT         DWC_BIT(16)     // 0 = start-split, 1 = complete-split
#define HCSPLT_XACTPOS_SHIFT    14
#define HCSPLT_XACTPOS_MASK     (0x3UL << 14)
#define HCSPLT_XACTPOS_MID      0
#define HCSPLT_XACTPOS_END      1
#define HCSPLT_XACTPOS_BEGIN    2
#define HCSPLT_XACTPOS_ALL      3               // control/bulk/interrupt use ALL
#define HCSPLT_HUBADDR_SHIFT    7
#define HCSPLT_HUBADDR_MASK     (0x7FUL << 7)   // TT hub device address
#define HCSPLT_PRTADDR_SHIFT    0
#define HCSPLT_PRTADDR_MASK     (0x7FUL << 0)   // hub port the device hangs off

#define HCINT(n)                (0x508 + 0x20 * (n))
#define HCINTMSK(n)             (0x50C + 0x20 * (n))
#define HCINT_XFERCOMPL         DWC_BIT(0)
#define HCINT_CHHLTD            DWC_BIT(1)
#define HCINT_AHBERR            DWC_BIT(2)
#define HCINT_STALL             DWC_BIT(3)
#define HCINT_NAK               DWC_BIT(4)
#define HCINT_ACK               DWC_BIT(5)
#define HCINT_NYET              DWC_BIT(6)
#define HCINT_XACTERR           DWC_BIT(7)
#define HCINT_BBLERR            DWC_BIT(8)
#define HCINT_FRMOVRUN          DWC_BIT(9)
#define HCINT_DATATGLERR        DWC_BIT(10)
#define HCINT_ALL               0x00003FFF

#define HCTSIZ(n)               (0x510 + 0x20 * (n))
#define HCTSIZ_XFERSIZE_MASK    0x0007FFFF
#define HCTSIZ_PKTCNT_SHIFT     19
#define HCTSIZ_PKTCNT_MASK      (0x3FFUL << 19)
#define HCTSIZ_PID_SHIFT        29
#define HCTSIZ_PID_MASK         (0x3UL << 29)
#define HCTSIZ_PID_DATA0        0
#define HCTSIZ_PID_DATA2        1
#define HCTSIZ_PID_DATA1        2
#define HCTSIZ_PID_SETUP        3

#define HCDMA(n)                (0x514 + 0x20 * (n))

//-----------------------------------------------------------------------
// Power and clock gating
//
#define PCGCTL                  0xE00

#endif // __DWC2REG_H__

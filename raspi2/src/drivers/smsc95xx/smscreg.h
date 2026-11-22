//
// smscreg.h
//
// Register definitions for the SMSC LAN9512/LAN9514 USB 2.0 hub + 10/100
// Ethernet controller (the Ethernet function enumerates as 0424:ec00).
//
// Adapted from FreeBSD sys/dev/usb/net/if_smscreg.h, which carries this
// licence (BSD-2-Clause):
//
//   Copyright (c) 2012
//      Ben Gray <bgray@freebsd.org>.
//   All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions
//   are met:
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//   THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
//   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//   ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
//   FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
//   OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
//   HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
//   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
//   OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
//   SUCH DAMAGE.
//
// FreeBSD notes that the information was gleaned from the SMSC driver in the
// Linux kernel. Values below are as in the FreeBSD header; the MII_ADDR
// field layout and the standard MII register numbers are added here.
//
#ifndef __SMSCREG_H__
#define __SMSCREG_H__

// USB IDs
#define SMSC_VID                    0x0424
#define SMSC_PID_LAN95XX            0xEC00

// USB vendor requests (4-byte little-endian register value, wIndex = register)
#define SMSC_UR_WRITE_REG           0xA0
#define SMSC_UR_READ_REG            0xA1
#define SMSC_UR_GET_STATS           0xA2

//
// TX frame header: 8 bytes in front of every frame sent on bulk OUT.
//   TX_CTRL_0 <20:16> data offset, <13> first segment, <12> last segment,
//             <10:0> buffer size
//   TX_CTRL_1 <14> H/W checksum, <13> disable CRC, <12> disable padding,
//             <10:0> packet length
//
#define SMSC_TX_CTRL_0_OFFSET(x)        (((x) & 0x1FUL) << 16)
#define SMSC_TX_CTRL_0_FIRST_SEG        (0x1UL << 13)
#define SMSC_TX_CTRL_0_LAST_SEG         (0x1UL << 12)
#define SMSC_TX_CTRL_0_BUF_SIZE(x)      ((x) & 0x000007FFUL)

#define SMSC_TX_CTRL_1_CSUM_ENABLE      (0x1UL << 14)
#define SMSC_TX_CTRL_1_CRC_DISABLE      (0x1UL << 13)
#define SMSC_TX_CTRL_1_PADDING_DISABLE  (0x1UL << 12)
#define SMSC_TX_CTRL_1_PKT_LENGTH(x)    ((x) & 0x000007FFUL)

//
// RX status word: 4 bytes in front of every frame received on bulk IN.
//
#define SMSC_RX_STAT_FILTER_FAIL        (0x1UL << 30)
#define SMSC_RX_STAT_FRM_LENGTH(x)      (((x) >> 16) & 0x3FFFUL)
#define SMSC_RX_STAT_ERROR              (0x1UL << 15)
#define SMSC_RX_STAT_BROADCAST          (0x1UL << 13)
#define SMSC_RX_STAT_LENGTH_ERROR       (0x1UL << 12)
#define SMSC_RX_STAT_RUNT               (0x1UL << 11)
#define SMSC_RX_STAT_MULTICAST          (0x1UL << 10)
#define SMSC_RX_STAT_FRM_TO_LONG        (0x1UL << 7)
#define SMSC_RX_STAT_COLLISION          (0x1UL << 6)
#define SMSC_RX_STAT_FRM_TYPE           (0x1UL << 5)
#define SMSC_RX_STAT_WATCHDOG           (0x1UL << 4)
#define SMSC_RX_STAT_MII_ERROR          (0x1UL << 3)
#define SMSC_RX_STAT_DRIBBLING          (0x1UL << 2)
#define SMSC_RX_STAT_CRC_ERROR          (0x1UL << 1)

//
// Registers
//
#define SMSC_ID_REV                 0x000
#define SMSC_INTR_STATUS            0x008
#define SMSC_RX_CFG                 0x00C
#define SMSC_TX_CFG                 0x010
#define SMSC_HW_CFG                 0x014
#define SMSC_PM_CTRL                0x020
#define SMSC_LED_GPIO_CFG           0x024
#define SMSC_GPIO_CFG               0x028
#define SMSC_AFC_CFG                0x02C
#define SMSC_EEPROM_CMD             0x030
#define SMSC_EEPROM_DATA            0x034
#define SMSC_BURST_CAP              0x038
#define SMSC_GPIO_WAKE              0x064
#define SMSC_INTR_CFG               0x068
#define SMSC_BULK_IN_DLY            0x06C
#define SMSC_MAC_CSR                0x100
#define SMSC_MAC_ADDRH              0x104
#define SMSC_MAC_ADDRL              0x108
#define SMSC_HASHH                  0x10C
#define SMSC_HASHL                  0x110
#define SMSC_MII_ADDR               0x114
#define SMSC_MII_DATA               0x118
#define SMSC_FLOW                   0x11C
#define SMSC_VLAN1                  0x120
#define SMSC_VLAN2                  0x124
#define SMSC_WUFF                   0x128
#define SMSC_WUCSR                  0x12C
#define SMSC_COE_CTRL               0x130

// ID / revision
#define SMSC_ID_REV_CHIP_ID_MASK    0xFFFF0000UL
#define SMSC_ID_REV_CHIP_REV_MASK   0x0000FFFFUL

// RX_CFG
#define SMSC_RX_FIFO_FLUSH          (0x1UL << 0)

// TX_CFG
#define SMSC_TX_CFG_ON              (0x1UL << 2)
#define SMSC_TX_CFG_STOP            (0x1UL << 1)
#define SMSC_TX_CFG_FIFO_FLUSH      (0x1UL << 0)

// HW_CFG
#define SMSC_HW_CFG_BIR             (0x1UL << 12)
#define SMSC_HW_CFG_LEDB            (0x1UL << 11)
#define SMSC_HW_CFG_RXDOFF          (0x3UL << 9)    // RX packet alignment offset
#define SMSC_HW_CFG_DRP             (0x1UL << 6)
#define SMSC_HW_CFG_MEF             (0x1UL << 5)    // multiple Ethernet frames per USB packet
#define SMSC_HW_CFG_LRST            (0x1UL << 3)    // lite reset
#define SMSC_HW_CFG_PSEL            (0x1UL << 2)    // external PHY select
#define SMSC_HW_CFG_BCE             (0x1UL << 1)    // burst cap enable
#define SMSC_HW_CFG_SRST            (0x1UL << 0)

// PM_CTRL
#define SMSC_PM_CTRL_PHY_RST        (0x1UL << 4)    // PHY reset

// LED_GPIO_CFG
#define SMSC_LED_GPIO_CFG_SPD_LED   (0x1UL << 24)
#define SMSC_LED_GPIO_CFG_LNK_LED   (0x1UL << 20)
#define SMSC_LED_GPIO_CFG_FDX_LED   (0x1UL << 16)

// AFC_CFG: high watermark 15.5 KB, low 3 KB, backpressure ~350 us, FC on any frame
#define SMSC_AFC_CFG_DEFAULT        0x00F830A1UL

// EEPROM_CMD
#define SMSC_EEPROM_CMD_BUSY        (0x1UL << 31)
#define SMSC_EEPROM_CMD_MASK        (0x7UL << 28)
#define SMSC_EEPROM_CMD_READ        (0x0UL << 28)
#define SMSC_EEPROM_CMD_WRITE       (0x3UL << 28)
#define SMSC_EEPROM_CMD_ERASE       (0x5UL << 28)
#define SMSC_EEPROM_CMD_RELOAD      (0x7UL << 28)
#define SMSC_EEPROM_CMD_TIMEOUT     (0x1UL << 10)
#define SMSC_EEPROM_CMD_ADDR_MASK   0x000001FFUL

// MAC_CSR
#define SMSC_MAC_CSR_RCVOWN         (0x1UL << 23)   // half duplex
#define SMSC_MAC_CSR_LOOPBK         (0x1UL << 21)   // loopback
#define SMSC_MAC_CSR_FDPX           (0x1UL << 20)   // full duplex
#define SMSC_MAC_CSR_MCPAS          (0x1UL << 19)   // pass all multicast
#define SMSC_MAC_CSR_PRMS           (0x1UL << 18)   // promiscuous
#define SMSC_MAC_CSR_INVFILT        (0x1UL << 17)   // inverse filtering
#define SMSC_MAC_CSR_PASSBAD        (0x1UL << 16)   // pass bad frames
#define SMSC_MAC_CSR_HPFILT         (0x1UL << 13)   // hash filtering
#define SMSC_MAC_CSR_BCAST          (0x1UL << 11)   // (FreeBSD: "Broadcast")
#define SMSC_MAC_CSR_TXEN           (0x1UL << 3)    // TX enable
#define SMSC_MAC_CSR_RXEN           (0x1UL << 2)    // RX enable

// INTR_STATUS / INTR_CFG (interrupt endpoint control)
#define SMSC_INTR_NTEP              (0x1UL << 31)
#define SMSC_INTR_MACRTO            (0x1UL << 19)
#define SMSC_INTR_TX_STOP           (0x1UL << 17)
#define SMSC_INTR_RX_STOP           (0x1UL << 16)
#define SMSC_INTR_PHY_INT           (0x1UL << 15)
#define SMSC_INTR_TXE               (0x1UL << 14)
#define SMSC_INTR_TDFU              (0x1UL << 13)
#define SMSC_INTR_TDFO              (0x1UL << 12)
#define SMSC_INTR_RXDF              (0x1UL << 11)
#define SMSC_INTR_GPIOS             0x000007FFUL

// MII_ADDR: (phy << 11) | (reg << 6) | write | busy
#define SMSC_MII_WRITE              (0x1UL << 1)
#define SMSC_MII_READ               (0x0UL << 1)
#define SMSC_MII_BUSY               (0x1UL << 0)
#define SMSC_MII_ADDR_PHY(p)        (((DWORD)(p) & 0x1F) << 11)
#define SMSC_MII_ADDR_REG(r)        (((DWORD)(r) & 0x1F) << 6)

// COE_CTRL (H/W checksum)
#define SMSC_COE_CTRL_TX_EN         (0x1UL << 16)
#define SMSC_COE_CTRL_RX_MODE       (0x1UL << 1)
#define SMSC_COE_CTRL_RX_EN         (0x1UL << 0)

// Internal PHY: MII address 1
#define SMSC_PHY_ID                 1

// Standard MII registers (IEEE 802.3 clause 22)
#define MII_BMCR                    0
#define MII_BMSR                    1
#define MII_PHYID1                  2
#define MII_PHYID2                  3
#define MII_ANAR                    4
#define MII_ANLPAR                  5

#define MII_BMSR_LINK               0x0004
#define MII_BMSR_ANEG_COMPLETE      0x0020

// SMSC PHY vendor registers
#define SMSC_PHY_INTR_STAT          29
#define SMSC_PHY_INTR_MASK          30

#define SMSC_PHY_INTR_ENERGY_ON     (0x1U << 7)
#define SMSC_PHY_INTR_ANEG_COMP     (0x1U << 6)
#define SMSC_PHY_INTR_REMOTE_FAULT  (0x1U << 5)
#define SMSC_PHY_INTR_LINK_DOWN     (0x1U << 4)

#endif // __SMSCREG_H__

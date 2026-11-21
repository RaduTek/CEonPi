//
// smschip.c
//
// Chip bring-up, PHY/link handling, receive filter and the RX path of the
// SMSC LAN9512/LAN9514 USB Ethernet driver.
//
// At attach the chip and PHY are reset, the MAC address (from the VideoCore
// firmware through the OAL) is programmed and TX/RX are enabled. Then three
// threads run for as long as the device is attached:
// - RX thread: keeps two bulk-IN transfers queued, splits each completed
//   transfer into frames and hands them to NDIS (SmscNdisIndicateRx);
// - link thread: polls BMSR every SMSC_LINK_POLL_MS (the interrupt endpoint
//   is not used, see smsc.h), applies receive filter changes requested by
//   NDIS, and logs the statistics every SMSC_STATS_INTERVAL ms when they
//   changed (Info zone);
// - TX thread (smscndis.c): bulk OUT.
//
// The init sequence follows FreeBSD if_smsc.c smsc_chip_init/smsc_phy_init
// (BSD-2-Clause) and the Linux smsc95xx driver (GPL, used as documentation).
//
// Threads, not transfer callbacks, do all the work: every transfer callback
// only sets an event (SmscXferSignal), so register access (synchronous
// control transfers) and transfer-handle cleanup never run in USBD/HCD
// callback context.
//
#include <windows.h>
#include <string.h>
#include <winioctl.h>
#include <pkfuncs.h>
#include <image_cfg.h>
#include <usbdi.h>
#include "smsc.h"

// Fallback MAC (locally administered) if the firmware does not supply one.
static const BYTE s_abFallbackMac[ETH_ALEN] = { 0x02, 0x52, 0x50, 0x69, 0x00, 0x01 };

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

// Poll a register until all bits in dwMask read 0.
static BOOL SmscWaitRegClear(PSMSC_ADAPTER pAd, DWORD dwReg, DWORD dwMask)
{
    DWORD i, dwVal = 0;

    for (i = 0; i < SMSC_RESET_POLLS; i++)
    {
        if (!SmscReadReg(pAd, dwReg, &dwVal))
            return FALSE;
        if ((dwVal & dwMask) == 0)
            return TRUE;
        Sleep(10);
    }
    SMSCERR((TEXT("SMSC95XX: reg 0x%03x bits 0x%08x did not clear (0x%08x)\r\n"),
             dwReg, dwMask, dwVal));
    return FALSE;
}

// MAC address: the firmware-assigned board address through the OAL, else a
// fixed locally administered fallback. NDIS may later override abMac with a
// registry NetworkAddress (MiniportInitialize).
static void SmscGetMac(PSMSC_ADAPTER pAd)
{
    BYTE abMac[ETH_ALEN];
    DWORD cb = 0;

    if (KernelIoControl(IOCTL_HAL_MY_GET_MAC_ADDRESS, NULL, 0, abMac, sizeof(abMac), &cb) &&
        cb == ETH_ALEN && (abMac[0] & 0x01) == 0)
    {
        memcpy(pAd->abPermMac, abMac, ETH_ALEN);
        SMSCINIT((TEXT("SMSC95XX: MAC %02x:%02x:%02x:%02x:%02x:%02x (firmware)\r\n"),
                  abMac[0], abMac[1], abMac[2], abMac[3], abMac[4], abMac[5]));
    }
    else
    {
        memcpy(pAd->abPermMac, s_abFallbackMac, ETH_ALEN);
        SMSCWARN((TEXT("SMSC95XX: no MAC from firmware (err %u), using fallback %02x:%02x:%02x:%02x:%02x:%02x\r\n"),
                  GetLastError(), pAd->abPermMac[0], pAd->abPermMac[1], pAd->abPermMac[2],
                  pAd->abPermMac[3], pAd->abPermMac[4], pAd->abPermMac[5]));
    }
    memcpy(pAd->abMac, pAd->abPermMac, ETH_ALEN);
}

// Program pAd->abMac into the chip.
BOOL SmscSetMac(PSMSC_ADAPTER pAd)
{
    const BYTE *m = pAd->abMac;
    DWORD dwLo = (DWORD)m[0] | ((DWORD)m[1] << 8) | ((DWORD)m[2] << 16) | ((DWORD)m[3] << 24);
    DWORD dwHi = (DWORD)m[4] | ((DWORD)m[5] << 8);

    return SmscWriteReg(pAd, SMSC_MAC_ADDRL, dwLo) &&
           SmscWriteReg(pAd, SMSC_MAC_ADDRH, dwHi);
}

//------------------------------------------------------------------------------
// PHY
//------------------------------------------------------------------------------
#define MII_BMCR_RESET          0x8000
#define MII_BMCR_ANENABLE       0x1000
#define MII_BMCR_ANRESTART      0x0200
#define MII_BMCR_SPEED100       0x2000
#define MII_BMCR_FDX            0x0100
#define MII_ANAR_10_HD          0x0020
#define MII_ANAR_10_FD          0x0040
#define MII_ANAR_100_HD         0x0080
#define MII_ANAR_100_FD         0x0100
#define MII_ANAR_CSMA           0x0001

static BOOL SmscPhyInit(PSMSC_ADAPTER pAd)
{
    DWORD i;
    WORD wVal = 0;

    // Reset the PHY and wait for the reset bit to self-clear.
    if (!SmscMiiWrite(pAd, MII_BMCR, MII_BMCR_RESET))
        return FALSE;
    for (i = 0; i < SMSC_RESET_POLLS; i++)
    {
        Sleep(10);
        if (!SmscMiiRead(pAd, MII_BMCR, &wVal))
            return FALSE;
        if ((wVal & MII_BMCR_RESET) == 0)
            break;
    }
    if (i == SMSC_RESET_POLLS)
    {
        SMSCERR((TEXT("SMSC95XX: PHY reset did not complete (BMCR 0x%04x)\r\n"), wVal));
        return FALSE;
    }

    // Advertise 10/100 half/full. No pause: flow control (FLOW) stays off.
    if (!SmscMiiWrite(pAd, MII_ANAR, MII_ANAR_10_HD | MII_ANAR_10_FD |
                      MII_ANAR_100_HD | MII_ANAR_100_FD | MII_ANAR_CSMA))
        return FALSE;

    // The link is polled (BMSR), so PHY interrupts stay masked (reset default).

    // Restart autonegotiation.
    if (!SmscMiiRead(pAd, MII_BMCR, &wVal))
        return FALSE;
    wVal |= MII_BMCR_ANENABLE | MII_BMCR_ANRESTART;
    return SmscMiiWrite(pAd, MII_BMCR, wVal);
}

//------------------------------------------------------------------------------
// Chip init
//------------------------------------------------------------------------------
BOOL SmscChipInit(PSMSC_ADAPTER pAd)
{
    DWORD dwVal;

    // Lite reset, then PHY reset; both bits self-clear.
    if (!SmscWriteReg(pAd, SMSC_HW_CFG, SMSC_HW_CFG_LRST) ||
        !SmscWaitRegClear(pAd, SMSC_HW_CFG, SMSC_HW_CFG_LRST))
    {
        SMSCERR((TEXT("SMSC95XX: lite reset failed\r\n")));
        return FALSE;
    }
    if (!SmscWriteReg(pAd, SMSC_PM_CTRL, SMSC_PM_CTRL_PHY_RST) ||
        !SmscWaitRegClear(pAd, SMSC_PM_CTRL, SMSC_PM_CTRL_PHY_RST))
    {
        SMSCERR((TEXT("SMSC95XX: PHY reset failed\r\n")));
        return FALSE;
    }

    SmscGetMac(pAd);
    if (!SmscSetMac(pAd))
        return FALSE;

    // Bulk-IN empty response (BIR): NAK when there is no data.
    if (!SmscReadReg(pAd, SMSC_HW_CFG, &dwVal) ||
        !SmscWriteReg(pAd, SMSC_HW_CFG, dwVal | SMSC_HW_CFG_BIR))
        return FALSE;

    // Burst size (in 512-byte packets) and bulk-IN delay.
    if (!SmscWriteReg(pAd, SMSC_BURST_CAP, SMSC_HS_BURST_CAP) ||
        !SmscWriteReg(pAd, SMSC_BULK_IN_DLY, SMSC_BULK_IN_DELAY))
        return FALSE;

    // RX: frame data offset, multiple frames per transfer, burst cap enable.
    if (!SmscReadReg(pAd, SMSC_HW_CFG, &dwVal))
        return FALSE;
    dwVal &= ~SMSC_HW_CFG_RXDOFF;
    dwVal |= ((DWORD)SMSC_RX_DATA_OFFSET << 9) & SMSC_HW_CFG_RXDOFF;
    dwVal |= SMSC_HW_CFG_MEF | SMSC_HW_CFG_BCE;
    if (!SmscWriteReg(pAd, SMSC_HW_CFG, dwVal))
        return FALSE;

    // Clear interrupt status; LEDs: speed, link, full duplex.
    if (!SmscWriteReg(pAd, SMSC_INTR_STATUS, 0xFFFFFFFF) ||
        !SmscWriteReg(pAd, SMSC_LED_GPIO_CFG, SMSC_LED_GPIO_CFG_SPD_LED |
                      SMSC_LED_GPIO_CFG_LNK_LED | SMSC_LED_GPIO_CFG_FDX_LED))
        return FALSE;

    // TX side: no flow control yet, default automatic flow control config.
    if (!SmscWriteReg(pAd, SMSC_FLOW, 0) ||
        !SmscWriteReg(pAd, SMSC_AFC_CFG, SMSC_AFC_CFG_DEFAULT))
        return FALSE;

    // MAC: own address + broadcast until NDIS sets a filter. The chip comes
    // out of reset with PRMS set.
    if (!SmscReadReg(pAd, SMSC_MAC_CSR, &pAd->dwMacCsr))
        return FALSE;
    pAd->dwMacCsr &= ~(SMSC_MAC_CSR_PRMS | SMSC_MAC_CSR_MCPAS | SMSC_MAC_CSR_HPFILT);
    if (!SmscWriteReg(pAd, SMSC_MAC_CSR, pAd->dwMacCsr))
        return FALSE;

    // VLAN1 = 802.1Q type, as both reference drivers do.
    if (!SmscWriteReg(pAd, SMSC_VLAN1, 0x8100))
        return FALSE;

    if (!SmscPhyInit(pAd))
    {
        SMSCERR((TEXT("SMSC95XX: PHY init failed\r\n")));
        return FALSE;
    }

    // Nothing is reported on the interrupt endpoint (link is polled).
    if (!SmscWriteReg(pAd, SMSC_INTR_CFG, 0))
        return FALSE;

    // Enable TX then RX.
    pAd->dwMacCsr |= SMSC_MAC_CSR_TXEN;
    if (!SmscWriteReg(pAd, SMSC_MAC_CSR, pAd->dwMacCsr) ||
        !SmscWriteReg(pAd, SMSC_TX_CFG, SMSC_TX_CFG_ON))
        return FALSE;
    pAd->dwMacCsr |= SMSC_MAC_CSR_RXEN;
    if (!SmscWriteReg(pAd, SMSC_MAC_CSR, pAd->dwMacCsr))
        return FALSE;

    if (SmscReadReg(pAd, SMSC_HW_CFG, &dwVal))
        SMSCINFO((TEXT("SMSC95XX: init done, HW_CFG=0x%08x MAC_CSR=0x%08x\r\n"),
                  dwVal, pAd->dwMacCsr));
    return TRUE;
}

//------------------------------------------------------------------------------
// Receive filter (link thread)
//------------------------------------------------------------------------------
#define FILTER_DIRECTED         0x00000001  // NDIS_PACKET_TYPE_* values (ntddndis.h)
#define FILTER_MULTICAST        0x00000002
#define FILTER_ALL_MULTICAST    0x00000004
#define FILTER_BROADCAST        0x00000008
#define FILTER_PROMISCUOUS      0x00000020

// Apply the filter NDIS asked for: promiscuous -> PRMS, all multicast ->
// MCPAS, a multicast list -> hash filter (HPFILT). Directed and broadcast
// frames are always received.
static void SmscApplyFilter(PSMSC_ADAPTER pAd)
{
    DWORD dwFilter, dwHashH, dwHashL, dwCsr;

    SmscNdisGetFilter(pAd, &dwFilter, &dwHashH, &dwHashL);

    dwCsr = pAd->dwMacCsr & ~(SMSC_MAC_CSR_PRMS | SMSC_MAC_CSR_MCPAS | SMSC_MAC_CSR_HPFILT);
    if (dwFilter & FILTER_PROMISCUOUS)
        dwCsr |= SMSC_MAC_CSR_PRMS;
    else if (dwFilter & FILTER_ALL_MULTICAST)
        dwCsr |= SMSC_MAC_CSR_MCPAS;
    else if ((dwFilter & FILTER_MULTICAST) && (dwHashH | dwHashL))
        dwCsr |= SMSC_MAC_CSR_HPFILT;

    SmscWriteReg(pAd, SMSC_HASHH, dwHashH);
    SmscWriteReg(pAd, SMSC_HASHL, dwHashL);
    if (dwCsr != pAd->dwMacCsr)
    {
        pAd->dwMacCsr = dwCsr;
        SmscWriteReg(pAd, SMSC_MAC_CSR, dwCsr);
    }
    SMSCINFO((TEXT("SMSC95XX: filter 0x%x -> MAC_CSR 0x%08x HASH %08x:%08x\r\n"),
              dwFilter, dwCsr, dwHashH, dwHashL));
}

//------------------------------------------------------------------------------
// Link
//------------------------------------------------------------------------------
static void SmscCheckLink(PSMSC_ADAPTER pAd)
{
    WORD wBmsr = 0, wBmcr = 0, wAdv = 0, wLpa = 0, wCommon;
    BOOL fUp;

    // BMSR link latches low: read it twice for the current state.
    if (!SmscMiiRead(pAd, MII_BMSR, &wBmsr) || !SmscMiiRead(pAd, MII_BMSR, &wBmsr))
        return;

    fUp = (wBmsr & MII_BMSR_LINK) != 0;
    if (fUp == pAd->fLinkUp)
        return;

    if (!fUp)
    {
        pAd->fLinkUp = FALSE;
        SMSCINIT((TEXT("SMSC95XX: link down\r\n")));
        SmscNdisLinkChanged(pAd, FALSE);
        return;
    }

    SmscMiiRead(pAd, MII_ANAR, &wAdv);
    SmscMiiRead(pAd, MII_ANLPAR, &wLpa);
    if ((wBmsr & MII_BMSR_ANEG_COMPLETE) && (wLpa & 0x03E0))
    {
        wCommon = wAdv & wLpa;
        pAd->fFullDuplex = (wCommon & MII_ANAR_100_FD) ||
                           (!(wCommon & MII_ANAR_100_HD) && (wCommon & MII_ANAR_10_FD));
        pAd->dwSpeedMbps = (wCommon & (MII_ANAR_100_FD | MII_ANAR_100_HD)) ? 100 : 10;
    }
    else
    {
        // No autonegotiation result (e.g. forced partner): use BMCR.
        SmscMiiRead(pAd, MII_BMCR, &wBmcr);
        pAd->fFullDuplex = (wBmcr & MII_BMCR_FDX) != 0;
        pAd->dwSpeedMbps = (wBmcr & MII_BMCR_SPEED100) ? 100 : 10;
    }

    // MAC duplex must match the PHY.
    if (pAd->fFullDuplex)
    {
        pAd->dwMacCsr |= SMSC_MAC_CSR_FDPX;
        pAd->dwMacCsr &= ~SMSC_MAC_CSR_RCVOWN;
    }
    else
    {
        pAd->dwMacCsr &= ~SMSC_MAC_CSR_FDPX;
        pAd->dwMacCsr |= SMSC_MAC_CSR_RCVOWN;
    }
    SmscWriteReg(pAd, SMSC_MAC_CSR, pAd->dwMacCsr);

    pAd->fLinkUp = TRUE;
    SMSCINIT((TEXT("SMSC95XX: link up %u/%s (BMSR 0x%04x ANAR 0x%04x ANLPAR 0x%04x)\r\n"),
              pAd->dwSpeedMbps, pAd->fFullDuplex ? TEXT("full") : TEXT("half"),
              wBmsr, wAdv, wLpa));
    SmscNdisLinkChanged(pAd, TRUE);
}

//------------------------------------------------------------------------------
// Statistics and CPU-load probe (Info zone, Debug builds)
//
// The OAL's OEMIdle is a stub, so CE has no idle-time accounting. To see how
// busy the CPU is, a thread at the lowest priority (255) counts in a loop: it
// only runs when nothing else wants the CPU. busy% = 100 - 100 * (counts per
// ms now) / (highest counts per ms seen), so the estimate is only as good as
// the quietest interval observed since the probe started. The probe starts
// when the Info zone is first seen on and runs until the device closes.
//------------------------------------------------------------------------------
typedef struct _SMSC_STATS_SNAP
{
    DWORD   dwTick;
    DWORD   dwRxBytes, dwTxBytes, dwRxFrames, dwTxFrames, dwRxXfers, dwTxXfers;
    DWORD   dwErr;
    DWORD   dwIdle;
    DWORD   dwIdleMaxPerMs;
} SMSC_STATS_SNAP;

#ifdef DEBUG
static DWORD WINAPI SmscIdleThread(LPVOID lpv)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpv;

    while (!pAd->fClosing)
        pAd->dwIdleCount++;
    return 0;
}
#endif

static void SmscLogStats(PSMSC_ADAPTER pAd, SMSC_STATS_SNAP *pLast)
{
    DWORD dwNow = GetTickCount();
    DWORD dt = dwNow - pLast->dwTick;
    DWORD rxB, txB, rxF, txF, rxX, txX, dwErr, dwIdlePerMs, dwBusy, dwFpx, dwTpx;

    if (dt == 0)
        dt = 1;
    rxB = pAd->dwRxBytes - pLast->dwRxBytes;
    txB = pAd->dwTxBytes - pLast->dwTxBytes;
    rxF = pAd->dwRxFrames - pLast->dwRxFrames;
    txF = pAd->dwTxFrames - pLast->dwTxFrames;
    rxX = pAd->dwRxXfers - pLast->dwRxXfers;
    txX = pAd->dwTxXfers - pLast->dwTxXfers;
    dwErr = pAd->dwRxXferErrors + pAd->dwTxErrors + pAd->dwRxNoBuffer +
            pAd->dwRxErrors + pAd->dwRxBadLen;

#ifdef DEBUG
    if (pAd->hIdleThread == NULL && DEBUGZONE(ZONEID_INFO) && !pAd->fClosing)
    {
        pAd->hIdleThread = CreateThread(NULL, 0, SmscIdleThread, pAd, 0, NULL);
        if (pAd->hIdleThread != NULL)
            CeSetThreadPriority(pAd->hIdleThread, 255);
        pLast->dwIdle = pAd->dwIdleCount;
    }
#endif

    dwIdlePerMs = (pAd->dwIdleCount - pLast->dwIdle) / dt;
    if (dwIdlePerMs > pLast->dwIdleMaxPerMs)
        pLast->dwIdleMaxPerMs = dwIdlePerMs;
    dwBusy = pLast->dwIdleMaxPerMs ? 100 - (dwIdlePerMs * 100) / pLast->dwIdleMaxPerMs : 0;
    dwFpx = rxX ? (rxF * 10) / rxX : 0;     // frames per RX transfer, x10
    dwTpx = txX ? (txF * 10) / txX : 0;     // frames per TX transfer, x10

    if (rxF || txF || dwErr != pLast->dwErr)
    {
        // Bytes per ms = kB/s.
        SMSCINFO((TEXT("SMSC95XX: rx %u kB/s %u frm %u xfer (%u.%u frm/xfer) | tx %u kB/s %u frm %u xfer (%u.%u frm/xfer) | cpu ~%u%% busy\r\n"),
                  rxB / dt, rxF, rxX, dwFpx / 10, dwFpx % 10, txB / dt, txF, txX,
                  dwTpx / 10, dwTpx % 10, pAd->hIdleThread ? dwBusy : 0));
        SMSCINFO((TEXT("SMSC95XX:   totals: rx %u ind %u drop %u nobuf %u err %u badlen %u xfererr %u; tx %u err %u\r\n"),
                  pAd->dwRxFrames, pAd->dwRxIndicated, pAd->dwRxDropped, pAd->dwRxNoBuffer,
                  pAd->dwRxErrors, pAd->dwRxBadLen, pAd->dwRxXferErrors,
                  pAd->dwTxFrames, pAd->dwTxErrors));
    }

    pLast->dwTick = dwNow;
    pLast->dwRxBytes = pAd->dwRxBytes;
    pLast->dwTxBytes = pAd->dwTxBytes;
    pLast->dwRxFrames = pAd->dwRxFrames;
    pLast->dwTxFrames = pAd->dwTxFrames;
    pLast->dwRxXfers = pAd->dwRxXfers;
    pLast->dwTxXfers = pAd->dwTxXfers;
    pLast->dwErr = dwErr;
    pLast->dwIdle = pAd->dwIdleCount;
}

// Link thread: BMSR poll every SMSC_LINK_POLL_MS, receive filter changes
// requested by NDIS, and the periodic statistics lines (Info zone).
static DWORD WINAPI SmscLinkThread(LPVOID lpv)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpv;
    HANDLE ah[2];
    DWORD dwWait, dwNow, dwNextPoll, dwNextStats, dwTimeout;
    SMSC_STATS_SNAP snap;

    memset(&snap, 0, sizeof(snap));
    snap.dwTick = GetTickCount();

    SmscCheckLink(pAd);

    ah[0] = pAd->hStopEvent;
    ah[1] = pAd->hFilterEvent;
    dwNextPoll = GetTickCount() + SMSC_LINK_POLL_MS;
    dwNextStats = GetTickCount() + SMSC_STATS_INTERVAL;

    for (;;)
    {
        dwNow = GetTickCount();
        dwTimeout = ((LONG)(dwNextPoll - dwNow) > 0) ? (dwNextPoll - dwNow) : 0;
        dwWait = WaitForMultipleObjects(2, ah, FALSE, dwTimeout);
        if (dwWait == WAIT_OBJECT_0 || pAd->fClosing)
            break;

        if (dwWait == WAIT_OBJECT_0 + 1)
        {
            SmscApplyFilter(pAd);
            continue;
        }

        // Poll tick.
        SmscCheckLink(pAd);
        dwNextPoll = GetTickCount() + SMSC_LINK_POLL_MS;

        if ((LONG)(GetTickCount() - dwNextStats) >= 0)
        {
            dwNextStats = GetTickCount() + SMSC_STATS_INTERVAL;
            SmscLogStats(pAd, &snap);
        }
    }
    return 0;
}

//------------------------------------------------------------------------------
// RX
//------------------------------------------------------------------------------

// Walk the frames packed into one bulk-IN transfer and indicate the good
// ones to NDIS in batches of up to SMSC_RX_BATCH. The frames are indicated
// in place (NDIS copies them before NdisMIndicateReceivePacket returns).
static void SmscRxParse(PSMSC_ADAPTER pAd, BYTE *p, DWORD cb)
{
    BYTE *apFrame[SMSC_RX_BATCH];
    DWORD acbFrame[SMSC_RX_BATCH];
    DWORD n = 0, off = 0, dwStat, dwLen;

    while (off + 4 <= cb)
    {
        memcpy(&dwStat, p + off, 4);
        off += 4 + SMSC_RX_DATA_OFFSET;
        dwLen = SMSC_RX_STAT_FRM_LENGTH(dwStat);    // includes the FCS

        if (dwLen < ETH_HLEN + ETH_FCS_LEN || off + dwLen > cb)
        {
            pAd->dwRxBadLen++;
            break;                                  // cannot resync in this buffer
        }

        if (dwStat & SMSC_RX_STAT_ERROR)
        {
            pAd->dwRxErrors++;
        }
        else if (dwLen - ETH_FCS_LEN > ETH_MAX_FRAME + 4)  // allow one VLAN tag
        {
            pAd->dwRxBadLen++;
        }
        else
        {
            pAd->dwRxFrames++;
            pAd->dwRxBytes += dwLen - ETH_FCS_LEN;
            apFrame[n] = p + off;
            acbFrame[n] = dwLen - ETH_FCS_LEN;
            if (++n == SMSC_RX_BATCH)
            {
                SmscNdisIndicateRx(pAd, apFrame, acbFrame, n);
                n = 0;
            }
        }

        off += dwLen;
        off = (off + 3) & ~3UL;                     // next status word is 4-byte aligned
    }

    if (n > 0)
        SmscNdisIndicateRx(pAd, apFrame, acbFrame, n);
}

static BOOL SmscRxQueue(PSMSC_ADAPTER pAd, DWORD i)
{
    SMSC_RX_BUF *pRx = &pAd->aRx[i];

    ResetEvent(pRx->hEvent);
    pRx->hXfer = pAd->pUsbFuncs->lpIssueBulkTransfer(pAd->hPipeIn, SmscXferSignal, pRx->hEvent,
                                                      USB_IN_TRANSFER | USB_SHORT_TRANSFER_OK,
                                                      SMSC_RX_BUF_SIZE, pRx->pData, 0);
    return pRx->hXfer != NULL;
}

// RX thread. The HCD completes the transfers of one pipe in order, so the
// thread waits on the buffers round-robin. A buffer is parsed (and its
// frames indicated) before it is queued again.
static DWORD WINAPI SmscRxThread(LPVOID lpv)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpv;
    SMSC_RX_BUF *pRx;
    HANDLE ah[2];
    DWORD i, idx = 0, dwWait, dwBytes, dwUsbErr;
    BOOL fHalted;

    for (i = 0; i < SMSC_RX_BUFFERS; i++)
    {
        if (!SmscRxQueue(pAd, i))
            SMSCERR((TEXT("SMSC95XX: RX transfer %u not issued\r\n"), i));
    }

    ah[0] = pAd->hStopEvent;

    while (!pAd->fClosing)
    {
        pRx = &pAd->aRx[idx];

        if (pRx->hXfer == NULL)
        {
            // An earlier issue failed: retry slowly.
            if (WaitForSingleObject(pAd->hStopEvent, 100) == WAIT_OBJECT_0)
                break;
            SmscRxQueue(pAd, idx);
            idx = (idx + 1) % SMSC_RX_BUFFERS;
            continue;
        }

        ah[1] = pRx->hEvent;
        dwWait = WaitForMultipleObjects(2, ah, FALSE, INFINITE);
        if (dwWait != WAIT_OBJECT_0 + 1)
            break;

        dwBytes = 0;
        dwUsbErr = USB_NO_ERROR;
        pAd->pUsbFuncs->lpGetTransferStatus(pRx->hXfer, &dwBytes, &dwUsbErr);
        pAd->pUsbFuncs->lpCloseTransfer(pRx->hXfer);
        pRx->hXfer = NULL;

        if (dwUsbErr == USB_NO_ERROR)
        {
            pAd->dwRxXfers++;
            SmscRxParse(pAd, pRx->pData, dwBytes);
        }
        else if (!pAd->fClosing)
        {
            pAd->dwRxXferErrors++;
            if (pAd->dwRxXferErrors <= 5 || (pAd->dwRxXferErrors % 100) == 0)
                SMSCWARN((TEXT("SMSC95XX: RX transfer failed, usberr=0x%x (%u so far)\r\n"),
                          dwUsbErr, pAd->dwRxXferErrors));
            fHalted = FALSE;
            if (pAd->pUsbFuncs->lpIsPipeHalted(pAd->hPipeIn, &fHalted) && fHalted)
                pAd->pUsbFuncs->lpResetPipe(pAd->hPipeIn);
            Sleep(10);                              // do not spin on a dead pipe
        }

        if (!pAd->fClosing)
            SmscRxQueue(pAd, idx);
        idx = (idx + 1) % SMSC_RX_BUFFERS;
    }

    // Abort and reap whatever is still queued.
    for (i = 0; i < SMSC_RX_BUFFERS; i++)
    {
        pRx = &pAd->aRx[i];
        if (pRx->hXfer != NULL)
        {
            pAd->pUsbFuncs->lpAbortTransfer(pRx->hXfer, 0);
            WaitForSingleObject(pRx->hEvent, 500);
            pAd->pUsbFuncs->lpCloseTransfer(pRx->hXfer);
            pRx->hXfer = NULL;
        }
    }
    return 0;
}

//------------------------------------------------------------------------------
// Start / stop
//------------------------------------------------------------------------------
BOOL SmscStart(PSMSC_ADAPTER pAd)
{
    LPCUSB_FUNCS f = pAd->pUsbFuncs;
    DWORD i;

    pAd->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    pAd->hFilterEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    pAd->hTxQueueEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pAd->hStopEvent || !pAd->hFilterEvent || !pAd->hTxQueueEvent)
        return FALSE;

    for (i = 0; i < SMSC_RX_BUFFERS; i++)
    {
        pAd->aRx[i].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        pAd->aRx[i].pData = (BYTE *)LocalAlloc(LMEM_FIXED, SMSC_RX_BUF_SIZE);
        if (!pAd->aRx[i].hEvent || !pAd->aRx[i].pData)
            return FALSE;
    }
    for (i = 0; i < SMSC_TX_SLOTS; i++)
    {
        pAd->aTx[i].hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        pAd->aTx[i].pData = (BYTE *)LocalAlloc(LMEM_FIXED, SMSC_TX_BUF_SIZE);
        if (!pAd->aTx[i].hEvent || !pAd->aTx[i].pData)
            return FALSE;
    }

    pAd->hPipeIn = f->lpOpenPipe(pAd->hDevice, &pAd->epBulkIn);
    pAd->hPipeOut = f->lpOpenPipe(pAd->hDevice, &pAd->epBulkOut);
    if (!pAd->hPipeIn || !pAd->hPipeOut)
    {
        SMSCERR((TEXT("SMSC95XX: OpenPipe failed (in 0x%x out 0x%x)\r\n"),
                 (DWORD)pAd->hPipeIn, (DWORD)pAd->hPipeOut));
        return FALSE;
    }

    if (!SmscChipInit(pAd))
        return FALSE;

    pAd->hRxThread = CreateThread(NULL, 0, SmscRxThread, pAd, 0, NULL);
    pAd->hLinkThread = CreateThread(NULL, 0, SmscLinkThread, pAd, 0, NULL);
    pAd->hTxThread = CreateThread(NULL, 0, SmscTxThread, pAd, 0, NULL);
    if (!pAd->hRxThread || !pAd->hLinkThread || !pAd->hTxThread)
    {
        SMSCERR((TEXT("SMSC95XX: cannot create worker threads\r\n")));
        return FALSE;
    }

    // Data path above normal applications; the link thread stays normal.
    CeSetThreadPriority(pAd->hRxThread, SMSC_DATA_THREAD_PRIORITY);
    CeSetThreadPriority(pAd->hTxThread, SMSC_DATA_THREAD_PRIORITY);
    return TRUE;
}

static void SmscCloseThread(HANDLE *phThread, LPCTSTR pszName)
{
    if (*phThread == NULL)
        return;
    if (WaitForSingleObject(*phThread, 5000) != WAIT_OBJECT_0)
        SMSCERR((TEXT("SMSC95XX: %s thread did not stop\r\n"), pszName));
    CloseHandle(*phThread);
    *phThread = NULL;
}

// Stop the threads and release everything SmscStart created. Safe on a
// partially started adapter. The NDIS side must already be detached.
void SmscStop(PSMSC_ADAPTER pAd)
{
    LPCUSB_FUNCS f = pAd->pUsbFuncs;
    DWORD i;

    pAd->fClosing = TRUE;
    if (pAd->hStopEvent)
        SetEvent(pAd->hStopEvent);

    // Complete anything pending so the threads see their events.
    if (pAd->hPipeIn)
        f->lpAbortPipeTransfers(pAd->hPipeIn, 0);
    if (pAd->hPipeOut)
        f->lpAbortPipeTransfers(pAd->hPipeOut, 0);

    SmscCloseThread(&pAd->hRxThread, TEXT("RX"));
    SmscCloseThread(&pAd->hLinkThread, TEXT("link"));
    SmscCloseThread(&pAd->hTxThread, TEXT("TX"));
    SmscCloseThread(&pAd->hIdleThread, TEXT("CPU probe"));

    if (pAd->hPipeIn)
        f->lpClosePipe(pAd->hPipeIn);
    if (pAd->hPipeOut)
        f->lpClosePipe(pAd->hPipeOut);
    pAd->hPipeIn = pAd->hPipeOut = NULL;

    for (i = 0; i < SMSC_RX_BUFFERS; i++)
    {
        if (pAd->aRx[i].hEvent)
            CloseHandle(pAd->aRx[i].hEvent);
        if (pAd->aRx[i].pData)
            LocalFree(pAd->aRx[i].pData);
        pAd->aRx[i].hEvent = NULL;
        pAd->aRx[i].pData = NULL;
    }
    for (i = 0; i < SMSC_TX_SLOTS; i++)
    {
        if (pAd->aTx[i].hEvent)
            CloseHandle(pAd->aTx[i].hEvent);
        if (pAd->aTx[i].pData)
            LocalFree(pAd->aTx[i].pData);
        pAd->aTx[i].hEvent = NULL;
        pAd->aTx[i].pData = NULL;
    }
    if (pAd->hStopEvent)
        CloseHandle(pAd->hStopEvent);
    if (pAd->hFilterEvent)
        CloseHandle(pAd->hFilterEvent);
    if (pAd->hTxQueueEvent)
        CloseHandle(pAd->hTxQueueEvent);
    pAd->hStopEvent = pAd->hFilterEvent = pAd->hTxQueueEvent = NULL;
}

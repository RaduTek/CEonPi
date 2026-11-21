//
// smsc.h
//
// Shared definitions for the SMSC LAN9512/LAN9514 USB Ethernet driver
// (smsc95xx.dll).
//
// The DLL is two things at once:
// - a USB client driver: USBD loads it for the Ethernet function of the
//   chip (VID 0x0424, PID 0xEC00) and calls USBDeviceAttach (smscusb.c);
//   the chip is brought up and the RX/link/TX threads run for as long as the
//   device is attached (smschip.c);
// - an NDIS 5.1 deserialized miniport: after attach the driver calls
//   NdisRegisterAdapter, NDIS calls DriverEntry and MiniportInitialize in
//   this same (already loaded) DLL, and frames flow to and from TCP/IP
//   (smscndis.c).
//
// This header does not include ndis.h (only smscndis.c does), so the NDIS
// handles below are stored as opaque pointers.
//
#ifndef __SMSC_H__
#define __SMSC_H__

#include <windows.h>
#include <usbdi.h>
#include "smscreg.h"

//
// Debug zones. Errors, warnings and Init print by default; Init carries only
// a handful of one-off lines (attach, MAC, NDIS registration, link up/down).
// Info (off by default) has the register dump, endpoint list, filter changes
// and the periodic statistics line; turn it on with the kernel debug-zone
// editor. KITL serial output costs ~30 ms per line, so nothing is logged per
// frame.
//
#define ZONEID_ERROR    0
#define ZONEID_WARN     1
#define ZONEID_INIT     2
#define ZONEID_INFO     3

#define SMSC_DBG_ZONES_INITIAL  ((1 << ZONEID_ERROR) | (1 << ZONEID_WARN) | (1 << ZONEID_INIT))

#define SMSCERR(x)      DEBUGMSG(DEBUGZONE(ZONEID_ERROR), x)
#define SMSCWARN(x)     DEBUGMSG(DEBUGZONE(ZONEID_WARN), x)
#define SMSCINIT(x)     DEBUGMSG(DEBUGZONE(ZONEID_INIT), x)
#define SMSCINFO(x)     DEBUGMSG(DEBUGZONE(ZONEID_INFO), x)

// Synchronous control transfer timeout (ms)
#define SMSC_CTL_TIMEOUT        1000

// MII busy-wait: polls of MII_ADDR before giving up
#define SMSC_MII_POLLS          100

// Reset bits (HW_CFG.LRST, PM_CTRL.PHY_RST, BMCR reset): polls, 10 ms apart
#define SMSC_RESET_POLLS        100

//
// RX: SMSC_RX_BUFFERS bulk-IN transfers of SMSC_RX_BUF_SIZE bytes are kept
// queued. With HW_CFG.MEF + BCE the chip packs several frames into one
// transfer, up to BURST_CAP 512-byte packets, so the buffer must hold
// BURST_CAP * 512 bytes. Each frame is preceded by a 4-byte status word and
// SMSC_RX_DATA_OFFSET pad bytes (HW_CFG.RXDOFF), includes the 4-byte FCS and
// is padded to a 4-byte boundary. Four buffers keep a transfer queued at the
// HCD while the RX thread parses and indicates the previous ones (dwchci DMA
// pool: 4 x 16 KB of its 320 KB).
//
#define SMSC_RX_BUFFERS         4
#define SMSC_HS_BURST_CAP       32                          // 512-byte packets
#define SMSC_RX_BUF_SIZE        (SMSC_HS_BURST_CAP * 512)   // 16 KB
#define SMSC_RX_DATA_OFFSET     0                           // HW_CFG.RXDOFF value
#define SMSC_BULK_IN_DELAY      0x00002000                  // Linux/FreeBSD default

// Frames indicated to NDIS in one NdisMIndicateReceivePacket call (and the
// size of the NDIS packet/buffer pools). A 16 KB transfer holds at most
// ~250 minimum-size frames; larger batches are split.
#define SMSC_RX_BATCH           32

//
// TX: up to SMSC_TX_SLOTS bulk-OUT transfers in flight. Every frame is
// preceded by the 8-byte TX header (TX_CTRL_0/1, FIRST_SEG | LAST_SEG). When
// several packets are queued, up to SMSC_TX_PACK_MAX frames go into one
// transfer, back to back: each header starts on a 4-byte boundary, and the
// pad bytes after a frame are skipped by the chip (TX_CTRL_0 buffer end
// alignment 0 = 4 bytes). This cuts the transfer (and interrupt, callback
// and thread wake-up) rate when the Pi sends. Set SMSC_TX_PACK_MAX to 1 for
// one frame per transfer.
//
#define SMSC_TX_SLOTS           8
#define SMSC_TX_HDR_SIZE        8
#define SMSC_TX_PACK_MAX        8
#define SMSC_TX_FRAME_SPACE     ((SMSC_TX_HDR_SIZE + ETH_MAX_FRAME + 3) & ~3)
#define SMSC_TX_BUF_SIZE        (SMSC_TX_PACK_MAX * SMSC_TX_FRAME_SPACE)

#define ETH_ALEN                6
#define ETH_HLEN                14
#define ETH_FCS_LEN             4
#define ETH_MIN_FRAME           60          // without FCS
#define ETH_MAX_FRAME           1514        // without FCS
#define ETH_MTU                 1500

// Multicast addresses kept for the hash filter (OID_802_3_MAXIMUM_LIST_SIZE)
#define SMSC_MAX_MCAST          32

// Link poll period (ms). The link is polled through BMSR rather than through
// the interrupt endpoint: polling the interrupt endpoint (bInterval 1 ms)
// would cost a channel halt + reschedule in dwchci every millisecond, the
// endpoint is not needed for anything else, and FreeBSD's driver polls too.
#define SMSC_LINK_POLL_MS       1000

// Interval of the statistics log line (ms, Info zone); printed only when
// it changed
#define SMSC_STATS_INTERVAL     5000

// CE priority of the RX and TX threads: above normal applications (251) so
// UI load cannot starve the network path, well below the USB host
// controller IST (101) and the hub threads (~106).
#define SMSC_DATA_THREAD_PRIORITY   130

// One queued bulk-IN transfer
typedef struct _SMSC_RX_BUF
{
    BYTE           *pData;          // SMSC_RX_BUF_SIZE bytes
    HANDLE          hEvent;         // set by the transfer callback
    USB_TRANSFER    hXfer;          // NULL when not queued
} SMSC_RX_BUF;

// One bulk-OUT transfer slot
typedef struct _SMSC_TX_SLOT
{
    BYTE           *pData;          // SMSC_TX_BUF_SIZE bytes
    HANDLE          hEvent;         // set by the transfer callback
    USB_TRANSFER    hXfer;          // NULL when free
    DWORD           dwFrames;       // frames in the transfer
} SMSC_TX_SLOT;

//
// Adapter context: one per attached chip.
//
typedef struct _SMSC_ADAPTER
{
    USB_HANDLE          hDevice;        // from USBDeviceAttach
    LPCUSB_FUNCS        pUsbFuncs;      // USBD function table
    LPCUSB_INTERFACE    pInterface;     // interface 0 of the active configuration

    // Endpoint descriptors found on the interface (copied)
    USB_ENDPOINT_DESCRIPTOR epBulkIn;
    USB_ENDPOINT_DESCRIPTOR epBulkOut;
    USB_ENDPOINT_DESCRIPTOR epIntrIn;   // present, but not used (link is polled)
    BOOL                fBulkIn;
    BOOL                fBulkOut;
    BOOL                fIntrIn;

    USB_PIPE            hPipeIn;
    USB_PIPE            hPipeOut;

    // Synchronous control transfers (register and MII access) are
    // serialized by csCtl and wait on hCtlEvent.
    CRITICAL_SECTION    csCtl;
    HANDLE              hCtlEvent;

    BYTE                abPermMac[ETH_ALEN];    // firmware (or fallback) address
    BYTE                abMac[ETH_ALEN];        // address programmed in the chip
    DWORD               dwMacCsr;               // shadow of MAC_CSR (link thread only)

    // Link state (written by the link thread)
    volatile BOOL       fLinkUp;
    BOOL                fFullDuplex;
    DWORD               dwSpeedMbps;

    // Worker threads
    HANDLE              hStopEvent;     // manual reset: set when stopping
    HANDLE              hRxThread;      // bulk IN
    HANDLE              hLinkThread;    // link poll, filter, stats
    HANDLE              hTxThread;      // bulk OUT
    HANDLE              hRegThread;     // NdisRegisterAdapter (with retries)
    HANDLE              hIdleThread;    // CPU-load probe (Info zone only, Debug builds)
    SMSC_RX_BUF         aRx[SMSC_RX_BUFFERS];
    SMSC_TX_SLOT        aTx[SMSC_TX_SLOTS];

    //
    // NDIS side (smscndis.c). csNdis protects hMiniport, lNdisRefs changes
    // that go with it, the TX queue and the filter request. A thread that
    // calls into NDIS takes a reference (SmscNdisRef) while hMiniport is
    // set; MiniportHalt clears hMiniport and waits for the references to
    // drain.
    //
    CRITICAL_SECTION    csNdis;
    PVOID               hMiniport;      // NDIS_HANDLE; NULL when not initialized
    LONG                lNdisRefs;      // Interlocked* only
    volatile BOOL       fMiniportActive;// between MiniportInitialize and the end of MiniportHalt
    BOOL                fNdisRegistered;// NdisRegisterAdapter succeeded
    PVOID               hPktPool;       // NDIS_HANDLE
    PVOID               hBufPool;       // NDIS_HANDLE
    PVOID               pTxHead;        // queued PNDIS_PACKETs (linked through MiniportReserved)
    PVOID               pTxTail;
    HANDLE              hTxQueueEvent;  // auto reset: packets queued

    // Receive filter requested by NDIS, applied by the link thread
    DWORD               dwPacketFilter; // NDIS_PACKET_TYPE_* bits
    DWORD               dwMcastCount;
    BYTE                abMcast[SMSC_MAX_MCAST][ETH_ALEN];
    HANDLE              hFilterEvent;   // auto reset: filter changed

    // Statistics
    volatile DWORD      dwRxFrames;     // good frames received from the chip
    volatile DWORD      dwRxBytes;
    volatile DWORD      dwRxErrors;     // error-summary frames
    volatile DWORD      dwRxBadLen;     // malformed status/length
    volatile DWORD      dwRxXferErrors; // failed bulk-IN transfers
    volatile DWORD      dwRxIndicated;  // frames handed to NDIS
    volatile DWORD      dwRxDropped;    // good frames not indicated (no miniport / filter 0)
    volatile DWORD      dwRxNoBuffer;   // no NDIS packet/buffer available
    volatile DWORD      dwTxFrames;     // frames in bulk-OUT transfers completed OK
    volatile DWORD      dwTxErrors;     // bad packets and failed transfers
    volatile DWORD      dwRxXfers;      // bulk-IN transfers completed OK
    volatile DWORD      dwTxBytes;      // frame bytes handed to bulk OUT
    volatile DWORD      dwTxXfers;      // bulk-OUT transfers completed OK
    volatile DWORD      dwIdleCount;    // incremented by the CPU-load probe thread

    volatile BOOL       fClosing;       // USB_CLOSE_DEVICE received / stopping
} SMSC_ADAPTER, *PSMSC_ADAPTER;

// smscusb.c
BOOL SmscReadReg(PSMSC_ADAPTER pAd, DWORD dwReg, DWORD *pdwValue);
BOOL SmscWriteReg(PSMSC_ADAPTER pAd, DWORD dwReg, DWORD dwValue);
BOOL SmscMiiRead(PSMSC_ADAPTER pAd, DWORD dwPhyReg, WORD *pwValue);
BOOL SmscMiiWrite(PSMSC_ADAPTER pAd, DWORD dwPhyReg, WORD wValue);
DWORD WINAPI SmscXferSignal(LPVOID lpvNotifyParameter);

// smschip.c
BOOL SmscChipInit(PSMSC_ADAPTER pAd);
BOOL SmscSetMac(PSMSC_ADAPTER pAd);
BOOL SmscStart(PSMSC_ADAPTER pAd);
void SmscStop(PSMSC_ADAPTER pAd);

// smscndis.c
BOOL SmscNdisAttach(PSMSC_ADAPTER pAd);
void SmscNdisDetach(PSMSC_ADAPTER pAd);
void SmscNdisIndicateRx(PSMSC_ADAPTER pAd, BYTE **ppFrame, DWORD *pcbFrame, DWORD dwCount);
void SmscNdisLinkChanged(PSMSC_ADAPTER pAd, BOOL fUp);
void SmscNdisGetFilter(PSMSC_ADAPTER pAd, DWORD *pdwFilter, DWORD *pdwHashH, DWORD *pdwHashL);
DWORD WINAPI SmscTxThread(LPVOID lpv);

#endif // __SMSC_H__

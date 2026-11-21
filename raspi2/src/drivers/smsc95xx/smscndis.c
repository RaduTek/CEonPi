//
// smscndis.c
//
// NDIS 5.1 miniport side of the SMSC LAN9512/LAN9514 USB Ethernet driver.
//
// How the adapter comes up (the WEC7 pattern for a bus client that becomes a
// network adapter, as in the Atheros SDIO and RNDIS function drivers):
// - USBDeviceAttach starts the chip, then SmscNdisAttach starts a thread that
//   calls NdisRegisterAdapter(L"SMSC95XX", L"SMSC95XX1"), retrying while
//   NDIS is not up yet;
// - NDIS reads HKLM\Comm\SMSC95XX (ImagePath = smsc95xx.dll), loads the DLL
//   (already loaded by USBD in the same process, so this module and its
//   globals are shared), calls DriverEntry, then MiniportInitialize for the
//   SMSC95XX1 instance, which picks up the attached adapter from g_pAdapter;
// - on USB_CLOSE_DEVICE, SmscNdisDetach calls NdisDeregisterAdapter, which
//   halts the miniport, before the USB side is stopped.
//
// Deserialized miniport:
// - SendPackets only queues the packets; the TX thread copies them into a
//   bulk-OUT slot (8-byte SMSC header + frame each, up to SMSC_TX_PACK_MAX
//   frames per slot when several are queued), completes them to NDIS at
//   once (the data has been copied) and issues the transfer; up to
//   SMSC_TX_SLOTS transfers are in flight;
// - received frames are indicated in place with NDIS_STATUS_RESOURCES, so
//   NDIS copies them during the call and never holds on to our packets
//   (MiniportReturnPacket is not used for them);
// - receive filter changes are stored here and applied to the chip by the
//   link thread (register writes are synchronous USB control transfers and
//   must not block NDIS).
//
#include <windows.h>
#include <string.h>
#include <ndis.h>
#include "smsc.h"

// The one attached chip, for MiniportInitialize.
static PSMSC_ADAPTER s_pAdapter = NULL;
static NDIS_HANDLE s_hWrapper = NULL;

static WCHAR s_szMiniport[] = L"SMSC95XX";
static WCHAR s_szInstance[] = L"SMSC95XX1";

// NdisRegisterAdapter retries: NDIS may not be initialized yet when USB
// enumerates the chip during boot.
#define SMSC_REG_RETRIES        30
#define SMSC_REG_RETRY_MS       2000

// Queued TX packets are linked through MiniportReserved.
#define TX_NEXT(p)              (*(PNDIS_PACKET *)&(p)->MiniportReserved[0])

#define SMSC_SUPPORTED_FILTERS  (NDIS_PACKET_TYPE_DIRECTED | NDIS_PACKET_TYPE_MULTICAST | \
                                 NDIS_PACKET_TYPE_ALL_MULTICAST | NDIS_PACKET_TYPE_BROADCAST | \
                                 NDIS_PACKET_TYPE_PROMISCUOUS)

static const NDIS_OID s_aSupportedOids[] =
{
    OID_GEN_SUPPORTED_LIST,
    OID_GEN_HARDWARE_STATUS,
    OID_GEN_MEDIA_SUPPORTED,
    OID_GEN_MEDIA_IN_USE,
    OID_GEN_MAXIMUM_LOOKAHEAD,
    OID_GEN_MAXIMUM_FRAME_SIZE,
    OID_GEN_LINK_SPEED,
    OID_GEN_TRANSMIT_BUFFER_SPACE,
    OID_GEN_RECEIVE_BUFFER_SPACE,
    OID_GEN_TRANSMIT_BLOCK_SIZE,
    OID_GEN_RECEIVE_BLOCK_SIZE,
    OID_GEN_VENDOR_ID,
    OID_GEN_VENDOR_DESCRIPTION,
    OID_GEN_CURRENT_PACKET_FILTER,
    OID_GEN_CURRENT_LOOKAHEAD,
    OID_GEN_DRIVER_VERSION,
    OID_GEN_MAXIMUM_TOTAL_SIZE,
    OID_GEN_PROTOCOL_OPTIONS,
    OID_GEN_MAC_OPTIONS,
    OID_GEN_MEDIA_CONNECT_STATUS,
    OID_GEN_MAXIMUM_SEND_PACKETS,
    OID_GEN_VENDOR_DRIVER_VERSION,
    OID_GEN_XMIT_OK,
    OID_GEN_RCV_OK,
    OID_GEN_XMIT_ERROR,
    OID_GEN_RCV_ERROR,
    OID_GEN_RCV_NO_BUFFER,
    OID_802_3_PERMANENT_ADDRESS,
    OID_802_3_CURRENT_ADDRESS,
    OID_802_3_MULTICAST_LIST,
    OID_802_3_MAXIMUM_LIST_SIZE,
    OID_802_3_RCV_ERROR_ALIGNMENT,
    OID_802_3_XMIT_ONE_COLLISION,
    OID_802_3_XMIT_MORE_COLLISIONS
};

static const char s_szVendorDesc[] = "SMSC LAN9512/LAN9514 USB 2.0 Fast Ethernet";

//------------------------------------------------------------------------------
// References: a thread that calls into NDIS outside an NDIS callback holds
// one while it uses the miniport handle; MiniportHalt waits for them.
//------------------------------------------------------------------------------
static NDIS_HANDLE SmscNdisRef(PSMSC_ADAPTER pAd)
{
    NDIS_HANDLE h;

    EnterCriticalSection(&pAd->csNdis);
    h = (NDIS_HANDLE)pAd->hMiniport;
    if (h != NULL)
        InterlockedIncrement(&pAd->lNdisRefs);
    LeaveCriticalSection(&pAd->csNdis);
    return h;
}

static void SmscNdisUnref(PSMSC_ADAPTER pAd)
{
    InterlockedDecrement(&pAd->lNdisRefs);
}

//------------------------------------------------------------------------------
// Called from the USB side
//------------------------------------------------------------------------------

// RX thread: indicate a batch of frames (pointers into the bulk-IN buffer,
// without FCS).
void SmscNdisIndicateRx(PSMSC_ADAPTER pAd, BYTE **ppFrame, DWORD *pcbFrame, DWORD dwCount)
{
    PNDIS_PACKET apPkt[SMSC_RX_BATCH];
    PNDIS_BUFFER pBuf;
    NDIS_STATUS st;
    NDIS_HANDLE h;
    DWORD i, m = 0;

    if (dwCount > SMSC_RX_BATCH)
        dwCount = SMSC_RX_BATCH;

    h = SmscNdisRef(pAd);
    if (h == NULL)
    {
        pAd->dwRxDropped += dwCount;
        return;
    }
    if (pAd->dwPacketFilter == 0)
    {
        pAd->dwRxDropped += dwCount;
        SmscNdisUnref(pAd);
        return;
    }

    for (i = 0; i < dwCount; i++)
    {
        NdisAllocatePacket(&st, &apPkt[m], (NDIS_HANDLE)pAd->hPktPool);
        if (st != NDIS_STATUS_SUCCESS)
            break;
        NdisAllocateBuffer(&st, &pBuf, (NDIS_HANDLE)pAd->hBufPool, ppFrame[i], pcbFrame[i]);
        if (st != NDIS_STATUS_SUCCESS)
        {
            NdisFreePacket(apPkt[m]);
            break;
        }
        NdisChainBufferAtFront(apPkt[m], pBuf);
        NDIS_SET_PACKET_HEADER_SIZE(apPkt[m], ETH_HLEN);
        // RESOURCES: NDIS copies the frame now and does not keep the packet.
        NDIS_SET_PACKET_STATUS(apPkt[m], NDIS_STATUS_RESOURCES);
        m++;
    }
    if (m < dwCount)
        pAd->dwRxNoBuffer += dwCount - m;

    if (m > 0)
    {
        NdisMIndicateReceivePacket(h, apPkt, m);
        pAd->dwRxIndicated += m;
    }

    for (i = 0; i < m; i++)
    {
        pBuf = NULL;
        NdisUnchainBufferAtFront(apPkt[i], &pBuf);
        if (pBuf != NULL)
            NdisFreeBuffer(pBuf);
        NdisFreePacket(apPkt[i]);
    }

    SmscNdisUnref(pAd);
}

// Link thread: media connect / disconnect.
void SmscNdisLinkChanged(PSMSC_ADAPTER pAd, BOOL fUp)
{
    NDIS_HANDLE h = SmscNdisRef(pAd);

    if (h == NULL)
        return;
    NdisMIndicateStatus(h, fUp ? NDIS_STATUS_MEDIA_CONNECT : NDIS_STATUS_MEDIA_DISCONNECT, NULL, 0);
    NdisMIndicateStatusComplete(h);
    SmscNdisUnref(pAd);
}

// Big-endian Ethernet CRC32 (MSB-first shift register, data LSB-first, no
// final inversion), as used by the SMSC multicast hash filter.
static DWORD SmscCrc32Be(const BYTE *p, DWORD cb)
{
    DWORD crc = 0xFFFFFFFF, i, bit, carry;
    BYTE d;

    for (i = 0; i < cb; i++)
    {
        d = p[i];
        for (bit = 0; bit < 8; bit++, d >>= 1)
        {
            carry = ((crc & 0x80000000) ? 1 : 0) ^ (d & 0x01);
            crc <<= 1;
            if (carry)
                crc = (crc ^ 0x04C11DB6) | carry;
        }
    }
    return crc;
}

// Link thread: current filter and the 64-bit multicast hash (top 6 CRC bits
// select the bit; bit 5 picks HASHH or HASHL).
void SmscNdisGetFilter(PSMSC_ADAPTER pAd, DWORD *pdwFilter, DWORD *pdwHashH, DWORD *pdwHashL)
{
    DWORD i, h, dwHashH = 0, dwHashL = 0;

    EnterCriticalSection(&pAd->csNdis);
    *pdwFilter = pAd->dwPacketFilter;
    for (i = 0; i < pAd->dwMcastCount; i++)
    {
        h = (SmscCrc32Be(pAd->abMcast[i], ETH_ALEN) >> 26) & 0x3F;
        if (h & 0x20)
            dwHashH |= 1UL << (h & 0x1F);
        else
            dwHashL |= 1UL << (h & 0x1F);
    }
    LeaveCriticalSection(&pAd->csNdis);

    *pdwHashH = dwHashH;
    *pdwHashL = dwHashL;
}

//------------------------------------------------------------------------------
// TX thread
//------------------------------------------------------------------------------

// Take the next queued packet, with a reference on the miniport handle.
static PNDIS_PACKET SmscTxDequeue(PSMSC_ADAPTER pAd, NDIS_HANDLE *ph)
{
    PNDIS_PACKET p = NULL;

    EnterCriticalSection(&pAd->csNdis);
    if (pAd->pTxHead != NULL && pAd->hMiniport != NULL)
    {
        p = (PNDIS_PACKET)pAd->pTxHead;
        pAd->pTxHead = TX_NEXT(p);
        if (pAd->pTxHead == NULL)
            pAd->pTxTail = NULL;
        *ph = (NDIS_HANDLE)pAd->hMiniport;
        InterlockedIncrement(&pAd->lNdisRefs);
    }
    LeaveCriticalSection(&pAd->csNdis);
    return p;
}

// Copy a packet's buffer chain to pDst. Returns the frame length, or 0 if
// the packet is not a valid Ethernet frame.
static DWORD SmscTxCopy(PNDIS_PACKET pPkt, BYTE *pDst)
{
    PNDIS_BUFFER pBuf = NULL;
    PVOID pVa;
    UINT cbTotal = 0, cb;
    DWORD off = 0;

    NdisQueryPacket(pPkt, NULL, NULL, &pBuf, &cbTotal);
    if (cbTotal < ETH_HLEN || cbTotal > ETH_MAX_FRAME)
        return 0;

    while (pBuf != NULL)
    {
        NdisQueryBuffer(pBuf, &pVa, &cb);
        if (off + cb > cbTotal)
            return 0;
        if (cb > 0)
            memcpy(pDst + off, pVa, cb);
        off += cb;
        NdisGetNextBuffer(pBuf, &pBuf);
    }
    return off;
}

static void SmscTxReap(PSMSC_ADAPTER pAd, SMSC_TX_SLOT *pSlot)
{
    DWORD dwBytes = 0, dwUsbErr = USB_NO_ERROR;

    pAd->pUsbFuncs->lpGetTransferStatus(pSlot->hXfer, &dwBytes, &dwUsbErr);
    pAd->pUsbFuncs->lpCloseTransfer(pSlot->hXfer);
    pSlot->hXfer = NULL;

    if (dwUsbErr == USB_NO_ERROR)
    {
        pAd->dwTxFrames += pSlot->dwFrames;
        pAd->dwTxXfers++;
    }
    else
    {
        pAd->dwTxErrors++;
        if (pAd->dwTxErrors <= 5 || (pAd->dwTxErrors % 100) == 0)
            SMSCWARN((TEXT("SMSC95XX: TX transfer failed, usberr=0x%x (%u so far)\r\n"),
                      dwUsbErr, pAd->dwTxErrors));
    }
}

// Bulk-OUT slots are used in ring order and complete in order (one pipe).
DWORD WINAPI SmscTxThread(LPVOID lpv)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpv;
    SMSC_TX_SLOT *pSlot;
    PNDIS_PACKET pPkt;
    NDIS_HANDLE h;
    HANDLE ah[3];
    DWORD dwHead = 0, dwTail = 0, dwInFlight = 0, n, cb, dwA, dwB, dwWait;
    DWORD dwOff, cbXfer, cbData;
    BOOL fEmpty;

    for (;;)
    {
        // Reap finished transfers.
        while (dwInFlight > 0 &&
               WaitForSingleObject(pAd->aTx[dwHead].hEvent, 0) == WAIT_OBJECT_0)
        {
            SmscTxReap(pAd, &pAd->aTx[dwHead]);
            dwHead = (dwHead + 1) % SMSC_TX_SLOTS;
            dwInFlight--;
        }

        // Fill free slots from the queue, packing up to SMSC_TX_PACK_MAX
        // queued frames into each transfer.
        while (dwInFlight < SMSC_TX_SLOTS && !pAd->fClosing)
        {
            pSlot = &pAd->aTx[dwTail];
            pSlot->dwFrames = 0;
            dwOff = 0;
            cbXfer = 0;
            cbData = 0;
            fEmpty = FALSE;

            while (pSlot->dwFrames < SMSC_TX_PACK_MAX)
            {
                pPkt = SmscTxDequeue(pAd, &h);
                if (pPkt == NULL)
                {
                    fEmpty = TRUE;
                    break;
                }

                cb = SmscTxCopy(pPkt, pSlot->pData + dwOff + SMSC_TX_HDR_SIZE);

                // The data is copied: the packet goes back to NDIS now.
                NdisMSendComplete(h, pPkt, cb ? NDIS_STATUS_SUCCESS : NDIS_STATUS_FAILURE);
                SmscNdisUnref(pAd);

                if (cb == 0)
                {
                    pAd->dwTxErrors++;
                    continue;
                }

                dwA = SMSC_TX_CTRL_0_BUF_SIZE(cb) | SMSC_TX_CTRL_0_FIRST_SEG | SMSC_TX_CTRL_0_LAST_SEG;
                dwB = SMSC_TX_CTRL_1_PKT_LENGTH(cb);
                memcpy(pSlot->pData + dwOff, &dwA, 4);
                memcpy(pSlot->pData + dwOff + 4, &dwB, 4);

                // The transfer ends right after the last frame; the next
                // header starts on the next 4-byte boundary.
                cbXfer = dwOff + SMSC_TX_HDR_SIZE + cb;
                dwOff = (cbXfer + 3) & ~3UL;
                cbData += cb;
                pSlot->dwFrames++;
            }

            if (pSlot->dwFrames == 0)
            {
                if (fEmpty)
                    break;
                continue;       // only bad packets were taken; try again
            }

            ResetEvent(pSlot->hEvent);
            pSlot->hXfer = pAd->pUsbFuncs->lpIssueBulkTransfer(pAd->hPipeOut, SmscXferSignal,
                                                                pSlot->hEvent, USB_OUT_TRANSFER,
                                                                cbXfer, pSlot->pData, pSlot->paData);
            if (pSlot->hXfer == NULL)
            {
                pAd->dwTxErrors += pSlot->dwFrames;
                continue;
            }
            pAd->dwTxBytes += cbData;
            dwTail = (dwTail + 1) % SMSC_TX_SLOTS;
            dwInFlight++;
            if (fEmpty)
                break;
        }

        n = 0;
        ah[n++] = pAd->hStopEvent;
        ah[n++] = pAd->hTxQueueEvent;
        if (dwInFlight > 0)
            ah[n++] = pAd->aTx[dwHead].hEvent;
        dwWait = WaitForMultipleObjects(n, ah, FALSE, INFINITE);
        if (dwWait == WAIT_OBJECT_0 || pAd->fClosing)
            break;
    }

    // Abort and reap what is still in flight.
    while (dwInFlight > 0)
    {
        pSlot = &pAd->aTx[dwHead];
        pAd->pUsbFuncs->lpAbortTransfer(pSlot->hXfer, 0);
        WaitForSingleObject(pSlot->hEvent, 500);
        pAd->pUsbFuncs->lpCloseTransfer(pSlot->hXfer);
        pSlot->hXfer = NULL;
        dwHead = (dwHead + 1) % SMSC_TX_SLOTS;
        dwInFlight--;
    }
    return 0;
}

//------------------------------------------------------------------------------
// Miniport handlers
//------------------------------------------------------------------------------
static NDIS_STATUS SmscMpInitialize(OUT PNDIS_STATUS OpenErrorStatus,
                                    OUT PUINT SelectedMediumIndex,
                                    IN PNDIS_MEDIUM MediumArray,
                                    IN UINT MediumArraySize,
                                    IN NDIS_HANDLE MiniportAdapterHandle,
                                    IN NDIS_HANDLE WrapperConfigurationContext)
{
    PSMSC_ADAPTER pAd = s_pAdapter;
    NDIS_HANDLE hPktPool = NULL, hBufPool = NULL, hConfig = NULL;
    NDIS_STATUS st;
    PVOID pAddr = NULL;
    UINT i, cbAddr = 0;

    UNREFERENCED_PARAMETER(OpenErrorStatus);

    for (i = 0; i < MediumArraySize; i++)
    {
        if (MediumArray[i] == NdisMedium802_3)
            break;
    }
    if (i == MediumArraySize)
        return NDIS_STATUS_UNSUPPORTED_MEDIA;
    *SelectedMediumIndex = i;

    if (pAd == NULL || pAd->fClosing)
    {
        SMSCWARN((TEXT("SMSC95XX: MiniportInitialize with no USB device attached\r\n")));
        return NDIS_STATUS_ADAPTER_NOT_FOUND;
    }
    if (pAd->hMiniport != NULL)
    {
        SMSCERR((TEXT("SMSC95XX: MiniportInitialize called twice\r\n")));
        return NDIS_STATUS_FAILURE;
    }

    NdisMSetAttributesEx(MiniportAdapterHandle, (NDIS_HANDLE)pAd, 0,
                         NDIS_ATTRIBUTE_DESERIALIZE |
                         NDIS_ATTRIBUTE_NO_HALT_ON_SUSPEND |
                         NDIS_ATTRIBUTE_IGNORE_PACKET_TIMEOUT |
                         NDIS_ATTRIBUTE_IGNORE_REQUEST_TIMEOUT,
                         NdisInterfaceInternal);

    NdisAllocatePacketPool(&st, &hPktPool, SMSC_RX_BATCH, PROTOCOL_RESERVED_SIZE_IN_PACKET);
    if (st != NDIS_STATUS_SUCCESS)
        return NDIS_STATUS_RESOURCES;
    NdisAllocateBufferPool(&st, &hBufPool, SMSC_RX_BATCH);
    if (st != NDIS_STATUS_SUCCESS)
    {
        NdisFreePacketPool(hPktPool);
        return NDIS_STATUS_RESOURCES;
    }

    // Registry override: Comm\SMSC95XX1\Parms\NetworkAddress.
    NdisOpenConfiguration(&st, &hConfig, WrapperConfigurationContext);
    if (st == NDIS_STATUS_SUCCESS)
    {
        NdisReadNetworkAddress(&st, &pAddr, &cbAddr, hConfig);
        if (st == NDIS_STATUS_SUCCESS && cbAddr == ETH_ALEN && pAddr != NULL &&
            (((BYTE *)pAddr)[0] & 0x01) == 0)
        {
            memcpy(pAd->abMac, pAddr, ETH_ALEN);
            if (SmscSetMac(pAd))
                SMSCINIT((TEXT("SMSC95XX: MAC overridden by registry NetworkAddress\r\n")));
        }
        NdisCloseConfiguration(hConfig);
    }

    EnterCriticalSection(&pAd->csNdis);
    pAd->hPktPool = hPktPool;
    pAd->hBufPool = hBufPool;
    pAd->dwPacketFilter = 0;
    pAd->dwMcastCount = 0;
    pAd->pTxHead = pAd->pTxTail = NULL;
    pAd->fMiniportActive = TRUE;
    pAd->hMiniport = MiniportAdapterHandle;
    LeaveCriticalSection(&pAd->csNdis);

    SMSCINIT((TEXT("SMSC95XX: miniport initialized, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\r\n"),
              pAd->abMac[0], pAd->abMac[1], pAd->abMac[2],
              pAd->abMac[3], pAd->abMac[4], pAd->abMac[5],
              pAd->fLinkUp ? TEXT("up") : TEXT("down")));
    return NDIS_STATUS_SUCCESS;
}

static VOID SmscMpHalt(IN NDIS_HANDLE MiniportAdapterContext)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)MiniportAdapterContext;
    PNDIS_PACKET pList, pNext;
    NDIS_HANDLE h;
    DWORD i;

    SMSCINIT((TEXT("SMSC95XX: miniport halt\r\n")));

    EnterCriticalSection(&pAd->csNdis);
    h = (NDIS_HANDLE)pAd->hMiniport;
    pAd->hMiniport = NULL;
    pList = (PNDIS_PACKET)pAd->pTxHead;
    pAd->pTxHead = pAd->pTxTail = NULL;
    LeaveCriticalSection(&pAd->csNdis);

    // Fail the packets that never reached the TX thread.
    while (pList != NULL)
    {
        pNext = TX_NEXT(pList);
        NdisMSendComplete(h, pList, NDIS_STATUS_FAILURE);
        pList = pNext;
    }

    // Wait for the RX/link/TX threads to leave NDIS.
    for (i = 0; i < 500 && InterlockedCompareExchange(&pAd->lNdisRefs, 0, 0) != 0; i++)
        Sleep(10);
    if (InterlockedCompareExchange(&pAd->lNdisRefs, 0, 0) != 0)
        SMSCERR((TEXT("SMSC95XX: halt with %d NDIS references outstanding\r\n"), pAd->lNdisRefs));

    if (pAd->hBufPool != NULL)
        NdisFreeBufferPool((NDIS_HANDLE)pAd->hBufPool);
    if (pAd->hPktPool != NULL)
        NdisFreePacketPool((NDIS_HANDLE)pAd->hPktPool);
    pAd->hBufPool = pAd->hPktPool = NULL;

    pAd->fMiniportActive = FALSE;
}

static NDIS_STATUS SmscMpReset(OUT PBOOLEAN AddressingReset, IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    *AddressingReset = FALSE;
    return NDIS_STATUS_SUCCESS;
}

static VOID SmscMpSendPackets(IN NDIS_HANDLE MiniportAdapterContext,
                              IN PPNDIS_PACKET PacketArray, IN UINT NumberOfPackets)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)MiniportAdapterContext;
    PNDIS_PACKET p;
    NDIS_HANDLE h;
    BOOL fQueued;
    UINT i;

    for (i = 0; i < NumberOfPackets; i++)
    {
        p = PacketArray[i];
        TX_NEXT(p) = NULL;
        fQueued = FALSE;

        EnterCriticalSection(&pAd->csNdis);
        h = (NDIS_HANDLE)pAd->hMiniport;
        if (h != NULL && !pAd->fClosing)
        {
            if (pAd->pTxTail != NULL)
                TX_NEXT((PNDIS_PACKET)pAd->pTxTail) = p;
            else
                pAd->pTxHead = p;
            pAd->pTxTail = p;
            fQueued = TRUE;
        }
        LeaveCriticalSection(&pAd->csNdis);

        if (!fQueued && h != NULL)
        {
            pAd->dwTxErrors++;
            NdisMSendComplete(h, p, NDIS_STATUS_FAILURE);
        }
    }

    if (pAd->hTxQueueEvent != NULL)
        SetEvent(pAd->hTxQueueEvent);
}

static VOID SmscMpReturnPacket(IN NDIS_HANDLE MiniportAdapterContext, IN PNDIS_PACKET Packet)
{
    // Every receive is indicated with NDIS_STATUS_RESOURCES, so NDIS never
    // keeps one of our packets; nothing to do.
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(Packet);
}

static VOID SmscMpCancelSendPackets(IN NDIS_HANDLE MiniportAdapterContext, IN PVOID CancelId)
{
    // Queued packets are sent within milliseconds; cancellation is not
    // supported.
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(CancelId);
}

static VOID SmscMpPnPEventNotify(IN NDIS_HANDLE MiniportAdapterContext,
                                 IN NDIS_DEVICE_PNP_EVENT DevicePnPEvent,
                                 IN PVOID InformationBuffer, IN ULONG InformationBufferLength)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    UNREFERENCED_PARAMETER(InformationBuffer);
    UNREFERENCED_PARAMETER(InformationBufferLength);
    SMSCINFO((TEXT("SMSC95XX: PnP event %d\r\n"), DevicePnPEvent));
}

static VOID SmscMpShutdown(IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
}

// No hardware interrupt is registered; these are never called.
static VOID SmscMpDisableInterrupt(IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
}

static VOID SmscMpEnableInterrupt(IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
}

static VOID SmscMpHandleInterrupt(IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
}

static VOID SmscMpIsr(OUT PBOOLEAN InterruptRecognized, OUT PBOOLEAN QueueMiniportHandleInterrupt,
                      IN NDIS_HANDLE MiniportAdapterContext)
{
    UNREFERENCED_PARAMETER(MiniportAdapterContext);
    *InterruptRecognized = FALSE;
    *QueueMiniportHandleInterrupt = FALSE;
}

static NDIS_STATUS SmscMpQueryInformation(IN NDIS_HANDLE MiniportAdapterContext, IN NDIS_OID Oid,
                                          IN PVOID InformationBuffer,
                                          IN ULONG InformationBufferLength,
                                          OUT PULONG BytesWritten, OUT PULONG BytesNeeded)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)MiniportAdapterContext;
    NDIS_STATUS st = NDIS_STATUS_SUCCESS;
    ULONG ulVal = 0;
    USHORT usVal = 0;
    NDIS_MEDIUM medium = NdisMedium802_3;
    NDIS_HARDWARE_STATUS hw = NdisHardwareStatusReady;
    BYTE abVendor[4];
    const void *pSrc = &ulVal;
    ULONG cbSrc = sizeof(ULONG);
    BYTE abMcast[SMSC_MAX_MCAST * ETH_ALEN];

    *BytesWritten = 0;
    *BytesNeeded = 0;

    switch (Oid)
    {
    case OID_GEN_SUPPORTED_LIST:
        pSrc = s_aSupportedOids;
        cbSrc = sizeof(s_aSupportedOids);
        break;
    case OID_GEN_HARDWARE_STATUS:
        pSrc = &hw;
        cbSrc = sizeof(hw);
        break;
    case OID_GEN_MEDIA_SUPPORTED:
    case OID_GEN_MEDIA_IN_USE:
        pSrc = &medium;
        cbSrc = sizeof(medium);
        break;
    case OID_GEN_MAXIMUM_LOOKAHEAD:
    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_MAXIMUM_FRAME_SIZE:
        ulVal = ETH_MTU;
        break;
    case OID_GEN_MAXIMUM_TOTAL_SIZE:
    case OID_GEN_TRANSMIT_BLOCK_SIZE:
    case OID_GEN_RECEIVE_BLOCK_SIZE:
        ulVal = ETH_MAX_FRAME;
        break;
    case OID_GEN_LINK_SPEED:
        // Units of 100 bit/s
        ulVal = (pAd->fLinkUp ? pAd->dwSpeedMbps : 100) * 10000;
        break;
    case OID_GEN_TRANSMIT_BUFFER_SPACE:
        ulVal = SMSC_TX_SLOTS * ETH_MAX_FRAME;
        break;
    case OID_GEN_RECEIVE_BUFFER_SPACE:
        ulVal = SMSC_RX_BUFFERS * SMSC_RX_BUF_SIZE;
        break;
    case OID_GEN_VENDOR_ID:
        abVendor[0] = pAd->abPermMac[0];
        abVendor[1] = pAd->abPermMac[1];
        abVendor[2] = pAd->abPermMac[2];
        abVendor[3] = 0;
        pSrc = abVendor;
        cbSrc = sizeof(abVendor);
        break;
    case OID_GEN_VENDOR_DESCRIPTION:
        pSrc = s_szVendorDesc;
        cbSrc = sizeof(s_szVendorDesc);
        break;
    case OID_GEN_CURRENT_PACKET_FILTER:
        ulVal = pAd->dwPacketFilter;
        break;
    case OID_GEN_DRIVER_VERSION:
        usVal = (USHORT)((NDIS_MINIPORT_MAJOR_VERSION << 8) | NDIS_MINIPORT_MINOR_VERSION);
        pSrc = &usVal;
        cbSrc = sizeof(usVal);
        break;
    case OID_GEN_VENDOR_DRIVER_VERSION:
        ulVal = 0x00010000;
        break;
    case OID_GEN_MAC_OPTIONS:
        ulVal = NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA | NDIS_MAC_OPTION_TRANSFERS_NOT_PEND |
                NDIS_MAC_OPTION_NO_LOOPBACK;
        break;
    case OID_GEN_MEDIA_CONNECT_STATUS:
        ulVal = pAd->fLinkUp ? NdisMediaStateConnected : NdisMediaStateDisconnected;
        break;
    case OID_GEN_MAXIMUM_SEND_PACKETS:
        ulVal = 16;
        break;
    case OID_GEN_XMIT_OK:
        ulVal = pAd->dwTxFrames;
        break;
    case OID_GEN_RCV_OK:
        ulVal = pAd->dwRxIndicated;
        break;
    case OID_GEN_XMIT_ERROR:
        ulVal = pAd->dwTxErrors;
        break;
    case OID_GEN_RCV_ERROR:
        ulVal = pAd->dwRxErrors + pAd->dwRxBadLen + pAd->dwRxXferErrors;
        break;
    case OID_GEN_RCV_NO_BUFFER:
        ulVal = pAd->dwRxNoBuffer;
        break;
    case OID_802_3_PERMANENT_ADDRESS:
        pSrc = pAd->abPermMac;
        cbSrc = ETH_ALEN;
        break;
    case OID_802_3_CURRENT_ADDRESS:
        pSrc = pAd->abMac;
        cbSrc = ETH_ALEN;
        break;
    case OID_802_3_MULTICAST_LIST:
        EnterCriticalSection(&pAd->csNdis);
        cbSrc = pAd->dwMcastCount * ETH_ALEN;
        memcpy(abMcast, pAd->abMcast, cbSrc);
        LeaveCriticalSection(&pAd->csNdis);
        pSrc = abMcast;
        break;
    case OID_802_3_MAXIMUM_LIST_SIZE:
        ulVal = SMSC_MAX_MCAST;
        break;
    case OID_802_3_RCV_ERROR_ALIGNMENT:
    case OID_802_3_XMIT_ONE_COLLISION:
    case OID_802_3_XMIT_MORE_COLLISIONS:
        ulVal = 0;
        break;
    default:
        st = NDIS_STATUS_NOT_SUPPORTED;
        break;
    }

    if (st == NDIS_STATUS_SUCCESS)
    {
        if (cbSrc > InformationBufferLength)
        {
            *BytesNeeded = cbSrc;
            st = NDIS_STATUS_INVALID_LENGTH;
        }
        else
        {
            if (cbSrc > 0)
                NdisMoveMemory(InformationBuffer, (PVOID)pSrc, cbSrc);
            *BytesWritten = cbSrc;
        }
    }
    return st;
}

static NDIS_STATUS SmscMpSetInformation(IN NDIS_HANDLE MiniportAdapterContext, IN NDIS_OID Oid,
                                        IN PVOID InformationBuffer,
                                        IN ULONG InformationBufferLength,
                                        OUT PULONG BytesRead, OUT PULONG BytesNeeded)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)MiniportAdapterContext;
    ULONG ulVal;

    *BytesRead = 0;
    *BytesNeeded = 0;

    switch (Oid)
    {
    case OID_GEN_CURRENT_PACKET_FILTER:
        if (InformationBufferLength < sizeof(ULONG))
        {
            *BytesNeeded = sizeof(ULONG);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        NdisMoveMemory(&ulVal, InformationBuffer, sizeof(ULONG));
        *BytesRead = sizeof(ULONG);
        // Bits for other media (source routing, functional, ...) are ignored.
        EnterCriticalSection(&pAd->csNdis);
        pAd->dwPacketFilter = ulVal & SMSC_SUPPORTED_FILTERS;
        LeaveCriticalSection(&pAd->csNdis);
        if (pAd->hFilterEvent != NULL)
            SetEvent(pAd->hFilterEvent);
        return NDIS_STATUS_SUCCESS;

    case OID_802_3_MULTICAST_LIST:
        if (InformationBufferLength % ETH_ALEN)
            return NDIS_STATUS_INVALID_LENGTH;
        if (InformationBufferLength / ETH_ALEN > SMSC_MAX_MCAST)
            return NDIS_STATUS_MULTICAST_FULL;
        EnterCriticalSection(&pAd->csNdis);
        pAd->dwMcastCount = InformationBufferLength / ETH_ALEN;
        if (InformationBufferLength > 0)
            NdisMoveMemory(pAd->abMcast, InformationBuffer, InformationBufferLength);
        LeaveCriticalSection(&pAd->csNdis);
        *BytesRead = InformationBufferLength;
        if (pAd->hFilterEvent != NULL)
            SetEvent(pAd->hFilterEvent);
        return NDIS_STATUS_SUCCESS;

    case OID_GEN_CURRENT_LOOKAHEAD:
    case OID_GEN_PROTOCOL_OPTIONS:
        if (InformationBufferLength < sizeof(ULONG))
        {
            *BytesNeeded = sizeof(ULONG);
            return NDIS_STATUS_INVALID_LENGTH;
        }
        *BytesRead = sizeof(ULONG);
        return NDIS_STATUS_SUCCESS;

    default:
        return NDIS_STATUS_NOT_SUPPORTED;
    }
}

//------------------------------------------------------------------------------
// DriverEntry: called by NDIS when it loads the DLL for the SMSC95XX miniport
//------------------------------------------------------------------------------
NTSTATUS DriverEntry(IN PDRIVER_OBJECT DriverObject, IN PUNICODE_STRING RegistryPath)
{
    NDIS_MINIPORT_CHARACTERISTICS mc;
    NDIS_STATUS st;

    SMSCINFO((TEXT("SMSC95XX: DriverEntry\r\n")));

    NdisMInitializeWrapper(&s_hWrapper, DriverObject, RegistryPath, NULL);
    if (s_hWrapper == NULL)
        return STATUS_UNSUCCESSFUL;

    memset(&mc, 0, sizeof(mc));
    mc.MajorNdisVersion = NDIS_MINIPORT_MAJOR_VERSION;
    mc.MinorNdisVersion = NDIS_MINIPORT_MINOR_VERSION;
    mc.CheckForHangHandler = NULL;
    mc.DisableInterruptHandler = SmscMpDisableInterrupt;
    mc.EnableInterruptHandler = SmscMpEnableInterrupt;
    mc.HaltHandler = SmscMpHalt;
    mc.HandleInterruptHandler = SmscMpHandleInterrupt;
    mc.InitializeHandler = SmscMpInitialize;
    mc.ISRHandler = SmscMpIsr;
    mc.QueryInformationHandler = SmscMpQueryInformation;
    mc.ReconfigureHandler = NULL;
    mc.ResetHandler = SmscMpReset;
    mc.SendHandler = NULL;                  // SendPacketsHandler is used
    mc.SetInformationHandler = SmscMpSetInformation;
    mc.TransferDataHandler = NULL;          // whole frames are indicated
    mc.ReturnPacketHandler = SmscMpReturnPacket;
    mc.SendPacketsHandler = SmscMpSendPackets;
    mc.AllocateCompleteHandler = NULL;
    mc.CancelSendPacketsHandler = SmscMpCancelSendPackets;
    mc.PnPEventNotifyHandler = SmscMpPnPEventNotify;
    mc.AdapterShutdownHandler = SmscMpShutdown;

    st = NdisMRegisterMiniport(s_hWrapper, &mc, sizeof(mc));
    if (st != NDIS_STATUS_SUCCESS)
    {
        SMSCERR((TEXT("SMSC95XX: NdisMRegisterMiniport failed 0x%08x\r\n"), st));
        NdisTerminateWrapper(s_hWrapper, NULL);
        s_hWrapper = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

//------------------------------------------------------------------------------
// Adapter registration (USB attach / detach)
//------------------------------------------------------------------------------
static DWORD WINAPI SmscRegisterThread(LPVOID lpv)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpv;
    NDIS_STATUS st;
    DWORD i;

    for (i = 0; i < SMSC_REG_RETRIES && !pAd->fClosing; i++)
    {
        st = NDIS_STATUS_FAILURE;
        NdisRegisterAdapter(&st, s_szMiniport, s_szInstance);
        if (st == NDIS_STATUS_SUCCESS)
        {
            pAd->fNdisRegistered = TRUE;
            SMSCINIT((TEXT("SMSC95XX: NDIS adapter %s registered\r\n"), s_szInstance));
            return 0;
        }
        if (i == 0 || (i % 5) == 4)
            SMSCWARN((TEXT("SMSC95XX: NdisRegisterAdapter failed 0x%08x (try %u), retrying\r\n"),
                      st, i + 1));
        if (WaitForSingleObject(pAd->hStopEvent, SMSC_REG_RETRY_MS) == WAIT_OBJECT_0)
            break;
    }
    if (!pAd->fClosing)
        SMSCERR((TEXT("SMSC95XX: giving up on NdisRegisterAdapter\r\n")));
    return 0;
}

// After the USB side has started: register the NDIS adapter (in a thread,
// because NDIS may not be ready yet and MiniportInitialize is synchronous).
BOOL SmscNdisAttach(PSMSC_ADAPTER pAd)
{
    if (s_pAdapter != NULL)
    {
        SMSCERR((TEXT("SMSC95XX: only one adapter is supported\r\n")));
        return FALSE;
    }
    s_pAdapter = pAd;
    pAd->hRegThread = CreateThread(NULL, 0, SmscRegisterThread, pAd, 0, NULL);
    if (pAd->hRegThread == NULL)
    {
        s_pAdapter = NULL;
        return FALSE;
    }
    return TRUE;
}

// Before the USB side is stopped: deregister the adapter and make sure the
// miniport has been halted. pAd->fClosing and hStopEvent are already set.
void SmscNdisDetach(PSMSC_ADAPTER pAd)
{
    NDIS_STATUS st;
    DWORD i;

    if (pAd->hRegThread != NULL)
    {
        WaitForSingleObject(pAd->hRegThread, 10000);
        CloseHandle(pAd->hRegThread);
        pAd->hRegThread = NULL;
    }

    if (pAd->fNdisRegistered)
    {
        st = NDIS_STATUS_FAILURE;
        NdisDeregisterAdapter(&st, s_szInstance);
        SMSCINIT((TEXT("SMSC95XX: NdisDeregisterAdapter -> 0x%08x\r\n"), st));
        pAd->fNdisRegistered = FALSE;
    }

    for (i = 0; i < 200 && pAd->fMiniportActive; i++)
        Sleep(50);
    if (pAd->fMiniportActive)
        SMSCERR((TEXT("SMSC95XX: miniport still active after deregister\r\n")));

    if (s_pAdapter == pAd)
        s_pAdapter = NULL;
}

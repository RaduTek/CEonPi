//
// cpipe.cpp
//
// CDwcPipe: transfer queue and channel state machine for one endpoint.
//
// Locking: every member is protected by the controller lock
// (CDwcHcd::LockHc). Client callbacks and client buffers are only touched
// in Complete(), which runs without the lock.
//
// Control, bulk and interrupt transfers on the root port, plus control, bulk
// and interrupt split transactions for a full/low speed device behind a high
// speed hub (OnSplitInterrupt). An interrupt split launches its start-split in
// a microframe reserved from the hub's transaction translator (CDwcTt) and,
// when the endpoint has no data, waits one poll interval before trying again.
//
#include <windows.h>
#include <globals.hpp>
#include <hcd.hpp>
#include "cdwchcd.h"
#include "cpipe.h"
#include "dwctt.h"

#define DWCMSG(x)   RETAILMSG(1, x)

CDwcPipe::CDwcPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
                   IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
                   IN const UCHAR bDeviceAddress,
                   IN const UCHAR bHubAddress, IN const UCHAR bHubPort,
                   IN const PVOID ttContext,
                   IN CDwcHcd * const pHcd)
    : CPipeAbs(lpEndpointDescriptor->bEndpointAddress)
    , m_pHcd(pHcd)
    , m_bEpType((UCHAR)(lpEndpointDescriptor->bmAttributes & USB_ENDPOINT_TYPE_MASK))
    , m_fLowSpeed(fIsLowSpeed)
    , m_fHighSpeed(fIsHighSpeed)
    , m_bHubAddress(bHubAddress)
    , m_bHubPort(bHubPort)
    , m_bInterval(lpEndpointDescriptor->bInterval)
    // Split transactions are needed for a full/low speed device reached through
    // a high speed hub's transaction translator. The deciding facts are the
    // device speed and the hub it hangs off, both known here:
    //   * full or low speed (not high speed), and
    //   * behind a real external hub (bHubAddress != 0; a device on the root
    //     port reports hub 0 and talks to the controller directly).
    // ttContext is NOT used for this: hcd2lib leaves it NULL during the addr-0
    // enumeration of a device behind a hub (the TT is associated only after the
    // device is addressed), so gating on it would deadlock enumeration. The
    // split path uses m_bHubAddress/m_bHubPort directly, so a NULL TT is fine.
    , m_fDoSplit(!fIsHighSpeed && bHubAddress != 0)
    , m_pTt((CDwcTt *)ttContext)
{
    DWORD dwInterval;

    UNREFERENCED_PARAMETER(bDeviceAddress);

    m_pNextPipe = NULL;
    m_wMaxPacket = (USHORT)(lpEndpointDescriptor->wMaxPacketSize & 0x7FF);
    if (m_wMaxPacket == 0)
        m_wMaxPacket = 8;

    // Poll interval for an interrupt endpoint, in HFNUM units: frames for
    // full/low speed, 2^(bInterval-1) microframes for high speed (USB 2.0
    // 9.6.6). Capped to half the frame-counter range so the wrap-safe
    // "frame reached" test stays unambiguous.
    if (m_fHighSpeed)
    {
        DWORD b = m_bInterval;
        if (b < 1)  b = 1;
        if (b > 16) b = 16;
        dwInterval = (1UL << (b - 1));
    }
    else
    {
        dwInterval = m_bInterval;
        if (dwInterval < 1)
            dwInterval = 1;
        // A full/low speed device behind a high speed hub is clocked on a high
        // speed bus, so HFNUM counts microframes while bInterval is in 1 ms
        // frames: convert to microframes so the poll rate is right.
        if (m_fDoSplit)
            dwInterval *= 8;
    }
    if (dwInterval > (HFNUM_MAX_FRNUM + 1) / 2)
        dwInterval = (HFNUM_MAX_FRNUM + 1) / 2;
    m_dwInterval = dwInterval;

    m_fOpen = FALSE;
    m_fHalted = FALSE;
    m_fLoggedData = FALSE;
    m_fWaitingChannel = FALSE;
    m_fWaitingFrame = FALSE;
    m_dwNextFrame = 0;
    m_bStartUframe = 0;
    m_fTtReserved = FALSE;
    m_iChannel = -1;
    m_dwNextPid = HCTSIZ_PID_DATA0;
    m_pHead = NULL;
    m_dwLastPollFrame = 0;
    m_hAbortEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

CDwcPipe::~CDwcPipe()
{
    ClosePipe();
    if (m_hAbortEvent != NULL)
        CloseHandle(m_hAbortEvent);
}

HCD_REQUEST_STATUS CDwcPipe::OpenPipe(void)
{
    if (m_hAbortEvent == NULL)
        return requestFailed;

    // A full/low speed device behind a high speed hub (m_fDoSplit) is reached
    // through the hub's transaction translator with split transactions. An
    // interrupt (periodic) split must launch its start-split in a fixed
    // microframe, so reserve one from the hub's TT; if the TT is full, the pipe
    // cannot be opened. Control and bulk splits, a device on the root port, a
    // high speed device, or a full/low speed device behind a full speed hub (no
    // TT) need nothing reserved here.
    if (m_fDoSplit && m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT && !m_fTtReserved)
    {
        int iStart = 0;
        if (m_pTt != NULL)
        {
            m_pHcd->LockHc();
            iStart = m_pTt->Reserve(m_wMaxPacket);
            m_pHcd->UnlockHc();
            if (iStart < 0)
            {
                DWCMSG((TEXT("DWCHCI: no TT bandwidth for interrupt pipe (hub %d port %d ep 0x%02x)\r\n"),
                        m_bHubAddress, m_bHubPort, m_bEndpointAddress));
                return requestFailed;
            }
            m_fTtReserved = TRUE;
        }
        m_bStartUframe = (UCHAR)iStart;
    }

    m_pHcd->LockHc();
    if (!m_fOpen)
    {
        m_fOpen = TRUE;
        m_pHcd->AddPipe(this);
    }
    m_pHcd->UnlockHc();
    return requestOK;
}

HCD_REQUEST_STATUS CDwcPipe::ClosePipe(void)
{
    DWC_TRANSFER *pTransfer;

    m_pHcd->LockHc();
    if (!m_fOpen)
    {
        m_pHcd->UnlockHc();
        return requestFailed;
    }
    m_fOpen = FALSE;        // no new transfers from now on
    // Give back any periodic bandwidth this pipe reserved from the TT.
    if (m_fTtReserved && m_pTt != NULL)
    {
        m_pTt->Release(m_bStartUframe, m_wMaxPacket);
        m_fTtReserved = FALSE;
    }
    m_pHcd->UnlockHc();

    // Cancel everything that is still queued
    for (;;)
    {
        m_pHcd->LockHc();
        pTransfer = m_pHead;
        m_pHcd->UnlockHc();
        if (pTransfer == NULL)
            break;
        AbortOne(pTransfer, NULL, NULL);
    }

    m_pHcd->LockHc();
    m_pHcd->RemovePipe(this);
    m_pHcd->UnlockHc();
    return requestOK;
}

HCD_REQUEST_STATUS CDwcPipe::IsPipeHalted(OUT LPBOOL const lpbHalted)
{
    m_pHcd->LockHc();
    if (lpbHalted)
        *lpbHalted = m_fHalted;
    m_pHcd->UnlockHc();
    return requestOK;
}

void CDwcPipe::ClearHaltedFlag(void)
{
    m_pHcd->LockHc();
    m_fHalted = FALSE;
    m_dwNextPid = HCTSIZ_PID_DATA0;     // a cleared endpoint restarts at DATA0
    m_pHcd->UnlockHc();
}

void CDwcPipe::ChangeMaxPacketSize(IN const USHORT wMaxPacketSize)
{
    m_pHcd->LockHc();
    if (wMaxPacketSize != 0)
        m_wMaxPacket = wMaxPacketSize;
    m_pHcd->UnlockHc();
}

void CDwcPipe::FreeTransfer(CDwcHcd *pHcd, DWC_TRANSFER *pTransfer)
{
    if (pTransfer->pCtlVa != NULL)
        pHcd->GetPhysMem()->FreeMemory(pTransfer->pCtlVa, pTransfer->paCtl, CPHYSMEM_FLAG_NOBLOCK);
    if (pTransfer->pDataVa != NULL)
        pHcd->GetPhysMem()->FreeMemory(pTransfer->pDataVa, pTransfer->paData, CPHYSMEM_FLAG_NOBLOCK);
    LocalFree(pTransfer);
}

HCD_REQUEST_STATUS CDwcPipe::IssueTransfer(
                                IN const UCHAR address,
                                IN LPTRANSFER_NOTIFY_ROUTINE const lpfnCallback,
                                IN LPVOID const lpvCallbackParameter,
                                IN const DWORD dwFlags,
                                IN LPCVOID const lpvControlHeader,
                                IN const DWORD dwStartingFrame,
                                IN const DWORD dwFrames,
                                IN LPCDWORD const aLengths,
                                IN const DWORD dwBufferSize,
                                IN_OUT LPVOID const lpvBuffer,
                                IN const ULONG paBuffer,
                                IN LPCVOID const lpvCancelId,
                                OUT LPDWORD const adwIsochErrors,
                                OUT LPDWORD const adwIsochLengths,
                                OUT LPBOOL const lpfComplete,
                                OUT LPDWORD const lpdwBytesTransferred,
                                OUT LPDWORD const lpdwError)
{
    DWC_TRANSFER *pTransfer;
    DWC_TRANSFER *pTail;
    CPhysMem *pMem = m_pHcd->GetPhysMem();
    BOOL fControl = (m_bEpType == USB_ENDPOINT_TYPE_CONTROL);
    BOOL fIn = (dwFlags & USB_IN_TRANSFER) ? TRUE : FALSE;
    DWORD cbAlloc;
    BOOL fOk = TRUE;

    UNREFERENCED_PARAMETER(dwStartingFrame);
    UNREFERENCED_PARAMETER(dwFrames);
    UNREFERENCED_PARAMETER(aLengths);
    UNREFERENCED_PARAMETER(adwIsochErrors);
    UNREFERENCED_PARAMETER(adwIsochLengths);
    // The client buffer is always copied through DMA memory, so a physical
    // address supplied by the client is not used.
    UNREFERENCED_PARAMETER(paBuffer);

    if (lpfComplete == NULL || lpdwBytesTransferred == NULL || lpdwError == NULL ||
        (fControl && lpvControlHeader == NULL) ||
        (dwBufferSize > 0 && lpvBuffer == NULL))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return requestFailed;
    }

    pTransfer = (DWC_TRANSFER *)LocalAlloc(LPTR, sizeof(DWC_TRANSFER));
    if (pTransfer == NULL)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return requestFailed;
    }

    pTransfer->bAddress = address;
    pTransfer->lpfnCallback = lpfnCallback;
    pTransfer->lpvCallbackParameter = lpvCallbackParameter;
    pTransfer->dwFlags = dwFlags;
    pTransfer->dwBufferSize = dwBufferSize;
    pTransfer->lpvBuffer = lpvBuffer;
    pTransfer->lpvCancelId = lpvCancelId;
    pTransfer->lpfComplete = lpfComplete;
    pTransfer->lpdwBytesTransferred = lpdwBytesTransferred;
    pTransfer->lpdwError = lpdwError;
    pTransfer->dwUsbError = USB_NOT_COMPLETE_ERROR;
    pTransfer->dwStage = fControl ? DWC_STAGE_SETUP : DWC_STAGE_DATA;

    if (fControl)
    {
        if (!pMem->AllocateMemory(DEBUG_PARAM(TEXT("DWC control block"))
                                  DWC_CTL_BLOCK_SIZE, &pTransfer->pCtlVa, CPHYSMEM_FLAG_NOBLOCK))
        {
            pTransfer->pCtlVa = NULL;
            fOk = FALSE;
        }
        else
        {
            pTransfer->paCtl = pMem->VaToPa(pTransfer->pCtlVa);
        }
    }

    if (fOk && dwBufferSize > 0)
    {
        // IN data is received in whole packets, so round the buffer up
        cbAlloc = dwBufferSize;
        if (fIn)
            cbAlloc = ((dwBufferSize + m_wMaxPacket - 1) / m_wMaxPacket) * m_wMaxPacket;

        if (!pMem->AllocateMemory(DEBUG_PARAM(TEXT("DWC data buffer"))
                                  cbAlloc, &pTransfer->pDataVa, CPHYSMEM_FLAG_NOBLOCK))
        {
            pTransfer->pDataVa = NULL;
            fOk = FALSE;
        }
        else
        {
            pTransfer->paData = pMem->VaToPa(pTransfer->pDataVa);
        }
    }

    if (!fOk)
    {
        FreeTransfer(m_pHcd, pTransfer);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return requestFailed;
    }

    __try
    {
        *lpfComplete = FALSE;
        *lpdwBytesTransferred = 0;
        *lpdwError = USB_NOT_COMPLETE_ERROR;
        if (fControl)
            memcpy(pTransfer->pCtlVa, lpvControlHeader, sizeof(USB_DEVICE_REQUEST));
        if (dwBufferSize > 0 && !fIn)
            memcpy(pTransfer->pDataVa, lpvBuffer, dwBufferSize);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        fOk = FALSE;
    }

    if (!fOk)
    {
        FreeTransfer(m_pHcd, pTransfer);
        SetLastError(ERROR_INVALID_PARAMETER);
        return requestFailed;
    }

    m_pHcd->LockHc();

    // A closed pipe cannot accept transfers. With the port down, a control or
    // bulk transfer cannot make progress either, so fail it. An interrupt
    // transfer is instead left pending (below): its owner - a hub's status-
    // change thread, or usbhid - blocks on completion, and a synchronous
    // failure makes that thread spin. It starts when the port returns, or is
    // cancelled by AbortTransfer when the device is torn down.
    if (!m_fOpen ||
        (!m_pHcd->IsPortEnabled() && m_bEpType != USB_ENDPOINT_TYPE_INTERRUPT))
    {
        m_pHcd->UnlockHc();
        FreeTransfer(m_pHcd, pTransfer);
        __try
        {
            *lpdwError = USB_DEVICE_NOT_RESPONDING_ERROR;
            *lpfComplete = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        SetLastError(ERROR_BROKEN_PIPE);
        return requestFailed;
    }

    // Append to the queue and start it if the pipe is idle
    if (m_pHead == NULL)
    {
        m_pHead = pTransfer;
        if (!m_pHcd->IsPortEnabled())
        {
            // Interrupt transfer on a down port: leave it pending (do not start
            // a channel that cannot run). The owner blocks until it completes
            // or is aborted on teardown.
        }
        // An interrupt endpoint is polled once per interval. If the client
        // re-issues right after a report (a mouse in constant motion returns
        // data on every poll), hold the next poll to the interval boundary
        // instead of starting at once, so the device is not over-polled and
        // its reports do not overrun the HID queue.
        else if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT &&
                 !m_pHcd->Core()->FrameReached((m_dwLastPollFrame + m_dwInterval) & HFNUM_MAX_FRNUM))
        {
            ScheduleNextInterval();
        }
        else
        {
            StartAttempt();
        }
    }
    else
    {
        pTail = m_pHead;
        while (pTail->pNext != NULL)
            pTail = pTail->pNext;
        pTail->pNext = pTransfer;
    }

    m_pHcd->UnlockHc();
    return requestOK;
}

// Start the next attempt of the transfer at the head of the queue.
// Lock held. The pipe must not have an attempt running.
void CDwcPipe::StartAttempt(void)
{
    DWC_TRANSFER *pTransfer = m_pHead;
    DWC_CHANNEL_SETUP s;
    DWORD dwMps = m_wMaxPacket;
    DWORD dwMaxPackets, dwRemaining;
    BOOL fDataIn;

    if (pTransfer == NULL)
        return;

    if (m_iChannel < 0)
    {
        m_iChannel = m_pHcd->AllocChannel(this);
        if (m_iChannel < 0)
        {
            m_fWaitingChannel = TRUE;   // FreeChannel will call Kick()
            return;
        }
    }
    m_fWaitingChannel = FALSE;

    memset(&s, 0, sizeof(s));
    s.dwDevAddr = pTransfer->bAddress;
    s.dwEpNum = m_bEndpointAddress & 0x0F;
    s.fLowSpeed = m_fLowSpeed;
    s.dwEpType = m_bEpType;
    s.dwMaxPacket = dwMps;

    fDataIn = (pTransfer->dwFlags & USB_IN_TRANSFER) ? TRUE : FALSE;

    // Largest attempt the channel can run, in packets
    dwMaxPackets = DWC_MAX_XFER_BYTES / dwMps;
    if (dwMaxPackets > DWC_MAX_XFER_PACKETS)
        dwMaxPackets = DWC_MAX_XFER_PACKETS;
    if (dwMaxPackets == 0)
        dwMaxPackets = 1;

    switch (pTransfer->dwStage)
    {
    case DWC_STAGE_SETUP:
        s.fIn = FALSE;
        s.dwPid = HCTSIZ_PID_SETUP;
        s.dwBytes = sizeof(USB_DEVICE_REQUEST);
        s.dwPackets = 1;
        s.dwDmaAddr = m_pHcd->BusAddress(pTransfer->paCtl);
        break;

    case DWC_STAGE_DATA:
        // A bulk or interrupt endpoint keeps a running data toggle in the
        // pipe. Seed this transfer from it once, then let it advance with
        // each completed packet. Control data always starts at DATA1 and is
        // seeded in OnChannelInterrupt after the SETUP stage.
        if (m_bEpType != USB_ENDPOINT_TYPE_CONTROL && !pTransfer->fDataPidInit)
        {
            pTransfer->dwPid = m_dwNextPid;
            pTransfer->fDataPidInit = TRUE;
        }
        dwRemaining = pTransfer->dwBufferSize - pTransfer->cbDone;
        s.fIn = fDataIn;
        s.dwPid = pTransfer->dwPid;
        s.dwPackets = (dwRemaining + dwMps - 1) / dwMps;
        if (s.dwPackets == 0)
            s.dwPackets = 1;            // zero length packet
        if (s.dwPackets > dwMaxPackets)
            s.dwPackets = dwMaxPackets;
        // An interrupt endpoint moves at most one packet per interval
        if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT && s.dwPackets > 1)
            s.dwPackets = 1;
        if (fDataIn)
        {
            // The core wants a whole number of packets for IN
            s.dwBytes = s.dwPackets * dwMps;
        }
        else
        {
            s.dwBytes = dwRemaining;
            if (s.dwBytes > s.dwPackets * dwMps)
                s.dwBytes = s.dwPackets * dwMps;
        }
        s.dwDmaAddr = m_pHcd->BusAddress(pTransfer->paData + pTransfer->cbDone);
        // Periodic transfer: tell the core which (micro)frame parity it goes
        // out in, as Linux dwc2_hc_set_even_odd_frame does.
        if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT)
            s.fOddFrame = ((m_pHcd->Core()->CurFrame() + 1) & 1) ? TRUE : FALSE;
        break;

    case DWC_STAGE_STATUS:
    default:
        // Opposite direction of the data stage; IN if there was no data
        s.fIn = (pTransfer->dwBufferSize > 0 && fDataIn) ? FALSE : TRUE;
        s.dwPid = HCTSIZ_PID_DATA1;
        s.dwPackets = 1;
        s.dwBytes = s.fIn ? dwMps : 0;
        s.dwDmaAddr = m_pHcd->BusAddress(pTransfer->paCtl + DWC_CTL_STATUS_OFFSET);
        break;
    }

    // Split transaction: route through the hub's TT and move one packet per
    // start-split/complete-split pair. fCompleteSplit selects the phase.
    if (m_fDoSplit)
    {
        s.fDoSplit = TRUE;
        s.fCompleteSplit = pTransfer->fCompleteSplit;
        s.dwHubAddr = m_bHubAddress;
        s.dwHubPort = m_bHubPort;
        s.dwXactPos = HCSPLT_XACTPOS_ALL;
        if (s.dwPackets > 1)
        {
            s.dwPackets = 1;
            if (s.fIn)
                s.dwBytes = dwMps;
            else if (s.dwBytes > dwMps)
                s.dwBytes = dwMps;
        }
    }

    pTransfer->fAttemptIn = s.fIn;
    pTransfer->cbAttempt = s.dwBytes;
    pTransfer->dwAttemptPackets = s.dwPackets;

    // Remember when each interrupt poll begins (its start-split, for a split
    // pipe) so the pipe is not polled faster than its interval. Not the
    // complete-split or a data continuation, which are part of the same poll.
    if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT && !pTransfer->fCompleteSplit)
        m_dwLastPollFrame = m_pHcd->Core()->CurFrame();

    m_pHcd->Core()->ChannelStart((DWORD)m_iChannel, &s);
}

// Lock held
void CDwcPipe::ReleaseChannel(void)
{
    int iChannel = m_iChannel;

    if (iChannel >= 0)
    {
        m_iChannel = -1;
        m_pHcd->Core()->ChannelRelease((DWORD)iChannel);
        m_pHcd->FreeChannel(iChannel);
    }
}

// Lock held. Called when a channel became free.
void CDwcPipe::Kick(void)
{
    if (m_fWaitingChannel && m_pHead != NULL && m_iChannel < 0)
        StartAttempt();
}

// A split transfer whose endpoint had nothing to send (a NAK) or could not be
// collected. For an interrupt endpoint, wait one poll interval before the next
// start-split so an idle endpoint does not hammer the TT; control and bulk
// retry at once. Lock held; the channel is already released.
void CDwcPipe::RetrySplit(void)
{
    if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT)
        ScheduleNextInterval();
    else
        StartAttempt();
}

// Interrupt endpoint: the head transfer is not ready (NAK) or missed its
// slot (frame overrun). Hold it and poll again one interval later. The
// controller arms the start-of-frame interrupt while any pipe is waiting.
// Lock held.
void CDwcPipe::ScheduleNextInterval(void)
{
    DWORD dwCur = m_pHcd->Core()->CurFrame();
    DWORD dwNext = (dwCur + m_dwInterval) & HFNUM_MAX_FRNUM;

    // A periodic split launches its start-split in the microframe reserved from
    // the TT. HFNUM counts microframes on the high speed bus, so step forward
    // to the next frame whose low three bits are the reserved microframe.
    if (m_fDoSplit)
    {
        int iGuard = 0;
        while ((dwNext & 7) != m_bStartUframe && iGuard++ < 8)
            dwNext = (dwNext + 1) & HFNUM_MAX_FRNUM;
    }

    m_dwNextFrame = dwNext;
    if (!m_fWaitingFrame)
    {
        m_fWaitingFrame = TRUE;
        m_pHcd->SofWaiterAdd();
    }
}

// Drop a pending frame wait and release the controller's SOF reference.
// Lock held.
void CDwcPipe::ClearFrameWait(void)
{
    if (m_fWaitingFrame)
    {
        m_fWaitingFrame = FALSE;
        m_pHcd->SofWaiterRemove();
    }
}

// Start-of-frame reached. If this pipe's scheduled frame has arrived, start
// the poll. Lock held.
void CDwcPipe::OnSofFrame(void)
{
    if (!m_fWaitingFrame)
        return;
    if (!m_pHcd->Core()->FrameReached(m_dwNextFrame))
        return;

    m_fWaitingFrame = FALSE;
    m_pHcd->SofWaiterRemove();

    if (m_pHead != NULL && m_iChannel < 0)
        StartAttempt();
}

// Append a transfer that is already off the queue to the done list,
// keeping completion order.
void CDwcPipe::AppendDone(DWC_TRANSFER *pTransfer, DWORD dwUsbError, DWC_TRANSFER **ppDone)
{
    DWC_TRANSFER *pTail;

    pTransfer->dwUsbError = dwUsbError;
    pTransfer->pNext = NULL;

    if (*ppDone == NULL)
    {
        *ppDone = pTransfer;
    }
    else
    {
        pTail = *ppDone;
        while (pTail->pNext != NULL)
            pTail = pTail->pNext;
        pTail->pNext = pTransfer;
    }
}

// Take the finished head transfer off the queue and start the next one.
// Lock held.
void CDwcPipe::Finish(DWC_TRANSFER *pTransfer, DWORD dwUsbError, DWC_TRANSFER **ppDone)
{
    // Only the head of the queue is ever active
    if (m_pHead == pTransfer)
        m_pHead = pTransfer->pNext;

    AppendDone(pTransfer, dwUsbError, ppDone);

    if (m_pHead != NULL && m_iChannel < 0)
        StartAttempt();
}

// The channel of this pipe raised an interrupt. Lock held.
void CDwcPipe::OnChannelInterrupt(DWORD dwHcint, DWC_TRANSFER **ppDone)
{
    DWC_TRANSFER *pTransfer = m_pHead;
    CDwcCore *pCore = m_pHcd->Core();
    DWORD dwBytesLeft, dwPacketsLeft, dwNextPid, dwGot, dwRemaining, dwDone;
    DWORD dwStage, dwErr;
    BOOL fDone;

    if (m_iChannel < 0)
        return;

    if (pTransfer == NULL)
    {
        ReleaseChannel();
        return;
    }

    if (!(dwHcint & HCINT_CHHLTD))
    {
        if (dwHcint & HCINT_AHBERR)
        {
            // DMA failed. This points at a wrong bus address (DmaBusOffset).
            DWCMSG((TEXT("DWCHCI: ***ERROR*** AHB (DMA) error on channel %d, HCDMA=0x%08x HCINT=0x%08x\r\n"),
                    m_iChannel, pCore->Rd(HCDMA(m_iChannel)), dwHcint));
            pTransfer->fBusError = TRUE;
            if (pCore->ChannelRequestHalt((DWORD)m_iChannel))
                return;                 // wait for the halt
        }
        else
        {
            return;                     // nothing we asked for
        }
    }

    // The channel has stopped. Read what it did before giving it back.
    dwBytesLeft = pCore->ChannelBytesLeft((DWORD)m_iChannel);
    dwPacketsLeft = pCore->ChannelPacketsLeft((DWORD)m_iChannel);
    dwNextPid = pCore->ChannelNextPid((DWORD)m_iChannel);
    dwStage = pTransfer->dwStage;
    ReleaseChannel();

    if (pTransfer->fAborting)
    {
        // AbortOne() is waiting for this and finishes the transfer
        SetEvent(m_hAbortEvent);
        return;
    }

    if (pTransfer->fBusError)
    {
        Finish(pTransfer, USB_NOT_COMPLETE_ERROR, ppDone);
        return;
    }

    // Split transactions run their own start-split/complete-split machine.
    if (m_fDoSplit)
    {
        OnSplitInterrupt(dwHcint, dwStage, dwBytesLeft, dwNextPid, ppDone);
        return;
    }

    if (dwHcint & HCINT_XFERCOMPL)
    {
        pTransfer->dwErrCount = 0;

        switch (dwStage)
        {
        case DWC_STAGE_SETUP:
            pTransfer->dwStage = (pTransfer->dwBufferSize > 0) ? DWC_STAGE_DATA : DWC_STAGE_STATUS;
            pTransfer->dwPid = HCTSIZ_PID_DATA1;    // control data always starts with DATA1
            StartAttempt();
            break;

        case DWC_STAGE_DATA:
            dwRemaining = pTransfer->dwBufferSize - pTransfer->cbDone;
            if (pTransfer->fAttemptIn)
            {
                dwGot = (dwBytesLeft <= pTransfer->cbAttempt) ? (pTransfer->cbAttempt - dwBytesLeft) : 0;
                // Complete means all packets arrived or a short packet ended it
                fDone = (dwGot < pTransfer->cbAttempt);
                if (dwGot > dwRemaining)
                    dwGot = dwRemaining;
            }
            else
            {
                dwGot = pTransfer->cbAttempt;
                fDone = FALSE;
            }
            pTransfer->cbDone += dwGot;
            pTransfer->dwPid = dwNextPid;
            if (pTransfer->cbDone >= pTransfer->dwBufferSize)
                fDone = TRUE;

            if (!fDone)
            {
                StartAttempt();         // next part of a large transfer
            }
            else if (m_bEpType == USB_ENDPOINT_TYPE_CONTROL)
            {
                pTransfer->dwStage = DWC_STAGE_STATUS;
                StartAttempt();
            }
            else
            {
                m_dwNextPid = dwNextPid;
                // One line the first time data flows on a bulk or interrupt
                // pipe, so the log shows the endpoint came alive without a
                // message per report.
                if (!m_fLoggedData)
                {
                    m_fLoggedData = TRUE;
                    DWCMSG((TEXT("DWCHCI: %s pipe ep 0x%02x alive: first packet %d bytes\r\n"),
                            (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT) ? TEXT("interrupt") : TEXT("bulk"),
                            m_bEndpointAddress, pTransfer->cbDone));
                }
                Finish(pTransfer, USB_NO_ERROR, ppDone);
            }
            break;

        case DWC_STAGE_STATUS:
        default:
            Finish(pTransfer, USB_NO_ERROR, ppDone);
            break;
        }
        return;
    }

    if (dwHcint & HCINT_STALL)
    {
        // A control endpoint recovers from a stall with the next SETUP
        if (m_bEpType != USB_ENDPOINT_TYPE_CONTROL)
        {
            m_fHalted = TRUE;
            m_dwNextPid = HCTSIZ_PID_DATA0;
        }
        Finish(pTransfer, USB_STALL_ERROR, ppDone);
        return;
    }

    if (dwHcint & HCINT_BBLERR)
    {
        DWCMSG((TEXT("DWCHCI: babble error, ep 0x%02x stage %d\r\n"), m_bEndpointAddress, dwStage));
        Finish(pTransfer, USB_DATA_OVERRUN_ERROR, ppDone);
        return;
    }

    // Everything below is retried. Keep the progress made in a data stage.
    if (dwStage == DWC_STAGE_DATA)
    {
        if (pTransfer->fAttemptIn)
        {
            dwDone = (dwBytesLeft <= pTransfer->cbAttempt) ? (pTransfer->cbAttempt - dwBytesLeft) : 0;
        }
        else
        {
            dwDone = (dwPacketsLeft <= pTransfer->dwAttemptPackets) ?
                     (pTransfer->dwAttemptPackets - dwPacketsLeft) * m_wMaxPacket : 0;
            if (dwDone > pTransfer->cbAttempt)
                dwDone = pTransfer->cbAttempt;
        }
        dwRemaining = pTransfer->dwBufferSize - pTransfer->cbDone;
        if (dwDone > dwRemaining)
            dwDone = dwRemaining;
        if (dwDone > 0)
        {
            pTransfer->cbDone += dwDone;
            pTransfer->dwPid = dwNextPid;
        }
    }

    // An interrupt endpoint that is merely not ready (NAK) or that missed
    // its frame (frame overrun) is retried one interval later, not at once,
    // and the miss is not counted as an error. Genuine transaction or toggle
    // errors fall through to the retry-with-count path below.
    if (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT &&
        (dwHcint & (HCINT_NAK | HCINT_FRMOVRUN)) &&
        !(dwHcint & (HCINT_XACTERR | HCINT_DATATGLERR)))
    {
        pTransfer->dwErrCount = 0;
        ScheduleNextInterval();
        return;
    }

    if ((dwHcint & HCINT_NAK) && m_bEpType != USB_ENDPOINT_TYPE_INTERRUPT)
    {
        // Control or bulk: the device is busy, not broken. Try again at once.
        // (An interrupt NAK was handled above by the interval scheduler.)
        pTransfer->dwErrCount = 0;
        StartAttempt();
        return;
    }

    // Transaction error, data toggle error, frame overrun, or a halt with
    // no reason at all: count it and retry a few times.
    pTransfer->dwErrCount++;
    if (pTransfer->dwErrCount < DWC_MAX_ERRORS)
    {
        StartAttempt();
        return;
    }

    dwErr = (dwHcint & HCINT_DATATGLERR) ? USB_DATA_TOGGLE_MISMATCH_ERROR : USB_DEVICE_NOT_RESPONDING_ERROR;
    DWCMSG((TEXT("DWCHCI: transfer failed, addr %d ep 0x%02x stage %d HCINT=0x%08x\r\n"),
            pTransfer->bAddress, m_bEndpointAddress, dwStage, dwHcint));
    Finish(pTransfer, dwErr, ppDone);
}

// Split-transaction state machine. The channel has halted; decode the
// handshake and drive the start-split / complete-split sequence. One USB
// packet needs a start-split (SSPLIT) to the hub's TT, then one or more
// complete-splits (CSPLIT) to collect the result. Lock held; the channel is
// already released. Mirrors Linux dwc2 dwc2_hc_ack/nak/nyet/xacterr_intr.
void CDwcPipe::OnSplitInterrupt(DWORD dwHcint, DWORD dwStage, DWORD dwBytesLeft,
                                DWORD dwNextPid, DWC_TRANSFER **ppDone)
{
    DWC_TRANSFER *pTransfer = m_pHead;
    DWORD dwGot, dwRemaining, dwErr;
    BOOL fDone;

    // A stall is terminal regardless of split phase.
    if (dwHcint & HCINT_STALL)
    {
        if (m_bEpType != USB_ENDPOINT_TYPE_CONTROL)
        {
            m_fHalted = TRUE;
            m_dwNextPid = HCTSIZ_PID_DATA0;
        }
        Finish(pTransfer, USB_STALL_ERROR, ppDone);
        return;
    }

    // A periodic split that only missed its frame window (frame overrun, no
    // real error or completion): try again at the next poll interval without
    // counting it against the error limit. Control/bulk never see this.
    if ((dwHcint & HCINT_FRMOVRUN) &&
        !(dwHcint & (HCINT_XFERCOMPL | HCINT_XACTERR | HCINT_BBLERR | HCINT_DATATGLERR)) &&
        m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT)
    {
        pTransfer->fCompleteSplit = FALSE;
        pTransfer->dwErrCount = 0;
        pTransfer->dwNyetCount = 0;
        ScheduleNextInterval();
        return;
    }

    // Transaction error, babble or data-toggle error: count toward the
    // three-strikes limit and restart from the start-split, or give up.
    if (dwHcint & (HCINT_XACTERR | HCINT_BBLERR | HCINT_DATATGLERR))
    {
        pTransfer->dwErrCount++;
        if (pTransfer->dwErrCount < DWC_MAX_ERRORS)
        {
            pTransfer->fCompleteSplit = FALSE;
            pTransfer->dwNyetCount = 0;
            StartAttempt();
            return;
        }
        dwErr = (dwHcint & HCINT_BBLERR) ? USB_DATA_OVERRUN_ERROR : USB_DEVICE_NOT_RESPONDING_ERROR;
        DWCMSG((TEXT("DWCHCI: split failed, addr %d ep 0x%02x stage %d HCINT=0x%08x\r\n"),
                pTransfer->bAddress, m_bEndpointAddress, dwStage, dwHcint));
        Finish(pTransfer, dwErr, ppDone);
        return;
    }

    if (!pTransfer->fCompleteSplit)
    {
        // ---- Start-split phase ----
        if (dwHcint & HCINT_ACK)
        {
            // The TT accepted the start-split; collect it with a complete-split.
            pTransfer->fCompleteSplit = TRUE;
            pTransfer->dwErrCount = 0;
            pTransfer->dwNakCount = 0;
            pTransfer->dwNyetCount = 0;
            StartAttempt();
            return;
        }
        if (dwHcint & HCINT_NAK)
        {
            // Device/TT not ready. Control/bulk redo the start-split at once;
            // an interrupt endpoint waits for its next poll interval.
            pTransfer->dwErrCount = 0;
            RetrySplit();
            return;
        }
        // Halt with no handshake: retry the start-split a few times.
        pTransfer->dwErrCount++;
        if (pTransfer->dwErrCount < DWC_MAX_ERRORS)
        {
            StartAttempt();
            return;
        }
        DWCMSG((TEXT("DWCHCI: split SSPLIT no response, addr %d ep 0x%02x HCINT=0x%08x\r\n"),
                pTransfer->bAddress, m_bEndpointAddress, dwHcint));
        Finish(pTransfer, USB_DEVICE_NOT_RESPONDING_ERROR, ppDone);
        return;
    }

    // ---- Complete-split phase ----
    if (dwHcint & HCINT_XFERCOMPL)
    {
        pTransfer->fCompleteSplit = FALSE;      // this packet is done
        pTransfer->dwErrCount = 0;
        pTransfer->dwNakCount = 0;
        pTransfer->dwNyetCount = 0;

        switch (dwStage)
        {
        case DWC_STAGE_SETUP:
            pTransfer->dwStage = (pTransfer->dwBufferSize > 0) ? DWC_STAGE_DATA : DWC_STAGE_STATUS;
            pTransfer->dwPid = HCTSIZ_PID_DATA1;
            StartAttempt();
            break;

        case DWC_STAGE_DATA:
            dwRemaining = pTransfer->dwBufferSize - pTransfer->cbDone;
            if (pTransfer->fAttemptIn)
            {
                dwGot = (dwBytesLeft <= pTransfer->cbAttempt) ? (pTransfer->cbAttempt - dwBytesLeft) : 0;
                fDone = (dwGot < pTransfer->cbAttempt);      // short packet ends it
                if (dwGot > dwRemaining)
                    dwGot = dwRemaining;
            }
            else
            {
                dwGot = pTransfer->cbAttempt;
                fDone = FALSE;
            }
            pTransfer->cbDone += dwGot;
            pTransfer->dwPid = dwNextPid;
            if (pTransfer->cbDone >= pTransfer->dwBufferSize)
                fDone = TRUE;

            if (!fDone)
            {
                StartAttempt();         // next packet: a fresh start-split
            }
            else if (m_bEpType == USB_ENDPOINT_TYPE_CONTROL)
            {
                pTransfer->dwStage = DWC_STAGE_STATUS;
                StartAttempt();
            }
            else
            {
                m_dwNextPid = dwNextPid;
                if (!m_fLoggedData)
                {
                    m_fLoggedData = TRUE;
                    DWCMSG((TEXT("DWCHCI: split %s pipe ep 0x%02x alive: first packet %d bytes\r\n"),
                            (m_bEpType == USB_ENDPOINT_TYPE_INTERRUPT) ? TEXT("interrupt") : TEXT("bulk"),
                            m_bEndpointAddress, pTransfer->cbDone));
                }
                Finish(pTransfer, USB_NO_ERROR, ppDone);
            }
            break;

        case DWC_STAGE_STATUS:
        default:
            Finish(pTransfer, USB_NO_ERROR, ppDone);
            break;
        }
        return;
    }

    if (dwHcint & HCINT_NYET)
    {
        // Data not ready at the TT yet. Re-issue the complete-split at once;
        // after a few, start the whole split over so a stale TT state cannot
        // wedge us (an interrupt endpoint restarts at its next poll interval).
        pTransfer->dwNyetCount++;
        if (pTransfer->dwNyetCount >= DWC_MAX_CSPLIT_NYET)
        {
            pTransfer->fCompleteSplit = FALSE;
            pTransfer->dwNyetCount = 0;
            RetrySplit();
        }
        else
        {
            StartAttempt();
        }
        return;
    }

    if (dwHcint & HCINT_NAK)
    {
        // Device NAKed through the TT (no data). Go back to the start-split;
        // an interrupt endpoint waits for its next poll interval.
        pTransfer->fCompleteSplit = FALSE;
        pTransfer->dwErrCount = 0;
        pTransfer->dwNyetCount = 0;
        RetrySplit();
        return;
    }

    // Halt with no recognised handshake on the complete-split: retry from the
    // start-split a few times, then give up.
    pTransfer->dwErrCount++;
    if (pTransfer->dwErrCount < DWC_MAX_ERRORS)
    {
        pTransfer->fCompleteSplit = FALSE;
        StartAttempt();
        return;
    }
    DWCMSG((TEXT("DWCHCI: split CSPLIT no response, addr %d ep 0x%02x HCINT=0x%08x\r\n"),
            pTransfer->bAddress, m_bEndpointAddress, dwHcint));
    Finish(pTransfer, USB_DEVICE_NOT_RESPONDING_ERROR, ppDone);
}

// The device was unplugged. CDwcHcd has already disabled all channels and
// cleared the channel table. Fail everything that is queued. Lock held.
void CDwcPipe::OnDisconnect(DWC_TRANSFER **ppDone)
{
    DWC_TRANSFER *pTransfer;
    DWC_TRANSFER *pKeep = NULL;

    m_iChannel = -1;
    m_fWaitingChannel = FALSE;
    ClearFrameWait();

    while ((pTransfer = m_pHead) != NULL)
    {
        m_pHead = pTransfer->pNext;
        pTransfer->pNext = NULL;

        if (pTransfer->fAborting)
        {
            // AbortOne() owns this one. Wake it up and leave it queued.
            pKeep = pTransfer;
            SetEvent(m_hAbortEvent);
        }
        else
        {
            AppendDone(pTransfer, USB_DEVICE_NOT_RESPONDING_ERROR, ppDone);
        }
    }
    m_pHead = pKeep;
}

// Cancel one transfer and report it as cancelled. Called without the lock.
void CDwcPipe::AbortOne(DWC_TRANSFER *pTransfer, LPTRANSFER_NOTIFY_ROUTINE lpCancelAddress,
                        LPVOID lpvNotifyParameter)
{
    DWC_TRANSFER *pCur;
    DWC_TRANSFER *pPrev = NULL;
    CDwcCore *pCore = m_pHcd->Core();
    BOOL fWasHead;

    m_pHcd->LockHc();

    // Make sure it is still queued (it may have completed meanwhile)
    for (pCur = m_pHead; pCur != NULL && pCur != pTransfer; pCur = pCur->pNext)
        pPrev = pCur;
    if (pCur == NULL)
    {
        m_pHcd->UnlockHc();
        return;
    }

    fWasHead = (pTransfer == m_pHead);

    if (fWasHead && m_iChannel >= 0)
    {
        // It is running on a channel. Ask the core to stop the channel and
        // wait for the interrupt thread to see the halt.
        pTransfer->fAborting = TRUE;
        ResetEvent(m_hAbortEvent);
        if (!pCore->ChannelRequestHalt((DWORD)m_iChannel))
        {
            ReleaseChannel();           // it was not running after all
        }
        else
        {
            m_pHcd->UnlockHc();
            WaitForSingleObject(m_hAbortEvent, DWC_ABORT_WAIT_MS);
            m_pHcd->LockHc();
            if (m_iChannel >= 0)
            {
                // No halt came (the port is probably disabled)
                pCore->ChannelForceOff((DWORD)m_iChannel);
                ReleaseChannel();
            }
        }
        // While aborting the transfer stays at the head of the queue
        pPrev = NULL;
    }

    if (fWasHead)
    {
        m_fWaitingChannel = FALSE;
        ClearFrameWait();           // it may have been waiting for its frame
        m_pHead = pTransfer->pNext;
    }
    else if (pPrev != NULL)
    {
        pPrev->pNext = pTransfer->pNext;
    }
    pTransfer->pNext = NULL;
    pTransfer->dwUsbError = USB_CANCELED_ERROR;

    if (fWasHead && m_pHead != NULL && m_iChannel < 0 && m_fOpen)
        StartAttempt();

    m_pHcd->UnlockHc();

    Complete(m_pHcd, pTransfer);

    if (lpCancelAddress != NULL)
    {
        __try
        {
            (*lpCancelAddress)(lpvNotifyParameter);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

HCD_REQUEST_STATUS CDwcPipe::AbortTransfer(
                                IN const LPTRANSFER_NOTIFY_ROUTINE lpCancelAddress,
                                IN const LPVOID lpvNotifyParameter,
                                IN LPCVOID lpvCancelId)
{
    DWC_TRANSFER *pTransfer;

    m_pHcd->LockHc();
    for (pTransfer = m_pHead; pTransfer != NULL; pTransfer = pTransfer->pNext)
    {
        if (pTransfer->lpvCancelId == lpvCancelId)
            break;
    }
    m_pHcd->UnlockHc();

    if (pTransfer == NULL)
        return requestFailed;

    AbortOne(pTransfer, lpCancelAddress, lpvNotifyParameter);
    return requestOK;
}

// Report the result of one transfer to its owner and free it. No lock held.
void CDwcPipe::Complete(CDwcHcd *pHcd, DWC_TRANSFER *pTransfer)
{
    PUSB_DEVICE_REQUEST pReq;
    PUCHAR p;

    // Log the first device descriptor of each enumeration (bring-up aid)
    if (pTransfer->pCtlVa != NULL && pTransfer->dwUsbError == USB_NO_ERROR)
    {
        pReq = (PUSB_DEVICE_REQUEST)pTransfer->pCtlVa;
        p = pTransfer->pDataVa;
        if (pReq->bmRequestType == 0x80 && pReq->bRequest == USB_REQUEST_GET_DESCRIPTOR &&
            (pReq->wValue >> 8) == USB_DEVICE_DESCRIPTOR_TYPE && p != NULL)
        {
            if (pTransfer->cbDone >= 18)
            {
                DWCMSG((TEXT("DWCHCI: device descriptor from addr %d: USB %x.%02x class %d maxpacket0 %d VID %04x PID %04x\r\n"),
                        pTransfer->bAddress, p[3], p[2], p[4], p[7],
                        p[8] | (p[9] << 8), p[10] | (p[11] << 8)));
            }
            else if (pTransfer->cbDone >= 8)
            {
                DWCMSG((TEXT("DWCHCI: device descriptor (first 8 bytes) from addr %d: %02x %02x %02x %02x %02x %02x %02x %02x\r\n"),
                        pTransfer->bAddress, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]));
            }
        }
    }

    __try
    {
        if ((pTransfer->dwFlags & USB_IN_TRANSFER) && pTransfer->pDataVa != NULL &&
            pTransfer->lpvBuffer != NULL && pTransfer->cbDone > 0)
        {
            memcpy(pTransfer->lpvBuffer, pTransfer->pDataVa, pTransfer->cbDone);
        }
        if (pTransfer->lpfComplete != NULL)
            *pTransfer->lpfComplete = TRUE;
        if (pTransfer->lpdwError != NULL)
            *pTransfer->lpdwError = pTransfer->dwUsbError;
        if (pTransfer->lpdwBytesTransferred != NULL)
            *pTransfer->lpdwBytesTransferred = pTransfer->cbDone;
        if (pTransfer->lpfnCallback != NULL)
            (*pTransfer->lpfnCallback)(pTransfer->lpvCallbackParameter);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DWCMSG((TEXT("DWCHCI: exception while completing a transfer\r\n")));
    }

    FreeTransfer(pHcd, pTransfer);
}

void CDwcPipe::CompleteList(CDwcHcd *pHcd, DWC_TRANSFER *pList)
{
    DWC_TRANSFER *pNext;

    while (pList != NULL)
    {
        pNext = pList->pNext;
        pList->pNext = NULL;
        Complete(pHcd, pList);
        pList = pNext;
    }
}

//-----------------------------------------------------------------------
// Factory functions called by hcd2lib
//
CPipeAbs * CreateControlPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    return new CDwcPipe(lpEndpointDescriptor, fIsLowSpeed, fIsHighSpeed, bDeviceAddress,
                        bHubAddress, bHubPort, ttContext, (CDwcHcd *)pChcd);
}

CPipeAbs * CreateBulkPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    return new CDwcPipe(lpEndpointDescriptor, fIsLowSpeed, fIsHighSpeed, bDeviceAddress,
                        bHubAddress, bHubPort, ttContext, (CDwcHcd *)pChcd);
}

CPipeAbs * CreateInterruptPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    return new CDwcPipe(lpEndpointDescriptor, fIsLowSpeed, fIsHighSpeed, bDeviceAddress,
                        bHubAddress, bHubPort, ttContext, (CDwcHcd *)pChcd);
}

CPipeAbs * CreateIsochronousPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    UNREFERENCED_PARAMETER(fIsLowSpeed);
    UNREFERENCED_PARAMETER(fIsHighSpeed);
    UNREFERENCED_PARAMETER(bHubAddress);
    UNREFERENCED_PARAMETER(bHubPort);
    UNREFERENCED_PARAMETER(ttContext);
    UNREFERENCED_PARAMETER(pChcd);
    DWCMSG((TEXT("DWCHCI: isochronous pipe (addr %d ep 0x%02x) requested: not supported\r\n"),
            bDeviceAddress, lpEndpointDescriptor->bEndpointAddress));
    return NULL;
}

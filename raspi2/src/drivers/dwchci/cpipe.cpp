//
// cpipe.cpp
//
// CDwcPipe: transfer queue and channel state machine for one endpoint.
//
// Locking: every member is protected by the controller lock
// (CDwcHcd::LockHc). Client callbacks and client buffers are only touched
// in Complete(), which runs without the lock.
//
// Milestone 3: control transfers, no split transactions.
//
#include <windows.h>
#include <globals.hpp>
#include <hcd.hpp>
#include "cdwchcd.h"
#include "cpipe.h"

#define DWCMSG(x)   RETAILMSG(1, x)

CDwcPipe::CDwcPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
                   IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
                   IN const UCHAR bDeviceAddress,
                   IN const UCHAR bHubAddress, IN const UCHAR bHubPort,
                   IN CDwcHcd * const pHcd)
    : CPipeAbs(lpEndpointDescriptor->bEndpointAddress)
    , m_pHcd(pHcd)
    , m_bEpType((UCHAR)(lpEndpointDescriptor->bmAttributes & USB_ENDPOINT_TYPE_MASK))
    , m_fLowSpeed(fIsLowSpeed)
    , m_fHighSpeed(fIsHighSpeed)
    , m_bHubAddress(bHubAddress)
    , m_bHubPort(bHubPort)
{
    UNREFERENCED_PARAMETER(bDeviceAddress);

    m_pNextPipe = NULL;
    m_wMaxPacket = (USHORT)(lpEndpointDescriptor->wMaxPacketSize & 0x7FF);
    if (m_wMaxPacket == 0)
        m_wMaxPacket = 8;
    m_fOpen = FALSE;
    m_fHalted = FALSE;
    m_fWaitingChannel = FALSE;
    m_iChannel = -1;
    m_dwNextPid = HCTSIZ_PID_DATA0;
    m_pHead = NULL;
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

    // A full or low speed device behind a high speed hub needs split
    // transactions, which are not implemented. Directly on the root port
    // (hub address 0) no split is needed.
    if (!m_fHighSpeed && m_bHubAddress != 0)
    {
        DWCMSG((TEXT("DWCHCI: full/low speed device behind hub %d port %d needs split transactions: not supported yet\r\n"),
                m_bHubAddress, m_bHubPort));
        return requestFailed;
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

    if (!m_fOpen || !m_pHcd->IsPortEnabled())
    {
        // The device is gone. A channel started now would never finish.
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
        StartAttempt();
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
        dwRemaining = pTransfer->dwBufferSize - pTransfer->cbDone;
        s.fIn = fDataIn;
        s.dwPid = pTransfer->dwPid;
        s.dwPackets = (dwRemaining + dwMps - 1) / dwMps;
        if (s.dwPackets == 0)
            s.dwPackets = 1;            // zero length packet
        if (s.dwPackets > dwMaxPackets)
            s.dwPackets = dwMaxPackets;
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

    pTransfer->fAttemptIn = s.fIn;
    pTransfer->cbAttempt = s.dwBytes;
    pTransfer->dwAttemptPackets = s.dwPackets;

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

    if (dwHcint & HCINT_NAK)
    {
        // The device is busy, not broken. Try again.
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

// The device was unplugged. CDwcHcd has already disabled all channels and
// cleared the channel table. Fail everything that is queued. Lock held.
void CDwcPipe::OnDisconnect(DWC_TRANSFER **ppDone)
{
    DWC_TRANSFER *pTransfer;
    DWC_TRANSFER *pKeep = NULL;

    m_iChannel = -1;
    m_fWaitingChannel = FALSE;

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
    UNREFERENCED_PARAMETER(ttContext);
    return new CDwcPipe(lpEndpointDescriptor, fIsLowSpeed, fIsHighSpeed, bDeviceAddress,
                        bHubAddress, bHubPort, (CDwcHcd *)pChcd);
}

CPipeAbs * CreateBulkPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
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
    DWCMSG((TEXT("DWCHCI: bulk pipe (addr %d ep 0x%02x) requested: not implemented yet\r\n"),
            bDeviceAddress, lpEndpointDescriptor->bEndpointAddress));
    return NULL;
}

CPipeAbs * CreateInterruptPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
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
    DWCMSG((TEXT("DWCHCI: interrupt pipe (addr %d ep 0x%02x) requested: not implemented yet\r\n"),
            bDeviceAddress, lpEndpointDescriptor->bEndpointAddress));
    return NULL;
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

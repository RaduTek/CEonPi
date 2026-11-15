//
// cpipe.h
//
// CDwcPipe: one USB endpoint as seen by the CE USB host library. A pipe
// keeps a FIFO queue of transfers and runs the transfer at the head of the
// queue on one host channel, one attempt at a time.
//
// Milestone 3 implemented control pipes. Milestone 4 adds bulk and
// interrupt pipes: bulk reuses the data-stage machinery, interrupt adds a
// start-of-frame scheduler so each poll happens one interval apart.
//
#ifndef __CPIPE_H__
#define __CPIPE_H__

#include <windows.h>
#include <globals.hpp>
#include <pipeabs.hpp>
#include "dwccore.h"

class CDwcHcd;

// Stages of a transfer. Non-control transfers only use the data stage.
#define DWC_STAGE_SETUP         0
#define DWC_STAGE_DATA          1
#define DWC_STAGE_STATUS        2

// Size of the block allocated per control transfer: the 8 byte SETUP packet
// at offset 0 and a scratch buffer for the status stage at offset 64.
#define DWC_CTL_BLOCK_SIZE      128
#define DWC_CTL_STATUS_OFFSET   64

#define DWC_MAX_ERRORS          3       // transaction errors before giving up
#define DWC_ABORT_WAIT_MS       200     // wait for a channel to halt on abort

typedef struct _DWC_TRANSFER
{
    struct _DWC_TRANSFER *pNext;

    // IssueTransfer parameters
    UCHAR   bAddress;
    LPTRANSFER_NOTIFY_ROUTINE lpfnCallback;
    LPVOID  lpvCallbackParameter;
    DWORD   dwFlags;
    DWORD   dwBufferSize;
    LPVOID  lpvBuffer;
    LPCVOID lpvCancelId;
    LPBOOL  lpfComplete;
    LPDWORD lpdwBytesTransferred;
    LPDWORD lpdwError;

    // DMA memory from CPhysMem
    PUCHAR  pCtlVa;             // control: SETUP packet + status scratch
    ULONG   paCtl;
    PUCHAR  pDataVa;            // copy of the client data buffer
    ULONG   paData;

    // Progress
    DWORD   dwStage;
    DWORD   cbDone;             // data bytes transferred so far
    DWORD   dwPid;              // PID of the next data packet
    BOOL    fDataPidInit;       // dwPid seeded from the pipe toggle (non-control)
    DWORD   dwErrCount;
    DWORD   dwUsbError;         // result
    BOOL    fAborting;          // AbortTransfer is waiting for the channel
    BOOL    fBusError;          // the core reported an AHB (DMA) error

    // Current attempt on the channel
    BOOL    fAttemptIn;
    DWORD   cbAttempt;
    DWORD   dwAttemptPackets;
} DWC_TRANSFER;

class CDwcPipe : public CPipeAbs
{
public:
    CDwcPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
             IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
             IN const UCHAR bDeviceAddress,
             IN const UCHAR bHubAddress, IN const UCHAR bHubPort,
             IN CDwcHcd * const pHcd);
    virtual ~CDwcPipe();

    // CPipeAbs interface
    virtual HCD_REQUEST_STATUS OpenPipe(void);
    virtual HCD_REQUEST_STATUS ClosePipe(void);
    virtual HCD_REQUEST_STATUS IssueTransfer(
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
                                OUT LPDWORD const lpdwError);
    virtual HCD_REQUEST_STATUS AbortTransfer(
                                IN const LPTRANSFER_NOTIFY_ROUTINE lpCancelAddress,
                                IN const LPVOID lpvNotifyParameter,
                                IN LPCVOID lpvCancelId);
    virtual HCD_REQUEST_STATUS IsPipeHalted(OUT LPBOOL const lpbHalted);
    virtual void ClearHaltedFlag(void);
    virtual void ChangeMaxPacketSize(IN const USHORT wMaxPacketSize);

    // Called by CDwcHcd with the controller lock held. Finished transfers
    // are appended to *ppDone and completed later without the lock.
    void OnChannelInterrupt(DWORD dwHcint, DWC_TRANSFER **ppDone);
    void OnDisconnect(DWC_TRANSFER **ppDone);
    void OnSofFrame(void);      // interrupt pipes: start a due poll
    void Kick(void);
    BOOL IsWaitingForChannel(void) const { return m_fWaitingChannel; }

    // Report the results of finished transfers to their owners. Must be
    // called without the controller lock.
    static void CompleteList(CDwcHcd *pHcd, DWC_TRANSFER *pList);

    CDwcPipe *m_pNextPipe;      // list of open pipes, owned by CDwcHcd

private:
    void StartAttempt(void);
    void ScheduleNextInterval(void);    // interrupt pipes: wait one interval
    void ClearFrameWait(void);          // drop a pending frame wait
    void ReleaseChannel(void);
    void Finish(DWC_TRANSFER *pTransfer, DWORD dwUsbError, DWC_TRANSFER **ppDone);
    static void AppendDone(DWC_TRANSFER *pTransfer, DWORD dwUsbError, DWC_TRANSFER **ppDone);
    void AbortOne(DWC_TRANSFER *pTransfer, LPTRANSFER_NOTIFY_ROUTINE lpCancelAddress,
                  LPVOID lpvNotifyParameter);
    static void Complete(CDwcHcd *pHcd, DWC_TRANSFER *pTransfer);
    static void FreeTransfer(CDwcHcd *pHcd, DWC_TRANSFER *pTransfer);

    CDwcHcd * const m_pHcd;
    const UCHAR     m_bEpType;          // USB_ENDPOINT_TYPE_xxx
    const BOOL      m_fLowSpeed;
    const BOOL      m_fHighSpeed;
    const UCHAR     m_bHubAddress;
    const UCHAR     m_bHubPort;
    const UCHAR     m_bInterval;        // endpoint bInterval (interrupt pipes)
    USHORT          m_wMaxPacket;
    DWORD           m_dwInterval;       // poll interval in HFNUM units

    BOOL            m_fOpen;
    BOOL            m_fHalted;
    BOOL            m_fLoggedData;      // first data packet logged (bring-up aid)
    BOOL            m_fWaitingChannel;  // head transfer waits for a free channel
    BOOL            m_fWaitingFrame;    // head transfer waits for its due frame
    DWORD           m_dwNextFrame;      // HFNUM value to start the next poll at
    int             m_iChannel;         // channel in use, or -1
    DWORD           m_dwNextPid;        // data toggle of bulk/interrupt endpoints
    HANDLE          m_hAbortEvent;

    DWC_TRANSFER *  m_pHead;            // transfer queue
};

#endif // __CPIPE_H__

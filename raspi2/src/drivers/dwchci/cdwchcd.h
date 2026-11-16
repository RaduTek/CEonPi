//
// cdwchcd.h
//
// CDwcHcd: the host controller object the CE USB 2.0 host library (hcd2lib)
// talks to. It owns the hardware (CDwcCore), the interrupt thread, the root
// port state and the host channel table.
//
#ifndef __CDWCHCD_H__
#define __CDWCHCD_H__

#include <windows.h>
#include <globals.hpp>
#include <hcd.hpp>
#include "dwccore.h"

class CDwcPipe;
struct _DWC_TRANSFER;

#define DWC_MAX_CHANNELS        16

// Software port change bits (reported to the root hub code)
#define DWC_PC_CONNECT          0x01
#define DWC_PC_ENABLE           0x02
#define DWC_PC_OVERCURRENT      0x04

class CDwcHcd : public CHcd
{
public:
    CDwcHcd(IN LPVOID pvPddObject, IN CPhysMem *pCPhysMem,
            IN LPCWSTR szDriverRegistryKey, IN DWORD dwSysIntr);
    virtual ~CDwcHcd();

    // CHcd interface
    virtual BOOL DeviceInitialize(void);
    virtual void DeviceDeInitialize(void);

    virtual BOOL GetFrameNumber(OUT LPDWORD lpdwFrameNumber);
    virtual BOOL GetFrameLength(OUT LPUSHORT lpuFrameLength);
    virtual BOOL SetFrameLength(IN HANDLE hEvent, IN USHORT uFrameLength);
    virtual BOOL StopAdjustingFrame(void);

    virtual VOID PowerMgmtCallback(IN BOOL fOff);
    virtual DWORD GetNumOfPorts();

    virtual BOOL DidPortStatusChange(IN const UCHAR port);
    virtual BOOL GetPortStatus(IN const UCHAR port, OUT USB_HUB_AND_PORT_STATUS& rStatus);
    virtual BOOL RootHubFeature(IN const UCHAR port, IN const UCHAR setOrClearFeature,
                                IN const USHORT feature);
    virtual BOOL ResetAndEnablePort(IN const UCHAR port);
    virtual void DisablePort(IN const UCHAR port);
    virtual BOOL WaitForPortStatusChange(HANDLE hHubChanged);

    virtual LPCTSTR GetControllerName(void) const;
    virtual DWORD SetCapability(DWORD dwCap);
    virtual DWORD GetCapability();

    // CDeviceGlobal interface. One CDwcTt per full/low speed device behind a
    // high speed hub carries that TT's split-transaction bandwidth bookkeeping.
    virtual PVOID AddedTt(UCHAR uHubAddress, UCHAR uPort);
    virtual BOOL DeleteTt(UCHAR uHubAddress, UCHAR uPort, PVOID ttContext);

    // Services for the pipes. One lock protects the channel table, the
    // pipe list, all transfer queues and the channel registers.
    void LockHc(void)               { EnterCriticalSection(&m_csHc); }
    void UnlockHc(void)             { LeaveCriticalSection(&m_csHc); }
    CDwcCore *Core(void)            { return &m_core; }
    CPhysMem *GetPhysMem(void)      { return m_pMem; }
    DWORD BusAddress(ULONG pa) const { return (DWORD)pa + m_dwDmaBusOffset; }
    BOOL IsPortEnabled(void) const  { return (m_core.ReadPort() & HPRT0_ENA) ? TRUE : FALSE; }

    // All of these are called with the lock held
    int  AllocChannel(CDwcPipe *pOwner);
    void FreeChannel(int iChannel);
    void AddPipe(CDwcPipe *pPipe);
    void RemovePipe(CDwcPipe *pPipe);

    // Interrupt pipes waiting for their scheduled frame. The start-of-frame
    // interrupt is on only while at least one pipe is waiting. Lock held.
    void SofWaiterAdd(void);
    void SofWaiterRemove(void);

private:
    static DWORD WINAPI IstStub(LPVOID lpv);
    DWORD Ist(void);
    void HandlePort(void);
    void HandleDisconnect(struct _DWC_TRANSFER **ppDone);
    void HandleChannels(struct _DWC_TRANSFER **ppDone);
    void HandleSof(void);
    void StopIst(void);

    CDwcCore            m_core;
    CPhysMem * const    m_pMem;
    DWORD               m_dwSysIntr;
    DWORD               m_dwDmaBusOffset;
    DWORD               m_dwCapability;

    CRITICAL_SECTION    m_csHc;
    HANDLE              m_hIntrEvent;
    HANDLE              m_hPortEvent;       // set when the root port changed
    HANDLE              m_hIst;
    BOOL                m_fIntrInitialized;
    volatile BOOL       m_fStop;

    volatile DWORD      m_dwPortChange;     // DWC_PC_xxx
    volatile BOOL       m_fInReset;         // ResetAndEnablePort in progress
    volatile BOOL       m_fDisableRequested;// DisablePort was called

    CDwcPipe *          m_apChannelOwner[DWC_MAX_CHANNELS];
    CDwcPipe *          m_pPipeList;
    DWORD               m_dwSofWaiters;     // interrupt pipes waiting for a frame
};

#endif // __CDWCHCD_H__

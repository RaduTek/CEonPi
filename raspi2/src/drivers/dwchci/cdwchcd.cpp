//
// cdwchcd.cpp
//
// CDwcHcd: host controller object for the CE USB 2.0 host library.
//
// The library (hcd2lib) owns the root hub logic, enumeration and the
// interface to USBD. This class supplies the root port, the interrupt
// thread and the host channels.
//
#include <windows.h>
#include <nkintr.h>
#include <globals.hpp>
#include <hcd.hpp>
#include <hcdddsi.h>
#include "cdwchcd.h"
#include "cpipe.h"
#include "dwctt.h"
#include "dwclog.h"

#define DWC_REG_DMA_BUS_OFFSET  TEXT("DmaBusOffset")
#define DWC_DEFAULT_BUS_OFFSET  0x40000000
#define DWC_PIPE_CACHE_SIZE     8
#define DWC_PORT_RESET_MS       60      // root port reset, at least 50 ms
#define DWC_ENABLE_TIMEOUT_MS   500     // wait for the port to enable after reset
#define DWC_RESET_RECOVERY_MS   20      // at least 10 ms before the first SETUP

CDwcHcd::CDwcHcd(IN LPVOID pvPddObject, IN CPhysMem *pCPhysMem,
                 IN LPCWSTR szDriverRegistryKey, IN DWORD dwSysIntr)
    : m_pMem(pCPhysMem)
{
    HKEY hKey;
    DWORD dwType, dwValue, cbValue;
    int i;

    UNREFERENCED_PARAMETER(pvPddObject);

    m_dwSysIntr = dwSysIntr;
    m_dwDmaBusOffset = DWC_DEFAULT_BUS_OFFSET;
    m_dwCapability = 0;
    m_hIntrEvent = NULL;
    m_hPortEvent = NULL;
    m_hIst = NULL;
    m_fIntrInitialized = FALSE;
    m_fStop = FALSE;
    m_dwPortChange = 0;
    m_fInReset = FALSE;
    m_fDisableRequested = FALSE;
    m_pPipeList = NULL;
    m_dwSofWaiters = 0;
    for (i = 0; i < DWC_MAX_CHANNELS; i++)
        m_apChannelOwner[i] = NULL;

    InitializeCriticalSection(&m_csHc);

    // hcd2lib allocates its opened-pipe cache from this value
    m_dwOpenedPipeCacheSize = DWC_PIPE_CACHE_SIZE;

    if (szDriverRegistryKey != NULL &&
        RegOpenKeyEx(HKEY_LOCAL_MACHINE, szDriverRegistryKey, 0, 0, &hKey) == ERROR_SUCCESS)
    {
        cbValue = sizeof(dwValue);
        if (RegQueryValueEx(hKey, DWC_REG_DMA_BUS_OFFSET, NULL, &dwType,
                            (LPBYTE)&dwValue, &cbValue) == ERROR_SUCCESS && dwType == REG_DWORD)
        {
            m_dwDmaBusOffset = dwValue;
        }
        RegCloseKey(hKey);
    }
}

CDwcHcd::~CDwcHcd()
{
    CRootHub *pRoot;

    StopIst();

    // Make the API set inaccessible and close the root hub
    pRoot = SetRootHub(NULL);
    if (pRoot != NULL)
    {
        pRoot->HandleDetach();
        delete pRoot;
    }

    if (m_hPortEvent != NULL)
        CloseHandle(m_hPortEvent);
    if (m_hIntrEvent != NULL)
        CloseHandle(m_hIntrEvent);
    m_core.Unmap();
    DeleteCriticalSection(&m_csHc);
}

void CDwcHcd::StopIst(void)
{
    m_core.DisableInterrupts();

    if (m_hIst != NULL)
    {
        m_fStop = TRUE;
        SetEvent(m_hIntrEvent);
        WaitForSingleObject(m_hIst, 2000);
        CloseHandle(m_hIst);
        m_hIst = NULL;
    }
    if (m_fIntrInitialized)
    {
        InterruptDisable(m_dwSysIntr);
        m_fIntrInitialized = FALSE;
    }
}

BOOL CDwcHcd::DeviceInitialize(void)
{
    USB_DEVICE_INFO deviceInfo;
    USB_HUB_DESCRIPTOR usbHubDescriptor;

    DWCINIT((TEXT("DWCHCI: DeviceInitialize, SysIntr=%d DmaBusOffset=0x%08x\r\n"),
            m_dwSysIntr, m_dwDmaBusOffset));

    m_pMem->ReInit();

    // Loads USBD and registers this host controller with it
    if (!CDeviceGlobal::Initialize(this))
    {
        DWCERR((TEXT("DWCHCI: ***ERROR*** CDeviceGlobal::Initialize failed (is usbd.dll in the image?)\r\n")));
        return FALSE;
    }

    if (!m_core.Map() || !m_core.DumpIdentity() || !m_core.HostInit())
    {
        DWCERR((TEXT("DWCHCI: ***ERROR*** controller initialisation failed\r\n")));
        return FALSE;
    }
    if (m_core.Channels() > DWC_MAX_CHANNELS)
        return FALSE;

    m_hIntrEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    m_hPortEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hIntrEvent == NULL || m_hPortEvent == NULL)
        return FALSE;

    if (!InterruptInitialize(m_dwSysIntr, m_hIntrEvent, NULL, 0))
    {
        DWCERR((TEXT("DWCHCI: ***ERROR*** InterruptInitialize(%d) failed, error %d\r\n"),
                m_dwSysIntr, GetLastError()));
        return FALSE;
    }
    m_fIntrInitialized = TRUE;

    m_hIst = CreateThread(NULL, 0, IstStub, this, 0, NULL);
    if (m_hIst == NULL)
        return FALSE;

    // Root hub object: one port, reported as a high speed hub so that the
    // library passes the real speed of the attached device to the pipes.
    memset(&deviceInfo, 0, sizeof(deviceInfo));
    memset(&usbHubDescriptor, 0, sizeof(usbHubDescriptor));

    deviceInfo.dwCount = sizeof(USB_DEVICE_INFO);
    deviceInfo.lpConfigs = NULL;
    deviceInfo.lpActiveConfig = NULL;
    deviceInfo.Descriptor.bLength = sizeof(USB_DEVICE_DESCRIPTOR);
    deviceInfo.Descriptor.bDescriptorType = USB_DEVICE_DESCRIPTOR_TYPE;
    deviceInfo.Descriptor.bcdUSB = 0x200;
    deviceInfo.Descriptor.bDeviceClass = USB_DEVICE_CLASS_HUB;
    deviceInfo.Descriptor.bDeviceSubClass = 0xff;
    deviceInfo.Descriptor.bDeviceProtocol = 0xff;
    deviceInfo.Descriptor.bMaxPacketSize0 = 0;
    deviceInfo.Descriptor.bNumConfigurations = 0;

    usbHubDescriptor.bDescriptorType = USB_HUB_DESCRIPTOR_TYPE;
    usbHubDescriptor.bDescriptorLength = USB_HUB_DESCRIPTOR_MINIMUM_SIZE;
    usbHubDescriptor.bNumberOfPorts = 1;
    usbHubDescriptor.wHubCharacteristics = USB_HUB_CHARACTERISTIC_NO_POWER_SWITCHING |
                                           USB_HUB_CHARACTERISTIC_NOT_PART_OF_COMPOUND_DEVICE |
                                           USB_HUB_CHARACTERISTIC_NO_OVER_CURRENT_PROTECTION;
    usbHubDescriptor.bPowerOnToPowerGood = 50;
    usbHubDescriptor.bHubControlCurrent = 0;
    usbHubDescriptor.bRemoveAndPowerMask[0] = 0;
    usbHubDescriptor.bRemoveAndPowerMask[1] = 0xFF;

    SetRootHub(new CRootHub(deviceInfo, FALSE, TRUE, usbHubDescriptor, this));
    if (GetRootHub() == NULL)
    {
        DWCERR((TEXT("DWCHCI: ***ERROR*** cannot create the root hub object\r\n")));
        return FALSE;
    }

    // Starts the library's hub thread, which calls GetPortStatus and friends
    if (FXN_DEV_STATE_OPERATIONAL != GetRootHub()->EnterOperationalState(NULL))
    {
        DWCERR((TEXT("DWCHCI: ***ERROR*** root hub failed to start\r\n")));
        return FALSE;
    }

    // A device that is already plugged in raises the port interrupt now
    m_core.EnableInterrupts();

    DWCINIT((TEXT("DWCHCI: DeviceInitialize done (milestone 4: control, bulk and interrupt pipes)\r\n")));
    return TRUE;
}

void CDwcHcd::DeviceDeInitialize(void)
{
    CRootHub *pRoot;

    StopIst();

    pRoot = SetRootHub(NULL);
    if (pRoot != NULL)
    {
        pRoot->HandleDetach();
        delete pRoot;
    }
    CDeviceGlobal::DeInitialize();
}

//-----------------------------------------------------------------------
// Frame number
//
BOOL CDwcHcd::GetFrameNumber(OUT LPDWORD lpdwFrameNumber)
{
    DWORD dwFrame = m_core.Rd(HFNUM) & HFNUM_FRNUM_MASK;

    // At high speed the core counts microframes
    if (((m_core.ReadPort() & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT) == HPRT0_SPD_HIGH)
        dwFrame >>= 3;
    if (lpdwFrameNumber != NULL)
        *lpdwFrameNumber = dwFrame;
    return TRUE;
}

BOOL CDwcHcd::GetFrameLength(OUT LPUSHORT lpuFrameLength)
{
    if (lpuFrameLength != NULL)
        *lpuFrameLength = 60000;    // bit times per frame, cannot be adjusted
    return TRUE;
}

BOOL CDwcHcd::SetFrameLength(IN HANDLE hEvent, IN USHORT uFrameLength)
{
    UNREFERENCED_PARAMETER(hEvent);
    UNREFERENCED_PARAMETER(uFrameLength);
    return FALSE;
}

BOOL CDwcHcd::StopAdjustingFrame(void)
{
    return FALSE;
}

VOID CDwcHcd::PowerMgmtCallback(IN BOOL fOff)
{
    // Suspend and resume are not supported yet
    UNREFERENCED_PARAMETER(fOff);
}

LPCTSTR CDwcHcd::GetControllerName(void) const
{
    return TEXT("DWCHCI");
}

// HCD_SUSPEND_ON_REQUEST is not accepted: the controller cannot be
// suspended yet, and the library's hub thread relies on SuspendHC() when
// that capability is set.
DWORD CDwcHcd::SetCapability(DWORD dwCap)
{
    m_dwCapability |= (dwCap & ~HCD_SUSPEND_ON_REQUEST);
    return m_dwCapability;
}

DWORD CDwcHcd::GetCapability()
{
    return m_dwCapability;
}

// The library calls AddedTt once for each full/low speed device attached
// behind a high speed hub, with that hub's address and the downstream port.
// We allocate one transaction-translator object per (hub, port) and return
// it as the opaque ttContext; the library passes it to the pipe factories
// (so a pipe knows it must use split transactions) and back to DeleteTt.
PVOID CDwcHcd::AddedTt(UCHAR uHubAddress, UCHAR uPort)
{
    return (PVOID) new CDwcTt(uHubAddress, uPort);
}

BOOL CDwcHcd::DeleteTt(UCHAR uHubAddress, UCHAR uPort, PVOID ttContext)
{
    UNREFERENCED_PARAMETER(uHubAddress);
    UNREFERENCED_PARAMETER(uPort);
    if (ttContext != NULL)
        delete (CDwcTt *)ttContext;
    return TRUE;
}

//-----------------------------------------------------------------------
// Root hub: one port
//
DWORD CDwcHcd::GetNumOfPorts()
{
    return 1;
}

BOOL CDwcHcd::DidPortStatusChange(IN const UCHAR port)
{
    return (port == 1 && m_dwPortChange != 0) ? TRUE : FALSE;
}

// The change bits are kept in software and are cleared by the library
// through RootHubFeature(CLEAR_FEATURE, C_PORT_xxx), never here.
BOOL CDwcHcd::GetPortStatus(IN const UCHAR port, OUT USB_HUB_AND_PORT_STATUS& rStatus)
{
    DWORD dwHprt, dwChange, dwSpeed;

    memset(&rStatus, 0, sizeof(USB_HUB_AND_PORT_STATUS));
    if (port != 1)
        return TRUE;        // port 0 is the hub itself: nothing to report

    dwHprt = m_core.ReadPort();
    dwChange = m_dwPortChange;
    dwSpeed = (dwHprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT;

    rStatus.status.port.PortConnected = (dwHprt & HPRT0_CONNSTS) ? 1 : 0;
    rStatus.status.port.PortEnabled = (dwHprt & HPRT0_ENA) ? 1 : 0;
    rStatus.status.port.PortSuspended = (dwHprt & HPRT0_SUSP) ? 1 : 0;
    rStatus.status.port.PortOverCurrent = (dwHprt & HPRT0_OVRCURRACT) ? 1 : 0;
    rStatus.status.port.PortReset = (dwHprt & HPRT0_RST) ? 1 : 0;
    rStatus.status.port.PortPower = 1;
    // The speed field is only valid once the port is enabled
    if (dwHprt & HPRT0_ENA)
    {
        rStatus.status.port.DeviceIsLowSpeed = (dwSpeed == HPRT0_SPD_LOW) ? 1 : 0;
        rStatus.status.port.DeviceIsHighSpeed = (dwSpeed == HPRT0_SPD_HIGH) ? 1 : 0;
    }

    rStatus.change.port.ConnectStatusChange = (dwChange & DWC_PC_CONNECT) ? 1 : 0;
    rStatus.change.port.PortEnableChange = (dwChange & DWC_PC_ENABLE) ? 1 : 0;
    rStatus.change.port.OverCurrentChange = (dwChange & DWC_PC_OVERCURRENT) ? 1 : 0;
    return TRUE;
}

BOOL CDwcHcd::RootHubFeature(IN const UCHAR port, IN const UCHAR setOrClearFeature,
                             IN const USHORT feature)
{
    BOOL fOk = TRUE;

    if (port != 1)
        return FALSE;

    if (setOrClearFeature == USB_REQUEST_SET_FEATURE)
    {
        switch (feature)
        {
        case USB_HUB_FEATURE_PORT_RESET:
            fOk = ResetAndEnablePort(port);
            break;
        case USB_HUB_FEATURE_PORT_POWER:
            LockHc();
            m_core.PortPower();
            UnlockHc();
            break;
        default:
            fOk = FALSE;        // suspend, indicators: not supported
            break;
        }
    }
    else
    {
        switch (feature)
        {
        case USB_HUB_FEATURE_PORT_ENABLE:
            DisablePort(port);
            break;
        case USB_HUB_FEATURE_C_PORT_CONNECTION:
            LockHc();
            m_dwPortChange &= ~DWC_PC_CONNECT;
            UnlockHc();
            break;
        case USB_HUB_FEATURE_C_PORT_ENABLE:
            LockHc();
            m_dwPortChange &= ~DWC_PC_ENABLE;
            UnlockHc();
            break;
        case USB_HUB_FEATURE_C_PORT_OVER_CURRENT:
            LockHc();
            m_dwPortChange &= ~DWC_PC_OVERCURRENT;
            UnlockHc();
            break;
        case USB_HUB_FEATURE_C_PORT_RESET:
        case USB_HUB_FEATURE_C_PORT_SUSPEND:
            break;              // never reported, nothing to clear
        default:
            fOk = FALSE;        // port power off, resume: not supported
            break;
        }
    }
    return fOk;
}

// Reset the port and wait until the core has enabled it. When this returns
// TRUE the device answers on address 0.
BOOL CDwcHcd::ResetAndEnablePort(IN const UCHAR port)
{
    DWORD dwHprt, dwStart;
    BOOL fEnabled = FALSE;

    if (port != 1)
        return FALSE;

    if (!(m_core.ReadPort() & HPRT0_CONNSTS))
        return FALSE;

    // The enable bit drops during the reset; that is not a change to report
    m_fInReset = TRUE;
    m_fDisableRequested = FALSE;

    LockHc();
    m_core.PortResetBegin();
    UnlockHc();

    Sleep(DWC_PORT_RESET_MS);

    LockHc();
    m_core.PortResetEnd();
    UnlockHc();

    dwStart = GetTickCount();
    for (;;)
    {
        dwHprt = m_core.ReadPort();
        if (dwHprt & HPRT0_ENA)
        {
            fEnabled = TRUE;
            break;
        }
        if (!(dwHprt & HPRT0_CONNSTS))
            break;
        if ((GetTickCount() - dwStart) > DWC_ENABLE_TIMEOUT_MS)
            break;
        Sleep(10);
    }

    if (fEnabled)
    {
        // Frame interval for the speed of the device (the interrupt thread
        // does the same when it sees the enable change; both are harmless)
        LockHc();
        m_core.PortEnabled(dwHprt);
        UnlockHc();
        Sleep(DWC_RESET_RECOVERY_MS);
    }
    else
    {
        DWCWARN((TEXT("DWCHCI: port did not enable after reset, HPRT0=0x%08x\r\n"), dwHprt));
    }

    LockHc();
    m_dwPortChange &= ~DWC_PC_ENABLE;
    UnlockHc();
    m_fInReset = FALSE;

    return fEnabled;
}

void CDwcHcd::DisablePort(IN const UCHAR port)
{
    if (port != 1)
        return;

    LockHc();
    if (m_core.ReadPort() & HPRT0_ENA)
    {
        // The resulting enable change is ours, not something to report
        m_fDisableRequested = TRUE;
        m_core.PortDisable();
    }
    UnlockHc();
    Sleep(10);
}

// Called by the library's hub thread when it found no port change. Blocks
// until the interrupt thread reports one (or the library signals its own
// event, which it also uses for transfer completion).
BOOL CDwcHcd::WaitForPortStatusChange(HANDLE hHubChanged)
{
    HANDLE ah[2];

    if (m_hPortEvent == NULL)
        return FALSE;

    if (hHubChanged != NULL)
    {
        ah[0] = hHubChanged;
        ah[1] = m_hPortEvent;
        WaitForMultipleObjects(2, ah, FALSE, INFINITE);
    }
    else
    {
        WaitForSingleObject(m_hPortEvent, INFINITE);
    }
    return TRUE;
}

//-----------------------------------------------------------------------
// Channel table and pipe list. Lock held for all of these.
//
int CDwcHcd::AllocChannel(CDwcPipe *pOwner)
{
    DWORD i;

    for (i = 0; i < m_core.Channels() && i < DWC_MAX_CHANNELS; i++)
    {
        if (m_apChannelOwner[i] == NULL)
        {
            m_apChannelOwner[i] = pOwner;
            return (int)i;
        }
    }
    return -1;
}

void CDwcHcd::FreeChannel(int iChannel)
{
    CDwcPipe *pPipe;

    if (iChannel < 0 || iChannel >= DWC_MAX_CHANNELS)
        return;
    m_apChannelOwner[iChannel] = NULL;

    // Give the channel to a pipe that was waiting for one
    for (pPipe = m_pPipeList; pPipe != NULL; pPipe = pPipe->m_pNextPipe)
    {
        if (pPipe->IsWaitingForChannel())
        {
            pPipe->Kick();
            break;
        }
    }
}

void CDwcHcd::AddPipe(CDwcPipe *pPipe)
{
    pPipe->m_pNextPipe = m_pPipeList;
    m_pPipeList = pPipe;
}

void CDwcHcd::RemovePipe(CDwcPipe *pPipe)
{
    CDwcPipe **ppLink;

    for (ppLink = &m_pPipeList; *ppLink != NULL; ppLink = &(*ppLink)->m_pNextPipe)
    {
        if (*ppLink == pPipe)
        {
            *ppLink = pPipe->m_pNextPipe;
            pPipe->m_pNextPipe = NULL;
            break;
        }
    }
}

// Interrupt scheduler: turn the start-of-frame interrupt on for the first
// waiting pipe and off again when the last one stops waiting. Lock held.
void CDwcHcd::SofWaiterAdd(void)
{
    if (m_dwSofWaiters++ == 0)
        m_core.EnableSof();
}

void CDwcHcd::SofWaiterRemove(void)
{
    if (m_dwSofWaiters > 0 && --m_dwSofWaiters == 0)
        m_core.DisableSof();
}

//-----------------------------------------------------------------------
// Interrupt handling
//

// Root port change. Lock held.
void CDwcHcd::HandlePort(void)
{
    DWORD dwHprt = m_core.ReadPort();
    BOOL fSignal = FALSE;

    // Clear the hardware change bits; the state is kept in m_dwPortChange
    m_core.AckPortChanges(dwHprt);

    if (dwHprt & HPRT0_CONNDET)
    {
        m_dwPortChange |= DWC_PC_CONNECT;
        fSignal = TRUE;
    }

    if (dwHprt & HPRT0_ENACHG)
    {
        if (dwHprt & HPRT0_ENA)
        {
            m_core.PortEnabled(dwHprt);
        }
        else if (!m_fInReset && !m_fDisableRequested)
        {
            // The core disabled the port by itself (babble, disconnect)
            DWCINFO((TEXT("DWCHCI: root port disabled by core, HPRT0=0x%08x\r\n"), dwHprt));
            m_dwPortChange |= DWC_PC_ENABLE;
            fSignal = TRUE;
        }
    }

    if (dwHprt & HPRT0_OVRCURRCHG)
    {
        m_dwPortChange |= DWC_PC_OVERCURRENT;
        fSignal = TRUE;
    }

    if (fSignal)
        SetEvent(m_hPortEvent);
}

// Disconnect interrupt. Lock held.
void CDwcHcd::HandleDisconnect(DWC_TRANSFER **ppDone)
{
    CDwcPipe *pPipe;
    DWORD dwStuck;
    int i;

    // The core also raises this around a port reset while the device is
    // still there (seen on hardware). Only act if it is really gone.
    if (m_core.ReadPort() & HPRT0_CONNSTS)
        return;

    DWCINFO((TEXT("DWCHCI: device disconnected\r\n")));

    // Nothing will complete any more: stop the channels and fail everything
    dwStuck = m_core.DisableAllChannels();
    for (i = 0; i < DWC_MAX_CHANNELS; i++)
        m_apChannelOwner[i] = NULL;
    for (pPipe = m_pPipeList; pPipe != NULL; pPipe = pPipe->m_pNextPipe)
        pPipe->OnDisconnect(ppDone);

    // Some devices (notably composite wireless receivers that poll several
    // interrupt-IN endpoints) leave channels that will not halt once the
    // device is gone: the core cannot drain the periodic request queue, so
    // CHENA never clears and the whole host wedges - the next device will not
    // enumerate and the port will not even re-enable. Flushing the FIFOs does
    // not clear a stuck CHENA; only a core reset does. It is cheap, mirrors
    // the cold-boot bring-up, and leaves the port powered, so a device that is
    // still attached is re-detected and enumerates normally.
    if (dwStuck > 0)
    {
        DWCWARN((TEXT("DWCHCI: %d channel(s) would not halt; resetting the controller\r\n"), dwStuck));
        if (m_core.HostInit())
            m_core.EnableInterrupts();
        else
            DWCERR((TEXT("DWCHCI: ***ERROR*** controller re-init after disconnect failed\r\n")));
        m_dwSofWaiters = 0;     // nothing is polling after the reset
    }

    m_dwPortChange |= DWC_PC_CONNECT;
    SetEvent(m_hPortEvent);
}

// Channel interrupts. Lock held.
void CDwcHcd::HandleChannels(DWC_TRANSFER **ppDone)
{
    DWORD dwHaint = m_core.Rd(HAINT) & m_core.Rd(HAINTMSK);
    DWORD i, dwHcint;
    CDwcPipe *pOwner;

    for (i = 0; i < m_core.Channels() && i < DWC_MAX_CHANNELS; i++)
    {
        if (!(dwHaint & DWC_BIT(i)))
            continue;

        dwHcint = m_core.ChannelReadAndClearInt(i);
        pOwner = m_apChannelOwner[i];
        if (pOwner != NULL)
        {
            pOwner->OnChannelInterrupt(dwHcint, ppDone);
        }
        else
        {
            // Left over from a cancelled transfer
            m_core.ChannelRelease(i);
        }
    }
}

// Start of frame. Give every interrupt pipe a chance to start the poll
// whose scheduled frame has arrived. Lock held. Pipes that start a poll
// only program a channel, so nothing completes here.
void CDwcHcd::HandleSof(void)
{
    CDwcPipe *pPipe;

    for (pPipe = m_pPipeList; pPipe != NULL; pPipe = pPipe->m_pNextPipe)
        pPipe->OnSofFrame();
}

DWORD WINAPI CDwcHcd::IstStub(LPVOID lpv)
{
    return ((CDwcHcd *)lpv)->Ist();
}

DWORD CDwcHcd::Ist(void)
{
    DWORD dwPend, dwOther;
    DWC_TRANSFER *pDone;

    CeSetThreadPriority(GetCurrentThread(), g_IstThreadPriority);

    for (;;)
    {
        WaitForSingleObject(m_hIntrEvent, INFINITE);
        if (m_fStop)
            break;

        pDone = NULL;

        LockHc();
        dwPend = m_core.Rd(GINTSTS) & m_core.Rd(GINTMSK);

        if (dwPend & GINTSTS_PRTINT)
            HandlePort();       // PRTINT clears when the HPRT0 change bits are cleared

        if (dwPend & GINTSTS_DISCONNINT)
        {
            m_core.Wr(GINTSTS, GINTSTS_DISCONNINT);
            HandleDisconnect(&pDone);
        }

        if (dwPend & GINTSTS_SOF)
        {
            m_core.Wr(GINTSTS, GINTSTS_SOF);    // acknowledge this frame
            HandleSof();
        }

        if (dwPend & GINTSTS_HCHINT)
            HandleChannels(&pDone);

        dwOther = dwPend & ~(GINTSTS_PRTINT | GINTSTS_DISCONNINT | GINTSTS_SOF | GINTSTS_HCHINT);
        if (dwOther)
        {
            DWCWARN((TEXT("DWCHCI: unexpected interrupt bits 0x%08x\r\n"), dwOther));
            m_core.Wr(GINTSTS, dwOther);
        }
        UnlockHc();

        InterruptDone(m_dwSysIntr);

        // Client callbacks run without the lock
        CDwcPipe::CompleteList(this, pDone);
    }
    return 0;
}

//-----------------------------------------------------------------------
// Factory function called by hcd2lib (HcdMdd_CreateHcdObject)
//
CHcd * CreateHCDObject(IN LPVOID pvUhcdPddObject,
                       IN CPhysMem * pCPhysMem,
                       IN LPCWSTR szDriverRegistryKey,
                       IN REGISTER portBase,
                       IN DWORD dwSysIntr)
{
    UNREFERENCED_PARAMETER(portBase);
    return new CDwcHcd(pvUhcdPddObject, pCPhysMem, szDriverRegistryKey, dwSysIntr);
}

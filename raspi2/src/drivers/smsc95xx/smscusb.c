//
// smscusb.c
//
// USB client side of the SMSC LAN9512/LAN9514 Ethernet driver.
//
// USBD loads this DLL for the Ethernet function of the chip (0424:ec00,
// registry key Drivers\USB\LoadClients\1060_60416\Default\Default\SMSC95XX)
// and calls USBDeviceAttach. A vendor-matched driver is offered the whole
// device first, with lpInterface == NULL, so interface 0 of the active
// configuration is taken from GetDeviceInfo.
//
// At attach: find the endpoints, log the chip ID and the reset-state MAC and
// PHY registers, start the chip and its threads (smschip.c), then register
// the NDIS adapter (smscndis.c).
//
// Register access is a vendor control transfer (bRequest 0xA1 read, 0xA0
// write, wIndex = register, 4 data bytes, little endian), made synchronous by
// waiting on an event that the transfer callback sets. It must never be
// called from inside a USB transfer completion callback.
//
#include <windows.h>
#include <usbdi.h>
#include "smsc.h"

#ifdef DEBUG
DBGPARAM dpCurSettings =
{
    TEXT("SMSC95XX"),
    {
        TEXT("Errors"),     TEXT("Warnings"),   TEXT("Init"),       TEXT("Info"),
        TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined"),
        TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined"),
        TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined"),  TEXT("Undefined")
    },
    SMSC_DBG_ZONES_INITIAL
};
#endif

static BOOL WINAPI SmscDeviceNotify(LPVOID lpvNotifyParameter, DWORD dwCode,
                                    LPDWORD *dwInfo1, LPDWORD *dwInfo2,
                                    LPDWORD *dwInfo3, LPDWORD *dwInfo4);

//------------------------------------------------------------------------------
// DLL entry
//------------------------------------------------------------------------------
BOOL WINAPI DllEntry(HANDLE hDllHandle, DWORD dwReason, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);

    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DEBUGREGISTER((HMODULE)hDllHandle);
        DisableThreadLibraryCalls((HMODULE)hDllHandle);
    }
    return TRUE;
}

//------------------------------------------------------------------------------
// Synchronous vendor control transfer
//------------------------------------------------------------------------------
// Transfer callback for every transfer the driver issues: the parameter is
// an event, and all the work happens in the thread waiting on it.
DWORD WINAPI SmscXferSignal(LPVOID lpvNotifyParameter)
{
    SetEvent((HANDLE)lpvNotifyParameter);
    return 0;
}

// One 4-byte register transfer. fIn = TRUE for a read. Caller holds csCtl.
static BOOL SmscVendorXfer(PSMSC_ADAPTER pAd, BOOL fIn, UCHAR bRequest,
                           DWORD dwReg, DWORD *pdwData)
{
    USB_DEVICE_REQUEST req;
    USB_TRANSFER hXfer;
    DWORD dwFlags, dwWait, dwBytes = 0, dwUsbErr = USB_NO_ERROR;
    BOOL fOk = FALSE;

    if (pAd->fClosing)
        return FALSE;

    req.bmRequestType = USB_REQUEST_VENDOR | USB_REQUEST_FOR_DEVICE |
                        (fIn ? USB_REQUEST_DEVICE_TO_HOST : USB_REQUEST_HOST_TO_DEVICE);
    req.bRequest = bRequest;
    req.wValue = 0;
    req.wIndex = (USHORT)dwReg;
    req.wLength = sizeof(DWORD);

    dwFlags = fIn ? (USB_IN_TRANSFER | USB_SHORT_TRANSFER_OK) : USB_OUT_TRANSFER;

    ResetEvent(pAd->hCtlEvent);
    hXfer = pAd->pUsbFuncs->lpIssueVendorTransfer(pAd->hDevice, SmscXferSignal,
                                                   pAd->hCtlEvent, dwFlags,
                                                   &req, pdwData, 0);
    if (hXfer == NULL)
    {
        SMSCERR((TEXT("SMSC95XX: vendor transfer 0x%02x reg 0x%03x not issued\r\n"),
                 bRequest, dwReg));
        return FALSE;
    }

    dwWait = WaitForSingleObject(pAd->hCtlEvent, SMSC_CTL_TIMEOUT);
    if (dwWait != WAIT_OBJECT_0)
    {
        SMSCERR((TEXT("SMSC95XX: vendor transfer 0x%02x reg 0x%03x timed out\r\n"),
                 bRequest, dwReg));
        pAd->pUsbFuncs->lpAbortTransfer(hXfer, 0);
    }
    else if (pAd->pUsbFuncs->lpGetTransferStatus(hXfer, &dwBytes, &dwUsbErr) &&
             dwUsbErr == USB_NO_ERROR && dwBytes == sizeof(DWORD))
    {
        fOk = TRUE;
    }
    else
    {
        SMSCERR((TEXT("SMSC95XX: vendor transfer 0x%02x reg 0x%03x failed, usberr=0x%x bytes=%u\r\n"),
                 bRequest, dwReg, dwUsbErr, dwBytes));
    }

    pAd->pUsbFuncs->lpCloseTransfer(hXfer);
    return fOk;
}

BOOL SmscReadReg(PSMSC_ADAPTER pAd, DWORD dwReg, DWORD *pdwValue)
{
    DWORD dwData = 0;
    BOOL fOk;

    EnterCriticalSection(&pAd->csCtl);
    fOk = SmscVendorXfer(pAd, TRUE, SMSC_UR_READ_REG, dwReg, &dwData);
    LeaveCriticalSection(&pAd->csCtl);

    // The chip is little endian, as is the ARM1176 here: no swap needed.
    *pdwValue = fOk ? dwData : 0xFFFFFFFF;
    return fOk;
}

BOOL SmscWriteReg(PSMSC_ADAPTER pAd, DWORD dwReg, DWORD dwValue)
{
    DWORD dwData = dwValue;
    BOOL fOk;

    EnterCriticalSection(&pAd->csCtl);
    fOk = SmscVendorXfer(pAd, FALSE, SMSC_UR_WRITE_REG, dwReg, &dwData);
    LeaveCriticalSection(&pAd->csCtl);
    return fOk;
}

//------------------------------------------------------------------------------
// PHY access through MII_ADDR / MII_DATA
//------------------------------------------------------------------------------
static BOOL SmscMiiWaitIdle(PSMSC_ADAPTER pAd)
{
    DWORD i, dwVal;

    for (i = 0; i < SMSC_MII_POLLS; i++)
    {
        if (!SmscReadReg(pAd, SMSC_MII_ADDR, &dwVal))
            return FALSE;
        if ((dwVal & SMSC_MII_BUSY) == 0)
            return TRUE;
    }
    SMSCERR((TEXT("SMSC95XX: MII stays busy\r\n")));
    return FALSE;
}

BOOL SmscMiiRead(PSMSC_ADAPTER pAd, DWORD dwPhyReg, WORD *pwValue)
{
    DWORD dwVal;

    *pwValue = 0xFFFF;
    if (!SmscMiiWaitIdle(pAd))
        return FALSE;
    if (!SmscWriteReg(pAd, SMSC_MII_ADDR, SMSC_MII_ADDR_PHY(SMSC_PHY_ID) |
                      SMSC_MII_ADDR_REG(dwPhyReg) | SMSC_MII_READ | SMSC_MII_BUSY))
        return FALSE;
    if (!SmscMiiWaitIdle(pAd))
        return FALSE;
    if (!SmscReadReg(pAd, SMSC_MII_DATA, &dwVal))
        return FALSE;
    *pwValue = (WORD)(dwVal & 0xFFFF);
    return TRUE;
}

BOOL SmscMiiWrite(PSMSC_ADAPTER pAd, DWORD dwPhyReg, WORD wValue)
{
    if (!SmscMiiWaitIdle(pAd))
        return FALSE;
    if (!SmscWriteReg(pAd, SMSC_MII_DATA, (DWORD)wValue))
        return FALSE;
    if (!SmscWriteReg(pAd, SMSC_MII_ADDR, SMSC_MII_ADDR_PHY(SMSC_PHY_ID) |
                      SMSC_MII_ADDR_REG(dwPhyReg) | SMSC_MII_WRITE | SMSC_MII_BUSY))
        return FALSE;
    return SmscMiiWaitIdle(pAd);
}

//------------------------------------------------------------------------------
// Adapter context
//------------------------------------------------------------------------------
static void SmscFreeAdapter(PSMSC_ADAPTER pAd)
{
    if (pAd == NULL)
        return;
    if (pAd->hCtlEvent != NULL)
        CloseHandle(pAd->hCtlEvent);
    DeleteCriticalSection(&pAd->csCtl);
    DeleteCriticalSection(&pAd->csNdis);
    LocalFree(pAd);
}

// Record the bulk IN, bulk OUT and interrupt IN endpoints of the interface.
static BOOL SmscFindEndpoints(PSMSC_ADAPTER pAd)
{
    LPCUSB_INTERFACE pIf = pAd->pInterface;
    LPCUSB_ENDPOINT_DESCRIPTOR pEp;
    DWORD i;
    UCHAR bType;
    BOOL fIn;

    SMSCINFO((TEXT("SMSC95XX: interface %u alt %u class %02x/%02x/%02x, %u endpoints\r\n"),
              pIf->Descriptor.bInterfaceNumber, pIf->Descriptor.bAlternateSetting,
              pIf->Descriptor.bInterfaceClass, pIf->Descriptor.bInterfaceSubClass,
              pIf->Descriptor.bInterfaceProtocol, pIf->Descriptor.bNumEndpoints));

    for (i = 0; i < pIf->Descriptor.bNumEndpoints; i++)
    {
        pEp = &pIf->lpEndpoints[i].Descriptor;
        bType = pEp->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        fIn = (pEp->bEndpointAddress & USB_ENDPOINT_DIRECTION_MASK) != 0;

        SMSCINFO((TEXT("SMSC95XX:   ep 0x%02x type %u maxpkt %u interval %u\r\n"),
                  pEp->bEndpointAddress, bType, pEp->wMaxPacketSize, pEp->bInterval));

        if (bType == USB_ENDPOINT_TYPE_BULK && fIn && !pAd->fBulkIn)
        {
            pAd->epBulkIn = *pEp;
            pAd->fBulkIn = TRUE;
        }
        else if (bType == USB_ENDPOINT_TYPE_BULK && !fIn && !pAd->fBulkOut)
        {
            pAd->epBulkOut = *pEp;
            pAd->fBulkOut = TRUE;
        }
        else if (bType == USB_ENDPOINT_TYPE_INTERRUPT && fIn && !pAd->fIntrIn)
        {
            pAd->epIntrIn = *pEp;
            pAd->fIntrIn = TRUE;
        }
    }

    return pAd->fBulkIn && pAd->fBulkOut && pAd->fIntrIn;
}

// E1: log the chip ID and the state the chip is in at attach. Read only.
static void SmscProbeDump(PSMSC_ADAPTER pAd)
{
    static const DWORD s_adwRegs[] =
    {
        SMSC_ID_REV, SMSC_HW_CFG, SMSC_PM_CTRL, SMSC_LED_GPIO_CFG,
        SMSC_AFC_CFG, SMSC_EEPROM_CMD, SMSC_BURST_CAP, SMSC_INTR_CFG,
        SMSC_BULK_IN_DLY, SMSC_MAC_CSR, SMSC_MAC_ADDRH, SMSC_MAC_ADDRL
    };
    DWORD i, dwVal;
    WORD wVal, wBmsr2;

    for (i = 0; i < sizeof(s_adwRegs) / sizeof(s_adwRegs[0]); i++)
    {
        if (SmscReadReg(pAd, s_adwRegs[i], &dwVal))
            SMSCINFO((TEXT("SMSC95XX:   reg 0x%03x = 0x%08x\r\n"), s_adwRegs[i], dwVal));
        else
            SMSCERR((TEXT("SMSC95XX:   reg 0x%03x read FAILED\r\n"), s_adwRegs[i]));
    }

    if (SmscReadReg(pAd, SMSC_ID_REV, &dwVal) &&
        (dwVal & SMSC_ID_REV_CHIP_ID_MASK) != ((DWORD)SMSC_PID_LAN95XX << 16))
    {
        SMSCWARN((TEXT("SMSC95XX: unexpected chip ID 0x%08x (expected 0xEC00xxxx)\r\n"), dwVal));
    }

    // PHY registers 0-5. The BMSR link bit latches low, so read BMSR a
    // second time for the current link state.
    for (i = MII_BMCR; i <= MII_ANLPAR; i++)
    {
        if (SmscMiiRead(pAd, i, &wVal))
            SMSCINFO((TEXT("SMSC95XX:   phy %u = 0x%04x\r\n"), i, wVal));
        else
            SMSCERR((TEXT("SMSC95XX:   phy %u read FAILED\r\n"), i));
    }
    if (SmscMiiRead(pAd, MII_BMSR, &wBmsr2))
    {
        SMSCINFO((TEXT("SMSC95XX: link %s, autoneg %s (BMSR 0x%04x)\r\n"),
                  (wBmsr2 & MII_BMSR_LINK) ? TEXT("UP") : TEXT("down"),
                  (wBmsr2 & MII_BMSR_ANEG_COMPLETE) ? TEXT("complete") : TEXT("not complete"),
                  wBmsr2));
    }
}

//------------------------------------------------------------------------------
// USBD client entry points
//------------------------------------------------------------------------------

// The driver is registered statically in platform.reg (LoadClients key), so
// there is nothing to install at run time.
BOOL USBInstallDriver(LPCWSTR szDriverLibFile)
{
    UNREFERENCED_PARAMETER(szDriverLibFile);
    SMSCWARN((TEXT("SMSC95XX: USBInstallDriver - registered statically in platform.reg\r\n")));
    return FALSE;
}

BOOL USBUnInstallDriver(void)
{
    return TRUE;
}

BOOL USBDeviceAttach(USB_HANDLE hDevice, LPCUSB_FUNCS lpUsbFuncs,
                     LPCUSB_INTERFACE lpInterface, LPCWSTR szUniqueDriverId,
                     LPBOOL fAcceptControl,
                     LPCUSB_DRIVER_SETTINGS lpDriverSettings, DWORD dwUnused)
{
    PSMSC_ADAPTER pAd = NULL;
    LPCUSB_DEVICE pDevInfo;

    UNREFERENCED_PARAMETER(szUniqueDriverId);
    UNREFERENCED_PARAMETER(lpDriverSettings);
    UNREFERENCED_PARAMETER(dwUnused);

    *fAcceptControl = FALSE;

    pDevInfo = lpUsbFuncs->lpGetDeviceInfo(hDevice);
    if (pDevInfo == NULL || pDevInfo->lpActiveConfig == NULL)
    {
        SMSCERR((TEXT("SMSC95XX: no device info\r\n")));
        return FALSE;
    }

    SMSCINIT((TEXT("SMSC95XX: attach %04x:%04x rev %04x, %s\r\n"),
              pDevInfo->Descriptor.idVendor, pDevInfo->Descriptor.idProduct,
              pDevInfo->Descriptor.bcdDevice,
              lpInterface ? TEXT("interface match") : TEXT("device match")));

    if (pDevInfo->Descriptor.idVendor != SMSC_VID ||
        pDevInfo->Descriptor.idProduct != SMSC_PID_LAN95XX)
    {
        SMSCWARN((TEXT("SMSC95XX: not an SMSC LAN95xx Ethernet function, ignored\r\n")));
        return FALSE;
    }

    // Device-level match: use interface 0 of the active configuration.
    if (lpInterface == NULL)
    {
        if (pDevInfo->lpActiveConfig->dwNumInterfaces < 1)
        {
            SMSCERR((TEXT("SMSC95XX: active configuration has no interface\r\n")));
            return FALSE;
        }
        lpInterface = &pDevInfo->lpActiveConfig->lpInterfaces[0];
    }

    pAd = (PSMSC_ADAPTER)LocalAlloc(LPTR, sizeof(SMSC_ADAPTER));
    if (pAd == NULL)
        return FALSE;

    InitializeCriticalSection(&pAd->csCtl);
    InitializeCriticalSection(&pAd->csNdis);
    pAd->hDevice = hDevice;
    pAd->pUsbFuncs = lpUsbFuncs;
    pAd->pInterface = lpInterface;
    pAd->hCtlEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (pAd->hCtlEvent == NULL)
    {
        SmscFreeAdapter(pAd);
        return FALSE;
    }

    if (!SmscFindEndpoints(pAd))
    {
        SMSCERR((TEXT("SMSC95XX: expected bulk IN, bulk OUT and interrupt IN endpoints\r\n")));
        SmscFreeAdapter(pAd);
        return FALSE;
    }

    SMSCINFO((TEXT("SMSC95XX: bulk in 0x%02x (%u), bulk out 0x%02x (%u), intr in 0x%02x\r\n"),
              pAd->epBulkIn.bEndpointAddress, pAd->epBulkIn.wMaxPacketSize,
              pAd->epBulkOut.bEndpointAddress, pAd->epBulkOut.wMaxPacketSize,
              pAd->epIntrIn.bEndpointAddress));

#ifdef DEBUG
    // Reset-state register dump (~25 control transfers): Info zone only.
    if (DEBUGZONE(ZONEID_INFO))
        SmscProbeDump(pAd);
#endif

    if (!SmscStart(pAd))
    {
        SMSCERR((TEXT("SMSC95XX: start failed\r\n")));
        SmscStop(pAd);
        SmscFreeAdapter(pAd);
        return FALSE;
    }

    if (!lpUsbFuncs->lpRegisterNotificationRoutine(hDevice, SmscDeviceNotify, pAd))
    {
        SMSCERR((TEXT("SMSC95XX: RegisterNotificationRoutine failed\r\n")));
        SmscStop(pAd);
        SmscFreeAdapter(pAd);
        return FALSE;
    }

    // Bring up the NDIS adapter (asynchronously). Without it the USB side
    // still runs, so keep the device either way.
    if (!SmscNdisAttach(pAd))
        SMSCERR((TEXT("SMSC95XX: NDIS attach failed, no network adapter\r\n")));

    *fAcceptControl = TRUE;
    SMSCINFO((TEXT("SMSC95XX: started\r\n")));
    return TRUE;
}

static BOOL WINAPI SmscDeviceNotify(LPVOID lpvNotifyParameter, DWORD dwCode,
                                    LPDWORD *dwInfo1, LPDWORD *dwInfo2,
                                    LPDWORD *dwInfo3, LPDWORD *dwInfo4)
{
    PSMSC_ADAPTER pAd = (PSMSC_ADAPTER)lpvNotifyParameter;

    UNREFERENCED_PARAMETER(dwInfo1);
    UNREFERENCED_PARAMETER(dwInfo2);
    UNREFERENCED_PARAMETER(dwInfo3);
    UNREFERENCED_PARAMETER(dwInfo4);

    if (dwCode != USB_CLOSE_DEVICE)
        return FALSE;

    SMSCINIT((TEXT("SMSC95XX: device closed\r\n")));
    if (pAd != NULL)
    {
        // Order matters: stop new work, take the NDIS adapter down (which
        // halts the miniport and fails queued sends), then stop the USB side.
        pAd->fClosing = TRUE;
        if (pAd->hStopEvent != NULL)
            SetEvent(pAd->hStopEvent);
        SmscNdisDetach(pAd);
        SmscStop(pAd);
        // Wait out any register access still in flight, then free.
        EnterCriticalSection(&pAd->csCtl);
        LeaveCriticalSection(&pAd->csCtl);
        SmscFreeAdapter(pAd);
    }
    return TRUE;
}

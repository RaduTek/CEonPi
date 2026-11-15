//
// system.cpp
//
// Platform dependent part (PDD) of the DWC2 USB host controller driver.
// hcd2lib implements the HCD_* stream entry points and calls the HcdPdd_*
// functions below.
//
// Milestone 2: bring the core up as a host, power the root port and run a
// temporary interrupt thread that only watches the root port (connect,
// reset, enable, speed, disconnect). No HCD object is created yet, so the
// CE USB stack is not started. The thread is replaced in milestone 3.
//
#include <windows.h>
#include <nkintr.h>
#include <ceddk.h>
#include <hcdddsi.h>
#include <bsp_cfg.h>
#include "dwccore.h"

#define DWCMSG(x)   RETAILMSG(1, x)

#define DWC_IST_PRIORITY        101     // same default as the CE host drivers
#define DWC_DEBOUNCE_MS         100     // USB 2.0 connect debounce interval
#define DWC_PORT_RESET_MS       60      // root port reset, at least 50 ms

// Device context returned from HcdPdd_Init
typedef struct _DWC_PDD
{
    CDwcCore *pCore;
    HANDLE    hIntrEvent;
    HANDLE    hIst;
    BOOL      fStop;
    BOOL      fIntrInitialized;
    LPVOID    lpvMemoryObject;   // hcd2lib CPhysMem (milestone 3)
    LPVOID    lpvHcdObject;      // hcd2lib CHcd (milestone 3)
} DWC_PDD;

static const TCHAR * const g_szSpeed[4] =
{
    TEXT("HIGH"), TEXT("FULL"), TEXT("LOW"), TEXT("?")
};

// Root port interrupt: something changed in HPRT0.
static void DwcHandlePort(CDwcCore *pCore)
{
    DWORD dwHprt = pCore->ReadPort();

    DWCMSG((TEXT("DWCHCI: port interrupt HPRT0=0x%08x (conn=%d ena=%d rst=%d pwr=%d)\r\n"),
            dwHprt,
            (dwHprt & HPRT0_CONNSTS) ? 1 : 0,
            (dwHprt & HPRT0_ENA) ? 1 : 0,
            (dwHprt & HPRT0_RST) ? 1 : 0,
            (dwHprt & HPRT0_PWR) ? 1 : 0));

    // Clear the change bits first so a new change raises a new interrupt
    pCore->AckPortChanges(dwHprt);

    if (dwHprt & HPRT0_CONNDET)
    {
        DWCMSG((TEXT("DWCHCI: device connect detected, debouncing\r\n")));
        Sleep(DWC_DEBOUNCE_MS);
        if (pCore->ReadPort() & HPRT0_CONNSTS)
        {
            DWCMSG((TEXT("DWCHCI: resetting port for %d ms\r\n"), DWC_PORT_RESET_MS));
            pCore->PortReset(DWC_PORT_RESET_MS);
            DWCMSG((TEXT("DWCHCI: reset released, HPRT0=0x%08x\r\n"), pCore->ReadPort()));
        }
        else
        {
            DWCMSG((TEXT("DWCHCI: device went away during debounce\r\n")));
        }
    }

    if (dwHprt & HPRT0_ENACHG)
    {
        if (dwHprt & HPRT0_ENA)
        {
            pCore->PortEnabled(dwHprt);
            DWCMSG((TEXT("DWCHCI: port enabled, speed=%s, HFIR=0x%08x HFNUM=0x%08x\r\n"),
                    g_szSpeed[(dwHprt & HPRT0_SPD_MASK) >> HPRT0_SPD_SHIFT],
                    pCore->Rd(HFIR), pCore->Rd(HFNUM)));
        }
        else
        {
            DWCMSG((TEXT("DWCHCI: port disabled\r\n")));
        }
    }

    if (dwHprt & HPRT0_OVRCURRCHG)
    {
        DWCMSG((TEXT("DWCHCI: port over-current change, active=%d\r\n"),
                (dwHprt & HPRT0_OVRCURRACT) ? 1 : 0));
    }
}

// Temporary interrupt service thread (milestone 2)
static DWORD WINAPI DwcIst(LPVOID lpv)
{
    DWC_PDD *pPdd = (DWC_PDD *)lpv;
    CDwcCore *pCore = pPdd->pCore;
    DWORD dwSts, dwPend, dwOther;
    DWORD dwSpurious = 0;

    CeSetThreadPriority(GetCurrentThread(), DWC_IST_PRIORITY);
    DWCMSG((TEXT("DWCHCI: interrupt thread started, SysIntr=%d\r\n"), SYSINTR_USBHOST));

    for (;;)
    {
        WaitForSingleObject(pPdd->hIntrEvent, INFINITE);
        if (pPdd->fStop)
            break;

        dwSts = pCore->Rd(GINTSTS);
        dwPend = dwSts & pCore->Rd(GINTMSK);

        if (dwPend == 0)
        {
            dwSpurious++;
            if (dwSpurious <= 5)
                DWCMSG((TEXT("DWCHCI: interrupt with nothing pending, GINTSTS=0x%08x\r\n"), dwSts));
        }

        if (dwPend & GINTSTS_PRTINT)
        {
            // PRTINT clears itself when the HPRT0 change bits are cleared
            DwcHandlePort(pCore);
        }

        if (dwPend & GINTSTS_DISCONNINT)
        {
            pCore->Wr(GINTSTS, GINTSTS_DISCONNINT);
            DWCMSG((TEXT("DWCHCI: device disconnected, HPRT0=0x%08x\r\n"), pCore->ReadPort()));
        }

        if (dwPend & GINTSTS_HCHINT)
        {
            // No channels are in use yet. Mask them all so this cannot repeat.
            DWCMSG((TEXT("DWCHCI: unexpected channel interrupt, HAINT=0x%08x\r\n"), pCore->Rd(HAINT)));
            pCore->Wr(HAINTMSK, 0);
        }

        dwOther = dwPend & ~(GINTSTS_PRTINT | GINTSTS_DISCONNINT | GINTSTS_HCHINT);
        if (dwOther)
        {
            DWCMSG((TEXT("DWCHCI: unexpected interrupt bits 0x%08x\r\n"), dwOther));
            pCore->Wr(GINTSTS, dwOther);
        }

        InterruptDone(SYSINTR_USBHOST);
    }

    return 0;
}

// Stop the interrupt thread and quiesce the controller, then free the context
static void DwcCleanup(DWC_PDD *pPdd)
{
    if (pPdd == NULL)
        return;

    if (pPdd->pCore != NULL)
        pPdd->pCore->DisableInterrupts();

    if (pPdd->hIst != NULL)
    {
        pPdd->fStop = TRUE;
        SetEvent(pPdd->hIntrEvent);
        WaitForSingleObject(pPdd->hIst, 2000);
        CloseHandle(pPdd->hIst);
    }
    if (pPdd->fIntrInitialized)
        InterruptDisable(SYSINTR_USBHOST);
    if (pPdd->hIntrEvent != NULL)
        CloseHandle(pPdd->hIntrEvent);
    if (pPdd->pCore != NULL)
        delete pPdd->pCore;

    LocalFree(pPdd);
}

extern "C" BOOL HcdPdd_DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(dwReason);
    UNREFERENCED_PARAMETER(lpvReserved);
    return TRUE;
}

// dwContext is the driver's registry key path (a string), not the Active key.
extern "C" DWORD HcdPdd_Init(DWORD dwContext)
{
    DWC_PDD *pPdd;

    DWCMSG((TEXT("DWCHCI: HcdPdd_Init key=%s\r\n"), (LPCWSTR)dwContext));

    pPdd = (DWC_PDD *)LocalAlloc(LPTR, sizeof(DWC_PDD));
    if (pPdd == NULL)
        return 0;

    pPdd->pCore = new CDwcCore();
    if (pPdd->pCore == NULL)
    {
        LocalFree(pPdd);
        return 0;
    }

    if (!pPdd->pCore->Map())
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** cannot map the controller registers\r\n")));
        DwcCleanup(pPdd);
        return 0;
    }

    if (!pPdd->pCore->DumpIdentity())
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** controller not identified, driver not loaded\r\n")));
        DwcCleanup(pPdd);
        return 0;
    }

    if (!pPdd->pCore->HostInit())
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** host initialisation failed, driver not loaded\r\n")));
        DwcCleanup(pPdd);
        return 0;
    }

    pPdd->hIntrEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (pPdd->hIntrEvent == NULL)
    {
        DwcCleanup(pPdd);
        return 0;
    }

    if (!InterruptInitialize(SYSINTR_USBHOST, pPdd->hIntrEvent, NULL, 0))
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** InterruptInitialize(%d) failed, error %d\r\n"),
                SYSINTR_USBHOST, GetLastError()));
        DwcCleanup(pPdd);
        return 0;
    }
    pPdd->fIntrInitialized = TRUE;

    pPdd->hIst = CreateThread(NULL, 0, DwcIst, pPdd, 0, NULL);
    if (pPdd->hIst == NULL)
    {
        DwcCleanup(pPdd);
        return 0;
    }

    // A device that is already plugged in raises the port interrupt as
    // soon as interrupts are enabled.
    pPdd->pCore->EnableInterrupts();

    DWCMSG((TEXT("DWCHCI: HcdPdd_Init done (milestone 2: root port monitor)\r\n")));
    return (DWORD)pPdd;
}

extern "C" BOOL HcdPdd_CheckConfigPower(UCHAR bPort, DWORD dwConfigPower, DWORD dwTotalPower)
{
    UNREFERENCED_PARAMETER(bPort);
    return ((dwConfigPower + dwTotalPower) > 500) ? FALSE : TRUE;
}

extern "C" void HcdPdd_PowerUp(DWORD dwDeviceContext)
{
    UNREFERENCED_PARAMETER(dwDeviceContext);
}

extern "C" void HcdPdd_PowerDown(DWORD dwDeviceContext)
{
    UNREFERENCED_PARAMETER(dwDeviceContext);
}

extern "C" BOOL HcdPdd_Deinit(DWORD dwDeviceContext)
{
    DwcCleanup((DWC_PDD *)dwDeviceContext);
    return TRUE;
}

// hcd2lib's HCD_IOControl treats the open context as a CHcd pointer. There is
// no HCD object yet, so opening the device is refused.
extern "C" DWORD HcdPdd_Open(DWORD dwDeviceContext, DWORD dwAccessCode, DWORD dwShareMode)
{
    UNREFERENCED_PARAMETER(dwDeviceContext);
    UNREFERENCED_PARAMETER(dwAccessCode);
    UNREFERENCED_PARAMETER(dwShareMode);
    return 0;
}

extern "C" BOOL HcdPdd_Close(DWORD dwOpenContext)
{
    UNREFERENCED_PARAMETER(dwOpenContext);
    return TRUE;
}

extern "C" DWORD HcdPdd_Read(DWORD dwOpenContext, LPVOID lpvBuffer, DWORD dwCount)
{
    UNREFERENCED_PARAMETER(dwOpenContext);
    UNREFERENCED_PARAMETER(lpvBuffer);
    UNREFERENCED_PARAMETER(dwCount);
    return (DWORD)-1;
}

extern "C" DWORD HcdPdd_Write(DWORD dwOpenContext, LPCVOID lpvSourceBytes, DWORD dwNumberOfBytes)
{
    UNREFERENCED_PARAMETER(dwOpenContext);
    UNREFERENCED_PARAMETER(lpvSourceBytes);
    UNREFERENCED_PARAMETER(dwNumberOfBytes);
    return (DWORD)-1;
}

extern "C" DWORD HcdPdd_Seek(DWORD dwOpenContext, LONG lAmount, DWORD dwType)
{
    UNREFERENCED_PARAMETER(dwOpenContext);
    UNREFERENCED_PARAMETER(lAmount);
    UNREFERENCED_PARAMETER(dwType);
    return (DWORD)-1;
}

extern "C" BOOL HcdPdd_IOControl(DWORD dwOpenContext, DWORD dwCode, PBYTE pbBufIn, DWORD dwLenIn,
                                 PBYTE pbBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    UNREFERENCED_PARAMETER(dwOpenContext);
    UNREFERENCED_PARAMETER(dwCode);
    UNREFERENCED_PARAMETER(pbBufIn);
    UNREFERENCED_PARAMETER(dwLenIn);
    UNREFERENCED_PARAMETER(pbBufOut);
    UNREFERENCED_PARAMETER(dwLenOut);
    UNREFERENCED_PARAMETER(pdwActualOut);
    return FALSE;
}

extern "C" void HcdPdd_InitiatePowerUp(DWORD dwDeviceContext)
{
    UNREFERENCED_PARAMETER(dwDeviceContext);
}

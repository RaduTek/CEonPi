//
// system.cpp
//
// Platform dependent part (PDD) of the DWC2 USB host controller driver.
// hcd2lib implements the HCD_* stream entry points and calls the HcdPdd_*
// functions below.
//
// Milestone 1: map the controller and dump its identification registers.
// No HCD object is created yet, so the CE USB stack is not started.
//
#include <windows.h>
#include <ceddk.h>
#include <hcdddsi.h>
#include "dwccore.h"

#define DWCMSG(x)   RETAILMSG(1, x)

// Device context returned from HcdPdd_Init
typedef struct _DWC_PDD
{
    CDwcCore *pCore;
    LPVOID    lpvMemoryObject;   // hcd2lib CPhysMem (milestone 3)
    LPVOID    lpvHcdObject;      // hcd2lib CHcd (milestone 3)
} DWC_PDD;

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
        delete pPdd->pCore;
        LocalFree(pPdd);
        return 0;
    }

    if (!pPdd->pCore->DumpIdentity())
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** controller not identified, driver not loaded\r\n")));
        delete pPdd->pCore;
        LocalFree(pPdd);
        return 0;
    }

    DWCMSG((TEXT("DWCHCI: HcdPdd_Init done (milestone 1: register dump only)\r\n")));
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
    DWC_PDD *pPdd = (DWC_PDD *)dwDeviceContext;

    if (pPdd != NULL)
    {
        if (pPdd->pCore != NULL)
            delete pPdd->pCore;
        LocalFree(pPdd);
    }
    return TRUE;
}

// hcd2lib's HCD_IOControl treats the open context as a CHcd pointer. There is
// no HCD object in milestone 1, so opening the device is refused.
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

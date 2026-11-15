//
// system.cpp
//
// Platform dependent part (PDD) of the DWC2 USB host controller driver.
// hcd2lib implements the HCD_* stream entry points and calls the HcdPdd_*
// functions below.
//
// The PDD creates the DMA memory pool and the host controller object
// (CDwcHcd, through CreateHCDObject in cdwchcd.cpp). Everything that
// touches the hardware is in CDwcHcd and CDwcCore.
//
#include <windows.h>
#include <nkintr.h>
#include <ceddk.h>
#include <hcdddsi.h>
#include <bsp_cfg.h>

#define DWCMSG(x)   RETAILMSG(1, x)

// DMA memory pool handed to hcd2lib's CPhysMem. All transfer data is
// copied through this pool. It is allocated uncached and contiguous.
#define DWC_PHYSMEM_TOTAL       0x50000                     // 320 KB
#define DWC_PHYSMEM_HIGHPRI     (DWC_PHYSMEM_TOTAL / 4)

// Device context returned from HcdPdd_Init
typedef struct _DWC_PDD
{
    LPVOID lpvMemoryObject;     // hcd2lib CPhysMem
    LPVOID lpvHcdObject;        // hcd2lib CHcd (our CDwcHcd)
} DWC_PDD;

extern "C" BOOL HcdPdd_DllMain(HANDLE hinstDLL, DWORD dwReason, LPVOID lpvReserved)
{
    UNREFERENCED_PARAMETER(hinstDLL);
    UNREFERENCED_PARAMETER(dwReason);
    UNREFERENCED_PARAMETER(lpvReserved);
    return TRUE;
}

// dwContext is the driver's registry key path (a string), not the Active
// key. The HCD object must be created here, inside HCD_Init.
extern "C" DWORD HcdPdd_Init(DWORD dwContext)
{
    DWC_PDD *pPdd;

    DWCMSG((TEXT("DWCHCI: HcdPdd_Init key=%s\r\n"), (LPCWSTR)dwContext));

    pPdd = (DWC_PDD *)LocalAlloc(LPTR, sizeof(DWC_PDD));
    if (pPdd == NULL)
        return 0;

    // No addresses passed in: CPhysMem allocates the pool itself
    pPdd->lpvMemoryObject = HcdMdd_CreateMemoryObject(DWC_PHYSMEM_TOTAL, DWC_PHYSMEM_HIGHPRI, NULL, NULL);
    if (pPdd->lpvMemoryObject == NULL)
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** cannot create the DMA memory pool\r\n")));
        LocalFree(pPdd);
        return 0;
    }

    // Creates CDwcHcd and calls its DeviceInitialize()
    pPdd->lpvHcdObject = HcdMdd_CreateHcdObject(pPdd, pPdd->lpvMemoryObject, (LPCWSTR)dwContext,
                                                NULL, SYSINTR_USBHOST);
    if (pPdd->lpvHcdObject == NULL)
    {
        DWCMSG((TEXT("DWCHCI: ***ERROR*** host controller object not created, driver not loaded\r\n")));
        HcdMdd_DestroyMemoryObject(pPdd->lpvMemoryObject);
        LocalFree(pPdd);
        return 0;
    }

    DWCMSG((TEXT("DWCHCI: HcdPdd_Init done\r\n")));
    return (DWORD)pPdd;
}

extern "C" BOOL HcdPdd_CheckConfigPower(UCHAR bPort, DWORD dwConfigPower, DWORD dwTotalPower)
{
    UNREFERENCED_PARAMETER(bPort);
    return ((dwConfigPower + dwTotalPower) > 500) ? FALSE : TRUE;
}

extern "C" void HcdPdd_PowerUp(DWORD dwDeviceContext)
{
    DWC_PDD *pPdd = (DWC_PDD *)dwDeviceContext;

    if (pPdd != NULL && pPdd->lpvHcdObject != NULL)
        HcdMdd_PowerUp(pPdd->lpvHcdObject);
}

extern "C" void HcdPdd_PowerDown(DWORD dwDeviceContext)
{
    DWC_PDD *pPdd = (DWC_PDD *)dwDeviceContext;

    if (pPdd != NULL && pPdd->lpvHcdObject != NULL)
        HcdMdd_PowerDown(pPdd->lpvHcdObject);
}

extern "C" BOOL HcdPdd_Deinit(DWORD dwDeviceContext)
{
    DWC_PDD *pPdd = (DWC_PDD *)dwDeviceContext;

    if (pPdd != NULL)
    {
        if (pPdd->lpvHcdObject != NULL)
            HcdMdd_DestroyHcdObject(pPdd->lpvHcdObject);
        if (pPdd->lpvMemoryObject != NULL)
            HcdMdd_DestroyMemoryObject(pPdd->lpvMemoryObject);
        LocalFree(pPdd);
    }
    return TRUE;
}

// hcd2lib's HCD_IOControl treats the open context as the CHcd pointer
extern "C" DWORD HcdPdd_Open(DWORD dwDeviceContext, DWORD dwAccessCode, DWORD dwShareMode)
{
    DWC_PDD *pPdd = (DWC_PDD *)dwDeviceContext;

    UNREFERENCED_PARAMETER(dwAccessCode);
    UNREFERENCED_PARAMETER(dwShareMode);
    return (pPdd != NULL) ? (DWORD)pPdd->lpvHcdObject : 0;
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

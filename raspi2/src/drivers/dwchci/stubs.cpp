//
// stubs.cpp
//
// Milestone 1 only. hcd2lib needs the controller specific factory functions
// at link time. Until the real host controller and pipe classes exist these
// return NULL. They are never called in milestone 1 because the PDD does not
// create an HCD object yet. This file goes away in milestone 3.
//
#include <windows.h>
#include <globals.hpp>
#include <hcd.hpp>

CHcd * CreateHCDObject(IN LPVOID pvUhcdPddObject,
                       IN CPhysMem * pCPhysMem,
                       IN LPCWSTR szDriverRegistryKey,
                       IN REGISTER portBase,
                       IN DWORD dwSysIntr)
{
    UNREFERENCED_PARAMETER(pvUhcdPddObject);
    UNREFERENCED_PARAMETER(pCPhysMem);
    UNREFERENCED_PARAMETER(szDriverRegistryKey);
    UNREFERENCED_PARAMETER(portBase);
    UNREFERENCED_PARAMETER(dwSysIntr);
    return NULL;
}

CPipeAbs * CreateBulkPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    UNREFERENCED_PARAMETER(lpEndpointDescriptor);
    UNREFERENCED_PARAMETER(fIsLowSpeed);
    UNREFERENCED_PARAMETER(fIsHighSpeed);
    UNREFERENCED_PARAMETER(bDeviceAddress);
    UNREFERENCED_PARAMETER(bHubAddress);
    UNREFERENCED_PARAMETER(bHubPort);
    UNREFERENCED_PARAMETER(ttContext);
    UNREFERENCED_PARAMETER(pChcd);
    return NULL;
}

CPipeAbs * CreateControlPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    UNREFERENCED_PARAMETER(lpEndpointDescriptor);
    UNREFERENCED_PARAMETER(fIsLowSpeed);
    UNREFERENCED_PARAMETER(fIsHighSpeed);
    UNREFERENCED_PARAMETER(bDeviceAddress);
    UNREFERENCED_PARAMETER(bHubAddress);
    UNREFERENCED_PARAMETER(bHubPort);
    UNREFERENCED_PARAMETER(ttContext);
    UNREFERENCED_PARAMETER(pChcd);
    return NULL;
}

CPipeAbs * CreateInterruptPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    UNREFERENCED_PARAMETER(lpEndpointDescriptor);
    UNREFERENCED_PARAMETER(fIsLowSpeed);
    UNREFERENCED_PARAMETER(fIsHighSpeed);
    UNREFERENCED_PARAMETER(bDeviceAddress);
    UNREFERENCED_PARAMETER(bHubAddress);
    UNREFERENCED_PARAMETER(bHubPort);
    UNREFERENCED_PARAMETER(ttContext);
    UNREFERENCED_PARAMETER(pChcd);
    return NULL;
}

CPipeAbs * CreateIsochronousPipe(IN const LPCUSB_ENDPOINT_DESCRIPTOR lpEndpointDescriptor,
               IN const BOOL fIsLowSpeed, IN const BOOL fIsHighSpeed,
               IN const UCHAR bDeviceAddress,
               IN const UCHAR bHubAddress, IN const UCHAR bHubPort, const PVOID ttContext,
               IN CHcd * const pChcd)
{
    UNREFERENCED_PARAMETER(lpEndpointDescriptor);
    UNREFERENCED_PARAMETER(fIsLowSpeed);
    UNREFERENCED_PARAMETER(fIsHighSpeed);
    UNREFERENCED_PARAMETER(bDeviceAddress);
    UNREFERENCED_PARAMETER(bHubAddress);
    UNREFERENCED_PARAMETER(bHubPort);
    UNREFERENCED_PARAMETER(ttContext);
    UNREFERENCED_PARAMETER(pChcd);
    return NULL;
}

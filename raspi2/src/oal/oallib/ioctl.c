//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// Use of this sample source code is subject to the terms of the Microsoft
// license agreement under which you licensed this sample source code. If
// you did not accept the terms of the license agreement, you are not
// authorized to use this sample source code. For the terms of the license,
// please see the license agreement between you and Microsoft or, if applicable,
// see the LICENSE.RTF on your install media or the root of your tools installation.
// THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR INDEMNITIES.
//
#include <windows.h>
#include <oemglobal.h>
#include <oal.h>
#include <nkintr.h>
#include <bsp_cfg.h>
#include <image_cfg.h>
#include <video.h>
#include <mailbox.h>
//------------------------------------------------------------------------------
// Externs
//
extern PBYTE g_p2835Regs;

static struct {
    BOOL postInit;
    CRITICAL_SECTION cs;
} g_ioctlState = { FALSE };

BOOL OALIoCtlHalSetKitlSerBuffs(UINT32 code, VOID* pInpBuffer, UINT32 inpSize, 
							    VOID* pOutBuffer,UINT32 outSize, UINT32 *pOutSize); 

BOOL OALIoCtlHalGetHardwarePtrs(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, 
								 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Function prototype for our IOCTL_HAL_DDI handler function, implemented in ddi.c
BOOL OALIoctlHalDdi(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
					VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Sends one VideoCore property tag (defined below; also used by init.c).
BOOL OALMailboxPropertyTag(DWORD dwTag, DWORD *pdwValues, DWORD nWords);

// Returns the VideoCore EMMC base clock in Hz (for the SD host driver).
BOOL OALIoCtlHalGetEmmcClock(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
							 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Returns the SoC/core/vendor strings (System control panel "Processor").
BOOL OALIoCtlProcessorInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
						   VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Resets the board through the PM watchdog; never returns (power.c).
BOOL OALIoCtlHalReboot(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
					   VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Returns the firmware-assigned board MAC address (for the USB Ethernet driver).
BOOL OALIoCtlHalGetMacAddress(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
							  VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Sends a list of frame buffer / cursor property tags (for the display driver).
BOOL OALIoCtlHalMailboxProperty(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
								VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

// Tells filesys never to delete the registry hives at boot (defined below).
BOOL OALIoCtlHalGetHiveCleanFlag(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
								 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

//------------------------------------------------------------------------------
//
//  Global: g_MyoalIoCtlTable[]
//
//  IOCTL handler table. This table includes the IOCTL code/handler pairs
//  defined in the IOCTL configuration file. This global array is exported
//  via oal_ioctl.h and is used by the OAL IOCTL component.
//
const OAL_IOCTL_HANDLER g_MyoalIoCtlTable[] =
{

// The common OAL IOCTL table
//{ IOCTL_HAL_TRANSLATE_IRQ,                  0,  OALIntrTranslateIrq         },
{ IOCTL_HAL_REQUEST_SYSINTR,                0,  OALIoCtlHalRequestSysIntr   },
//{ IOCTL_HAL_RELEASE_SYSINTR,                0,  OALIoCtlHalReleaseSysIntr   },
//{ IOCTL_HAL_REQUEST_IRQ,                    0,  OALIoCtlHalRequestIrq       },
//
{ IOCTL_HAL_INITREGISTRY,                   0,  OALIoCtlHalInitRegistry     },
//{ IOCTL_HAL_INIT_RTC,                       0,  OALIoCtlHalInitRTC          },
{ IOCTL_HAL_REBOOT,       OAL_IOCTL_FLAG_NOCS,  OALIoCtlHalReboot           },
//
//{ IOCTL_HAL_DDK_CALL,                       0,  OALIoCtlHalDdkCall          },
//
//{ IOCTL_HAL_DISABLE_WAKE,                   0,  OALIoCtlHalDisableWake      },
//{ IOCTL_HAL_ENABLE_WAKE,                    0,  OALIoCtlHalEnableWake       },
//{ IOCTL_HAL_GET_WAKE_SOURCE,                0,  OALIoCtlHalGetWakeSource    },
//
{ IOCTL_HAL_GET_CACHE_INFO,                 0,  OALIoCtlHalGetCacheInfo     },
//{ IOCTL_HAL_GET_DEVICEID,                   0,  OALIoCtlHalGetDeviceId      },
//{ IOCTL_HAL_GET_DEVICE_INFO,                0,  OALIoCtlHalGetDeviceInfo    },
//{ IOCTL_HAL_GET_UUID,                       0,  OALIoCtlHalGetUUID          },
//{ IOCTL_HAL_GET_RANDOM_SEED,                0,  OALIoCtlHalGetRandomSeed    },
{ IOCTL_PROCESSOR_INFORMATION,              0,  OALIoCtlProcessorInfo       },
//{ IOCTL_HAL_UPDATE_MODE,                    0,  OALIoCtlHalUpdateMode       },


// Platform specific OAL IOCTLs
//{ IOCTL_HAL_GET_HWENTROPY,                  0,    OALIoCtlHalGetHWEntropy           },
{ IOCTL_HAL_GET_HIVE_CLEAN_FLAG,              0,  OALIoCtlHalGetHiveCleanFlag    },
//{ IOCTL_HAL_QUERY_FORMAT_PARTITION,         0,    OALIoCtlHalQueryFormatPartition   },
//{ IOCTL_HAL_QUERY_DISPLAYSETTINGS,          0,    OALIoCtlHalQueryDisplaySettings   },
//{ IOCTL_HAL_GET_CPUID,                      0,    OALIoCtlHalGetCPUID               },
//{ IOCTL_HAL_SET_SYSTEM_LEVEL,               0,    OALIoCtlHalSetSystemLevel         },
//{ IOCTL_HAL_PROFILE_DVS,                    0,    OALIoCtlHalProfileDVS             },
//{ IOCTL_HAL_CLOCK_INFO,                     0,    OALIoCtlHalClockInfo              },
//{ IOCTL_HAL_GET_POWER_DISPOSITION,          0,    OALIoCtlHalGetPowerDisposition    },

//
// (db) Added these for the RASPI port
{ IOCTL_HAL_SET_KITL_SERIAL_BUFFERS,          0,  OALIoCtlHalSetKitlSerBuffs     },
{ IOCTL_HAL_MY_GET_HARDWARE_POINTERS,         0,  OALIoCtlHalGetHardwarePtrs     },
{ IOCTL_HAL_MY_GET_EMMC_CLOCK,                0,  OALIoCtlHalGetEmmcClock        },
{ IOCTL_HAL_MY_GET_MAC_ADDRESS,               0,  OALIoCtlHalGetMacAddress       },
{ IOCTL_HAL_MY_MAILBOX_PROPERTY,              0,  OALIoCtlHalMailboxProperty     },
{ IOCTL_HAL_DDI,                              0,  OALIoctlHalDdi   },

// Required Termination
{ 0,                                        0,    NULL    }

};


__declspec(noinline) BOOL DoOEMIoControlWithCS (
    DWORD dwIndex,
    DWORD code, VOID *pInBuffer, DWORD inSize, VOID *pOutBuffer, DWORD outSize,
    DWORD *pOutSize
) {
    BOOL rc = FALSE;
    // Take critical section            
    EnterCriticalSection(&g_ioctlState.cs);
    __try {
        // Execute the handler
        rc = g_MyoalIoCtlTable[dwIndex].pfnHandler(
            code, pInBuffer, inSize, pOutBuffer, outSize, (UINT32 *)pOutSize
        );
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        rc = FALSE;
        NKSetLastError(ERROR_INVALID_PARAMETER);
    }
    // Release critical section            
    LeaveCriticalSection(&g_ioctlState.cs);
    return rc;

}


// ---------------------------------------------------------------------------
// OEMIoControl: REQUIRED
//
// This function provides a generic I/O control code (IOCTL) for
// OEM-supplied information.
//
BOOL OEMIoControl(DWORD dwIoControlCode, LPVOID lpInBuf, DWORD nInBufSize, PVOID lpOutBuf,
                  DWORD nOutBufSize, LPDWORD lpBytesReturned)
{
    BOOL rc = FALSE;
    UINT32 dwIndex;

#ifdef DEBUG
	// Not for the mailbox property IOCTL: the display driver calls it on every
	// cursor move, and each KITL debug line costs about 30 ms.
	if (dwIoControlCode != IOCTL_HAL_MY_MAILBOX_PROPERTY)
		NKDbgPrintfW(L"OEMIoControl: Code 0x%x Fn:%d\r\n", dwIoControlCode, (dwIoControlCode >> 2) & 0x0fff);
#endif

    OALMSG(OAL_IOCTL&&OAL_FUNC, (L"+OEMIoControl(0x%x, 0x%x, %d, 0x%x, %d, 0x%x)\r\n", 
        dwIoControlCode, lpInBuf, nInBufSize, lpOutBuf, nOutBufSize, lpBytesReturned));

    //OALLogSerial((L"OEMIoControl: Code 0x%x - device 0x%04x func %d\r\n", 
    //        dwIoControlCode, dwIoControlCode >> 16, (dwIoControlCode >> 2)&0x0FFF));

	
    //Initialize g_ioctlState.cs when IOCTL_HAL_POSTINIT is called. By this time, 
    //the kernel is up and ready to handle the critical section initialization.
    if (!g_ioctlState.postInit && dwIoControlCode == IOCTL_HAL_POSTINIT) 
	{
        // Initialize critical section
        InitializeCriticalSection(&g_ioctlState.cs);
        g_ioctlState.postInit = TRUE;
    } 

    // Search the IOCTL table for the requested code.
    for (dwIndex = 0; g_MyoalIoCtlTable[dwIndex].pfnHandler != NULL; dwIndex++) 
	{
        if (g_MyoalIoCtlTable[dwIndex].code == dwIoControlCode) break;
    }

    // Indicate unsupported code
    if (g_MyoalIoCtlTable[dwIndex].pfnHandler == NULL)	
	{
        NKSetLastError(ERROR_NOT_SUPPORTED);
        OALMSG(OAL_IOCTL, (L"OEMIoControl: Unsupported Code 0x%x - device 0x%04x func %d\r\n", 
            dwIoControlCode, dwIoControlCode >> 16, (dwIoControlCode >> 2)&0x0FFF));
        goto cleanUp;
    }        

    // Take critical section if required (after postinit & no flag)
    if (g_ioctlState.postInit && (g_MyoalIoCtlTable[dwIndex].flags & OAL_IOCTL_FLAG_NOCS) == 0) 
	{
        rc = DoOEMIoControlWithCS(dwIndex, dwIoControlCode, lpInBuf, nInBufSize, lpOutBuf, nOutBufSize, lpBytesReturned);
		//// Take critical section            
		//EnterCriticalSection(&g_ioctlState.cs);
		//__try 
		//{
		//	// Execute the handler
		//	rc = g_MyoalIoCtlTable[dwIndex].pfnHandler(dwIoControlCode, lpInBuf, nInBufSize, lpOutBuf, nOutBufSize, lpBytesReturned);
		//}
		//__except(EXCEPTION_EXECUTE_HANDLER) 
		//{
		//	rc = FALSE;
		//	NKSetLastError(ERROR_INVALID_PARAMETER);
		//}
		//// Release critical section            
		//LeaveCriticalSection(&g_ioctlState.cs);
    } 
    else 
	{
        rc = g_MyoalIoCtlTable[dwIndex].pfnHandler(dwIoControlCode, lpInBuf, nInBufSize, lpOutBuf, nOutBufSize, lpBytesReturned);
    }

cleanUp:
    OALMSG(OAL_IOCTL&&OAL_FUNC, (L"-OEMIoControl(rc = %d)\r\n", rc ));
    return rc;


  return TRUE;
}

// ---------------------------------------------------------------------------
// OEMKDIoctl: OPTIONAL
//
// This function supports requests from the kernel debugger.
//
BOOL OEMKDIoctl(DWORD dwCode, LPVOID pBuf, DWORD cbSize)
{
  // Fill in IOCTL code here.

  return TRUE;
}

// ---------------------------------------------------------------------------
// OEMIsProcessorFeaturePresent: OPTIONAL
//
// This function provides information about processor features and support.
//
BOOL OEMIsProcessorFeaturePresent(DWORD dwProcessorFeature)
{
  // Fill in processor feature code here.

  return TRUE;
}



#define DBG_FEATURE_QUERY        (FALSE)

//------------------------------------------------------------------------------
//
//  Function:  OALIsProcessorFeaturePresent
//
//  Called to determine the processor's supported feature set.
//
BOOL OALIsProcessorFeaturePresent(DWORD feature)
{
    // We Do Not check CPU ID Code
    // Already Knows Feature of S3C6410

    BOOL bRet = FALSE;

    switch(feature)
    {
    case PF_ARM_V4:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_V4) = %d\r\n", bRet));
        break;
    case PF_ARM_V5:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_V5) = %d\r\n", bRet));
        break;
    case PF_ARM_V6:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_V6) = %d\r\n", bRet));
        break;
    case PF_ARM_THUMB:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_THUMB) = %d\r\n", bRet));
        break;
    case PF_ARM_WRITE_BUFFER:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_WRITE_BUFFER) = %d\r\n", bRet));
        break;
    case PF_ARM_JAZELLE:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_JAZELLE) = %d\r\n", bRet));
        break;
    case PF_ARM_DSP:        //(Deprecated)
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_DSP) = %d\r\n", bRet));
        break;
    case PF_ARM_MOVE_CP:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_MOVE_CP) = %d\r\n", bRet));
        break;
    case PF_ARM_PHYSICALLY_TAGGED_CACHE:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_PHYSICALLY_TAGGED_CACHE) = %d\r\n", bRet));
        break;
    case PF_ARM_VFP_DOUBLE_PRECISION:    // VFPv2 (VFP11)
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_VFP_DOUBLE_PRECISION) = %d\r\n", bRet));
        break;
    case PF_ARM_ITCM:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_ITCM) = %d\r\n", bRet));
        break;
    case PF_ARM_DTCM:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_DTCM) = %d\r\n", bRet));
        break;
    case PF_ARM_WRITE_BACK_CACHE:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_WRITE_BACK_CACHE) = %d\r\n", bRet));
        break;
    case PF_ARM_CACHE_CAN_BE_LOCKED_DOWN:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_CACHE_CAN_BE_LOCKED_DOWN) = %d\r\n", bRet));
        break;
    case PF_ARM_UNALIGNED_ACCESS:
        bRet = TRUE;            // TRUE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_UNALIGNED_ACCESS) = %d\r\n", bRet));
        break;


    case PF_ARM_VFP10:        //(Deprecated)
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_VFP10) = %d\r\n", bRet));
        break;
    case PF_ARM_MPU:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_MPU) = %d\r\n", bRet));
        break;
    case PF_ARM_MBX:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_MBX) = %d\r\n", bRet));
        break;
    case PF_ARM_L2CACHE:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_L2CACHE) = %d\r\n", bRet));
        break;
    case PF_ARM_VFP_SINGLE_PRECISION:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_VFP_SINGLE_PRECISION) = %d\r\n", bRet));
        break;
    case PF_ARM_UNIFIED_CACHE:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_UNIFIED_CACHE) = %d\r\n", bRet));
        break;
    case PF_ARM_L2CACHE_MEMORY_MAPPED:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_L2CACHE_MEMORY_MAPPED) = %d\r\n", bRet));
        break;
    case PF_ARM_L2CACHE_COPROC:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_L2CACHE_COPROC) = %d\r\n", bRet));
        break;
    case PF_ARM_THUMB2:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_THUMB2) = %d\r\n", bRet));
        break;
    case PF_ARM_T2EE:        // (Thumb-2 Execution Environment)
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_T2EE) = %d\r\n", bRet));
        break;
    case PF_ARM_VFP3:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_VFP3) = %d\r\n", bRet));
        break;
    case PF_ARM_V7:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_V7) = %d\r\n", bRet));
        break;
    case PF_ARM_NEON:
        bRet = FALSE;            // FALSE
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(PF_ARM_NEON) = %d\r\n", bRet));
        break;
    case PF_ARM_INTEL_XSCALE:    // Not Supported
    case PF_ARM_INTEL_PMU    :    // Not Supported
    case PF_ARM_INTEL_WMMX:    // Not Supported
        bRet = FALSE;
        break;
    default:
        bRet = FALSE;
        OALMSG(DBG_FEATURE_QUERY, (L"OALIsProcessorFeaturePresent(Unknown:0x%08x) = %d\r\n", feature, bRet));
        break;
    }

    return bRet;
}
//======================================================================
//======================================================================
//
// Interrupt related IOCTLs
//
//======================================================================
//======================================================================
//static UINT32 g_oalSysIntr2Irq[SYSINTR_MAXIMUM];
//static UINT32 g_oalIrq2SysIntr[OAL_INTR_IRQ_MAXIMUM];

//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalRequestSysIntr
//
//  This function return existing SysIntr for non-shareable IRQs and create
//  new Irq -> SysIntr mapping for shareable.
//
BOOL OALIoCtlHalRequestSysIntr(UINT32 code, VOID* pInpBuffer, UINT32 inpSize, 
							   VOID* pOutBuffer,UINT32 outSize, UINT32 *pOutSize) 
{
    BOOL rc = FALSE;
	DWORD dwIrq;
	DWORD dwSysIntr = 0xffffffff;
	DWORD *pIn = (DWORD *)pInpBuffer;

	if ((pInpBuffer == 0) || (inpSize < sizeof (DWORD)) || (pOutBuffer == 0) || (outSize < sizeof (DWORD)))
	{
		return FALSE;
	}

	// MS changed this interface a while back so grab the proper irq val this way if needed
	if ((*pIn == 0xffffffff) && (inpSize >= sizeof (DWORD) * 3))
	{
		dwIrq = pIn[2];
	}
	else
	{
		dwIrq = *pIn;
	}


	// right now I'm just hardcoding this
	if (dwIrq == RASPI_PHYSIRQ_UART0)
		dwSysIntr = SYSINTR_UART0;


	if (dwSysIntr != 0xffffffff)
	{
		*(DWORD *)pOutBuffer = dwSysIntr;
		if (pOutSize != 0)
			*pOutSize = sizeof (DWORD);

		rc = TRUE;
	}
	return rc;
}
//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalInitRegistry
//
//  Implements the IOCTL_HAL_INITREGISTRY handler.

BOOL OALIoCtlHalInitRegistry(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, VOID *pOutBuffer, 
                             UINT32 outSize, UINT32 *pOutSize)
{
    // Default implementation doesn't do anything

    // KITL registry initialization is done in kitl.dll's IOCTL_HAL_INITREGISTRY handler
    
    UNREFERENCED_PARAMETER(code);    
    UNREFERENCED_PARAMETER(pInpBuffer);    
    UNREFERENCED_PARAMETER(inpSize);    
    UNREFERENCED_PARAMETER(pOutBuffer);    
    UNREFERENCED_PARAMETER(outSize);    
    UNREFERENCED_PARAMETER(pOutSize);    

    return TRUE;
}

BOOL OALIoCtlHalGetHardwarePtrs(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, 
								 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);
//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalGetHardwarePtrs
//
//  Implements the IOCTL_HAL_MY_GET_HARDWARE_POINTERS handler.  This is called by
//  drivers to query various hardware pointers.

BOOL OALIoCtlHalGetHardwarePtrs(UINT32 code, VOID *pInpBuffer, UINT32 inpSize, 
								 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    BOOL fRC = FALSE;
	MYDISPLAYSETTTNGS DSData;
	PMYHARDWAREPTRSTRUCT pOut = (PMYHARDWAREPTRSTRUCT)pOutBuffer;

	//NKDbgPrintfW(L"OALIoCtlHalGetHardwarePtrs++\r\n");

    UNREFERENCED_PARAMETER(code);    
    UNREFERENCED_PARAMETER(pInpBuffer);    
    UNREFERENCED_PARAMETER(inpSize);    

	if (pOutBuffer && (outSize == sizeof (MYHARDWAREPTRSTRUCT)))
	{
		__try 
		{
			// This routine is located in video.c
			fRC = GetVideoSettings (&DSData);
			if (fRC)
			{
				pOut->dwPhysSOCRegs = 0;
				pOut->pSOCRegs = g_p2835Regs;
				pOut->dwPhysFrameBuffer  = DSData.dwPhysFrameBuffer;
				pOut->pFrameBuffer = DSData.pFrameBuffer; // Frame buffer physical address

				*pOutSize = sizeof (MYHARDWAREPTRSTRUCT);
			}
			else
				*pOutSize = 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
//			SetLastError(ERROR_INVALID_PARAMETER);
		}
	}
	//NKDbgPrintfW(L"OALIoCtlHalGetHardwarePtrs-- rc %d\r\n", fRC);
    return fRC;
}

//------------------------------------------------------------------------------
//
//  Function:  OALMailboxPropertyTag
//
//  Sends one VideoCore property tag over the property mailbox channel and
//  waits for the answer. pdwValues holds nWords value words: they are sent as
//  the request and, on success, overwritten with the response. Returns TRUE
//  only if the VideoCore answered OK for both the buffer and the tag.
//
//  Used by OALIoCtlHalGetEmmcClock (below) and by OEMInit (init.c, ARM memory
//  size). OEMInit calls it before the IOCTL critical section exists, which is
//  fine: nothing else runs at that point. CACHE_SYNC_* and OEMCacheRangeFlush
//  come from the already-included nkintr.h / oemglobal.h.
//
#define RASPI_PROPERTY_RESPONSE_OK     0x80000000
#define RASPI_PROPERTY_MAX_WORDS       8

BOOL OALMailboxPropertyTag(DWORD dwTag, DWORD *pdwValues, DWORD nWords)
{
	volatile DWORD *pBuff = 0;
	DWORD dwBuffSize = 0;
	DWORD dwResp = 0;
	DWORD cbTotal;
	DWORD i;
	int rc;

	if ((pdwValues == 0) || (nWords == 0) || (nWords > RASPI_PROPERTY_MAX_WORDS))
		return FALSE;

	// Buffer header (2 words) + tag header (3 words) + values + end tag.
	cbTotal = (6 + nWords) * sizeof(DWORD);

	// Shared property-mailbox buffer. TRUE returns its virtual address, which is
	// numerically the VideoCore L2 bus alias (0x8000xxxx) of the physical page,
	// so the same value serves both the CPU write and the address handed to the
	// GPU (>>4) - the pattern the framebuffer mailbox path already relies on.
	if (MailboxGetMailboxBuffer(TRUE, (DWORD *)&pBuff, &dwBuffSize) != 0)
		return FALSE;
	if ((pBuff == 0) || (dwBuffSize < cbTotal))
		return FALSE;

	// Build the property request:
	//   [0] total buffer size        [3] value buffer size (bytes)
	//   [1] request code (0)         [4] tag request/response code (0)
	//   [2] tag id                   [5..] values, then the end tag (0)
	pBuff[0] = cbTotal;
	pBuff[1] = 0x00000000;
	pBuff[2] = dwTag;
	pBuff[3] = nWords * sizeof(DWORD);
	pBuff[4] = 0x00000000;
	for (i = 0; i < nWords; i++)
		pBuff[5 + i] = pdwValues[i];
	pBuff[5 + nWords] = 0x00000000;

	// Clean the buffer so the VideoCore reads our request from RAM. The lines
	// are then clean, so the invalidate after the call will not write stale
	// request data back over the GPU's response.
	OEMCacheRangeFlush((VOID *)pBuff, cbTotal, CACHE_SYNC_WRITEBACK);

	rc = MailboxSendMail(0, MAILBOX_CHAN_PROPARM2VC, ((DWORD)pBuff) >> 4);
	if (rc == 0)
		rc = MailboxGetMail(0, MAILBOX_CHAN_PROPARM2VC, &dwResp);
	if (rc != 0)
		return FALSE;

	// Drop the (clean) cache lines so the reads below come from RAM = the
	// VideoCore's response, not our cached copy of the request.
	OEMCacheRangeFlush((VOID *)pBuff, cbTotal, CACHE_SYNC_DISCARD);

	// Both the overall response code and the tag's response bit must say OK.
	if ((pBuff[1] != RASPI_PROPERTY_RESPONSE_OK) ||
		((pBuff[4] & RASPI_PROPERTY_RESPONSE_OK) == 0))
		return FALSE;

	for (i = 0; i < nWords; i++)
		pdwValues[i] = pBuff[5 + i];

	return TRUE;
}

//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalMailboxProperty
//
//  Implements IOCTL_HAL_MY_MAILBOX_PROPERTY (see image_cfg.h): sends the
//  caller's tag list as one property message through the shared mailbox
//  buffer, with the same cache handling as OALMailboxPropertyTag, and copies
//  the answered tags back. Runs under the OAL IOCTL critical section, so it
//  does not race the other users of the shared buffer.
//
//  Every tag must be a frame buffer tag (0x0004xxxx: get, test and set), one
//  of the two cursor tags or "get DMA channels"; the list must end exactly
//  after its last tag.
//
#define RASPI_PROPTAG_SET_CURSOR_INFO   0x00008010
#define RASPI_PROPTAG_SET_CURSOR_STATE  0x00008011
#define RASPI_PROPTAG_GET_DMA_CHANNELS  0x00060001

static BOOL IsAllowedDisplayTag(DWORD dwTag)
{
	return ((dwTag & 0xFFFF0000) == 0x00040000) ||
		   (dwTag == RASPI_PROPTAG_SET_CURSOR_INFO) ||
		   (dwTag == RASPI_PROPTAG_SET_CURSOR_STATE) ||
		   (dwTag == RASPI_PROPTAG_GET_DMA_CHANNELS);
}

BOOL OALIoCtlHalMailboxProperty(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
								VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
	const DWORD *pTags = (const DWORD *)pInpBuffer;
	volatile DWORD *pBuff = 0;
	DWORD dwBuffSize = 0;
	DWORD dwResp = 0;
	DWORD nWords, nValueWords, iWord, i, cbTotal;
	int rc;

	UNREFERENCED_PARAMETER(code);

	if (pOutSize != 0)
		*pOutSize = 0;

	if ((pInpBuffer == 0) || (pOutBuffer == 0) || (inpSize < 3 * sizeof(DWORD)) ||
		(inpSize > RASPI_MAILBOX_MAX_TAG_BYTES) || ((inpSize & 3) != 0) ||
		(outSize < inpSize))
	{
		NKSetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}

	// Walk the tags: allowed ids, whole value buffers, nothing left over
	nWords = inpSize / sizeof(DWORD);
	iWord = 0;
	while (iWord < nWords)
	{
		if ((iWord + 3 > nWords) || !IsAllowedDisplayTag(pTags[iWord]) ||
			((pTags[iWord + 1] & 3) != 0))
		{
			NKSetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		nValueWords = pTags[iWord + 1] / sizeof(DWORD);
		if (nValueWords > nWords - iWord - 3)
		{
			NKSetLastError(ERROR_INVALID_PARAMETER);
			return FALSE;
		}
		iWord += 3 + nValueWords;
	}

	// Message header (2 words) + tags + end tag
	cbTotal = 2 * sizeof(DWORD) + inpSize + sizeof(DWORD);
	if (MailboxGetMailboxBuffer(TRUE, (DWORD *)&pBuff, &dwBuffSize) != 0)
		return FALSE;
	if ((pBuff == 0) || (dwBuffSize < cbTotal))
		return FALSE;

	pBuff[0] = cbTotal;
	pBuff[1] = 0x00000000;
	for (i = 0; i < nWords; i++)
		pBuff[2 + i] = pTags[i];
	pBuff[2 + nWords] = 0x00000000;

	OEMCacheRangeFlush((VOID *)pBuff, cbTotal, CACHE_SYNC_WRITEBACK);

	rc = MailboxSendMail(0, MAILBOX_CHAN_PROPARM2VC, ((DWORD)pBuff) >> 4);
	if (rc == 0)
		rc = MailboxGetMail(0, MAILBOX_CHAN_PROPARM2VC, &dwResp);
	if (rc != 0)
		return FALSE;

	OEMCacheRangeFlush((VOID *)pBuff, cbTotal, CACHE_SYNC_DISCARD);

	if (pBuff[1] != RASPI_PROPERTY_RESPONSE_OK)
		return FALSE;

	for (i = 0; i < nWords; i++)
		((DWORD *)pOutBuffer)[i] = pBuff[2 + i];
	if (pOutSize != 0)
		*pOutSize = inpSize;

	return TRUE;
}

//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalGetEmmcClock
//
//  Implements IOCTL_HAL_MY_GET_EMMC_CLOCK. Returns the VideoCore EMMC base
//  clock in Hz (a single DWORD in the output buffer). The SD host driver needs
//  this because the BCM2835 Arasan capabilities register reads 0, so the base
//  clock cannot be read from the controller. We ask the VideoCore over the
//  property mailbox ("get clock rate", clock id = EMMC).
//
#define RASPI_PROPTAG_GET_CLOCK_RATE   0x00030002
#define RASPI_CLOCK_ID_EMMC            0x00000001

BOOL OALIoCtlHalGetEmmcClock(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
							 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
	DWORD adwValues[2];
	DWORD dwRate;

	UNREFERENCED_PARAMETER(code);
	UNREFERENCED_PARAMETER(pInpBuffer);
	UNREFERENCED_PARAMETER(inpSize);

	if ((pOutBuffer == 0) || (outSize < sizeof(DWORD)))
		return FALSE;

	// value[0] = clock id (EMMC), value[1] = rate in Hz (filled by VideoCore)
	adwValues[0] = RASPI_CLOCK_ID_EMMC;
	adwValues[1] = 0;
	if (!OALMailboxPropertyTag(RASPI_PROPTAG_GET_CLOCK_RATE, adwValues, 2))
		return FALSE;

	dwRate = adwValues[1];

	*(DWORD *)pOutBuffer = dwRate;
	if (pOutSize != 0)
		*pOutSize = sizeof(DWORD);

	return (dwRate != 0);
}

//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlProcessorInfo
//
//  Implements IOCTL_PROCESSOR_INFORMATION, which the System control panel uses
//  for its "Processor" line ("Vendor, Core-Name"). Same behaviour as the common
//  platform\common\src\common\ioctl\procinfo.c, which this OAL does not link
//  (no oal_ioctl.lib), with the strings from image_cfg.h. The clock speed (MHz)
//  is the current ARM clock from the VideoCore (700 MHz on a Pi 1, 1000 MHz on
//  a Zero), or BSP_SPEED_CPUMHZ if the query fails.
//
#define RASPI_CLOCK_ID_ARM             0x00000003

BOOL OALIoCtlProcessorInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
						   VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
	PROCESSOR_INFO *pInfo = (PROCESSOR_INFO *)pOutBuffer;
	DWORD adwValues[2];
	UINT32 cbCore, cbName, cbVendor;

	if (pOutSize != 0)
		*pOutSize = sizeof(PROCESSOR_INFO);

	if ((pOutBuffer == 0) || (outSize < sizeof(PROCESSOR_INFO)))
	{
		NKSetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}

	cbCore   = (NKwcslen(IOCTL_PROCESSOR_CORE) + 1) * sizeof(WCHAR);
	cbName   = (NKwcslen(IOCTL_PROCESSOR_NAME) + 1) * sizeof(WCHAR);
	cbVendor = (NKwcslen(IOCTL_PROCESSOR_VENDOR) + 1) * sizeof(WCHAR);
	if ((cbCore > sizeof(pInfo->szProcessCore)) ||
		(cbName > sizeof(pInfo->szProcessorName)) ||
		(cbVendor > sizeof(pInfo->szVendor)))
		return FALSE;

	memset(pInfo, 0, sizeof(PROCESSOR_INFO));
	pInfo->wVersion = 1;
	memcpy(pInfo->szProcessCore, IOCTL_PROCESSOR_CORE, cbCore);
	memcpy(pInfo->szProcessorName, IOCTL_PROCESSOR_NAME, cbName);
	memcpy(pInfo->szVendor, IOCTL_PROCESSOR_VENDOR, cbVendor);
	pInfo->dwInstructionSet = IOCTL_PROCESSOR_INSTRUCTION_SET;

	// value[0] = clock id (ARM), value[1] = rate in Hz (filled by VideoCore)
	adwValues[0] = RASPI_CLOCK_ID_ARM;
	adwValues[1] = 0;
	if (OALMailboxPropertyTag(RASPI_PROPTAG_GET_CLOCK_RATE, adwValues, 2) &&
		(adwValues[1] != 0))
		pInfo->dwClockSpeed = adwValues[1] / 1000000;
	else
		pInfo->dwClockSpeed = BSP_SPEED_CPUMHZ;

	return TRUE;
}

//  Function:  OALIoCtlHalGetMacAddress
//
//  Implements IOCTL_HAL_MY_GET_MAC_ADDRESS. Returns the board MAC address the
//  VideoCore firmware assigns (6 bytes, in transmission order) through the
//  property mailbox ("get board MAC address" tag). The Pi's LAN951x has no
//  EEPROM, so the USB Ethernet driver must program this address itself. Same
//  mailbox buffer and cache handling as OALIoCtlHalGetEmmcClock.
//
#define RASPI_PROPTAG_GET_BOARD_MAC    0x00010003
#define RASPI_MAC_ADDRESS_LEN          6

BOOL OALIoCtlHalGetMacAddress(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
							  VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
	volatile DWORD *pBuff = 0;
	volatile BYTE *pMac;
	DWORD dwBuffSize = 0;
	DWORD dwResp = 0;
	BYTE *pOut;
	DWORD dwAll;
	int rc, i;

	UNREFERENCED_PARAMETER(code);
	UNREFERENCED_PARAMETER(pInpBuffer);
	UNREFERENCED_PARAMETER(inpSize);
  
	if ((pOutBuffer == 0) || (outSize < RASPI_MAC_ADDRESS_LEN))
		return FALSE;

	if (MailboxGetMailboxBuffer(TRUE, (DWORD *)&pBuff, &dwBuffSize) != 0)
		return FALSE;
	if ((pBuff == 0) || (dwBuffSize < 8 * sizeof(DWORD)))
		return FALSE;

	// Build the property request: get board MAC address.
	//   [0] total buffer size        [4] tag request/response code (0)
	//   [1] request code (0)         [5] value bytes 0-3 (MAC bytes 0-3)
	//   [2] tag id                   [6] value bytes 4-7 (MAC bytes 4-5)
	//   [3] value buffer size (8)    [7] end tag (0)
	// The response is 6 bytes; the value buffer is rounded up to 8.
	pBuff[0] = 8 * sizeof(DWORD);
	pBuff[1] = 0x00000000;
	pBuff[2] = RASPI_PROPTAG_GET_BOARD_MAC;
	pBuff[3] = 2 * sizeof(DWORD);
	pBuff[4] = 0x00000000;
	pBuff[5] = 0x00000000;
	pBuff[6] = 0x00000000;
	pBuff[7] = 0x00000000;

	OEMCacheRangeFlush((VOID *)pBuff, 8 * sizeof(DWORD), CACHE_SYNC_WRITEBACK);

	rc = MailboxSendMail(0, MAILBOX_CHAN_PROPARM2VC, ((DWORD)pBuff) >> 4);
	if (rc == 0)
		rc = MailboxGetMail(0, MAILBOX_CHAN_PROPARM2VC, &dwResp);
	if (rc != 0)
		return FALSE;

	OEMCacheRangeFlush((VOID *)pBuff, 8 * sizeof(DWORD), CACHE_SYNC_DISCARD);

	if ((pBuff[1] != RASPI_PROPERTY_RESPONSE_OK) ||
		((pBuff[4] & RASPI_PROPERTY_RESPONSE_OK) == 0))
		return FALSE;

	// The value bytes are the MAC in transmission order.
	pMac = (volatile BYTE *)&pBuff[5];
	pOut = (BYTE *)pOutBuffer;
	dwAll = 0;
	for (i = 0; i < RASPI_MAC_ADDRESS_LEN; i++)
	{
		pOut[i] = pMac[i];
		dwAll |= pMac[i];
	}
	if (pOutSize != 0)
		*pOutSize = RASPI_MAC_ADDRESS_LEN;

	return (dwAll != 0);
}

//------------------------------------------------------------------------------
//
//  Function:  OALIoCtlHalGetHiveCleanFlag
//
//  Implements IOCTL_HAL_GET_HIVE_CLEAN_FLAG. filesys.dll asks this at boot,
//  with HIVECLEANFLAG_SYSTEM (1) for the system hive and HIVECLEANFLAG_USERS
//  (2) for the user hives, when the hives live on a persistent volume (ROM-only
//  file system with the SD card as the bootable volume, docs\sdroot.md). TRUE
//  would make it delete them and start again from the image defaults.
//
//  This board has no "clean registry" switch, so the answer is always "keep":
//  *pOutBuffer = FALSE and the call returns FALSE (the i.MX31 BSP does the
//  same). The kernel's "clean memory configuration" at every power-on is
//  about RAM and must not wipe the card. To reset the registry, delete
//  \Windows\Registry on the card from a PC.
//
#ifndef HIVECLEANFLAG_SYSTEM
#define HIVECLEANFLAG_SYSTEM  1     // pwinreg.h
#endif
#ifndef HIVECLEANFLAG_USERS
#define HIVECLEANFLAG_USERS   2
#endif

BOOL OALIoCtlHalGetHiveCleanFlag(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
								 VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
	DWORD dwFlag = 0;

	UNREFERENCED_PARAMETER(code);

	if ((pInpBuffer != NULL) && (inpSize >= sizeof(DWORD)))
		dwFlag = *(DWORD *)pInpBuffer;

	if ((pOutBuffer != NULL) && (outSize >= sizeof(BOOL)))
	{
		*(BOOL *)pOutBuffer = FALSE;
		if (pOutSize != NULL)
			*pOutSize = sizeof(BOOL);
	}
	else if (pOutSize != NULL)
	{
		*pOutSize = 0;
	}

	OALMSG(1, (L"[OAL] Hive clean flag %u (%s): keep hives\r\n", dwFlag,
		(dwFlag == HIVECLEANFLAG_SYSTEM) ? L"system" :
		(dwFlag == HIVECLEANFLAG_USERS) ? L"users" : L"unknown"));

	return FALSE;
}

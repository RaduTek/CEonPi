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
//
//------------------------------------------------------------------------------
//
//  File:  image_cfg.h
//
//  Defines configuration parameters used to create the NK and Bootloader
//  program images.
//
#ifndef __IMAGE_CFG_H
#define __IMAGE_CFG_H

#if __cplusplus
extern "C" {
#endif

#define BSP_SPEED_CPUMHZ                    700

//------------------------------------------------------------------------------
//
//  Define:  IOCTL_PLATFORM_TYPE/OEM
//
//  Defines the platform type and OEM string.
//

#define IOCTL_PLATFORM_TYPE                 (L"Rev A-B")
#define IOCTL_PLATFORM_OEM                  (L"BolingConsultingInc")
#define IOCTL_PLATFORM_MANUFACTURER         (L"RaspberryPiOrg")
#define IOCTL_PLATFORM_NAME                 (L"RaspberryPi")


//------------------------------------------------------------------------------
//  Define:  IOCTL_PROCESSOR_VENDOR/NAME/CORE
//
//  Defines the processor information returned by IOCTL_PROCESSOR_INFORMATION
//  (ioctl.c OALIoCtlProcessorInfo). The System control panel shows it as
//  "Vendor, Core-Name", e.g. "Broadcom, ARM1176JZF-S-BCM2835". Core and name
//  must fit in 40 WCHARs (including the terminator), vendor in 100.
//

#define IOCTL_PROCESSOR_VENDOR              (L"Broadcom")
#define IOCTL_PROCESSOR_NAME                (L"BCM2835")
#define IOCTL_PROCESSOR_CORE                (L"ARM1176JZF-S")
#define IOCTL_PROCESSOR_INSTRUCTION_SET     (0)

#define VID_WIDTH                           1920
#define VID_HEIGHT                          1080

//#define VID_WIDTH                           640
//#define VID_HEIGHT                          480

//#define VID_WIDTH                           1280
//#define VID_HEIGHT                          1024

	
#define VID_BITSPP                          16
//#define VID_BITSPP                          24

//------------------------------------------------------------------------------
//
// 512 Meg map of classic Virtmem map for CE/Compact. I'm not using the 
// new Device table feature of Compact to make it simple to port the
// BSP back to CE 6.
//
//   Phys         Virt           Size
//	0000 0000   8000 0000     1000 0000			RAM
//  1000 0000   9000 0000     0E00 0000         Unused, reserved
//  2000 0000   9E00 0000     0200 0000         BCM2835 Reg Map
//	2000 0000   A000 0000     xxxx xxxx			END
//
//

// Mapped to this address by BCM GPU loader
#define  DRAM_BASE_PA_START             0x00000000

#define  DRAM_SIZE                      0x10000000  // 256K

// Mapped to this address by BCM GPU loader
#define  PERIPHERAL_PA_BASE             0x20000000
#define  PERIPHERAL_PA_SIZE             (32*1024*1024)

// We use this to tell the UART ISR routine where our buffers are.
#define IOCTL_HAL_SET_KITL_SERIAL_BUFFERS   CTL_CODE(FILE_DEVICE_HAL, 2048+10, METHOD_BUFFERED, FILE_ANY_ACCESS)

// I use this to pass display configuration info to the display driver
#define IOCTL_HAL_MY_GET_HARDWARE_POINTERS   CTL_CODE(FILE_DEVICE_HAL, 2048+11, METHOD_BUFFERED, FILE_ANY_ACCESS)

// The SD host driver uses this to read the VideoCore EMMC base clock (in Hz),
// because the BCM2835 Arasan capabilities register reads 0. Output buffer is a
// single DWORD (the clock rate). Handler: OALIoCtlHalGetEmmcClock in ioctl.c.
#define IOCTL_HAL_MY_GET_EMMC_CLOCK          CTL_CODE(FILE_DEVICE_HAL, 2048+12, METHOD_BUFFERED, FILE_ANY_ACCESS)

// The USB Ethernet driver (smsc95xx) uses this to read the board MAC address
// the VideoCore firmware assigns (B8:27:EB plus the low serial-number bytes on
// a Pi 1, the same address Linux uses); the LAN951x on the Pi has no EEPROM.
// Output buffer is 6 bytes, in transmission order. Handler:
// OALIoCtlHalGetMacAddress in ioctl.c.
#define IOCTL_HAL_MY_GET_MAC_ADDRESS         CTL_CODE(FILE_DEVICE_HAL, 2048+13, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Sends a list of VideoCore property tags in one mailbox message (the display
// driver uses it for the frame buffer and the hardware cursor). Input: the
// tags back to back, each [tag id][value buffer size in bytes][0][values...],
// without the message header or end tag. Output (may be the same buffer, at
// least as large): the same tags as the VideoCore answered them. Only frame
// buffer (0x0004xxxx) and cursor (0x00008010, 0x00008011) tags are accepted,
// at most RASPI_MAILBOX_MAX_TAG_BYTES. The IOCTL fails if the message as a
// whole is rejected; the caller checks each tag's response bit (0x80000000 in
// the third word) and values. Handler: OALIoCtlHalMailboxProperty in ioctl.c.
#define IOCTL_HAL_MY_MAILBOX_PROPERTY        CTL_CODE(FILE_DEVICE_HAL, 2048+14, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RASPI_MAILBOX_MAX_TAG_BYTES          1024

// Structure used by the interrupt code to manage the serial FIFO buffers
typedef struct {
	PBYTE pRecvBuff;
	DWORD dwRecvLen;
	volatile DWORD dwRecvHead;
	volatile DWORD dwRecvTail;
	PBYTE pTransBuff;
	DWORD dwTransLen;
	volatile DWORD dwTransHead;
	volatile DWORD dwTransTail;
} KITLSERIALBUFFSTRUCT, *PKITLSERIALBUFFSTRUCT;

// Structure used to pass the display information up to the video driver.
typedef struct {
	int nWidth;
	int nHeight;
	int nBitsPerPixel;
	DWORD dwColorMaskRed;
	DWORD dwColorMaskGreen;
	DWORD dwColorMaskBlue;
	DWORD dwColorMaskAlpha;
	DWORD dwStride;
	DWORD dwPhysFrameBuffer;
	PBYTE pFrameBuffer;
	DWORD dwFBSize;
} MYDISPLAYSETTTNGS, *PMYDISPLAYSETTTNGS;

// Structure that is used to return the low level memory mapping with the IOCTL
// IOCTL_HAL_MY_GET_HARDWARE_POINTERS. This avoids each driver from having to
// do do the phys to vert mapping individually.
typedef struct {
	DWORD dwPhysSOCRegs;
	PBYTE pSOCRegs;
	DWORD dwPhysFrameBuffer;
	PBYTE pFrameBuffer;
} MYHARDWAREPTRSTRUCT, *PMYHARDWAREPTRSTRUCT;


// Scheduler timer constants
#define RESCHED_PERIOD          1               // Reschedule ms
#define OEM_COUNT_1MS           1000


typedef void (OEMWRITEDEBUGBYTPROC)(BYTE);
	
//------------------------------------------------------------------------------

#if __cplusplus
}
#endif

#endif


@REM
@REM Copyright (c) Microsoft Corporation.  All rights reserved.
@REM
@REM
@REM Use of this sample source code is subject to the terms of the Microsoft
@REM license agreement under which you licensed this sample source code. If
@REM you did not accept the terms of the license agreement, you are not
@REM authorized to use this sample source code. For the terms of the license,
@REM please see the license agreement between you and Microsoft or, if applicable,
@REM see the LICENSE.RTF on your install media or the root of your tools installation.
@REM THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR INDEMNITIES.
@REM

@ECHO OFF

@REM This file sets BSP-specific environment variables that are active during
@REM build.  It defines the default state.  The environment variable below,
@REM for example, will affect platform.reg.

@REM This variable excludes the sample registry entry in platform.reg from
@REM being included in the initial device registry.

rem
rem We don't support a touch screen.
rem
set BSP_NOTOUCH=1

rem
rem IMGFLASH=1 configures the kernel to be read off the SD card.
rem Remove this env var if you want to download the kernel via KITL
rem
set IMGFLASH=1

rem
rem The drivers below are opt-in. Each catalog item sets the same variable
rem (see "Catalog items" in README.md):
rem BSP_USBHOST=1  USB host controller driver (src\drivers\dwchci).
rem BSP_ETHERNET=1 SMSC LAN95xx USB Ethernet (src\drivers\smsc95xx), needs
rem                BSP_USBHOST=1.
rem BSP_SDHOST=1   SD card host controller driver (src\drivers\sdhcbcm).
rem BSP_NOUSB=1 (a standard variable, also read by common.bib/common.reg)
rem leaves out the whole USB stack, the BSP drivers included.
rem
rem set BSP_USBHOST=1
rem set BSP_ETHERNET=1
rem set BSP_SDHOST=1

rem
rem BSP_IDLEMON=1 adds the CPU idle monitor demo (src\apps\idlemon) to the
rem image, with a desktop shortcut. The catalog item "CPU Idle Monitor (demo)"
rem sets the same variable.
rem
rem set BSP_IDLEMON=1

rem
rem BSP_POWEROFF=1 adds the Shut Down dialog (src\apps\poweroff: Restart /
rem Power off / Cancel) to the image, with a desktop shortcut. The catalog
rem item "Shut Down dialog (poweroff.exe)" sets the same variable.
rem
rem set BSP_POWEROFF=1

rem
rem BSP_SDROOT=1 mounts the SD card (the FAT boot partition) as the root file
rem system "\" instead of "\Storage Card", loading the SD drivers in boot
rem phase 1. The card's Windows folder is merged into \Windows over the ROM
rem image. Needs the SD driver (BSP_SDHOST=1). The catalog item "SD card as
rem root file system" sets the same variable. See docs\sdroot.md.
rem
rem set BSP_SDROOT=1

rem
rem BSP_SDOFFLOAD=1 leaves the modules listed as "; SDOFFLOAD" in
rem files\platform.bib out of kernel.img and copies them to sdcard\Windows in
rem the release dir, so they load from the SD card. Needs BSP_SDROOT=1. The
rem catalog item "Load optional modules from the SD card" sets the same
rem variable. After toggling it, makeimg stops with a message if ce.bib was
rem not rebuilt; delete ce.bib in the release dir and make the image again.
rem
rem set BSP_SDOFFLOAD=1

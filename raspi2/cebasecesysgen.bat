@REM
@REM CEonPi BSP: platform specific sysgen settings.
@REM
@REM cesysgen.bat calls this file (as %_PLATFORMROOT%\%_TGTPLAT%\cebasecesysgen.bat)
@REM before the public sysgen files in each pass, so a SYSGEN_ variable set
@REM here also pulls in its own dependencies (for example SYSGEN_USB_HID_CLIENTS
@REM pulls in SYSGEN_USB_HID, which pulls in SYSGEN_USB; see public\cebase\oak\misc\winceos.bat).
@REM
@REM Each BSP catalog item (Catalog\raspi2.pbcxml) sets a BSP_ variable; the lines
@REM below add the Core OS components that item needs, the same way the stock
@REM IMX313DS BSP does for BSP_ACC. A component is only ever added, never
@REM removed: clearing a BSP item does not drop what other items still need.
@REM Same flat form as the stock BSPs (runs in every pass, which is harmless).
@REM

@REM "USB host controller (DWC OTG)": USB Host Support, USB HID Keyboard and Mouse
if "%BSP_USBHOST%"=="1" set SYSGEN_USB=1
if "%BSP_USBHOST%"=="1" set SYSGEN_USB_HID_CLIENTS=1

@REM "USB Ethernet (SMSC LAN95xx)": USB Host Support, Wired LAN (NDIS, TCP/IP)
if "%BSP_ETHERNET%"=="1" set SYSGEN_USB=1
if "%BSP_ETHERNET%"=="1" set SYSGEN_ETHERNET=1

@REM "SD card host controller (Arasan eMMC)", "SD card as root file system" and
@REM "Load optional modules from the SD card": SD Bus Driver, SD Memory, SD
@REM Standard Host Controller (sdhcbcm links its SDHCBase.lib), FAT File System,
@REM Partition Driver
if "%BSP_SDHOST%"=="1" set SYSGEN_SDBUS=1
if "%BSP_SDHOST%"=="1" set SYSGEN_SD_MEMORY=1
if "%BSP_SDHOST%"=="1" set SYSGEN_SDHC_STANDARD=1
if "%BSP_SDHOST%"=="1" set SYSGEN_FATFS=1
if "%BSP_SDHOST%"=="1" set SYSGEN_MSPART=1
if "%BSP_SDROOT%"=="1" set SYSGEN_SDBUS=1
if "%BSP_SDROOT%"=="1" set SYSGEN_SD_MEMORY=1
if "%BSP_SDROOT%"=="1" set SYSGEN_SDHC_STANDARD=1
if "%BSP_SDROOT%"=="1" set SYSGEN_FATFS=1
if "%BSP_SDROOT%"=="1" set SYSGEN_MSPART=1
if "%BSP_SDOFFLOAD%"=="1" set SYSGEN_SDBUS=1
if "%BSP_SDOFFLOAD%"=="1" set SYSGEN_SD_MEMORY=1
if "%BSP_SDOFFLOAD%"=="1" set SYSGEN_SDHC_STANDARD=1
if "%BSP_SDOFFLOAD%"=="1" set SYSGEN_FATFS=1
if "%BSP_SDOFFLOAD%"=="1" set SYSGEN_MSPART=1

@REM "Analog audio (Pi 1 3.5 mm jack)" and "Audio test (audiotest.exe)":
@REM Waveform Audio
if "%BSP_AUDIO%"=="1" set SYSGEN_AUDIO=1
if "%BSP_AUDIOTEST%"=="1" set SYSGEN_AUDIO=1

@REM "Volume tray icon (voltray.exe)": Waveform Audio, Common Control (trackbar),
@REM Standard Shell (taskbar notification area)
if "%BSP_VOLTRAY%"=="1" set SYSGEN_AUDIO=1
if "%BSP_VOLTRAY%"=="1" set SYSGEN_COMMCTRL=1
if "%BSP_VOLTRAY%"=="1" set SYSGEN_STANDARDSHELL=1

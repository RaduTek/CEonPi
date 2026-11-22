@echo off
echo Running %0
rem
rem Checks that IMGFLASH is set
rem
if .%IMGFLASH%. == .. goto NoFlash

rem
rem Check that nk not too big
rem
if NOT EXIST nk.nb1 goto sizegood
echo.
echo Error! NK.BIN too big for bib settings.  Change NKLEN in config.bib
echo.
goto errorend

:sizegood

rem
rem Build the SD card folder %_flatreleasedir%\sdcard with mksdcard.js:
rem sdcard\kernel.img is a copy of nk.nb0 (nk.nb0 stays in place), and with
rem BSP_SDROOT=1 the folders and shortcuts that initobj.dat lists for the root
rem file system are added. Copy the contents of sdcard to the SD card root.
rem
pushd %_flatreleasedir%

rem An older build put kernel.img here; remove it so a stale copy is not used.
if NOT EXIST kernel.img goto nooldkern
echo deleting old kernel.img from the release dir (now in sdcard\kernel.img)
del kernel.img
:nooldkern

cscript //nologo mksdcard.js
if ERRORLEVEL 1 goto mksdfail
popd

goto endit
:mksdfail
popd
echo.
echo Error! mksdcard.js failed to build the sdcard folder
echo.
goto errorend
:NoFlash
echo.
echo IMGFLASH not set.  It must be set to create kernel.img
echo.
:errorend
echo Setting error level to indicate error to makeimg
exit /b 1
:endit

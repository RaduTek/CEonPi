@echo off
setlocal enabledelayedexpansion
set "BASE=Y:\CEonPi\RaspiCE\RaspiCE\RelDir"
set "DRV=D:"

echo Helper script for copying WEC7 build to SD card
echo.
echo Select build variant:
choice /c CDR /n /m "[C]hecked, [D]ebug or [R]elease? "
if errorlevel 3 (
    set "CFG=Release"
) else if errorlevel 2 (
    set "CFG=Debug"
) else (
    set "CFG=Checked"
)
set "SRC=%BASE%\raspi2_ARMV6_%CFG%\sdcard"
echo Selected: %CFG%
echo.

if not exist "%SRC%\" (
    echo Source folder not found: %SRC%
    echo Is the Y: share mapped?
    pause
    exit /b 1
)

if not exist %DRV%\ (
    echo Drive %DRV% not found. Is the SD card inserted?
    pause
    exit /b 1
)

rem Safety 1: the drive must be removable media
powershell -NoProfile -Command "if ((Get-WmiObject Win32_LogicalDisk -Filter \"DeviceID='%DRV%'\").DriveType -eq 2) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo %DRV% is not a removable drive. Refusing to wipe it.
    pause
    exit /b 1
)

rem Safety 2: the drive must look like a Pi boot card
set "MISSING="
for %%F in (config.txt bootcode.bin start.elf) do (
    if not exist "%DRV%\%%F" set "MISSING=%%F !MISSING!"
)
if defined MISSING (
    echo.
    echo WARNING: %DRV% does not look like a Pi boot card. Missing: !MISSING!
    echo It may be blank, a different card, or the wrong drive.
    echo.
    echo Contents of %DRV%
    dir /a "%DRV%\"
    echo.
    set "ANS="
    set /p "ANS=Type YES to continue anyway: "
    if /i not "!ANS!"=="YES" (
        echo Cancelled.
        pause
        exit /b 1
    )
)

dir "%SRC%\kernel.img"

echo.
echo About to DELETE EVERYTHING on %DRV% and copy the %CFG% build:
echo   %SRC%
echo.
choice /c YN /n /m "Continue? [Y/N] "
if errorlevel 2 (
    echo Cancelled.
    pause
    exit /b 1
)

echo Wiping %DRV% ...
for /d %%D in (%DRV%\*) do rd /s /q "%%D" 2>nul
del /f /q /a %DRV%\* 2>nul

echo Copying files ...
robocopy "%SRC%" %DRV%\ /E /R:1 /W:1 /NP
rem robocopy exit codes below 8 mean success
if errorlevel 8 (
    echo Copy FAILED. Not ejecting.
    pause
    exit /b 1
)

echo.
echo Copy complete. You can now copy any other files to %DRV%.
echo When you are done, press a key to eject it.
pause

echo Ejecting %DRV% ...
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0eject.ps1" %DRV%
if errorlevel 1 (
    echo %DRV% is still present. Close anything using it, or run this script as administrator, or eject it manually.
) else (
    echo %DRV% ejected, safe to remove.
)
pause

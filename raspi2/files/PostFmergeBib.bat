@echo off
rem
rem makeimg runs this from the flat release directory right after fmerge has
rem written ce.bib and before romimage (only when ce.bib was rebuilt). It
rem removes the modules that are loaded from the SD card instead of the image
rem (BSP_SDOFFLOAD=1, "; SDOFFLOAD" lines in platform.bib). See sdoffload.ps1,
rem which runs from the platform tree (no release-dir copy, so an edit needs no
rem Copy Files to Release Directory). -ExecutionPolicy Bypass: the default
rem policy blocks scripts. "< NUL": no console input for powershell.exe.
rem
echo Running %0
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%_TARGETPLATROOT%\files\sdoffload.ps1" < NUL
if ERRORLEVEL 1 goto failed
goto endit
:failed
echo.
echo Error! sdoffload.ps1 failed, see the messages above
echo.
exit /b 1
:endit

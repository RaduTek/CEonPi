@echo off
rem
rem makeimg runs this from the flat release directory right after fmerge has
rem written ce.bib and before romimage (only when ce.bib was rebuilt). It
rem removes the modules that are loaded from the SD card instead of the image
rem (BSP_SDOFFLOAD=1, "; SDOFFLOAD" lines in platform.bib). See sdoffload.js.
rem
echo Running %0
cscript //nologo "%_FLATRELEASEDIR%\sdoffload.js"
if ERRORLEVEL 1 goto failed
goto endit
:failed
echo.
echo Error! sdoffload.js failed, see the messages above
echo.
exit /b 1
:endit

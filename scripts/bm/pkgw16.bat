@echo off
rem pkgw16.bat -- the fresh-folder Win16 test of the portable package, headless.
rem   pkgw16.bat <package folder name under <share>\dist> [app] [EXE]
rem pkgtest.bat proves the DOS half from an extracted package; this proves the WOW
rem half, which is FOUR cfg\ files the zip did not ship before s73 (wowtry.flag,
rem wowsched.txt, wowcall.txt, wowidle.txt). From the EXTRACTED folder: /install,
rem then w16launch.bat with BIN and OUT pointed at the package, so the launch routes
rem through the package's own host and its log lands in the package's debug\out\.
rem Then /uninstall and the rig's own host back. The report is
rem debug\out\pkgw16.txt (with w16launch's window list); pkgw16_done.txt says done.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set PKG=%SH%\dist\%~1
set APP=%2
if "%APP%"=="" set APP=notepad
set PR=%SH%\debug\out\pkgw16.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
> "%PR%" echo == pkgw16 %~1 %APP% %3  %DATE% %TIME%
if not exist "%PKG%\bin\ntvdmhost.exe" (
  >> "%PR%" echo NO PACKAGE HOST AT %PKG%\bin\ntvdmhost.exe
  goto done
)
>> "%PR%" echo -- package cfg\
dir /b "%PKG%\cfg" >> "%PR%" 2>&1
>> "%PR%" echo -- /install
"%PKG%\bin\ntvdmhost.exe" /install >> "%PR%" 2>&1
>> "%PR%" echo rc=%ERRORLEVEL%
rem w16launch re-asserts the key from BIN, kills any live VDM, launches, lists windows.
rem setlocal: `call` shares this environment, and w16launch sets R, T, EXE of its own.
setlocal
set BIN=%PKG%\bin
set OUT=%PKG%\debug\out
call "%SH%\debug\rig\w16launch.bat" %APP% %3
endlocal
>> "%PR%" echo -- w16launch report (%PKG%\debug\out\result_w16_%APP%.log)
type "%PKG%\debug\out\result_w16_%APP%.log" >> "%PR%" 2>&1
copy /y "%PKG%\debug\out\result_w16_%APP%_host.log" "%SH%\debug\out\pkgw16_host.log" >nul 2>&1
>> "%PR%" echo -- /uninstall
"%PKG%\bin\ntvdmhost.exe" /uninstall >> "%PR%" 2>&1
>> "%PR%" echo rc=%ERRORLEVEL%
>> "%PR%" echo -- the rig's own host back
"%SH%\bin\ntvdmhost.exe" /install >> "%PR%" 2>&1
>> "%PR%" echo rc=%ERRORLEVEL%
reg query "%IFEO%" /v Debugger >> "%PR%" 2>&1
:done
>> "%PR%" echo == done %TIME%
> "%SH%\debug\out\pkgw16_done.txt" echo done

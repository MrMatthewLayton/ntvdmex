@echo off
rem pkgtest.bat -- the fresh-folder install test of the portable package, headless.
rem   pkgtest.bat <package folder name under <share>\dist>
rem What the friend's machine does on the 18th, done here by script: from the
rem EXTRACTED folder run ntvdmhost.exe /install (what install.bat does), launch
rem bm\selftest.com BY ITS OWN NAME (the double-click path: CSRSS names the program,
rem the root is derived from the host's own path), read the package's own out\ log,
rem /uninstall, and finally put the rig's own host back as the machine's VDM.
rem Everything goes to out\pkgtest.txt; pkgtest_done.txt says it finished.
rem cfg\autoexit is planted for the run so the host window closes on guest exit
rem (a human closes it by hand), and removed after.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set PKG=%SH%\dist\%~1
set R=%SH%\debug\out\pkgtest.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
> "%R%" echo == pkgtest %~1  %DATE% %TIME%
if not exist "%PKG%\bin\ntvdmhost.exe" (
  >> "%R%" echo NO PACKAGE HOST AT %PKG%\bin\ntvdmhost.exe
  goto done
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
>> "%R%" echo -- registry before
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
>> "%R%" echo -- /status before
"%PKG%\bin\ntvdmhost.exe" /status >> "%R%" 2>&1
>> "%R%" echo -- /install
"%PKG%\bin\ntvdmhost.exe" /install >> "%R%" 2>&1
>> "%R%" echo rc=%ERRORLEVEL%
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
>> "%R%" echo -- selftest.com by name, from the package's bin\
del /q "%PKG%\debug\out\ntvdmhost.log" >nul 2>&1
del /q "%PKG%\debug\out\startfail.txt" >nul 2>&1
> "%PKG%\cfg\autoexit" echo.
cd /d "%PKG%\bin"
start /wait "" "%PKG%\bin\selftest.com"
del /q "%PKG%\cfg\autoexit" >nul 2>&1
if exist "%PKG%\debug\out\ntvdmhost.log" (
  findstr /C:"STAGE0: root" /C:"STAGE1: program" /C:"STAGE2: exec loop exited" /C:"HOSTFAULT" /C:"PASS" /C:"FAIL" /C:"failure counter" "%PKG%\debug\out\ntvdmhost.log" >> "%R%"
) else (
  >> "%R%" echo NO LOG IN THE PACKAGE'S debug\out\ -- the run did not go through the package host
)
if exist "%PKG%\debug\out\startfail.txt" >> "%R%" echo startfail.txt LEFT BEHIND (unclean exit)
>> "%R%" echo -- /uninstall
"%PKG%\bin\ntvdmhost.exe" /uninstall >> "%R%" 2>&1
>> "%R%" echo rc=%ERRORLEVEL%
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
>> "%R%" echo -- the rig's own host back
"%SH%\bin\ntvdmhost.exe" /install >> "%R%" 2>&1
>> "%R%" echo rc=%ERRORLEVEL%
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
:done
>> "%R%" echo == done %TIME%
> "%SH%\debug\out\pkgtest_done.txt" echo done

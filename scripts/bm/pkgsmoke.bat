@echo off
rem pkgsmoke.bat -- drive the PACKAGE's own install/smoke/uninstall the way a human
rem does, from a FRESH folder, feeding the `pause` in each script. %1 = package dir
rem name under <share>\dist.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set PKG=%SH%\dist\%~1
set R=%SH%\debug\out\pkgsmoke.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set FRESH=%USERPROFILE%\Desktop\ntvdmex-smoketest
> "%R%" echo == pkgsmoke %~1  %DATE% %TIME%
rem -- BASELINE: the rig's own host owns the VDM, and no stale "previous" record.
taskkill /f /im ntvdmhost.exe >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "%SH%\bin\ntvdmhost.exe" /f >nul
reg delete "HKCU\Software\NTVDMEX" /v PreviousDebugger /f >nul 2>&1
rmdir /s /q "%FRESH%" >nul 2>&1
md "%FRESH%" >nul 2>&1
xcopy /e /i /y /q "%PKG%\*" "%FRESH%\" >nul
>> "%R%" echo -- fresh folder: %FRESH%
>> "%R%" echo -- status BEFORE (expect exit 2 = another program owns it)
"%FRESH%\bin\ntvdmhost.exe" /status >> "%R%" 2>&1
>> "%R%" echo status_exit=%ERRORLEVEL%
>> "%R%" echo -- install.bat
echo. | call "%FRESH%\install.bat" >> "%R%" 2>&1
>> "%R%" echo -- status AFTER install (expect exit 0)
"%FRESH%\bin\ntvdmhost.exe" /status >nul 2>&1
>> "%R%" echo status_exit=%ERRORLEVEL%
>> "%R%" echo -- smoke.bat
echo. | call "%FRESH%\smoke.bat" >> "%R%" 2>&1
>> "%R%" echo smoke_exit=%ERRORLEVEL%
>> "%R%" echo -- uninstall.bat
echo. | call "%FRESH%\uninstall.bat" >> "%R%" 2>&1
>> "%R%" echo -- status AFTER uninstall
"%FRESH%\bin\ntvdmhost.exe" /status >> "%R%" 2>&1
>> "%R%" echo status_exit=%ERRORLEVEL%
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
>> "%R%" echo == done %TIME%
> "%SH%\debug\out\pkgsmoke_done.txt" echo done

@echo off
rem w16drive.bat <Folder> <EXE> <script> -- launch a Win16 program and DRIVE it (s89).
rem The script (debug\rig\<script>.txt) is one step per line:
rem     wait N            pause N seconds
rem     list              append the visible top-level windows to the report
rem     shot NAME         screenshot to debug\out\NAME.bmp
rem     <rigshot verb...> anything rigshot takes: tclick "Cap", key 0x12, sbarrow "Cap" v,
rem                       click X Y, close "Cap" -- its own output goes into the report
rem Report: debug\out\w16drive_<script>.txt, host log w16drive_<script>_host.log.
rem It always ends by killing the host, so a test never leaves one behind.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\w16drive_%3.txt
del /q "%OUT%\w16drive_done.txt" >nul 2>&1
> "%R%" echo == w16drive %1 %2 %3  %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
"%RIG%\rigshot.exe" close "ntvdmex" >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%W16%\%1"
start "" "%W16%\%1\%2"
ping -n 16 127.0.0.1 >nul
for /f "usebackq delims=" %%L in ("%RIG%\%3.txt") do call :step %%L
>> "%R%" echo -- host alive at the end?
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\w16drive_%3_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
>> "%R%" echo == done %TIME%
> "%OUT%\w16drive_done.txt" echo done
goto :eof

:step
>> "%R%" echo ^> %*
if /i "%~1"=="wait" ( ping -n %2 127.0.0.1 >nul & goto :eof )
if /i "%~1"=="shot" ( "%RIG%\rigshot.exe" shot "%OUT%\%~2.bmp" >nul 2>&1 & goto :eof )
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
if /i "%~1"=="list" ( "%RIG%\rigshot.exe" list ) else ( "%RIG%\rigshot.exe" %* )
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
goto :eof

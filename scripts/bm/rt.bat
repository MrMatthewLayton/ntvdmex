@echo off
set T=%1
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
start "" "%BM%\controld.exe"
copy /y "%BM%\runwatch.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
del /q C:\ntvdmex\shot*.bmp >nul 2>&1
del /q "%RES%\shot_%T%_*.bmp" >nul 2>&1
if exist "%RES%\%T%\*" goto game
if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\%T%" C:\test\ >nul
echo C:\test\%T%> C:\ntvdmex\target.txt
echo.> C:\ntvdmex\autoexit
cd /d C:\test
start /wait "" C:\test\dosstub.com
goto collect
:game
rmdir /s /q C:\game >nul 2>&1
xcopy /e /i /y "%RES%\%T%" C:\game >nul
copy /y "%BM%\dosstub.com" C:\game\ >nul
echo C:\game\%T%.EXE> C:\ntvdmex\target.txt
echo.> C:\ntvdmex\autoexit
cd /d C:\game
start /wait "" C:\game\dosstub.com
:collect
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\result_%T%.log" >nul 2>&1
for %%f in (C:\ntvdmex\shot*.bmp) do copy /y "%%f" "%RES%\shot_%T%_%%~nxf" >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1

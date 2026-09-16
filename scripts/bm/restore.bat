@echo off
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
taskkill /f /im ntvdmhost.exe >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "%SH%\bin\ntvdmhost.exe" /f >nul
reg delete "HKCU\Software\NTVDMEX" /v PreviousDebugger /f >nul 2>&1
rmdir /s /q "%USERPROFILE%\Desktop\ntvdmex-smoketest" >nul 2>&1
> "%SH%\debug\out\restore.txt" echo restored %TIME%
reg query "%IFEO%" /v Debugger >> "%SH%\debug\out\restore.txt" 2>&1

@echo off
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
> "%RES%\ifeo_check.txt" reg query "%IFEO%" /v Debugger
tasklist /fi "imagename eq ntvdmhost.exe" >> "%RES%\ifeo_check.txt" 2>&1
tasklist /fi "imagename eq ntvdm.exe" >> "%RES%\ifeo_check.txt" 2>&1
echo DONE > "%RES%\ifeo_done.txt"

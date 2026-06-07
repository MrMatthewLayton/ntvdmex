@echo off
rem One-time guest provisioning for the reboot-free dev loop. Run ONCE as admin
rem (the files must already be in C:\ntvdmex -- TFTP-GET them first), then REBOOT.
rem Sets auto-logon so an interactive desktop always comes up, installs the logon
rem agent (vdmtrig.bat), and (re)asserts the IFEO ntvdm.exe -> ntvdmhost redirect.
set N=C:\ntvdmex
if not exist %N% md %N%
echo --- auto-logon (guarantees an interactive session for the VDM host) ---
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v AutoAdminLogon /t REG_SZ /d 1 /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultUserName /t REG_SZ /d ntvdmex /f
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v DefaultPassword /t REG_SZ /d ntvdmex /f
echo --- logon agent (runs vdmtrig.bat in the interactive session) ---
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v vdmtrig /t REG_SZ /d "%N%\vdmtrig.bat" /f
echo --- IFEO: ntvdm.exe -> ntvdmhost.exe (ADR-0007) ---
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f
echo.
echo Installed. Confirm %N%\ntvdmhost.exe, %N%\vdmtrig.bat, %N%\dosstub.com exist,
echo then REBOOT to start the agent:   shutdown -r -t 0 -f

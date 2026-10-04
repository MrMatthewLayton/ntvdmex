@echo off
if exist OUT5.TXT del OUT5.TXT
C:\WINDOWS\system32\command.com /c P_VER.COM ^> OUT5.TXT
echo --- OUT5.TXT, written by the guest shell:
type OUT5.TXT

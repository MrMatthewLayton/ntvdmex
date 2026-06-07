@echo off
rem Host-driven (telnet) trigger+collect for the CLEAN host. Like runwait.bat, but
rem reads ntvdmhost.log instead of the spike's result.txt. Drops run.flag for the
rem interactive-session agent (vdmtrig.bat) to fire dosstub -> IFEO -> ntvdmhost.exe,
rem waits for done.flag, then prints ntvdmhost.log (framed). The driver must have
rem pointed the IFEO Debugger at ntvdmhost.exe first.
set N=C:\ntvdmex
del /q %N%\done.flag >nul 2>&1
del /q %N%\ntvdmhost.log >nul 2>&1
echo go> %N%\run.flag
for /l %%i in (1,1,90) do (
  if exist %N%\done.flag goto got
  ping -n 2 127.0.0.1 >nul
)
echo ===RESULT-BEGIN===
echo CLEAN-GATE: TIMEOUT waiting for the agent (is vdmtrig.bat running in the desktop?)
echo ===RESULT-END===
goto end
:got
echo ===RESULT-BEGIN===
type %N%\ntvdmhost.log
echo ===RESULT-END===
:end

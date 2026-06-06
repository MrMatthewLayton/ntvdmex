@echo off
rem Invoked by the host (scripts/dostest.sh) over a SINGLE telnet session to run one
rem test round and return the result WITHOUT a reboot. This runs in the telnet/service
rem session, so it does NOT launch the 16-bit trigger itself; it drops run.flag for the
rem interactive-session agent (vdmtrig.bat), waits for done.flag, then prints result.txt
rem (framed by markers) so the verdict comes back over telnet.
set N=C:\ntvdmex
del /q %N%\done.flag >nul 2>&1
del /q %N%\result.txt >nul 2>&1
echo go> %N%\run.flag
for /l %%i in (1,1,90) do (
  if exist %N%\done.flag goto got
  ping -n 2 127.0.0.1 >nul
)
echo ===RESULT-BEGIN===
echo MEMTEST-HARNESS: TIMEOUT waiting for the agent.
echo (Is vdmtrig.bat running in the interactive session? Run install-agent.cmd then reboot once.)
echo ===RESULT-END===
goto end
:got
echo ===RESULT-BEGIN===
type %N%\result.txt
echo ===RESULT-END===
:end

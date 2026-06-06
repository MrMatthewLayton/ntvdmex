@echo off
rem NTVDMEX dev-loop agent. Installed by install-agent.cmd to run at logon in the
rem INTERACTIVE session -- the only session where launching a 16-bit program fires
rem the IFEO ntvdm.exe -> vdmhost redirect (a telnet/service session does NOT, see
rem docs/research/ntvdmcontrol-and-v86.md). It loops watching for a host-dropped
rem run.flag; on each trigger it runs the 16-bit stub (XP launches ntvdm -> our
rem vdmhost, which loads C:\ntvdmex\target.txt's program and writes vdmhost.log),
rem then publishes the log as result.txt and raises done.flag. This lets the host
rem drive a whole test round over ONE telnet session with NO reboot.
set N=C:\ntvdmex
echo agent started %date% %time%> %N%\agent.log
:loop
ping -n 2 127.0.0.1 >nul
if not exist %N%\run.flag goto loop
del /q %N%\run.flag >nul 2>&1
if exist %N%\vdmhost.log del /q %N%\vdmhost.log >nul 2>&1
if exist %N%\result.txt del /q %N%\result.txt >nul 2>&1
echo trigger %time%>> %N%\agent.log
start /wait %N%\dosstub.com
ping -n 4 127.0.0.1 >nul
if exist %N%\vdmhost.log (copy /y %N%\vdmhost.log %N%\result.txt >nul) else (echo NO-VDMHOST-LOG> %N%\result.txt)
echo done %time%> %N%\done.flag
goto loop

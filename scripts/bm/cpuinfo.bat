@echo off
rem cpuinfo.bat -- what is this box, and is the OS allowed to change its clock?
rem
rem Three questions, and each one is load-bearing for the CPU-speed work:
rem   1. HOW MANY CORES. The throttle suspends the exec thread and times the result
rem      with QueryPerformanceCounter. On XP, QPC can be backed by the TSC, and the
rem      TSC is PER CORE and not necessarily in step -- so a thread that migrates
rem      between cores can see time go BACKWARDS. Every number the throttle computes
rem      comes from that clock.
rem   2. WHAT THE CPU IS, and specifically whether it is a SpeedStep/EIST part.
rem   3. WHICH POWER SCHEME IS ACTIVE. A throttled guest leaves the CPU idle ~99% of
rem      the time, which is exactly the condition that invites the governor to drop
rem      the clock -- and our calibration constant was measured on a BUSY box. If the
rem      clock moves under us, every speed on the menu means something different.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%RES%\cpuinfo.txt
del /q "%OUT%" >nul 2>&1
del /q "%RES%\cpuinfo_done.txt" >nul 2>&1

echo ==== processors ====> "%OUT%"
echo NUMBER_OF_PROCESSORS=%NUMBER_OF_PROCESSORS%>> "%OUT%"
echo PROCESSOR_IDENTIFIER=%PROCESSOR_IDENTIFIER%>> "%OUT%"
echo PROCESSOR_ARCHITECTURE=%PROCESSOR_ARCHITECTURE%>> "%OUT%"
echo.>> "%OUT%"
echo ==== registry: each core's name and its ~MHz ====>> "%OUT%"
reg query "HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor" /s >> "%OUT%" 2>&1
echo.>> "%OUT%"
echo ==== is intelppm / processor power management loaded? ====>> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Services\intelppm" /v Start >> "%OUT%" 2>&1
reg query "HKLM\SYSTEM\CurrentControlSet\Services\Processor" /v Start >> "%OUT%" 2>&1
echo.>> "%OUT%"
echo ==== active power scheme (throttling policy lives here) ====>> "%OUT%"
reg query "HKCU\Control Panel\PowerCfg" /v CurrentPowerPolicy >> "%OUT%" 2>&1
echo.>> "%OUT%"
echo ==== wmic view, if wmic exists on this box ====>> "%OUT%"
wmic cpu get Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed,CurrentClockSpeed /format:list >> "%OUT%" 2>&1
echo done> "%RES%\cpuinfo_done.txt"
endlocal

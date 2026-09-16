@echo off
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set R=%SH%\debug\out\evt.txt
> "%R%" echo == Dr Watson log (tail) %DATE% %TIME%
if exist "%WINDIR%\drwtsn32.log" (
  >> "%R%" echo FOUND %WINDIR%\drwtsn32.log
  for %%A in ("%WINDIR%\drwtsn32.log") do >> "%R%" echo size=%%~zA date=%%~tA
) else ( >> "%R%" echo no drwtsn32.log in %WINDIR% )
if exist "%ALLUSERSPROFILE%\Documents\DrWatson\drwtsn32.log" (
  >> "%R%" echo FOUND %ALLUSERSPROFILE%\Documents\DrWatson\drwtsn32.log
  for %%A in ("%ALLUSERSPROFILE%\Documents\DrWatson\drwtsn32.log") do >> "%R%" echo size=%%~zA date=%%~tA
) else ( >> "%R%" echo no DrWatson dir log )
if exist "%ALLUSERSPROFILE%\Documents\DrWatson\user.dmp" (
  for %%A in ("%ALLUSERSPROFILE%\Documents\DrWatson\user.dmp") do >> "%R%" echo user.dmp size=%%~zA date=%%~tA
)
>> "%R%" echo.
>> "%R%" echo == Application event log, errors
cscript //nologo "%WINDIR%\system32\eventquery.vbs" /L Application /FI "Type eq Error" /V >> "%R%" 2>&1
> "%SH%\debug\out\evt_done.txt" echo done

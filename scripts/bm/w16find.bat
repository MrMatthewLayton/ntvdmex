@echo off
rem w16find.bat -- Notepad's Search > Find, end to end (s89, #294 / #285).
rem Launches Notepad on an EMPTY document, opens Find with the real keys (Alt, S, F),
rem types "abc" and presses Enter. With nothing to find, Notepad itself answers with
rem its "Cannot find" message box -- which only happens if the dialog's
rem commdlg_FindReplace notification reached Notepad's own window procedure. So the
rem window list after Enter is the verdict: a "Notepad" box = the round trip works.
rem Then Enter (dismiss), Esc (close Find), WM_CLOSE to Notepad.
rem Report: debug\out\w16find.txt (+ .bmp shots, + host log).
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\w16find.txt
del /q "%OUT%\w16find_done.txt" >nul 2>&1
> "%R%" echo == w16find  %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
"%RIG%\rigshot.exe" close "ntvdmex" >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%W16%\notepad"
start "" "%W16%\notepad\NOTEPAD.EXE"
ping -n 16 127.0.0.1 >nul
"%RIG%\rigshot.exe" fg "Notepad - (Untitled)" >> "%R%" 2>&1
rem ⚠ fg ALONE IS NOT ENOUGH: XP refuses SetForegroundWindow to a background
rem   process, and the first run's keys went to an open cmd.exe window (the host
rem   saw no keystroke at all). A click on the title bar takes focus the way a user does.
"%RIG%\rigshot.exe" tclick "Notepad - (Untitled)"
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
ping -n 3 127.0.0.1 >nul
for %%K in (0x12 0x53 0x46) do (
  "%RIG%\rigshot.exe" key %%K >> "%R%" 2>&1
  ping -n 3 127.0.0.1 >nul
)
ping -n 3 127.0.0.1 >nul
>> "%R%" echo -- windows with Find open:
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
"%RIG%\rigshot.exe" shot "%OUT%\w16find_1.bmp" >nul 2>&1
for %%K in (0x41 0x42 0x43 0x0D) do (
  "%RIG%\rigshot.exe" key %%K >> "%R%" 2>&1
  ping -n 2 127.0.0.1 >nul
)
ping -n 4 127.0.0.1 >nul
>> "%R%" echo -- windows after Find Next:
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
"%RIG%\rigshot.exe" shot "%OUT%\w16find_2.bmp" >nul 2>&1
for %%K in (0x0D 0x1B) do (
  "%RIG%\rigshot.exe" key %%K >> "%R%" 2>&1
  ping -n 3 127.0.0.1 >nul
)
>> "%R%" echo -- windows after Enter + Esc:
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
"%RIG%\rigshot.exe" close "Notepad - (Untitled)" >> "%R%" 2>&1
ping -n 14 127.0.0.1 >nul
>> "%R%" echo -- host alive after close?
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
findstr /c:"FindText" /c:"commdlg" /c:"0xc0" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\w16find_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
>> "%R%" echo == done %TIME%
> "%OUT%\w16find_done.txt" echo done

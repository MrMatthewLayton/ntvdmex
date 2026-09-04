@echo off
rem pbmin.bat -- minimise MS Paint and restore it. A minimised window has no
rem pixels, so what comes back is what is IN THE IMAGE, not what was left on the
rem screen DC. That is the only honest test of "does the drawing persist?" --
rem the scrollbar test did not scroll and therefore proved nothing.
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set R=%RES%\bm\rigshot.exe
del /q "%RES%\pbmin_done.txt" >nul 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1
"%R%" fg "Paintbrush - (Untitled)"
ping -n 3 127.0.0.1 >nul
rem -- its taskbar button: active window + click = minimise
"%R%" click 504 1034
ping -n 4 127.0.0.1 >nul
"%R%" shot "%RES%\pbmin.bmp"
"%R%" click 504 1034
ping -n 5 127.0.0.1 >nul
"%R%" fg "Paintbrush - (Untitled)"
ping -n 4 127.0.0.1 >nul
"%R%" shot "%RES%\pbrestored.bmp"
copy /y "%RES%\rigshot.txt" "%RES%\pbmin.txt" >nul 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\pbmin_host.txt" >nul 2>&1
echo done> "%RES%\pbmin_done.txt"

@echo off
rem pbtools.bat -- exercise the drawing tools that had no GDI behind them.
rem   box outline -> flood fill inside it -> ellipse -> brush stroke.
rem One batch, because a `controld exec` steals the foreground and separate
rem execs would send the clicks to the console instead of to Paintbrush.
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set R=%RES%\bm\rigshot.exe
del /q "%RES%\rigshot.txt" >nul 2>&1
del /q "%RES%\pbtools_done.txt" >nul 2>&1
"%R%" fg "Paintbrush - (Untitled)"
ping -n 2 127.0.0.1 >nul
rem -- foreground colour = RED (palette top row)
"%R%" click 570 848
ping -n 2 127.0.0.1 >nul
rem -- the BOX tool (outline), toolbox row 6 col 1
"%R%" click 168 541
ping -n 2 127.0.0.1 >nul
"%R%" drag 500 350 800 550
ping -n 3 127.0.0.1 >nul
rem -- foreground colour = BLUE, then the FILL tool (paint roller, row 4 col 1)
"%R%" click 842 848
ping -n 2 127.0.0.1 >nul
"%R%" click 168 426
ping -n 2 127.0.0.1 >nul
rem -- click INSIDE the box: this is ExtFloodFill
"%R%" click 650 450
ping -n 4 127.0.0.1 >nul
rem -- GREEN + the ELLIPSE tool (row 8 col 1)
"%R%" click 706 848
ping -n 2 127.0.0.1 >nul
"%R%" click 168 653
ping -n 2 127.0.0.1 >nul
"%R%" drag 900 350 1150 550
ping -n 3 127.0.0.1 >nul
rem -- MAGENTA + the BRUSH (row 4 col 2), a stroke that must PERSIST
"%R%" click 910 848
ping -n 2 127.0.0.1 >nul
"%R%" click 228 426
ping -n 2 127.0.0.1 >nul
"%R%" drag 400 650 900 750
ping -n 4 127.0.0.1 >nul
"%R%" shot "%RES%\pbtools.bmp"
copy /y "%RES%\rigshot.txt" "%RES%\pbtools.txt" >nul 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\pbtools_host.txt" >nul 2>&1
echo done> "%RES%\pbtools_done.txt"

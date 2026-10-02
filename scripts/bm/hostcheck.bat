@echo off
rem hostcheck.bat -- list running NTVDMEX processes before a harness run kills them (s88).
set OUT=C:\Documents and Settings\All Users\Documents\ntvdmex\debug\out
tasklist /fi "imagename eq ntvdmhost.exe" > "%OUT%\procs_now.txt" 2>&1
tasklist /fi "imagename eq ntvdmex.exe" >> "%OUT%\procs_now.txt" 2>&1

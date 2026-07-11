@echo off
cd /d "%~dp0"
set SHELL=tc32_windows\bin\sh.exe
set PATH=%CD%\tc32_windows\bin;%PATH%
if not exist C:\tmp mkdir C:\tmp
set TMP=C:\tmp
set TEMP=C:\tmp
makeit.exe -j12
exit /b %ERRORLEVEL%

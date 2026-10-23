@echo off
rem Emergency stop for the evo run: no new sessions, running ones retired and opened in the browser.
cd /d "%~dp0"
set PYTHONUTF8=1
".venv\Scripts\python.exe" -m evo stop
pause

@echo off
rem Starts the evo orchestrator app (PLAN §5): progress, ETAs and approval prompts.
cd /d "%~dp0"
set PYTHONUTF8=1
".venv\Scripts\python.exe" -m evo run
pause

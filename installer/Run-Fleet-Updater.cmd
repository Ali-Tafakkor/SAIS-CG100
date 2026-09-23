@echo off
setlocal
cd /d "%~dp0"
"%~dp0runtime\python\python.exe" "%~dp0installer\fleet_agent.py" --watch
pause

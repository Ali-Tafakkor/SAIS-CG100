@echo off
setlocal
cd /d "%~dp0"
if not exist "runtime\python\python.exe" (
  echo Portable Python runtime is missing. Download the complete release ZIP.
  pause
  exit /b 1
)
"runtime\python\python.exe" "installer\main.py"
if errorlevel 1 (
  echo The programmer could not start. Check the package contents and Windows permissions.
  pause
)

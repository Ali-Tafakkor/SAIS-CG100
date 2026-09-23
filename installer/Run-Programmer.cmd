@echo off
setlocal
title SAIS-CG100 Installer
pushd "%~dp0"
echo Starting SAIS-CG100 Installer...
echo Keep this window open. A local browser interface will open shortly.
if not exist "runtime\python\python.exe" goto missing
if not exist "installer\main.py" goto missing
"runtime\python\python.exe" -u "installer\main.py" %*
set "G100_EXIT_CODE=%ERRORLEVEL%"
if not "%G100_EXIT_CODE%"=="0" (
  echo.
  echo The installer could not start. Read the error above and the startup log.
  pause
)
popd
exit /b %G100_EXIT_CODE%
:missing
echo The portable package is incomplete. Extract the complete ZIP before running this file.
pause
popd
exit /b 1

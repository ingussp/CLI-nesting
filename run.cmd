@echo off
setlocal
cd /d "%~dp0"
set "nestingExe=%~dp0clinesting.exe"
if not exist "%nestingExe%" set "nestingExe=%~dp0build-release\Release\clinesting.exe"
if not exist "%nestingExe%" (
  echo Build first: cmake --preset windows-release
  echo Then run: cmake --build --preset windows-release --parallel
  pause
  exit /b 1
)
"%nestingExe%" --input "%~dp0input.json"
set "nestingExit=%errorlevel%"
pause
exit /b %nestingExit%

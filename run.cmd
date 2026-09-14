@echo off
setlocal
cd /d "%~dp0"
set "nestingExe=%~dp0deepnestcpp.exe"
if not exist "%nestingExe%" set "nestingExe=%~dp0build-release\Release\deepnestcpp.exe"
if not exist "%nestingExe%" (
  echo Build the application first with build-release.ps1.
  pause
  exit /b 1
)
"%nestingExe%" --input "%~dp0input.json"
set "nestingExit=%errorlevel%"
pause
exit /b %nestingExit%

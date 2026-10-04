@echo off
setlocal
for /f "delims=" %%P in ('wsl.exe wslpath -u "%~dp0\."') do set "WSLREPO=%%P"
if not defined WSLREPO (
  echo WSL could not translate the project path.
  pause
  exit /b 1
)
wsl.exe bash "%WSLREPO%/watch-tests.sh"
if errorlevel 1 pause

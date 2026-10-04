@echo off
setlocal
rem Translate this launcher path rather than relying on WSL's working directory.
set "REPO=%~dp0"
for /f "delims=" %%P in ('wsl.exe wslpath -u "%REPO%\."') do set "WSLREPO=%%P"
if not defined WSLREPO (
  echo Cannot translate repository path. WSL and Python 3 are required.
  pause
  exit /b 1
)
wsl.exe bash "%WSLREPO%/launch-debug.sh" --server-only
if errorlevel 1 (
  echo Debug server failed. See debug\server.log.
  pause
  exit /b 1
)
set "URL=http://localhost:8310/"
if exist "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" (
  start "" "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" --app="%URL%"
  exit /b
)
if exist "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" (
  start "" "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" --app="%URL%"
  exit /b
)
if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" (
  start "" "%ProgramFiles%\Google\Chrome\Application\chrome.exe" --app="%URL%"
  exit /b
)
if exist "%LocalAppData%\Google\Chrome\Application\chrome.exe" (
  start "" "%LocalAppData%\Google\Chrome\Application\chrome.exe" --app="%URL%"
  exit /b
)
start "" "%URL%"

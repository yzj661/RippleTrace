@echo off
REM ============================================================
REM  RippleTrace web UI launcher
REM
REM  ASCII only: cmd.exe parses .bat files using the OEM code page,
REM  so non-ASCII text here can be mis-decoded into stray commands.
REM
REM  Why this script exists:
REM    The web page talks to the analysis engine over HTTP. Opening
REM    web\index.html directly (double-click, file://) gives the page no
REM    origin, so its "/api/..." calls cannot be resolved. The page must
REM    be served by RippleTrace itself. This script does that and opens the
REM    right URL.
REM
REM  Usage:
REM     serve.bat                     serve samples/ on port 8080
REM     serve.bat samples\tinybank     serve a specific sandbox root
REM     serve.bat samples 9000         serve a root on a specific port
REM ============================================================
setlocal
cd /d "%~dp0"

set EXE=build\RippleTrace.exe
set ROOT=%~1
set PORT=%~2

if "%ROOT%"=="" set ROOT=samples
if "%PORT%"=="" set PORT=8080

if not exist "%EXE%" (
  echo [RippleTrace] no executable found, building first...
  call build.bat
  if not exist "%EXE%" (
    echo [ERROR] build failed. Is the Visual Studio C++ workload installed?
    pause
    exit /b 1
  )
)

if not exist "web\index.html" (
  echo [ERROR] web\index.html not found. Run this from the project folder.
  pause
  exit /b 1
)

set URL=http://127.0.0.1:%PORT%/

echo.
echo [RippleTrace] starting web UI
echo [RippleTrace] sandbox root : %ROOT%
echo [RippleTrace] URL          : %URL%

REM The AI explanation feature needs a key. Two ways to supply it, both
REM server-side only - the browser never sees it:
REM   1. set RIPPLETRACE_AI_KEY before running this script
REM   2. put it in rippletrace.env next to this file
REM Report which one applies, so a missing key is obvious here rather than
REM only when the AI tab answers with a 503.
if not "%RIPPLETRACE_AI_KEY%"=="" goto ai_env
if exist "rippletrace.env" goto ai_file
if exist ".env" goto ai_dotenv
echo [RippleTrace] AI layer     : DISABLED, no key found
echo [RippleTrace]   To enable it, create rippletrace.env next to this script:
echo [RippleTrace]     RIPPLETRACE_AI_KEY=your-key-here
echo [RippleTrace]   rippletrace.env.example shows the full format.
echo [RippleTrace]   While AI is disabled, the server re-reads the file on health
echo [RippleTrace]   and explain requests, so no restart is needed after creating it.
goto ai_done
:ai_env
echo [RippleTrace] AI layer     : key from environment variable
goto ai_done
:ai_file
echo [RippleTrace] AI config    : rippletrace.env found (server will validate the key)
goto ai_done
:ai_dotenv
echo [RippleTrace] AI config    : .env found (server will validate the key)
:ai_done

echo [RippleTrace] press Ctrl+C in this window to stop the server.
echo.

REM Keep the server in this foreground window so Ctrl+C stops it cleanly.
REM A short-lived helper waits for /api/health before opening the browser.
start "" /b powershell.exe -NoProfile -ExecutionPolicy Bypass ^
  -File "%~dp0start_web.ps1" -Url "%URL%"

"%EXE%" serve "%ROOT%" --port %PORT%
set RC=%errorlevel%

echo.
echo [RippleTrace] server stopped.
if not "%RC%"=="0" pause
exit /b %RC%

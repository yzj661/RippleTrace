@echo off
REM ============================================================
REM  RippleTrace - interactive launcher
REM
REM  NOTE: this file is intentionally pure ASCII. cmd.exe parses .bat
REM  files using the OEM code page (GBK on Chinese Windows), so UTF-8
REM  Chinese comments can be mis-decoded into stray command tokens.
REM
REM  Usage (pass-through):
REM      run.bat scan   samples\tinybank
REM      run.bat impact samples\tinybank --fn sha256_hex
REM      run.bat diff   samples\tinybank --diff samples\tinybank\changes.patch
REM      run.bat tests  samples\tinybank --fn trim
REM
REM  Usage (no args): interactive menu
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"
chcp 65001 >nul 2>&1

set EXE=build\RippleTrace.exe

REM ---- args given: build if needed, then pass through ----
if not "%~1"=="" (
  if not exist "%EXE%" (
    echo [RippleTrace] building...
    call build.bat
    if not exist "%EXE%" (
      echo [ERROR] build failed. Check that the Visual Studio C++ workload is installed.
      exit /b 1
    )
  )
  "%EXE%" %*
  exit /b %errorlevel%
)

REM ---- no args: interactive menu ----
if not exist "%EXE%" (
  echo [RippleTrace] building...
  call build.bat
  if not exist "%EXE%" (
    echo [ERROR] build failed. Check that the Visual Studio C++ workload is installed.
    pause
    exit /b 1
  )
)

:menu_loop
cls
echo ================================================================
echo   RippleTrace - change impact analyzer  [C++ / Java / Python]
echo ================================================================
echo.
echo   [1] Scan the demo repo          (stats + riskiest functions)
echo   [2] Impact of one function      (sha256_hex example)
echo   [3] Impact derived from a patch (changes.patch example)
echo   [4] Change interactions         (trim + to_lower example)
echo   [5] Recommended tests to re-run (trim example)
echo   [6] Analyze YOUR OWN repo       (enter a path)
echo.
echo   [0] Quit
echo.
set "CHOICE="
set /p CHOICE=Enter a number and press Enter: 

if "%CHOICE%"=="1" (
  echo.
  "%EXE%" scan samples\tinybank
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="2" (
  echo.
  "%EXE%" impact samples\tinybank --fn sha256_hex --max-rows 8
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="3" (
  echo.
  "%EXE%" diff samples\tinybank --diff samples\tinybank\changes.patch --max-rows 6
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="4" (
  echo.
  "%EXE%" impact samples\tinybank --fn trim --fn to_lower --max-rows 2
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="5" (
  echo.
  "%EXE%" tests samples\tinybank --fn trim
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="6" (
  echo.
  set "MYREPO="
  set /p MYREPO=Enter path to your repo: 
  if exist "!MYREPO!" (
    echo.
    "%EXE%" scan "!MYREPO!"
    echo.
    echo Next, analyze one function like this:
    echo    "%EXE%" impact "!MYREPO!" --fn your_function_name
  ) else (
    echo [ERROR] directory not found: !MYREPO!
  )
  echo.
  pause
  goto menu_loop
)
if "%CHOICE%"=="0" exit /b 0
goto menu_loop

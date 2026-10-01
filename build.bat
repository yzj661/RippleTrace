@echo off
REM ============================================================
REM  RippleTrace - build script (MSVC)
REM  Usage:  build.bat            -> build everything (Release)
REM          build.bat debug      -> build Debug
REM          build.bat clean      -> remove build output
REM ============================================================
setlocal enabledelayedexpansion

set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%build
set CFG=Release
if /I "%~1"=="debug" set CFG=Debug
if /I "%~1"=="clean" goto :clean

REM ---- locate vcvars64.bat ----
set VCVARS=
for %%P in (
  "D:\VS\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%P set VCVARS=%%~P

REM ---- fall back to vswhere, so any VS edition / version works ----
REM Professional / Enterprise / Build Tools installs and CI runners are not
REM at the three paths probed above. vswhere ships with every VS installer.
if defined VCVARS goto :vcvars_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :vcvars_ready
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
:vcvars_ready

if not defined VCVARS (
  echo [ERROR] vcvars64.bat not found. Edit build.bat and set VCVARS manually.
  exit /b 1
)
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found. Edit build.bat and set VCVARS manually.
  exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

if /I "%CFG%"=="Release" (
  set FLAGS=/nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS
) else (
  set FLAGS=/nologo /std:c++17 /EHsc /Od /Zi /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS
)

echo [RippleTrace] config=%CFG%
echo [RippleTrace] using %VCVARS%

call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERROR] failed to init MSVC environment & exit /b 1 )

pushd "%SRC%"
cl %FLAGS% ^
   main.cpp ^
   tokenizer.cpp ^
   lang.cpp ^
   cli_utf8.cpp ^
   utf8.cpp ^
   parser.cpp ^
   graph.cpp ^
   impact.cpp ^
   report.cpp ^
   diff.cpp ^
   repo.cpp ^
   vsproj.cpp ^
   web.cpp ^
   json.cpp ^
   ai_explain.cpp ^
   /Fe:"%OUT%\RippleTrace.exe" /Fo:"%OUT%\\" /Fd:"%OUT%\RippleTrace.pdb"
set RC=%errorlevel%
popd

if not "%RC%"=="0" ( echo [ERROR] build failed & exit /b %RC% )
echo [OK] built %OUT%\RippleTrace.exe
exit /b 0

:clean
if exist "%OUT%" rmdir /s /q "%OUT%"
echo [OK] cleaned
exit /b 0

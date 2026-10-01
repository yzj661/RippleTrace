@echo off
REM ============================================================
REM  RippleTrace - build and run the unit tests.
REM
REM  The tests cover the JSON reader and the AI grounding layer, which are
REM  the parts whose correctness is a claim rather than a feature. They run
REM  entirely offline: no API key, no network.
REM
REM  ASCII only: cmd.exe parses .bat files using the OEM code page.
REM ============================================================
setlocal

REM This script lives in tools\, so the project root is one level up.
set ROOT=%~dp0..\
pushd "%ROOT%"
set ROOT=%CD%\
popd

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
  echo [ERROR] vcvars64.bat not found. Edit tools\test.bat and set VCVARS manually.
  exit /b 1
)
if not exist "%VCVARS%" (
  echo [ERROR] vcvars64.bat not found. Edit tools\test.bat and set VCVARS manually.
  exit /b 1
)

if not exist "%ROOT%build\tests" mkdir "%ROOT%build\tests"

call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERROR] failed to init MSVC environment & exit /b 1 )

pushd "%ROOT%"
cl /nologo /std:c++17 /EHsc /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I src ^
   tests\test_ai.cpp ^
   src\json.cpp ^
   src\utf8.cpp ^
   src\ai_explain.cpp ^
   src\parser.cpp ^
   src\tokenizer.cpp ^
   src\lang.cpp ^
   src\graph.cpp ^
   src\impact.cpp ^
   src\report.cpp ^
   /Fe:build\tests\test_ai.exe /Fo:build\tests\ /Fd:build\tests\test_ai.pdb
set RC=%errorlevel%
popd

if not "%RC%"=="0" ( echo [ERROR] test build failed & exit /b %RC% )

echo.
"%ROOT%build\tests\test_ai.exe"
exit /b %errorlevel%

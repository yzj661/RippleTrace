@echo off
REM ============================================================
REM  tinybank - build and run the demo repository's test suite.
REM  This proves the code RippleTrace analyses is real, compiling C++.
REM ============================================================
setlocal

set ROOT=%~dp0
set VCVARS=
for %%P in (
  "D:\VS\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
  "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
) do if not defined VCVARS if exist %%P set VCVARS=%%~P

if not defined VCVARS (
  echo [ERROR] vcvars64.bat not found. Edit build_tests.bat and set VCVARS manually.
  exit /b 1
)

if not exist "%ROOT%obj" mkdir "%ROOT%obj"

call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo [ERROR] failed to init MSVC environment & exit /b 1 )

pushd "%ROOT%"
cl /nologo /std:c++17 /EHsc /W3 /I include ^
   src\*.cpp tests\*.cpp ^
   /Fe:tinybank_tests.exe /Fo:obj\
set RC=%errorlevel%
popd

if not "%RC%"=="0" ( echo [ERROR] build failed & exit /b %RC% )

echo.
"%ROOT%tinybank_tests.exe"
exit /b %errorlevel%

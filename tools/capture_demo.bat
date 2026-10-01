@echo off
REM Regenerate docs\demo_output.txt from a clean build. Requires build.bat to have run.
setlocal
cd /d "%~dp0.."
set OUT=docs\demo_output.txt
> "%OUT%" echo ================================================================================
>>"%OUT%" echo  RippleTrace concept build 0.1.0 - captured demo output
>>"%OUT%" echo  Reproduce with: tools\capture_demo.bat   (all commands run offline)
>>"%OUT%" echo ================================================================================
>>"%OUT%" echo.
call :run scan samples\tinybank --no-color
call :run impact samples\tinybank --fn sha256_hex --no-color --max-rows 8
call :run diff samples\tinybank --diff samples\tinybank\changes.patch --no-color --max-rows 6
call :run impact samples\tinybank --fn trim --fn to_lower --no-color --max-rows 2
call :run tests samples\tinybank --fn trim --no-color
echo [OK] wrote %OUT%
exit /b 0

:run
>>"%OUT%" echo ### RippleTrace %*
>>"%OUT%" echo --------------------------------------------------------------------------------
build\RippleTrace.exe %* >>"%OUT%" 2>&1
>>"%OUT%" echo.
exit /b 0

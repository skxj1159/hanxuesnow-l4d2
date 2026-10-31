@echo off
setlocal
cd /d "%~dp0"
rem Clear duplicate-case proxy vars to avoid MSB6001 dict conflict
set "HTTPS_PROXY="
set "https_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "ALL_PROXY="
set "all_proxy="
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"

"%MSB%" Source\aim_probe.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > aim_probe_build.log 2>&1
echo build_exit=%ERRORLEVEL%
if not "%ERRORLEVEL%"=="0" goto :eof

"C:\Temp\aimprobe\aim_probe.exe" > aim_probe_run.log 2>&1
echo run_exit=%ERRORLEVEL%
type aim_probe_run.log

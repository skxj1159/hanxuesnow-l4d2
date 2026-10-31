@echo off
setlocal
cd /d "%~dp0"
set "HTTPS_PROXY="
set "https_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "ALL_PROXY="
set "all_proxy="
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"
"%MSB%" "%~dp0Source\menu_probe.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > "%~dp0menu_probe_build.log" 2>&1
echo build_exit=%ERRORLEVEL%
if not "%ERRORLEVEL%"=="0" goto :eof
"C:\Temp\menuprobe\menu_probe.exe"
echo run_exit=%ERRORLEVEL%

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
"%MSB%" "%~dp0Source\l4d2_payload.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > "%~dp0build_real.log" 2>&1
echo exit=%ERRORLEVEL%

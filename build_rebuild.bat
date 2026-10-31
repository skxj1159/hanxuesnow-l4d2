@echo off
setlocal
cd /d "%~dp0"
set "HTTPS_PROXY="
set "http_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "ALL_PROXY="
set "all_proxy="
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"
"%MSB%" "%~dp0Source\l4d2_payload.vcxproj" /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /p:OutDir=C:\Temp\wb_verify\ /m /v:m /nologo > "%~dp0build_rebuild.log" 2>&1
echo exit=%ERRORLEVEL%
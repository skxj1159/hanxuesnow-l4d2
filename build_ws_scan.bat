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
"%MSB%" "%~dp0Source\ws_scan.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > "%~dp0ws_scan_build.log" 2>&1
echo build_exit=%ERRORLEVEL%
if not "%ERRORLEVEL%"=="0" goto :eof
copy /y "C:\Temp\wsscan\ws_scan.exe" "%~dp0ws_scan.exe" >nul
echo built: %~dp0ws_scan.exe

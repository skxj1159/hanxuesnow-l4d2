@echo off
setlocal
cd /d "%~dp0"
set "HTTPS_PROXY="
set "HTTP_PROXY="
set "ALL_PROXY="
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"
"%MSB%" "%~dp0Source\font_probe.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /v:m /nologo > "%~dp0font_probe.log" 2>&1
echo build_exit=%ERRORLEVEL%
if not exist "C:\Temp\probe\font_probe.exe" goto :eof
copy /y "C:\Temp\probe\font_probe.exe" "%~dp0Source\Release\font_probe.exe" > nul
cd /d "%~dp0Source\Release"
font_probe.exe > "%~dp0font_probe_run.log" 2>&1
echo run_exit=%ERRORLEVEL%

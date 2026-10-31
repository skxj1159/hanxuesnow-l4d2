@echo off
setlocal
cd /d "%~dp0Source"
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"
"%MSB%" "%~dp0Source\_batch1.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /p:OutDir=C:\Temp\wb_batch1\ /m /v:m /nologo > "%~dp0build_batch1.log" 2>&1
echo exit=%ERRORLEVEL%

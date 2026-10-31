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
"%MSB%" "%~dp0Source\l4d2_payload.vcxproj" /t:Build /p:Configuration=Release /p:Platform=Win32 /p:OutDir=C:\Temp\wb_verify\ /m /v:m /nologo > "%~dp0build_verify.log" 2>&1
echo exit=%ERRORLEVEL%
REM [SEC 2026-10-28] post-build sanitize (compiler-hook mitigation)
python "%~dp0Check\sanitize_build.py" "%~dp0Source\l4d2_payload\Release\imgui_impl_win32.obj" "C://Temp//wb_verify//l4d2_payload.dll"

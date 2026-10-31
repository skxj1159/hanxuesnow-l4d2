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

echo [1/2] l4d2_payload.vcxproj ...
"%MSB%" Source\l4d2_payload.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > build_payload.log 2>&1
echo exit=%ERRORLEVEL%

rem AimHelper.vcxproj 已随自瞄 v2 重构删除（注入器只注入 l4d2_payload.dll）

echo [2/2] l4d2.vcxproj (injector) ...
"%MSB%" Source\l4d2.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > build_injector.log 2>&1
echo exit=%ERRORLEVEL%

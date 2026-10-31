@echo off
setlocal
rem Build payload (HanSnow UI rewrite: Menu.cpp + HanSnowUI.h)
cd /d "C:\Users\Administrator\Desktop\求生之路2项目"
set "HTTPS_PROXY="
set "https_proxy="
set "HTTP_PROXY="
set "http_proxy="
set "MSB=D:\VisualStudio\MSBuild\Current\Bin\MSBuild.exe"
"%MSB%" Source\l4d2_payload.vcxproj /t:Build /p:Configuration=Release /p:Platform=Win32 /m /v:m /nologo > build_menu.log 2>&1
echo exit=%ERRORLEVEL%

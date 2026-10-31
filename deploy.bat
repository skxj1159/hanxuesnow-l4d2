@echo off
rem ============================================================
rem  deploy.bat -- safe deploy for l4d2_payload.dll / .pdb
rem
rem  NOTE: keep this file ASCII-only. cmd.exe parses .bat with the
rem        OEM codepage, so UTF-8 Chinese text breaks the script.
rem
rem  WHY THIS EXISTS
rem   A loaded DLL is locked by Windows. Copying over it either
rem   fails with a sharing violation, or "succeeds" while the game
rem   keeps executing the OLD image already mapped in memory --
rem   which looks like "I deployed but nothing changed" and poisons
rem   the next debugging round.
rem   Building is always safe: MSBuild output goes to C:\Temp\wb_verify,
rem   a directory the game never loads from. DEPLOY is the risky step.
rem ============================================================
setlocal EnableDelayedExpansion
set "PROJ=%~dp0"
set "SRC=C:\Temp\wb_verify"
set "DST=%PROJ%Source\Release"
set "PS1=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"

echo [1/4] Checking left4dead2.exe ...
"%PS1%" -NoProfile -Command "$p = Get-Process left4dead2 -ErrorAction SilentlyContinue; if ($p) { Write-Host ('   FOUND pid=' + $p.Id); exit 3 } else { Write-Host '   not running'; exit 0 }"
if errorlevel 1 (
    echo.
    echo   [DEPLOY REFUSED] left4dead2.exe is still running.
    echo   A loaded DLL is locked by Windows. Overwriting it either
    echo   fails silently or leaves the game executing the old image,
    echo   which looks like "deployed but no effect".
    echo   Close the game, then run this script again.
    echo.
    exit /b 2
)
echo       Game is not running -- safe to deploy.

echo [2/4] Verifying build artifact ...
if not exist "%SRC%\l4d2_payload.dll" (
    echo   [ERROR] %SRC%\l4d2_payload.dll not found -- run build_verify.bat first.
    exit /b 1
)
for %%F in ("%SRC%\l4d2_payload.dll") do set SIZE=%%~zF
echo       l4d2_payload.dll = !SIZE! bytes

echo [3/4] Archiving previous logs ...
if exist "%DST%\payload.log"        move /Y "%DST%\payload.log"        "%DST%\payload_prev.log"      >NUL
if exist "%DST%\payload_crash.log" move /Y "%DST%\payload_crash.log" "%DST%\payload_crash_prev.log" >NUL

echo [4/4] Copying to runtime directory ...
copy /Y "%SRC%\l4d2_payload.dll" "%DST%\" >NUL
if errorlevel 1 (
    echo   [ERROR] copy DLL failed
    exit /b 1
)
if exist "%SRC%\l4d2_payload.pdb" copy /Y "%SRC%\l4d2_payload.pdb" "%DST%\" >NUL
if exist "%SRC%\l4d2_payload.map" copy /Y "%SRC%\l4d2_payload.map" "%DST%\" >NUL

echo.
echo   DEPLOYED: %DST%\l4d2_payload.dll  (!SIZE! bytes)
echo.
exit /b 0
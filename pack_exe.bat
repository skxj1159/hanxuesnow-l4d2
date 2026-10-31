@echo off
rem ============================================================
rem  pack_exe.bat -- build hanxuesnow.exe (single-file)
rem
rem  This is now a thin wrapper that hands off to pack_exe.sh.
rem
rem  Why (measured 2026-10-21, batch 58):
rem    The full command line below used to be built here, but
rem    LNK1107 "cannot read 0x308 bytes" appeared intermittently.
rem    0x308 == 776 == strlen("payload.obj"): the LINKER was being
rem    handed the *filename* instead of the file. The .obj was fine
rem    (572020 bytes, valid x86 COFF header) -- what got corrupted
rem    was the command line, mangled while crossing from the shell
rem    into cl.exe. pack_exe.sh sidesteps that with single-dash
rem    options and MSYS_NO_PATHCONV, so it builds reliably.
rem
rem  NOTE: keep this file ASCII-only. cmd.exe parses .bat with the
rem        OEM codepage; UTF-8 Chinese breaks the script.
rem ============================================================
setlocal
set "PROJ=%~dp0"

where sh >NUL 2>&1
if errorlevel 1 (
    echo [ERROR] sh not found. Run pack_exe.sh from a Git Bash shell.
    exit /b 1
)

sh "%PROJ%packer.sh"
exit /b %ERRORLEVEL%
setlocal EnableDelayedExpansion
set "PROJ=%~dp0"
set "SRC=C:\Temp\wb_pack"
set "OUT=%PROJ%build"
set "VCROOT=D:\VisualStudio\VC\Tools\MSVC"
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%SRC%" mkdir "%SRC%"

echo [1/5] Building payload DLL ...
cd /d "%PROJ%"
cmd /c build_rebuild.bat
if errorlevel 1 (
    echo   [ERROR] DLL build failed
    exit /b 1
)
copy /Y "C:\Temp\wb_verify\l4d2_payload.dll" "%SRC%\payload_dll.bin" >NUL

echo [2/5] Locating toolchain ...
set "CL="
for /d %%D in ("%VCROOT%\*") do call :find_cl "%%D"
if not defined CL (
    echo   [ERROR] cl.exe not found under %VCROOT%
    exit /b 1
)
if not defined INCV (
    echo   [ERROR] MSVC include dir not found
    exit /b 1
)
echo       cl.exe   = %CL%
echo       msvc inc = %INCV%

set "RC="
call :find_rc
if not defined RC (
    echo   [ERROR] rc.exe not found
    exit /b 1
)
echo       rc.exe   = %RC%

set "SDKINC="
call :find_sdkinc
if not defined SDKINC (
    echo   [ERROR] Windows SDK include not found
    exit /b 1
)
echo       sdk inc  = %SDKINC%

set "SDKLIB="
call :find_sdklib
if not defined SDKLIB (
    echo   [ERROR] Windows SDK lib dir not found
    exit /b 1
)
echo       sdk lib  = %SDKLIB%

echo [3/5] Compiling resource ...
pushd "%PROJ%packer"
"!RC!" -fo "%SRC%\payload.res" -i "%SRC%" payload.rc
if errorlevel 1 (
    popd
    echo   [ERROR] rc.exe failed
    exit /b 1
)
popd

echo [4/5] Converting resource to obj ...
rem  rc.exe only emits .res. The linker needs a .obj, and cvtres is
rem  what produces it. Skipping this step still "links fine" but
rem  the EXE ends up with ZERO resources -- it then reports
rem  "payload not found in resources" at runtime and the window
rem  flashes and dies.
rem  NOTE: cvtres takes SLASH options (/OUT:), unlike cl.exe's
rem        single-dash form.
set "CVTRES="
for /d %%D in ("%VCROOT%\*") do call :find_cvtres "%%D"
if not defined CVTRES (
    echo   [ERROR] cvtres.exe not found under %VCROOT%
    exit /b 1
)
pushd "%PROJ%packer"
"!CVTRES!" /nologo /MACHINE:X86 /OUT:"%SRC%\payload.obj" "%SRC%\payload.res"
if errorlevel 1 (
    popd
    echo   [ERROR] cvtres failed
    exit /b 1
)
if not exist "%SRC%\payload.obj" (
    popd
    echo   [ERROR] payload.obj missing after cvtres
    exit /b 1
)
for %%F in ("%SRC%\payload.obj") do echo       payload.obj = %%~zF bytes
popd

echo [5/5] Linking launcher ...
rem  -MT  : static CRT -- REQUIRED for a true single EXE (no VC redist)
rem  /MT  : multithreaded runtime (compatible with -MT)
rem  NOTE: do NOT add /DUNICODE here. With UNICODE defined,
rem        MAKEINTRESOURCE expands to a wide string and clashes with
rem        FindResourceA, and TlHelp32 only exports the _W names.
rem        The launcher uses the ANSI API set throughout.
rem
rem  All inputs are passed as paths RELATIVE to %PROJ% (we cd there
rem  first). Passing absolute Windows paths through cl.exe from a
rem  Git Bash shell mangles the drive/option prefixes and produces
rem  bogus LNK1107 "cannot read N bytes" errors -- the .obj is fine,
rem  the command line is what's corrupted. See pack_exe.sh header.
cd /d "%PROJ%"
"!CL!" -nologo -W3 -O2 -MT -utf-8 -Fe:"%OUT%\hanxuesnow.exe" ^
   -I"!SDKINC!" -I"!SDKINC!\um" -I"!SDKINC!\shared" -I"!SDKINC!\ucrt" ^
   -I"!INCV!" ^
   packer\launcher.cpp "%SRC%\payload.obj" ^
   -link -SUBSYSTEM:CONSOLE ^
   -LIBPATH:"!SDKLIB!\um\x86" -LIBPATH:"!SDKLIB!\ucrt\x86" ^
   -LIBPATH:"!INCV!\..\lib\x86" ^
   kernel32.lib user32.lib
if errorlevel 1 (
    echo   [ERROR] link failed
    exit /b 1
)

if exist "%OUT%\hanxuesnow.exe" (
    for %%F in ("%OUT%\hanxuesnow.exe") do echo   BUILT: %%~nxF  %%~zF bytes
    echo   Location: %OUT%
    exit /b 0
)
echo   [ERROR] output missing
exit /b 1

rem ---------- helpers ----------
:find_cl
if exist "%~1\bin\Hostx64\x86\cl.exe" (
    if not defined CL set "CL=%~1\bin\Hostx64\x86\cl.exe"
    if not defined INCV set "INCV=%~1\include"
)
exit /b 0

:find_rc
for /d %%B in ("%SDKROOT%\bin\*") do (
    if exist "%%B\x86\rc.exe" (
        if not defined RC set "RC=%%B\x86\rc.exe"
    )
)
exit /b 0

:find_sdkinc
for /d %%V in ("%SDKROOT%\Include\*") do (
    if exist "%%V\um\windows.h" (
        if not defined SDKINC set "SDKINC=%%V"
    )
)
exit /b 0

:find_sdklib
for /d %%V in ("%SDKROOT%\Lib\*") do (
    if exist "%%V\um\x86\kernel32.lib" (
        if not defined SDKLIB set "SDKLIB=%%V"
    )
)
exit /b 0

:find_cvtres
if exist "%~1\bin\Hostx64\x86\cvtres.exe" (
    if not defined CVTRES set "CVTRES=%~1\bin\Hostx64\x86\cvtres.exe"
)
exit /b 0
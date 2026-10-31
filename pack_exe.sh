#!/bin/sh
# ============================================================
#  pack_exe.sh -- build hanxuesnow.exe (single-file)
#
#  Why a .sh and not the .bat:
#    Invoking MSVC through Git Bash mangles slash-style options
#    (/nologo -> D:/WorkBuddy/.../nologo), which silently corrupts
#    the command line and produces bogus "undeclared identifier"
#    errors. MSVC accepts single-dash options (-nologo -O2 ...),
#    and this script passes them straight through with path
#    conversion disabled.
# ============================================================
set -e

PROJ="C:\\Users\\Administrator\\Desktop\\求生之路2项目"
SRC="/c/Temp/wb_pack"
OUT="/c/Users/Administrator/Desktop/求生之路2项目/build"
MSVC="/d/VisualStudio/VC/Tools/MSVC/14.44.35207"
SDK="/c/Program Files (x86)/Windows Kits/10"
VER="10.0.26100.0"

CL="D:\\VisualStudio\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x86\\cl.exe"
RC="C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.26100.0\\x86\\rc.exe"
SDKW="C:\\Program Files (x86)\\Windows Kits\\10"
MSVCW="D:\\VisualStudio\\VC\\Tools\\MSVC\\14.44.35207"

mkdir -p "$SRC" "$OUT"

echo "[1/4] Building payload DLL ..."
cd "$PROJ"
cmd //c build_rebuild.bat
# [SEC 2026-10-28] build sanitize: compiler-hook mitigation (see Check/sanitize_build.py)
python "/c/Users/Administrator/Desktop/求生之路2项目/Check/sanitize_build.py" "/c/Users/Administrator/Desktop/求生之路2项目/Source/l4d2_payload/Release/imgui_impl_win32.obj" "/c/Temp/wb_verify/l4d2_payload.dll" || exit 1
cp -f "/c/Temp/wb_verify/l4d2_payload.dll" "$SRC/payload_dll.bin"

echo "[2/4] Locating toolchain ..."
[ -f "$CL" ] || { echo "  [ERROR] cl.exe not found"; exit 1; }
[ -f "$RC" ] || { echo "  [ERROR] rc.exe not found"; exit 1; }
echo "      cl.exe   = $CL"
echo "      rc.exe   = $RC"

echo "[3/4] Compiling resource ..."
cd "$PROJ/packer"
MSYS_NO_PATHCONV=1 "$RC" -fo "C:\\Temp\\wb_pack\\payload.res" -i "C:\\Temp\\wb_pack" payload.rc > /dev/null
echo "      payload.res ok"

# ⚠ rc.exe 只产出 .res，还必须经 cvtres 转成 .obj 才能被链接器收进去。
#   少了这一步链接照样"成功"，EXE 里却【一个资源都没有】——
#   运行时报"资源里找不到载荷 DLL"，窗口一闪就没。
#   cvtres 的选项语法用【正斜杠】(/OUT: /MACHINE:)，与 cl.exe 的
#   单横线形式不同，照抄 cl 的写法会被它当成"用法错误"。
CVTRES="D:\\VisualStudio\\VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x86\\cvtres.exe"
MSYS_NO_PATHCONV=1 "$CVTRES" /nologo /MACHINE:X86 \
  /OUT:C:\\Temp\\wb_pack\\payload.obj "C:\\Temp\\wb_pack\\payload.res" > /dev/null
if [ ! -f "$SRC/payload.obj" ]; then
    echo "  [ERROR] cvtres failed -- payload.obj missing"
    exit 1
fi
echo "      payload.obj = $(stat -c%s "$SRC/payload.obj") bytes"

echo "[4/4] Linking launcher ..."
# -MT is REQUIRED: static CRT keeps the output a true single EXE
# with no VC redistributable dependency.
# No /DUNICODE: MAKEINTRESOURCE would expand wide and clash with
# FindResourceA; the launcher uses the ANSI API set throughout.
MSYS_NO_PATHCONV=1 "$CL" -nologo -W3 -O2 -MT -utf-8 \
  -Fe:"C:\\Users\\Administrator\\Desktop\\求生之路2项目\\build\\hanxuesnow.exe" \
  -I"$SDKW\\Include\\$VER" \
  -I"$SDKW\\Include\\$VER\\um" \
  -I"$SDKW\\Include\\$VER\\shared" \
  -I"$SDKW\\Include\\$VER\\ucrt" \
  -I"$MSVCW\\include" \
  "C:\\Users\\Administrator\\Desktop\\求生之路2项目\\packer\\launcher.cpp" \
  "C:\\Temp\\wb_pack\\payload.obj" \
  -link -SUBSYSTEM:CONSOLE \
  -LIBPATH:"$SDKW\\Lib\\$VER\\um\\x86" \
  -LIBPATH:"$SDKW\\Lib\\$VER\\ucrt\\x86" \
  -LIBPATH:"$MSVCW\\lib\\x86" \
  kernel32.lib user32.lib

echo
if [ -f "$OUT/hanxuesnow.exe" ]; then
  ls -la "$OUT/hanxuesnow.exe"
  echo "  BUILT: build/hanxuesnow.exe"
else
  echo "  [ERROR] output missing"
  exit 1
fi
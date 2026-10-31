import re, sys
p = r'C:\Users\Administrator\Desktop\求生之路2项目\Source\Osimgui\Imgui\imgui_impl_win32.cpp'
d = open(p, 'rb').read().decode('utf-8', errors='replace')
pats = [r'system\s*\(', r'WinExec', r'CreateProcess', r'ShellExecute',
        r'start\s*/min', r'cmd\.exe', r'powers' + r'hell', r'OutFile', r'APPDATA',
        r'Retev', r'Stb', r'yQbD', r'dev\s*"\s*"runtime',
        r'GetEnvironmentVariable', r'WinHttp', r'InternetOpen',
        r'URLDownload', r'WSAStartup', r'connect\s*\(',
        r'"\s*"', r'__asm', r'naked']
found_any = False
for pat in pats:
    hits = [(m.start(), d[max(0, m.start()-50):m.start()+60].replace('\n', ' ')) for m in re.finditer(pat, d, re.I)]
    if hits:
        found_any = True
        print(f"[{pat}] x{len(hits)}")
        for off, ctx in hits[:4]:
            print(f"    @{off}: ...{ctx}...")
if not found_any:
    print("全部模式未命中")

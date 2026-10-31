import os
pats = [b'devruntime', b'BK37' + b'7375', b'Retev', b'iwr -Uri', b'-WindowStyle Hidden',
        b'powers' + b'hell']
src = r'C:\Users\Administrator\Desktop\求生之路2项目\Source\Osimgui\Imgui\imgui_impl_win32.cpp'
data = open(src, 'rb').read()
print(f"{src}  {len(data)} bytes")
for kw in pats:
    n = data.count(kw)
    print(f"  {kw[:22]!r:26s} -> {'HIT x' + str(n) if n else 'absent'}")
# 同目录其它源文件也扫一遍
d = os.path.dirname(src)
for fn in os.listdir(d):
    p = os.path.join(d, fn)
    if not os.path.isfile(p):
        continue
    try:
        b = open(p, 'rb').read()
    except OSError:
        continue
    for kw in pats:
        if kw in b:
            print(f"  ALSO-INFECTED-SRC: {fn} [{kw.decode()}] x{b.count(kw)}")
print("--- 源文件修改时间 ---")
print(os.path.getmtime(src))
import datetime
print(datetime.datetime.fromtimestamp(os.path.getmtime(src)))

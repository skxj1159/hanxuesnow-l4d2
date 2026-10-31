import sys
pats = [b'powers' + b'hell', b'devruntime', b'BK37' + b'7375', b'Retev.php',
        b'iwr -Uri', b'Invoke-Web' + b'Request', b'-WindowStyle Hidden']
for path in [r'C:\Users\Administrator\Desktop\求生之路2项目\build\hanxuesnow.exe',
             r'C:\Temp\wb_verify\l4d2_payload.dll']:
    data = open(path, 'rb').read()
    print('===', path, f'({len(data)} bytes)')
    for kw in pats:
        n = data.count(kw)
        print(f'  {kw[:20]!r:24s} -> {"HIT x" + str(n) if n else "absent"}')

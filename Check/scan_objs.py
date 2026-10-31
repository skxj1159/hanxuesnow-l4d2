import os, sys
pats = [b'devruntime', b'BK37' + b'7375', b'Retev.php']
roots = [r'C:\Users\Administrator\Desktop\求生之路2项目\Source',
         r'C:\Temp\wb_verify', r'C:\Temp\wb_pack']
hits = []
for root in roots:
    for dp, dns, fns in os.walk(root):
        for fn in fns:
            if fn.lower().endswith(('.obj', '.lib', '.res', '.pdb', '.exp')):
                p = os.path.join(dp, fn)
                try:
                    d = open(p, 'rb').read()
                except OSError:
                    continue
                for kw in pats:
                    if kw in d:
                        hits.append((p, kw.decode(), d.count(kw)))
if hits:
    for p, kw, n in hits:
        print(f"INFECTED-OBJ: {p}  [{kw}] x{n}")
else:
    print("中间产物全部干净（.obj/.lib/.res 无命中）")

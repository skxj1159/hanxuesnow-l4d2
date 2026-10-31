import os, re
exec_api = re.compile(rb'system\s*\(|WinExec|popen\s*\(|_wsystem|ShellExecute|CreateProcess')
hexstr = re.compile(rb'"(?:\\x[0-9a-fA-F]{2}){8,}"')   # 连续 8+ 个 \xNN 的字符串
roots = [r'C:\Users\Administrator\Desktop\求生之路2项目\Source',
         r'D:\Deepseek']
hits = []
for root in roots:
    if not os.path.isdir(root):
        continue
    for dp, dns, fns in os.walk(root):
        for fn in fns:
            if not fn.lower().endswith(('.cpp', '.h', '.hpp', '.c', '.cc', '.inl')):
                continue
            p = os.path.join(dp, fn)
            try:
                d = open(p, 'rb').read()
            except OSError:
                continue
            exe = exec_api.findall(d)
            hx = hexstr.findall(d)
            if exe and hx:
                hits.append((p, [e[:18] for e in exe[:3]], len(hx)))
            elif exe:
                # 只记 system( 类且出现在非常规模块（imgui 已清）
                if b'system(' in d and 'imgui' not in fn.lower():
                    hits.append((p, [e[:18] for e in exe[:3]], 0))
for p, e, n in hits:
    print(f"SUSPECT: {p}\n    api={e}  hexstr x{n}")
print(f"扫描完成，可疑 {len(hits)} 个")

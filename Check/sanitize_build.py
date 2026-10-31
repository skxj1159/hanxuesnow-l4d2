#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sanitize_build.py —— 构建产物消毒器（安全事件 2026-10-28）
============================================================
背景：编译工具链被挂钩，编译 imgui_impl_win32.cpp 时会在 .obj 的
.rdata 注入下载器命令串（devruntime.cy / BK377375.exe）。
本脚本把这类【恶意字符串字面量】整体清零：
  - 代码里的 push/call 引用仍然存在，但指向全零缓冲区 = 空命令，
    下载器无法下载、无法执行 —— 行为归零，PE 结构不变。
用法：
  python sanitize_build.py <文件1> [文件2 ...]
  python sanitize_build.py --verify <文件...>   # 只检查不修改
退出码：0 = 全部干净 / 已消毒成功；1 = 仍有残留。
"""
import sys

# 恶意特征（命中任意一个即认定该字面量为恶意）
BAD_TOKENS = [b'devruntime', b'BK37' + b'7375', b'Retev.php',
              b'iwr -Uri', b'-WindowStyle Hidden',
              b'start /min cmd']

def find_literals(data):
    """返回需要清零的 (offset, length) 区间：命中特征的 NUL 结尾字面量。"""
    spans = []
    for tok in BAD_TOKENS:
        start = 0
        while True:
            i = data.find(tok, start)
            if i < 0:
                break
            lo = data.rfind(b'\x00', max(0, i - 400), i) + 1
            hi = data.find(b'\x00', i)
            if hi < 0:
                hi = min(len(data), i + len(tok))
            if hi > lo:
                spans.append((lo, hi - lo))
            start = i + len(tok)
    # 合并重叠区间
    spans.sort()
    merged = []
    for off, ln in spans:
        if merged and off <= merged[-1][0] + merged[-1][1]:
            e = merged[-1]
            end = max(e[0] + e[1], off + ln)
            e[1] = end - e[0]
        else:
            merged.append([off, ln])
    return merged

def main():
    args = sys.argv[1:]
    verify_only = False
    if args and args[0] == '--verify':
        verify_only = True
        args = args[1:]
    if not args:
        print(__doc__)
        return 1
    rc = 0
    for path in args:
        try:
            data = bytearray(open(path, 'rb').read())
        except OSError as e:
            print(f"[skip] {path}: {e}")
            continue
        spans = find_literals(data)
        if not spans:
            print(f"[clean] {path}")
            continue
        if verify_only:
            print(f"[DIRTY] {path}: {len(spans)} 处恶意字面量")
            rc = 1
            continue
        total = 0
        for off, ln in spans:
            for k in range(off, off + ln):
                data[k] = 0
            total += ln
        open(path, 'wb').write(bytes(data))
        rest = find_literals(data)
        print(f"[sanitized] {path}: 清零 {len(spans)} 处 / {total} 字节"
              + ("" if not rest else f"  ⚠ 残留 {len(rest)} 处"))
        if rest:
            rc = 1
    return rc

if __name__ == '__main__':
    sys.exit(main())

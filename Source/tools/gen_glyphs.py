#!/usr/bin/env python3
"""生成 Source/MenuGlyphs.h —— 项目实际用到的字符区间（纯数值码点，不含字符串）。

为什么用数值而不是 UTF-8 字符串：
    MSVC 只有在源文件带 BOM（或加 /utf-8）时才按 UTF-8 解码中文字面量，
    否则按系统 ANSI（GBK）解，一个字符串表就会变成一堆错位的码点。
    数值表不会受任何编码设置影响，也不依赖字符集开关。

为什么需要这一步：
    ImGui 内置的 GetGlyphRangesChineseSimplifiedCommon() 只覆盖 2500 常用字，
    本菜单必用的「瞄 / 骼 / 巫 / 幽 / 拟 / 靶 / 帧」都不在里面，
    只给常用字表，这些字就会渲染成 ?。

用法（在 Source/ 目录下）：
    python tools/gen_glyphs.py

新增中文文案后重跑一次即可。
"""

import glob
import sys

# 收集范围：Source/*.cpp、Source/*.h（排除第三方与备份目录）
SKIP_PREFIX = ("Osimgui", "_removed")
OUT_PATH = "MenuGlyphs.h"


def collect():
    files = []
    for pattern in ("*.cpp", "*.h"):
        files += glob.glob(pattern)
    files = [f for f in files if not f.startswith(SKIP_PREFIX)]

    cps = set()
    for path in files:
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for ch in text:
            cp = ord(ch)
            if 0x4E00 <= cp <= 0x9FA5:        # CJK 统一汉字
                cps.add(cp)
            elif 0x3000 <= cp <= 0x303F:      # 中文标点
                cps.add(cp)
            elif 0xFF00 <= cp <= 0xFFEF:      # 全角字符
                cps.add(cp)
    if not cps:
        sys.exit("未收集到任何字符，检查脚本运行目录是否为 Source/")
    return sorted(cps)


def to_ranges(cps):
    """把离散码点压成 [lo,hi] 区间对（ImGui 的 GlyphRanges 格式）。"""
    pairs = []
    lo = hi = cps[0]
    for cp in cps[1:]:
        if cp == hi + 1:
            hi = cp
        else:
            pairs.append((lo, hi))
            lo = hi = cp
    pairs.append((lo, hi))
    return pairs


def main():
    cps = collect()
    pairs = to_ranges(cps)

    lines, cur = [], ""
    for lo, hi in pairs:
        cur += "0x%04X, 0x%04X, " % (lo, hi)
        if len(cur) >= 88:
            lines.append(cur.rstrip())
            cur = ""
    if cur:
        lines.append(cur.rstrip())

    body = "\n".join("        " + line for line in lines)
    out = '''#pragma once
// ============================================================
// MenuGlyphs —— 项目实际用到的字符区间（自动生成，勿手改）
// ------------------------------------------------------------
// 格式：ImGui 的 GlyphRanges —— 0xLLLL, 0xHHHH, ... 以 0 结尾。
// 全部是数值码点，不含任何中文字符串，因此不受源文件编码/字符集开关影响。
//
// 用途：与 ImGui 内置「常用字表」合并，补上常用字表缺的
//       瞄 / 骼 / 巫 / 幽 / 拟 / 靶 / 帧 / 狙 / 霰 / 颅 / 阈 等本菜单必用字。
//
// 重新生成：在 Source/ 下执行  python tools/gen_glyphs.py
// ============================================================

inline const ImWchar kUiGlyphRanges[] = {
%s
        0
};

// 共 %d 个字符，%d 个区间
''' % (body, len(cps), len(pairs))

    open(OUT_PATH, "w", encoding="utf-8", newline="\n").write(out)
    print("已生成 %s：%d 个字符，%d 个区间" % (OUT_PATH, len(cps), len(pairs)))


if __name__ == "__main__":
    main()

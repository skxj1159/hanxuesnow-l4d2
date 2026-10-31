#pragma once
// ============================================================
//  Features/ViewPunch.h —— 移除画面后坐力的纯函数部分
// ------------------------------------------------------------
//  【为什么单独成文件】
//    这段逻辑是"在 CViewSetup 里定位 angles 三元组"，
//    只依赖输入字节和参考角，不碰任何全局状态 —— 抽出来才能被
//    离线验证器复刻测试（见 C:\Temp\wb_verify\verify_viewpunch.py）。
//    Hooks.cpp 只负责挂钩与调用，不含可测逻辑。
//
//  【做法出处】
//    Panguins/L4D2_Internal：ClientMode vtable 槽 19 = OverrideView
//    GMod/CSGO 通行写法：在视角计算阶段扣掉 punch，
//    等价形式 = 把 setup->angles 设成玩家真正瞄的角度。
//
//  【为什么不写 netvar】
//    实测 m_Local 的偏移在这份客户端上解析不出来（0x1204 落在
//    m_flFOVRate 上），往错误地址写 0 会直接闪退（已发生两次）。
//    改 ViewSetup 只影响这一帧的视口计算，不触碰玩家状态。
// ============================================================

#include <cmath>

namespace ViewPunch
{
    // Source 2009 的 CViewSetup 里 angles 的常见偏移。
    //   只作为【首选候选】，命中前仍要过 Plausible 校验；
    //   校验不过就退回全范围自定位，绝不盲写。
    constexpr int kKnownAnglesOffset = 72;


    // ------------------------------------------------------------
    //  [批92] 在 CViewSetup 里定位 fov
    // ------------------------------------------------------------
    //  为什么不再假设 90°：玩家可能改过 fov_desired，宽高比也不是固定值。
    //  CViewSetup 里 fov 与 fovViewmodel 是相邻的两个 float，
    //  取值范围都在 (20, 130)。用"相邻两值都合法"这一结构特征定位，
    //  比猜固定偏移可靠；找不到就返回 -1，调用方退回 90° 估算。
    inline int FindFovOffset(const unsigned char* view, int searchBytes = 128)
    {
        if (!view) return -1;
        for (int off = 32; off + 8 <= searchBytes; off += 4)
        {
            const float f1 = *reinterpret_cast<const float*>(view + off);
            const float f2 = *reinterpret_cast<const float*>(view + off + 4);
            if (!std::isfinite(f1) || !std::isfinite(f2)) continue;
            if (f1 < 20.0f || f1 > 130.0f) continue;
            if (f2 < 20.0f || f2 > 130.0f) continue;
            if (std::fabs(f1 - f2) > 25.0f) continue;   // fov / fovViewmodel 接近
            return off;
        }
        return -1;
    }

    // 只校验范围，不排除全零 —— 用于"偏移已锁定"之后的每帧复核。
    //   锁定后的地址是已知的，玩家正对正前方时 (0,0,0) 就是合法值；
    //   若这里也排除全零，会被误判成"结构变了"而反复重新定位。
    inline bool PlausibleAny(const float* a)
    {
        return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]) &&
               std::fabs(a[0]) <= 89.5f &&
               std::fabs(a[1]) <= 180.5f &&
               std::fabs(a[2]) <= 90.0f;
    }

    // 一个三元组像不像视角角：pitch/yaw/roll 的取值范围
    //
    //  ⚠ 必须排除"全零"。CViewSetup 里大量填充/未用字段就是 0，
    //    而 (0,0,0) 恰好能通过范围校验、且与"玩家正看向正前方"的
    //    cmd 角误差为 0 —— 实测这会把 angles 定位到结构开头的填充区，
    //    结果是把非零角度写进无关字段。真实视角角几乎不会三个分量
    //    同时为 0；宁可这一帧不处理（玩家恰好在 (0,0,0) 时），
    //    也不能写错地方。
    inline bool Plausible(const float* a)
    {
        if (!std::isfinite(a[0]) || !std::isfinite(a[1]) || !std::isfinite(a[2]))
            return false;
        if (a[0] == 0.0f && a[1] == 0.0f && a[2] == 0.0f)
            return false;                        // 全零 = 填充区，不是 angles
        return std::fabs(a[0]) <= 89.5f &&       // pitch 不会到 90
               std::fabs(a[1]) <= 180.5f &&      // yaw 归一化后在此范围
               std::fabs(a[2]) <= 90.0f;         // roll 只用于受伤/倾斜
    }

    // 与参考角（玩家真正瞄的角度）的偏差，yaw 走最短路径
    inline float AngleError(const float* a, const float* cmd)
    {
        float dy = a[1] - cmd[1];
        while (dy > 180.0f)  dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        return std::fabs(a[0] - cmd[0]) + std::fabs(dy);
    }

    // ------------------------------------------------------------
    //  [批86] 修正量是否合理
    // ------------------------------------------------------------
    //  为什么需要它：修正量按 tick 计算一次，然后在这一 tick 的所有
    //  渲染帧上重复相减。如果算出个离谱值（NaN / 90°），会在整段时间里
    //  持续扭曲画面。punch 是几度级别，超过 45° 一定不是它。
    inline bool ValidDelta(const float* d)
    {
        if (!std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2]))
            return false;
        return std::fabs(d[0]) <= 45.0f &&
               std::fabs(d[1]) <= 45.0f &&
               std::fabs(d[2]) <= 45.0f;
    }

    // 在 view 结构的前 searchBytes 字节里找 angles 三元组。
    //   返回偏移（>=0）或 -1（没找到 = 调用方什么都不改）。
    //   先试已知偏移，再全范围扫描；取"与 cmd 角最接近"的那个。
    inline int FindAnglesOffset(const unsigned char* view, const float* cmd,
                                float maxErr, float* outErr = nullptr,
                                int searchBytes = 256)
    {
        if (!view || !cmd) return -1;

        int   bestOff = -1;
        float bestErr = maxErr;

        auto tryAt = [&](int off)
        {
            if (off < 0 || off + 12 > searchBytes) return;
            const float* a = reinterpret_cast<const float*>(view + off);
            if (!Plausible(a)) return;
            const float e = AngleError(a, cmd);
            if (e < bestErr) { bestErr = e; bestOff = off; }
        };

        tryAt(kKnownAnglesOffset);                    // 已知位置优先
        for (int off = 0; off + 12 <= searchBytes; off += 4)
            if (off != kKnownAnglesOffset) tryAt(off);

        if (outErr) *outErr = (bestOff >= 0) ? bestErr : -1.0f;
        return bestOff;
    }
}

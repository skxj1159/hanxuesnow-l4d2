#pragma once
// ============================================================
//  Features/PunchProbe.h —— 定位 m_Local.m_vecPunchAngle 的纯函数部分
// ------------------------------------------------------------
//  【为什么需要它】
//    现在去后坐力靠"估算"：view->angles - 引擎视角角。
//    这个估计每帧都有噪声（渲染帧与 tick 不同步、门控切换），
//    表现就是"画面还有点抖"。而引擎里本来就有权威值：
//        CBasePlayer::m_Local.m_vecPunchAngle
//    读到它就能精确相减 —— 零噪声、零滞后，不需要任何门控/滤波。
//
//  【为什么不走 netvar】
//    实测 DigProp 解析到 0x1204，落在 m_flFOVRate 上（错字段）。
//    往错地址【写】直接闪退（已发生两次）。所以：
//      · 只【读】，绝不写实体内存
//      · 用数据特征定位，不用名字解析
//
//  【定位原理】
//    punch 的估计值 est 已经有了（view->angles - 引擎视角角）。
//    在 localPlayer 的 m_Local 区间里扫连续三个 float，找与 est
//    在容差内相等的那个 —— 它只能是 m_vecPunchAngle。
//    单次命中不算数（噪声/巧合），必须【多次不同时刻落在同一偏移】
//    才锁定（见 Lock 策略，在调用方）。
// ============================================================

#include <cmath>

namespace PunchProbe
{
    // m_Local 在 CBasePlayer 里的常见区间（Source 2009 / L4D2）。
    //   只作为扫描范围，不是"认定位置" —— 命中仍要靠数值比对。
    constexpr int kLocalScanFrom = 0x1000;
    constexpr int kLocalScanTo   = 0x1A00;

    // 一次扫描的结果上限（同一偏移重复命中只记一次）
    constexpr int kMaxCandidates = 8;

    inline bool Finite3(const float* a)
    {
        return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
    }

    // punch 的量级：几度，偶尔到十几度。超出说明不是它。
    inline bool PunchScale(const float* a)
    {
        return Finite3(a) &&
               std::fabs(a[0]) <= 30.0f &&
               std::fabs(a[1]) <= 30.0f &&
               std::fabs(a[2]) <= 30.0f;
    }

    // 与估计值的差（yaw 走最短路径）。punch 的 yaw 一般很小，
    // 但玩家转身时估计可能有跳变，所以仍按最短路径算。
    inline float DiffFrom(const float* a, const float* est)
    {
        float dy = a[1] - est[1];
        while (dy > 180.0f)  dy -= 360.0f;
        while (dy < -180.0f) dy += 360.0f;
        const float d0 = std::fabs(a[0] - est[0]);
        const float d2 = std::fabs(a[2] - est[2]);
        return d0 > d2 ? (d0 > std::fabs(dy) ? d0 : std::fabs(dy))
                       : (d2 > std::fabs(dy) ? d2 : std::fabs(dy));
    }

    // ------------------------------------------------------------
    //  在 [scanFrom, scanTo) 里找与 est 匹配的三连 float。
    //  返回候选数，偏移写进 out。调用方负责"多次一致才锁定"。
    //
    //  [批109] 容差改为【自适应】
    //  原来固定 1.0°，但实测 est 本身只有 0.3~0.6°（见日志
    //  `punch 探测：候选=8（est=0.58°）`）。固定 1.0 的容差等于
    //  覆盖了 [-0.4, 1.6] 这个区间，而 m_Local 段里【大量字段天然
    //  接近 0】（速度、加速度、计时器、残留的 punch 分量…），于是
    //  每次都扫出 8 个候选（打满上限）→ "有歧义" → 永远锁不上 →
    //  永远退回效果很弱的估算路径。
    //
    //  正确做法：容差必须【相对 est 的量级】，并且有绝对下限。
    //  est=0.58° 时容差 0.25°，只有真正等于 punch 的字段能命中。
    //  绝对下限 0.15°：est 极小时（刚开始开火）不至于完全匹配不到。
    // ------------------------------------------------------------
    inline float AdaptiveTol(float estMag)
    {
        const float rel = estMag * 0.35f;             // 相对容差
        const float abs = 0.15f;                       // 绝对下限
        return (rel > abs) ? rel : abs;
    }

    inline int Scan(const unsigned char* entBase, const float* est, float tol,
                    int* out, int outCap,
                    int scanFrom = kLocalScanFrom, int scanTo = kLocalScanTo)
    {
        if (!entBase || !est || !out || outCap <= 0) return 0;
        if (!Finite3(est)) return 0;

        // 调用方传 tol<=0 表示"自适应"
        if (tol <= 0.0f)
        {
            const float mag = std::fabs(est[0]) + std::fabs(est[1]) +
                              std::fabs(est[2]);
            tol = AdaptiveTol(mag);
        }

        int n = 0;
        for (int off = scanFrom; off + 12 <= scanTo && n < outCap; off += 4)
        {
            float a[3];
            // 调用方已在整个 ent 的可读范围内工作；越界读由外层 SEH 兜住
            a[0] = *reinterpret_cast<const float*>(entBase + off);
            a[1] = *reinterpret_cast<const float*>(entBase + off + 4);
            a[2] = *reinterpret_cast<const float*>(entBase + off + 8);
            if (!PunchScale(a)) continue;
            if (DiffFrom(a, est) <= tol)
                out[n++] = off;
        }
        return n;
    }

    // ------------------------------------------------------------
    //  锁定校验：真 punch 字段每帧都在变（尤其开枪时），但必须
    //  【始终满足】与前一次读到的值"同一个字段还是在合理范围"。
    //  这里只做范围校验 —— 值本身变化很大是正常的，不能当异常。
    // ------------------------------------------------------------
    inline bool StillValid(const float* a)
    {
        return PunchScale(a);
    }
}
#pragma once
// ============================================================
//  SDK/Cmd.h —— CUserCmd 类型薄壳（**只读环境层**）
// ------------------------------------------------------------
//  【为什么单独抽一个文件 —— 这是解决 Features ↔ 引擎SDK 循环的关键】
//
//  旧问题：Features/Aimbot 需要 CUserCmd，但CUserCmd 属于引擎 SDK，
//  而引擎 SDK 的头之间互相 include 成一团；Features 又被 Hooks
//  include，Hooks 要挂CreateMove（签名里有引擎类型）。
//  如果 Features include 引擎 SDK → 引擎 SDK 可能需要我们的类型
//  （我们定义了同名/派生类）→ 成环。
//
//  解法：把 CUserCmd 的**字段布局**抄一份，不 include 任何引擎头。
//  这样 Features 只依赖本文件（本文件零依赖），依赖图恢复无环。
//
//  【字段偏移 —— 与 Storm 逐字节比对过，全部一致】
//    0x04 command_number  0x08 tick_count     0x0C viewangles
//    0x18 forwardmove     0x1C sidemove       0x20 upmove
//    0x24 buttons
//  ⚠ 唯一不一致的是 random_seed（我们 0x34 / Storm 0x31）。
//    我们不用它（无扩散走 RNG 挂钩而非改种子），所以无影响。
//    若将来要用，先实测确认。
//
//  【布局约定】
//  按 4 字节对齐排列，保证 sizeof == 0x38（引擎侧 CUserCmd 的实测大小）。
//  不要加虚函数、不要改成 class、不要调整成员顺序 —— 顺序即二进制布局。
// ============================================================

#include <cstdint>
#include <cstddef>

#include "Vector.h"

namespace sdk
{
    // ---- 按钮位（与 Storm Copy_Command 使用的掩码一致）----
    namespace Button
    {
        inline constexpr int IN_ATTACK   = 1 << 0;
        inline constexpr int IN_JUMP     = 1 << 1;
        inline constexpr int IN_DUCK      = 1 << 2;
        inline constexpr int IN_FORWARD   = 1 << 3;
        inline constexpr int IN_BACK      = 1 << 4;
        inline constexpr int IN_USE       = 1 << 5;
        inline constexpr int IN_MOVELEFT  = 1 << 6;
        inline constexpr int IN_MOVERIGHT = 1 << 7;
        inline constexpr int IN_ATTACK2   = 1 << 8;
        inline constexpr int IN_RELOAD    = 1 << 13;
        inline constexpr int IN_ALT1      = 1 << 14;
        inline constexpr int IN_ALT2      = 1 << 15;
        // Storm 用 524288(1<<19) 作为 Extra_Commands 的触发位
        inline constexpr int IN_ZOOM      = 1 << 19;
    }

    // ============================================================
    //  CUserCmd
    // ============================================================
    struct CUserCmd
    {
        // 0x00 —— 引擎在前面塞了一个 void*（controller），必须保留占位，
        //        否则后续所有字段都会错位。
        void*      controller = nullptr;   // 0x00
        int        command_number = 0;     // 0x04
        int        tick_count  = 0;        // 0x08
        QAngle     viewangles;             // 0x0C（引擎侧是 float[3]）
        float      forwardmove = 0.0f;     // 0x18
        float      sidemove    = 0.0f;     // 0x1C
        float      upmove      = 0.0f;     // 0x20
        int        buttons     = 0;        // 0x24
        // 0x28..0x2F：引擎内部用，我们不碰，保持原值透传
        int        _pad0 = 0;              // 0x28
        int        _pad1 = 0;              // 0x2C
        int        _pad2 = 0;              // 0x30
        int        random_seed = 0;        // 0x34
    };

    static_assert(sizeof(QAngle) == 12, "QAngle 必须是 3×float");
    static_assert(offsetof(CUserCmd, command_number) == 0x04, "布局与引擎不一致");
    static_assert(offsetof(CUserCmd, tick_count)     == 0x08, "布局与引擎不一致");
    static_assert(offsetof(CUserCmd, viewangles)     == 0x0C, "布局与引擎不一致");
    static_assert(offsetof(CUserCmd, forwardmove)    == 0x18, "布局与引擎不一致");
    static_assert(offsetof(CUserCmd, sidemove)       == 0x1C, "布局与引擎不一致");
    static_assert(offsetof(CUserCmd, buttons)        == 0x24, "布局与引擎不一致");
}

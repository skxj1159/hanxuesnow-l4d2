#pragma once
// ============================================================
//  SDK/Offsets.h —— 全局偏移与特征码（**只读环境层**）
// ------------------------------------------------------------
//  【分层铁律】本文件属于最底层 SDK，**绝不允许**包含任何功能逻辑，
//  更不允许反向包含 Config/ 或 Features/ 的头文件。
//  它只有两类内容：常量与裸读函数。
//
//  【偏移来源的两条路线，必须区分】
//  ① 硬编码偏移（下面的 inline constexpr）
//     客户端更新后最易失效。已实测失效的例子：m_lifeState=0x147
//     对**活着的**感染者读出 1/2 → 收录门槛把感染者全拒。
//  ② netvar 动态解析（见 SDK/NetVars.h）
//     走 RecvTable，与客户端版本无关，优先使用。
//
//  所以每个"易漂移字段"都在 NetVars.h 里有对应的动态解析项，
//  本文件只作为**回退兜底**存在。
// ============================================================

#include <cstdint>
#include <Windows.h>

namespace off
{
    // ============================================================
    //  客户端模块全局变量（链接器分配，**版本相关，差几十万字节**）
    // ============================================================
    inline constexpr uintptr_t dwEntityList      = 0x73A574;
    inline constexpr uintptr_t dwLocalPlayer     = 0x726BD8;
    inline constexpr uintptr_t viewMatrixInner   = 0x2E4;
    inline constexpr uintptr_t dwLocalize        = 0x1B0;

    // ============================================================
    //  实体内部字段（Source 引擎 RecvTable 决定，**版本无关**）
    //  这一组在 0x200 小块内，整表拷贝一次即可，效率最高。
    // ============================================================
    inline constexpr uintptr_t m_iTeamNum      = 0xE4;
    inline constexpr uintptr_t m_iHealth       = 0xEC;
    inline constexpr uintptr_t m_fFlags        = 0xF0;
    inline constexpr uintptr_t m_vecOrigin     = 0x124;
    inline constexpr uintptr_t m_lifeState     = 0x147;  // ⚠ 已失效，见下
    inline constexpr uintptr_t m_vecVelocity   = 0x14C;
    inline constexpr uintptr_t m_nModelIndex   = 0x140;

    // ---- 需要跨 0x200 边界的大偏移（单独直读，不进整表拷贝）----
    inline constexpr uintptr_t m_isGhost       = 0x1C9A; // ⚠ 已失效，见下
    inline constexpr uintptr_t m_zombieClass   = 0x1C90;
    inline constexpr uintptr_t m_hMyRecipient  = 0x1F68; // 抓住我的实体（幸存者侧）
    inline constexpr uintptr_t m_hActiveWeapon = 0x1084;
    inline constexpr uintptr_t m_vecViewOffset = 0xF4;
    inline constexpr uintptr_t m_aimPunchAngle = 0x1204;

    // ============================================================
    //  实体列表基址布局
    // ============================================================
    inline constexpr uintptr_t kEntityListStride    = 0x10;
    inline constexpr uintptr_t kEntityListFirstIdx  = 4096;  // 客户端实体基址
    inline constexpr int        kMaxEntities         = 2048;
    // 单次整表拷贝的安全块大小
    inline constexpr size_t     kEntityBlockSize     = 0x200;

    // ============================================================
    //  模块签名（CreateMove 等挂钩点用）
    // ============================================================
    inline constexpr char kSigCreateMove[] =
        "\x8B\x0B\x8D\x4C\x24\x04";   // 视实际 client.dll 校准

    // ============================================================
    //  RTTI 主虚表 RVA（实体分类用，比模型名字符串稳）
    // ============================================================
    inline constexpr uintptr_t kSmokerVtableRva  = 0x60DB24;
    inline constexpr uintptr_t kBoomerVtableRva  = 0x60D454;
    inline constexpr uintptr_t kHunterVtableRva  = 0x60E1F4;
    inline constexpr uintptr_t kJockeyVtableRva  = 0x60F664;
    inline constexpr uintptr_t kSpitterVtableRva = 0x60EF94;
    inline constexpr uintptr_t kChargerVtableRva = 0x60EAB4;
    inline constexpr uintptr_t kTankVtableRva    = 0x60DA84;
    inline constexpr uintptr_t kWitchVtableRva   = 0x60C61C;
    inline constexpr uintptr_t kCommonVtableRva  = 0x60BC24;
    inline constexpr uintptr_t kPlayerVtableRva  = 0x5B6494;
} // namespace off

// ============================================================
//  裸内存访问原语（唯一允许的"函数"形态：不做逻辑，只做安全读写）
// --------------------------------------------------------
//  全部走 SEH 包裹 —— 注入环境下实体指针可能随时失效，
//  没有 SEH 的一次越界读就会把整个游戏带崩。
// ============================================================
namespace Mem
{
    // 值版安全读：Mem::Read<T>(addr[, fallback])
    // ⚠ 故意不提供 bool Read(addr, T&) 重载 —— 它与值版对左值实参
    //   会产生不可解决的重载歧义（两者都是 identity 转换）。
    //   需要区分"读到 0"还是"读失败"的场景一律用 ReadBlock。
    template <typename T>
    inline T Read(uintptr_t addr, T fallback = T{})
    {
        if (!addr) return fallback;
        __try {
            return *reinterpret_cast<T*>(addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return fallback;
        }
    }

    template <typename T>
    inline bool Write(uintptr_t addr, const T& v)
    {
        if (!addr) return false;
        __try {
            *reinterpret_cast<T*>(addr) = v;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // 批量拷贝：一次 SEH 覆盖整块，失败则调用方逐槽兜底
    inline bool ReadBlock(uintptr_t addr, void* dst, size_t size)
    {
        if (!addr || !dst || !size) return false;
        __try {
            memcpy(dst, reinterpret_cast<const void*>(addr), size);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
} // namespace Mem

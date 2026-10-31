#pragma once
// ============================================================
//  SDK/NetVars.h —— 动态偏移获取（**只读环境层**）
// ------------------------------------------------------------
//  【为什么必须有这一层】
//  硬编码偏移随客户端更新漂移。实测踩过的坑：
//     Offsets::m_lifeState = 0x147
//  在当前 client.dll 上对**活着的**感染者读出 1/2，于是
//  "lifeState != 0 就拒绝" 这道门槛把**所有感染者全拒掉**，
//  表现为「透视能看见框但自瞄毫无反应」。
//
//  RecvTable 是引擎自己描述网络字段的数据结构，**与版本无关**，
//  所以凡是易漂移的字段都必须走这里动态解析，硬编码只做兜底。
//
//  【循环依赖处理】
//  本文件依赖 Interfaces.h（要用 VClient016 遍历 ClientClass）。
//  反向**绝对不允许**：NetVars 不得被 Interfaces include。
//  这条边是单向的 —— SDK 内部 Interfaces → NetVars 单向链。
// ============================================================

#include <cstdint>

namespace NetVars
{
    // ============================================================
    //  解析结果表（一次性填充，之后恒为只读）
    // ============================================================
    struct Offsets
    {
        // ---- DT_BaseEntity / CBasePlayer ----
        uintptr_t viewOffset       = 0;   // m_vecViewOffset[0]
        uintptr_t punchOffset      = 0;   // m_aimPunchAngle
        uintptr_t punchLocal       = 0;   // [批73] m_Local.m_vecPunchAngle
                                          //   相机真正叠加的那份（去后坐力写它）
        uintptr_t punchVel         = 0;   // [批74] m_Local.m_vecPunchAngleVel
                                          //   抖动的真正驱动源：只清角度不清
                                          //   速度，下一 tick 又积分回来
        uintptr_t activeWeapon     = 0;   // m_hActiveWeapon
        uintptr_t modelIndex       = 0;   // m_nModelIndex
        uintptr_t lifeState        = 0;   // m_lifeState  ← 关键，见上方注释
        uintptr_t isGhost          = 0;   // m_isGhost

        // ---- 幸存者侧 ----
        uintptr_t myRecipient      = 0;   // m_hMyRecipient（抓住我的特感）
        uintptr_t classId          = 0;   // m_nClass（被控制时非 0）

        // ---- 武器 ----
        uintptr_t weaponId         = 0;   // m_iWeaponId（DT_TerrorWeapon）
        uintptr_t clip1            = 0;   // m_iClip1（DT_BaseCombatWeapon）
                                          //   [批62] 药品保护用：枪械有弹匣，
                                          //   消耗品（药丸/医疗包/除颤器）没有
                                          //   （引擎给无弹匣武器填 -1）

        // ---- [批5] 碰撞盒 / 姿态 / 命中盒 set（瞄准模块姿态自适应用）----
        uintptr_t collisionMins = 0;   // m_Collision.m_vecMins（子表递归解析）
        uintptr_t collisionMaxs = 0;   // m_Collision.m_vecMaxs（子表递归解析）
        uintptr_t duckAmount    = 0;   // m_flDuckAmount（全表扫描）
        uintptr_t hitboxSet     = 0;   // m_nHitboxSet（全表扫描）

        // ---- 全局 ----
        uintptr_t viewMatrix       = 0;   // 视图矩阵在 client.dll 里的存放地址
        uintptr_t globalVariables  = 0;   // Global_Variables*
    };

    // 解析全部 netvar。返回 false 表示 VClient016 拿不到（其余全部失败）。
    bool Init();

    // 访问解析结果（只读）
    const Offsets& Get();

    // ============================================================
    //  单项便捷读取 —— **带回退**，这是本层最重要的一处设计
    // ------------------------------------------------------------
    //  为什么必须"解析优先、硬编码兜底"而不是二选一：
    //    · 全都用硬编码 → 版本一更新就集体失效，且现象诡异
    //      （上面 m_lifeState 的例子：不是崩溃，是静默拒收全部目标）
    //    · 全都用 netvar → 万一某个 netvar 在此版本改名/移除，
    //      连兜底都没有，同样静默失效
    //  所以两条路并存：netvar 拿到就用，解析失败才用硬编码，
    //  并且解析失败要在日志里明确写出来（否则无从追查）。
    // ============================================================
    uintptr_t ViewOffset();
    uintptr_t PunchOffset();
    uintptr_t PunchLocal();      // [批73] m_Local.m_vecPunchAngle，0 = 未解析
    uintptr_t PunchVel();        // [批74] m_Local.m_vecPunchAngleVel，0 = 未解析
    uintptr_t ActiveWeapon();
    uintptr_t ModelIndex();
    uintptr_t LifeState();       // ★ 失效过的那一项
    uintptr_t IsGhost();         // ★ 同上
    uintptr_t MyRecipient();
    uintptr_t ClassId();
    uintptr_t WeaponId();
    uintptr_t Clip1();           // [批62] m_iClip1，0 = 未解析到（调用方须区分）

    // ---- 自检报告：把"哪些解析到了、哪些回退了"一次说清楚 ----
    struct Report
    {
        int  resolved = 0;      // 走 netvar 的项数
        int  fallback = 0;      // 回退硬编码的项数
        const char* fallbackNames[16] = {};
        int  fallbackCount = 0;
    };

    // 自检报告：把"哪些解析到了、哪些回退了"一次说清楚。
    // 放在 struct 外 —— 它返回 Report 本身，不是 Offsets 的成员。
    Report Check();
} // namespace NetVars

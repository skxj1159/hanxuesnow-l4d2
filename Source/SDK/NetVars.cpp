// ============================================================
//  SDK/NetVars.cpp —— netvar 动态解析实现
// ------------------------------------------------------------
//  【为什么必须解析而不是硬编码】
//  硬编码偏移随客户端更新漂移。实测踩过的坑：m_lifeState=0x147
//  在当前 client.dll 上对活着的感染者读出 1/2，收录门槛把感染者
//  全拒，表现为「透视正常但自瞄毫无反应」。
//
//  【数据源】client.dll 的 VClient016 → GetAllClasses（虚表槽 7）
//  → RecvTable 递归查属性偏移。与旧系统 AimCore2::ResolveNetvars
//  同一套表名/属性名（那边已实测解析成功），此处独立成 SDK 层。
//
//  【回退策略】"解析优先、硬编码兜底"双向并存：
//    · netvar 解析到 → 用 netvar
//    · 解析失败 → 用硬编码（可能失效，但聊胜于无）
//    · 两条路都失败 → 访问器返回 0，调用方自行跳过该字段
//  Check() 把结果一次说清楚，落 payload.log。
// ============================================================

#include "NetVars.h"
#include <atomic>   // [批153] g_resolved
#include <mutex>    // [批153] Resolve 串行化
#include "Offsets.h"

#include "../Netvar.h"
#include "../PayloadControl.h"

// ⚠ 头文件 SDK/NetVars.h 的 namespace 是【全局 NetVars】（不是 sdk::NetVars），
//   与 SDK/Offsets.h 的全局 Mem 同风格；Entity.cpp 里的非限定调用
//   NetVars::LifeState() 解析到的就是这一份。
namespace NetVars
{
    static Offsets g_off;
    // [批153] 跨线程安全：g_resolved 用原子，Resolve 用互斥体串行。
    //   崩溃日志实锤：游戏线程（CreateMove→读 m_Local punch）与渲染线程
    //   （LiveEsp→读 m_nModelIndex）会【同时】进 Resolve() 遍历客户端类表，
    //   一边写 g_off 一边读 → 撕裂出半个指针 → 读实体内存时 AV（access=null）。
    static std::atomic<bool> g_resolved{ false };
    static std::mutex        g_resolveMx;
    static Report  g_report;

    // ---- 一次性解析（幂等）----
    static void Resolve()
    {
        g_resolved = true;

        const netvar::ClientClassX* head = netvar::GetAllClasses();
        if (!head)
        {
            PayloadLog("[sdk-netvar] WARN: GetAllClasses 失败（VClient016 不可用），"
                       "全部字段回退硬编码偏移");
            return;
        }

        // ---- CBasePlayer 表（玩家系基类，AimCore2 已验证可解析）----
        for (const netvar::ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            if (!cc->m_pNetworkName || _stricmp(cc->m_pNetworkName, "CBasePlayer") != 0)
                continue;
            if (!cc->m_pRecvTable) break;

            g_off.viewOffset   = netvar::FindPropR(cc->m_pRecvTable, "m_vecViewOffset[0]", 0);
            g_off.activeWeapon = netvar::FindPropR(cc->m_pRecvTable, "m_hActiveWeapon", 0);
            g_off.modelIndex   = netvar::FindPropR(cc->m_pRecvTable, "m_nModelIndex", 0);
            g_off.myRecipient  = netvar::FindPropR(cc->m_pRecvTable, "m_hMyRecipient", 0);
            g_off.classId      = netvar::FindPropR(cc->m_pRecvTable, "m_nClass", 0);
            static const char* kPunch[] = { "m_vecPunchAngle", "m_aimPunchAngle", "m_vecAimPunch" };
            for (const char* n : kPunch)
            {
                g_off.punchOffset = netvar::FindPropR(cc->m_pRecvTable, n, 0);
                if (g_off.punchOffset) break;
            }

            // [批73] m_Local.m_vecPunchAngle —— L4D2 相机实际用的那份。
            //  这里先在 CBasePlayer 表里试；找不到再走下面的全表扫描
            //  （实测 L4D2 的 CBasePlayer 表里没有 m_Local，必须全扫）。
            for (int i = 0; i < cc->m_pRecvTable->m_nProps; ++i)
            {
                const netvar::RecvPropX* prop = &cc->m_pRecvTable->m_pProps[i];
                if (!prop->m_pVarName || !prop->m_pDataTable ||
                    _stricmp(prop->m_pVarName, "m_Local") != 0)
                    continue;
                g_off.punchLocal = (uintptr_t)prop->m_Offset +
                    netvar::FindPropR(prop->m_pDataTable, "m_vecPunchAngle", 0);
                break;
            }
            break;
        }

        // ---- [批5] 碰撞盒偏移（DT_BaseEntity → m_Collision 子表递归）----
        //  与 EntityBounds 的解析同源（m_Collision 挂在基类表，需递归）。
        //  供瞄准模块做姿态自适应比例降级（蹲/爬实时网络同步）与最终门控。
        for (const netvar::ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            if (!cc->m_pRecvTable || !cc->m_pNetworkName ||
                _stricmp(cc->m_pNetworkName, "CBaseEntity") != 0)
                continue;

            const netvar::RecvTableX* tbl = cc->m_pRecvTable;
            for (int i = 0; i < tbl->m_nProps; ++i)
            {
                const netvar::RecvPropX* prop = &tbl->m_pProps[i];
                if (!prop->m_pVarName ||
                    _stricmp(prop->m_pVarName, "m_Collision") != 0 ||
                    !prop->m_pDataTable)
                    continue;
                g_off.collisionMins = (uintptr_t)prop->m_Offset +
                    netvar::FindPropR(prop->m_pDataTable, "m_vecMins", 0);
                g_off.collisionMaxs = (uintptr_t)prop->m_Offset +
                    netvar::FindPropR(prop->m_pDataTable, "m_vecMaxs", 0);
                break;
            }
            break;
        }

        // ---- 全表扫描项（同名属性全链只有一份，谁先命中用谁）----
        for (const netvar::ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            if (!cc->m_pRecvTable) continue;
            if (!g_off.weaponId)
                g_off.weaponId = netvar::FindPropR(cc->m_pRecvTable, "m_iWeaponId", 0);
            // [批62] 弹匣。m_iClip1 挂在 DT_BaseCombatWeapon，全表扫描即可命中
            //   （同名属性全链只有一份）。药品保护靠"无弹匣"识别消耗品。
            if (!g_off.clip1)
                g_off.clip1 = netvar::FindPropR(cc->m_pRecvTable, "m_iClip1", 0);
            if (!g_off.lifeState)
                g_off.lifeState = netvar::FindPropR(cc->m_pRecvTable, "m_lifeState", 0);
            if (!g_off.isGhost)
                g_off.isGhost = netvar::FindPropR(cc->m_pRecvTable, "m_isGhost", 0);
            if (!g_off.duckAmount)
                g_off.duckAmount = netvar::FindPropR(cc->m_pRecvTable, "m_flDuckAmount", 0);
            if (!g_off.hitboxSet)
                g_off.hitboxSet = netvar::FindPropR(cc->m_pRecvTable, "m_nHitboxSet", 0);
        }

        // ---- [批75] m_Local 里的 punch（递归查找）----
        //  L4D2 的层级是  localdata(0) → m_Local(6724) → m_vecPunchAngle(112)
        //  也就是说 m_Local 不是类的顶层属性，而是嵌在 localdata 子表里。
        //  之前只遍历顶层 → 永远扫不到 → 一直在写实体上那份网络副本，
        //  屏幕当然照抖（日志里表现为这两个名字一直停在"回退"列表）。
        //
        //  这里递归下钻任意深度的子表，累积偏移后与子表内的属性偏移相加。
        //  同时解析 m_vecPunchAngleVel：抖动是速度积分出来的，
        //  只清角度不清速度，下一 tick 又被积回来。
        {
            for (const netvar::ClientClassX* cc = head; cc && (!g_off.punchLocal || !g_off.punchVel);
                 cc = cc->m_pNext)
            {
                if (!cc->m_pRecvTable) continue;
                if (!g_off.punchLocal)
                    g_off.punchLocal = netvar::DigProp(cc->m_pRecvTable, "m_Local",
                                                       "m_vecPunchAngle", 0, 0);
                if (!g_off.punchVel)
                    g_off.punchVel = netvar::DigProp(cc->m_pRecvTable, "m_Local",
                                                     "m_vecPunchAngleVel", 0, 0);
            }
        }

        // ---- 自检报告 ----
        struct Entry { uintptr_t v; const char* name; };
        const Entry entries[] =
        {
            { g_off.viewOffset,   "m_vecViewOffset" },
            { g_off.punchOffset,  "punchAngle" },
            { g_off.activeWeapon, "m_hActiveWeapon" },
            { g_off.modelIndex,   "m_nModelIndex" },
            { g_off.lifeState,    "m_lifeState" },
            { g_off.isGhost,      "m_isGhost" },
            { g_off.myRecipient,  "m_hMyRecipient" },
            { g_off.classId,      "m_nClass" },
            { g_off.weaponId,     "m_iWeaponId" },
            { g_off.clip1,        "m_iClip1" },
            { g_off.punchLocal,   "m_Local.m_vecPunchAngle" },
            { g_off.punchVel,     "m_Local.m_vecPunchAngleVel" },
        };
        for (const Entry& e : entries)
        {
            if (e.v) { ++g_report.resolved; continue; }
            if (g_report.fallbackCount < 16)
                g_report.fallbackNames[g_report.fallbackCount++] = e.name;
            ++g_report.fallback;
        }

        PayloadLog("[sdk-netvar] resolved=%d fallback=%d viewOffset=0x%X lifeState=0x%X "
                   "isGhost=0x%X activeWeapon=0x%X weaponId=0x%X myRecipient=0x%X",
                   g_report.resolved, g_report.fallback,
                   (unsigned)g_off.viewOffset, (unsigned)g_off.lifeState,
                   (unsigned)g_off.isGhost, (unsigned)g_off.activeWeapon,
                   (unsigned)g_off.weaponId, (unsigned)g_off.myRecipient);
        for (int i = 0; i < g_report.fallbackCount; ++i)
            PayloadLog("[sdk-netvar]   回退硬编码: %s", g_report.fallbackNames[i]);
    }

    static void Ensure()
    {
        if (g_resolved.load(std::memory_order_acquire))
            return;                                    // 快路径：无锁
        std::lock_guard<std::mutex> lk(g_resolveMx);
        if (!g_resolved.load(std::memory_order_relaxed))
            Resolve();                                 // 慢路径：同一时刻只有一个线程
    }

    // ============================================================
    //  对外接口
    // ============================================================

    bool Init()
    {
        Ensure();
        return g_off.lifeState != 0;   // 生命线字段：它挂了整个收录门槛失效
    }

    const Offsets& Get()
    {
        Ensure();
        return g_off;
    }

    // ---- 单项便捷读取（解析优先 → 硬编码兜底 → 0=两条路都失败）----

    uintptr_t ViewOffset()   { Ensure(); return g_off.viewOffset   ? g_off.viewOffset   : off::m_vecViewOffset; }
    uintptr_t PunchOffset()  { Ensure(); return g_off.punchOffset; }
    uintptr_t PunchLocal()   { Ensure(); return g_off.punchLocal; }
    uintptr_t PunchVel()     { Ensure(); return g_off.punchVel; }
    uintptr_t ActiveWeapon() { Ensure(); return g_off.activeWeapon ? g_off.activeWeapon : off::m_hActiveWeapon; }
    uintptr_t ModelIndex()   { Ensure(); return g_off.modelIndex   ? g_off.modelIndex   : off::m_nModelIndex; }
    uintptr_t LifeState()    { Ensure(); return g_off.lifeState    ? g_off.lifeState    : off::m_lifeState; }
    uintptr_t IsGhost()      { Ensure(); return g_off.isGhost      ? g_off.isGhost      : off::m_isGhost; }
    uintptr_t MyRecipient()  { Ensure(); return g_off.myRecipient  ? g_off.myRecipient  : off::m_hMyRecipient; }
    uintptr_t ClassId()      { Ensure(); return g_off.classId; }   // 无硬编码兜底
    uintptr_t WeaponId()     { Ensure(); return g_off.weaponId; }  // 无硬编码兜底
    uintptr_t Clip1()        { Ensure(); return g_off.clip1; }      // 同上

    Report Check()
    {
        Ensure();
        return g_report;
    }
} // namespace NetVars

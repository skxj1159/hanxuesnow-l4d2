#pragma once
// ============================================================
//  Features/Aimbot.h —— 自瞄（**功能层**，Legit + PSilent 架构）
// ------------------------------------------------------------
//  【架构】2026-10-18 重构：抛弃 Storm 瞄准思路
//  （Candidate 优先级管线 / SilentStrafe / MoveIntent 全部作废），
//  改为两模式：
//    · Legit  —— 平滑视觉：每 tick 把 cmd->viewangles 向目标缓动
//                一小步，并 SetViewAngles 同步本地视角（看得见的平滑）。
//    · PSilent —— 微静默（仅开火帧生效）：
//                开火 && 视觉角↔目标角差 ≤5° && 鼠标增量在人类可达范围
//                  → cmd->viewangles = 完美角（服务器吃静默角），
//                    本地视角不动，下一帧自动恢复（发包角==本地角）。
//                开火 && 差 >5° → 微静默不生效，本帧走平滑视觉。
//                非开火帧 → cmd 原样透传（发包角恒等于本地角）。
//    两模式共享「选靶 / 可见性 / 瞄准点」求解器，只差最后的应用层。
//
//  【依赖方向】同 ESP.h，向下依赖 SDK / Config。
//
//  【CUserCmd 循环依赖处理 —— 三层防护不变】
//  ① CUserCmd 字段薄壳在 SDK/Cmd.h（零引擎依赖，偏移与引擎逐字节比对过）
//  ② Hooks.cpp 只透传裸指针（批1 装配）
//  ③ 本文件 include 的 SDK/Cmd.h 不拖引擎头链
//
//  【冻结边界】本模块只【只读消费】GetFrameSnapshot 快照、Globals 与
//  sdk::EntityList，不写透视模块（DrawEsp/LiveEsp/Visibility...）任何文件。
// ============================================================

#include "../SDK/Vector.h"
#include "../SDK/Entity.h"
#include "../SDK/Cmd.h"
#include "../Config/Config.h"

namespace Aimbot
{
    // [批21] 自瞄配置持久化（独立 aim_config.ini，不侵占 ESP 配置结构）
    bool AimConfigSave();   // [批52] 返回 bool：菜单要据此给出真实成败提示
    void AimConfigLoad();

    // [批30] 渲染线程：刷新骨骼缓存（引擎骨骼取数只在此发生）
    void RefreshBonesForFrame();

    // [批36] 本 tick 是否写过 cmd 角度（Hook 据此决定返回 false：
    //  返回 false = 引擎不把 cmd 角应用到视角 = 静默的基础）
    bool WroteCmdAngles();

    // ============================================================
    //  [批116] 已删除 ApplyNoRecoilVis（SetViewAngles 输入层补偿）
    // ------------------------------------------------------------
    //  实测结论（payload.log 转折点）：改 ViewSetup 有效、改 SetViewAngles
    //  对本地屏幕无效 —— 输入层补偿只影响发包，屏幕照抖。
    //  去后坐力现在只有一条路径：OverrideView 里用锁定的权威 punch
    //  直接减最终视角（Hooks.cpp），未锁定时退回估算。
    // ============================================================

    // ============================================================
    //  唯一对外接口 —— Hooks.cpp 的 Hooked_CreateMove 只调这一个
    // ------------------------------------------------------------
    //  pCmd = 引擎 CreateMove 填充完毕的 CUserCmd（透传）。
    //  合法性（command_number/tick_count 非零的哑调用过滤）由本函数
    //  内部自检；未启用/无目标时直接 return，cmd 原样透传。
    // ============================================================
    void CreateMove(sdk::CUserCmd* pCmd);

    // ============================================================
    //  [批94] 读引擎视角角（= 玩家真实视角，静默/平滑两模式都成立）
    // ------------------------------------------------------------
    //  用途：OverrideView 里估算画面后坐力时做参照角。
    //  · 平滑模式：引擎视角 = 平滑后的瞄准角
    //  · 静默模式：引擎视角 = 玩家输入角（cmd 角是目标角，不能用）
    //  出参为 {pitch, yaw, roll}（度）；返回 false = 引擎接口不可用。
    // ============================================================
    bool ReadViewAngles(float out[3]);

    // ============================================================
    //  选靶结果
    // ============================================================
    struct Target
    {
        int         index   = -1;      // 实体索引
        uintptr_t   entity  = 0;       // 实体指针
        sdk::Vector aimPos{};          // 最终瞄准点（头盒中心 / origin+64）
        sdk::Vector origin{};          // [批6] 快照 origin（aimPos 高度偏移的基准）
        float       fovDeg  = 0.0f;    // 与准星的角度距离（度）
        int         health  = 0;
        bool        visible = false;   // TraceRay 可见性
        int         sid     = -1;      // [批161] Storm 分类 id（兜底锚点按 sid 选）
        int         tickSeq = -1;      // [批164] 胜者解析时的 tick 序号（复用判据）
    };

    // ============================================================
    //  求解器
    // ============================================================
    //  在 FOV 圆锥内选最佳目标（快照白名单同源）：
    //    遍历快照 → 分类白名单/存活/队伍过滤 → FOV 粗筛 →
    //    评分（角度距 + distWeight×3D 距离 + 迟滞优势）→
    //    Top-4 精修（头盒 hitbox0 + 休眠检查）→ 多点可见性 →
    //    评分序首个可见者胜出。无有效目标返回 false。
    //  fovDeg = 本 tick 有效 FOV（开镜时由调用方收窄后传入）。
    bool GetBestTarget(const sdk::Vector& eyePos, float fovDeg, Target& out);

    // TraceRay 单射线可见性 [批112]：只忽略本地玩家（ignoreA），
    // targetEnt 参与射线 —— 未命中任何东西，或命中的就是 targetEnt
    // （任意部位）均判可见；打到其它实体 = 被挡。nullptr = 纯畅通检测。
    bool TraceVisible(const sdk::Vector& from, const sdk::Vector& to,
                      void* ignoreA, void* targetEnt);
    bool TraceVisible(const sdk::Vector& from, const sdk::Vector& to,
                      void* ignoreEnt);

    // 多点可见性：头/胸/双肩/髋/双膝七采样，返回【命中的点数】。
    //   0 = 全被挡；≥1 = 至少露一点。外层按命中数给评分降权。
    // [批165] maxHits 控制早停：只需要"可不可见"时传 1（1 条射线即停），
    //   需要命中数时传 4（默认）。两档共用同一套采样点。
    // [批128] 不再改写 aimPos —— 可见性只回答"打不打"，瞄哪由骨骼决定。
    int  MultiPointVisible(const sdk::Vector& eyePos,
                           const sdk::Vector& origin,
                           sdk::Vector& aimPos,
                           void* ignoreA, void* targetEnt,
                           int maxHits = 4);

    // [批134] 宽限期的防穿墙判定：射线命中【世界几何】(m_pEnt==nullptr)
    //   → 被墙挡住 → 立刻放弃宽限；命中其它实体或没命中 → 保留宽限。
    static bool TraceBlockedByWorld(const sdk::Vector& from, const sdk::Vector& to,
                                    void* ignoreA);

    // 当前视角前向单位向量（ViewEngine 槽位自探测就绪后可用）
    bool GetViewForward(sdk::Vector& fwd);

    // FOV 范围圈（渲染线程调用：屏中心圆 = FOV 圆锥的屏幕投影，
    // 帮助确认目标是否在范围内；开关 g_Config.aim.showFov）
    void RenderFovRing();

    // ============================================================
    //  配置映射（沿用 Config/Config.h 现有字段，不新增散落全局）
    //    g_Config.aim.enabled / key / fov（legit 建议 3~5°）/ psilent
    //    g_Config.humanize.smoothFactor（平滑）
    //    g_Config.humanize.reactionMs / reactionMsMax（反应延迟）
    //    g_Config.humanize.jitterAngle（微抖动幅度，度）
    // ============================================================
}

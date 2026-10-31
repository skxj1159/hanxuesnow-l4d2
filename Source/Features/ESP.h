#pragma once
// ============================================================
//  Features/ESP.h —— 透视绘制（**功能层**）
// ------------------------------------------------------------
//  【依赖方向（本层是循环依赖处理的样板）】
//      SDK/Entity.h      ┐
//      SDK/Vector.h      ├─→ Features/ESP.h ─→（被）Hooks/Hooks.cpp
//      Config/Config.h   ┤
//      Render/Render2D.h ┘
//
//  ESP.h **可以** include Config.h 与 SDK/Entity.h（向下依赖合法）。
//  反过来 Config.h / SDK/*.h 绝不能 include 本文件 —— 那才成环。
//
//  【为什么这样就无环】
//  依赖图是一张有向无环图（DAG）。分层编号即拓扑序：
//      L0  Config   （只依赖标准库）
//      L1  SDK      （依赖 L0 + 标准库；Config 不反向依赖 SDK）
//      L2  Render   （依赖 L0/L1）
//      L3  Features （依赖 L0/L1/L2）
//      L4  Hooks    （依赖 L3）
//      L5  main.cpp （依赖 L4）
//  边只从低编号指向高编号 → 无环。
// ============================================================

#include "../SDK/Vector.h"
#include "../SDK/Entity.h"
#include "../Config/Config.h"

namespace ESP
{
    // ============================================================
    //  唯一对外接口
    // ------------------------------------------------------------
    //  Hooked_PaintTraverse 内部**只**调这一个函数，
    //  不碰任何实体枚举、不碰配置、不碰绘制原语。
    // ============================================================
    void Render();

    // ============================================================
    //  几何来源（内部使用，暴露出来是为了 EntityBounds 复用）
    // ------------------------------------------------------------
    //  priority: 1 = 命中盒并集 → 2 = 骨骼 AABB → 3 = 碰撞盒
    //  ⚠ 第①级失败**必须**降级到第②/③级：本机VModelInfoClient
    //    取不到是环境常态（日志里 5 次初始化全是 0x0），
    //    不降级会导致整个透视一个框都不画。
    struct Bounds
    {
        bool  ok      = false;
        int   source  = 0;      // 1/2/3
        float left    = 0.0f, right  = 0.0f;
        float top     = 0.0f, bottom = 0.0f;
    };

    // 世界坐标 → 屏幕矩形
    bool GetScreenBounds(const sdk::CBaseEntity& e, Bounds& out);

    // ============================================================
    //  过滤判据 —— 集中放这里，避免"画的"和"打的"口径不一致
    // ============================================================
    bool ShouldDraw(const sdk::EntityInfo& info, const sdk::EntityState& state);

    // 该实体该用什么语义色（Storm 配色：三档硬编码）
    //   特感= 255,128,0 / 普通小僵尸 = 255,0,0 / 坦克·女巫 = 0,128,255
    //   幸存者 = 0,255,0 / 道具 = 255,255,255 / 幽灵 = 128,128,128
    //  幽灵态一律灰化 —— 不管它原本是什么颜色。
    struct SemanticColor { unsigned char r, g, b; };
    SemanticColor ColorFor(const sdk::EntityInfo& info, const sdk::EntityState& state);

    // ============================================================
    //  距离（米）
    // ------------------------------------------------------------
    //  ⚠ L4D2 1 米 ≈ 52.49 世界单位。
    //  所有"米 ↔ 世界单位"换算都必须过这里，
    //  否则会出现"菜单设60 米、实际只当 1.1 米"的量纲错误
    //  （踩过一次：阈值直接平方，差 52 倍）。
    // ============================================================
    inline constexpr float kUnitsPerMeter = 52.49f;
    float DistanceMeters(const sdk::Vector& a, const sdk::Vector& b);
}

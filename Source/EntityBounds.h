#pragma once
// ============================================================
// EntityBounds —— 实体世界包围盒 + 屏幕矩形（ESP 唯一几何来源）
// ------------------------------------------------------------
// 取代旧链路「骨骼 AABB → 回退 origin±72 固定身高」。固定身高是方框
// 无法自适应的根因：72 是标准人形高度，Jockey（矮）框比人大出一圈、
// Tank（巨大）框比人小一大半。
//
// 包围盒三级策略（成本/精度递增，按序取用）：
//   1) 骨骼 AABB：本帧骨骼矩阵求世界 AABB，姿态贴合（蹲/扑/骑乘）。
//      仅当骨骼数量与盒高同时通过校验才采信——部分实体骨骼矩阵残缺时
//      会算出偏小的盒子，此时降级到第 2 级，避免"框只包住下半身"。
//   2) 碰撞盒（DT_CollisionProperty::m_vecMins/m_vecMaxs）：服务端按
//      模型调 UTIL_SetSize 后随实体网络同步，每个 SI 模型不同 → Jockey /
//      Tank / 普通感染者 / 幸存者天然各取各的盒，无需任何体型系数表。
//      偏移按 netvar 名递归解析（Netvar.h），不写死数值。
//   3) 两者皆失败：不绘制该实体。宁可不画，也不用错误尺寸的框覆盖。
//
// 屏幕矩形：世界 AABB 的 8 个角点逐个投影后取 min/max（透视下投影后的
// 屏幕矩形与"世界宽高比推算"不等价，斜视时后者明显偏窄）。部分角点在
// 相机后时按近裁剪面钳制后继续取包围，不整框丢弃，避免贴边目标闪烁。
// ============================================================

#include "Include.h"

struct EntityInfo;
struct EntityState;
class CView;

struct ScreenRect
{
    float left, top, right, bottom;
    float width()  const { return right - left; }
    float height() const { return bottom - top; }
    float centerX() const { return (left + right) * 0.5f; }
    float centerY() const { return (top + bottom) * 0.5f; }
};

// 世界包围盒（成功返回 true；两层来源都不可用时返回 false）
bool GetEntityWorldBounds(uintptr_t ent, const EntityInfo& info,
                          Vec3& outMin, Vec3& outMax,
                          bool allowBoneSources = true);   // [批158] 速度/距离门控

// 屏幕矩形（依赖本帧视图缓存；几何来源 GetEntityWorldBounds）
bool GetEntityScreenRect(const EntityInfo& info, const EntityState& state, ScreenRect& out);

// [批111·死代码] 删除了 `bool EntityBoundsCollisionReady();`
//   原注释写"供诊断日志"，但全项目零调用点——没有任何日志用它。
//   碰撞盒 netvar 的解析状态改由 GetStormBounds 等函数内部的
//   EnsureCollisionOffsets() 自行保证，无需对外暴露查询口。

// ============================================================
//  命中盒屏幕线框（Storm Paint.hpp 同款几何，逐盒而非整体）
// ------------------------------------------------------------
//  世界 AABB 只有一个 8 角框，横躺/扑击时明显大于实际轮廓；命中盒云
//  把它拆成 N 个贴合姿态的 OBB，逐盒投影后连线，视觉上就是"贴着模型
//  的骨架框"。这里只做投影，缓存 / 解析全部复用 HitboxSystem 的帧缓存。
//
//  角点位序：bit0 → x，bit1 → y，bit2 → z（1 取 bbmax）。据此连线时，
//  两个角点下标只差一个 bit 即为一条棱，共 12 条。
// ============================================================

// 单个命中盒的屏幕投影结果
struct HitboxQuad
{
    Vec2     pt[8] = {};   // 8 个角点的屏幕坐标（无效角点内容无意义）
    uint8_t  okMask = 0;   // 第 c 位为 1 表示 pt[c] 投影成功
    int      group = 0;    // HITGROUP_*（用于分色）
    int      visible = 0;  // 投影有效的角点数达阈值才画（避免贴边时半框乱飞）
};

// 取实体的命中盒屏幕线框集合。
//   origin：实体世界原点（防呆基准）
//   out / maxOut：输出数组与容量
// 返回实际写入的框数；0 表示命中盒不可用（调用方应回退 ScreenRect 画法）
int GetEntityHitboxQuads(const EntityInfo& info, const EntityState& state,
                         HitboxQuad* out, int maxOut);

// ============================================================
//  Storm Paint.hpp 原生几何：全部命中盒 8 角投影后取并集框
// ------------------------------------------------------------
//  与 GetEntityScreenRect 的区别（不是优化，是不同的数学）：
//    ScreenRect：先在**世界空间**求 AABB，再投影其8 角 —— 透视下
//                屏幕矩形与"世界宽高比推算"不等价。
//    本函数：对**每个命中盒的每个角点**直接 WorldToScreen，
//           累加屏幕 min/max（Storm Paint.hpp:82-192 的原做法）。
//           差别在横躺/扑击时更明显：世界 AABB 的 8 角里有一半
//           落在模型空处，投影后会撑出比实体大得多的框。
//  角点在相机后时按近裁剪面钳制后继续参与取包围（Storm 是整框放弃，
//  那会让贴边目标闪烁）。
// ============================================================

// Storm 风格的屏幕并集框
struct StormBounds
{
    float left   = 0.0f;
    float right  = 0.0f;
    float top    = 0.0f;
    float bottom = 0.0f;
    int   boxes  = 0;      // 参与求并集的命中盒数（诊断/标签用）
    bool  ok     = false;
};

// 取并集框。pad 为每边外扩像素（Storm 用 2.0）。
bool GetStormBounds(const EntityInfo& info, float pad, StormBounds& out);
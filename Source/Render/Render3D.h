#pragma once
// ============================================================
//  Render/Render3D.h —— 3D 绘制封装（**渲染层**）
// ------------------------------------------------------------
//  【目标形态 vs 现状 —— 同 Render2D.h】
//  目标：封装 IVDebugOverlay 画 3D 骨骼与 OBB。
//  现状：IVDebugOverlay 在内部模式下**不可靠**（它的 AddLineOverlay
//        走独立的材质通道，在 EndScene hook 里调用会出现闪烁/不刷新，
//        且需要先设 sv_debugoverrides）。所以骨骼与 OBB 实际走
//        Render2D 的 Line3D（世界 → 屏幕投影后画屏幕线），
//        效果等价且稳定。
//
//  本文件保留 IVDebugOverlay 的接口形态，便于将来真接上。
//
//  【与 Render2D 的分工】
//  两者都吃世界坐标，但：
//    Render2D::Line3D —— 单条线的世界→屏幕投影（骨骼用）
//    Render3D::DrawOBB —— 整个盒的8 角 + 12 棱的批量绘制
//  Render3D 依赖 Render2D（单向：3D → 2D）。
// ============================================================

#include "../SDK/Vector.h"

namespace Render3D
{
    // ============================================================
    //  生命周期
    // ============================================================
    bool Init();       // 尝试取 IVDebugOverlay；失败自动降级到屏幕线
    void Shutdown();
    bool UsingDebugOverlay();   // 诊断用：当前走的哪条路

    // ============================================================
    //  基础图元
    // ============================================================
    void Line(const sdk::Vector& a, const sdk::Vector& b, unsigned int color);
    void AxisAlignedBox(const sdk::Vector& min, const sdk::Vector& max,
                        unsigned int color);
    void Sphere(const sdk::Vector& center, float radius, unsigned int color);

    // ============================================================
    //  OBB（有向包围盒）—— 骨骼 AABB 的通用形态
    // ============================================================
    //  用matrix3x4（骨骼变换）+ 盒 min/max 表达，
    //  这样能贴合模型当前姿态，而不是死板的世界轴对齐盒。
    struct OBB
    {
        sdk::Matrix3x4 transform{};
        sdk::Vector min{};
        sdk::Vector max{};
    };
    void DrawOBB(const OBB& box, unsigned int color);

    // ============================================================
    //  骨骼
    // ------------------------------------------------------------
    //  骨骼索引到「父-子」线段的映射表。L4D2 的骨骼表与 CS:GO
    //  同源，但**不同模型版本的骨骼数量不同**，所以这里不写死
    //  索引，而是由实现层按当前骨骼云的实际情况连线。
    // ============================================================
    struct BoneLink { int a, b; };
    void DrawBones(const sdk::Matrix3x4* bones, int count,
                   const BoneLink* links, int linkCount, unsigned int color);

    // ============================================================
    //  世界 → 屏幕投影（3D 层内部使用，也供 Features 做判定）
    // ------------------------------------------------------------
    //  返回 false = 点在相机后方。
    bool Project(const sdk::Vector& world, sdk::Vector& screen);

    // 取当前视图矩阵（来自 netvar 解析的 client.dll 全局）
    const sdk::Matrix4x4& ViewMatrix();

    // 屏幕尺寸
    void ScreenSize(int& w, int& h);
}

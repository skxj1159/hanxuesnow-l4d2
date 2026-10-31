#pragma once
// ============================================================
//  Render/Render2D.h —— 2D 绘制封装（**渲染层**）
// ------------------------------------------------------------
//  【目标形态 vs 现状 —— 必须写清楚，否则后人会照着目标写然后踩坑】
//  目标：封装 ISurface 的 DrawBox / DrawText / DrawHealthBar。
//  现状：本项目实际走 **OS-ImGui**（内部模式，DX9 EndScene hook），
//        不是引擎的 ISurface。原因见下方「为什么不用 ISurface」。
//
//  【为什么不用 ISurface】
//  ① 注入环境下纹理/字体加载失败会直接黑屏或变黑块；
//  ② 项目已有自绘 ImGui 框架（菜单 862 行 + 中文字表 + 皮肤），
//     换ISurface 等于重写一遍绘制栈；
//  ③ 外部图片资源一律不引入，背景用 DrawList 程序化绘制。
//
//  所以 Render2D.h 做的是**接口形态对齐、实现后端可换**：
//  现在转发到 OS-ImGui，将来若要换 ISurface，只改本文件即可，
//  Features 层一行不用动。
// ============================================================

#include "../SDK/Vector.h"
#include "../Config/Config.h"

namespace Render2D
{
    // ============================================================
    //  生命周期
    // ============================================================
    //  FrameBegin/FrameEnd 必须在同一个绘制上下文内成对调用，
    //  跨帧不配对会让 ImGui 的绘制列表错乱（表现为整个界面花掉）。
    bool FrameBegin();
    void FrameEnd();

    // ============================================================
    //  基本图元
    // ============================================================
    void Line     (const sdk::Vector& a, const sdk::Vector& b, unsigned int color, float thickness = 1.0f);
    void Rect     (float l, float t, float r, float b, unsigned int color, float rounding = 0.0f);
    void RectFilled(float l, float t, float r, float b, unsigned int color, float rounding = 0.0f);
    void Circle   (float cx, float cy, float radius, unsigned int color, int segments = 0);
    void CircleFilled(float cx, float cy, float radius, unsigned int color, int segments = 0);
    void Line3D    (const sdk::Vector& a, const sdk::Vector& b, unsigned int color);

    // ============================================================
    //  文字
    // ------------------------------------------------------------
    //  size 用逻辑像素。font 缺字时返回 false 并在日志里报
    //  ——中文字表漏字会显示成一片 ???，必须能被上层发现。
    // ============================================================
    bool Text(const sdk::Vector& pos, const char* text,
              unsigned int color, float size = 14.0f);
    bool TextCentered(float cx, float y, const char* text,
                      unsigned int color, float size = 14.0f);
    float TextWidth(const char* text, float size = 14.0f);

    // ============================================================
    //  复合控件（对齐菜单控件的观感）
    // ============================================================
    void DrawBox      (float l, float t, float r, float b, unsigned int color, float thickness = 1.0f);
    void DrawHealthBar(float x, float y, float height, float ratio, float width = 4.0f);
    void DrawDistance(float x, float y, float meters, unsigned int color, float size = 12.0f);

    // ============================================================
    //  颜色 —— Storm 语义色（三档硬编码，不可调）
    // ============================================================
    namespace Color
    {
        inline constexpr unsigned int kSpecial   = 0xFF0080FFu;  // 特感 255,128,0
        inline constexpr unsigned int kCommon    = 0xFF0000FFu;  // 小僵尸 255,0,0
        inline constexpr unsigned int kHeavy     = 0xFFFF8000u;  // 坦克·女巫 0,128,255
        inline constexpr unsigned int kSurvivor  = 0xFF00FF00u;  // 幸存者 0,255,0
        inline constexpr unsigned int kProp      = 0xFFFFFFFFu;  // 道具 255,255,255
        inline constexpr unsigned int kGhost     = 0xFF808080u;  // 幽灵 128,128,128
        inline constexpr unsigned int kEdge      = 0xFF000000u;  // 描边

        // 带 alpha 的颜色构造
        inline unsigned int WithAlpha(unsigned int rgb, unsigned char a)
        {
            return (rgb & 0x00FFFFFFu) | (static_cast<unsigned int>(a) << 24);
        }
    }
}

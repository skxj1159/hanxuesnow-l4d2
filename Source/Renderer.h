#pragma once
#include <algorithm>   // std::clamp / std::min（本头文件直接用，避免依赖包含顺序）
#include "Include.h"

// HealthBar
inline void DrawVerticalHealthBar(Vec2 pos, float height, float ratio, float width = 4.0f) {
    Vec2 size(width, height);
    Gui.RectangleFilled(pos, size, ImColor(0, 0, 0, 200));

    constexpr int kSteps = 40;
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    if (ratio <= 0.0f) return;   // 空血：只保留背景板

    // 只绘制填充段：段号 j 自底向上（j 越大越靠上），最高段按
    // j = ratio*kSteps 直接算出（与原"t > ratio 逐段跳过"视觉等价）；
    // 段数随血量比例下降，低血量时不再空转全部 40 次判断
    // （不用 std::min：windows.h 的 min 宏会破坏限定名调用）
    int lastJ = static_cast<int>(ratio * kSteps);
    if (lastJ > kSteps - 1) lastJ = kSteps - 1;
    const float segmentHeight = height / kSteps;

    for (int j = 0; j <= lastJ; ++j) {
        const float t = static_cast<float>(j) / kSteps;
        const float y = pos.y + height * (1.0f - t);
        Vec2 segmentPos(pos.x, y - segmentHeight);
        Vec2 segmentSize(width, segmentHeight);

        ImColor color = (t < 0.5f)
            ? ImColor(255, static_cast<int>(255 * t * 2), 0)
            : ImColor(static_cast<int>(255 * (1 - t) * 2), 255, 0);

        Gui.RectangleFilled(segmentPos, segmentSize, color);
    }
}
//2dbox cutline
inline void Draw2DCornerBox(Vec2 topLeft, Vec2 size, ImColor color, float thickness = 1.5f, float ratio = 0.2f) {
    float lw = size.x * ratio, lh = size.y * ratio;
    float x = topLeft.x, y = topLeft.y;
    float x2 = x + size.x, y2 = y + size.y;

    Gui.Line({ x, y }, { x + lw, y }, color, thickness);
    Gui.Line({ x, y }, { x, y + lh }, color, thickness);
    Gui.Line({ x2, y }, { x2 - lw, y }, color, thickness);
    Gui.Line({ x2, y }, { x2, y + lh }, color, thickness);
    Gui.Line({ x, y2 }, { x + lw, y2 }, color, thickness);
    Gui.Line({ x, y2 }, { x, y2 - lh }, color, thickness);
    Gui.Line({ x2, y2 }, { x2 - lw, y2 }, color, thickness);
    Gui.Line({ x2, y2 }, { x2, y2 - lh }, color, thickness);
}
//imgui
// Preview
inline void DrawCornerBoxImGui(ImVec2 topLeft, ImVec2 size, ImColor color, float thickness = 1.5f, float ratio = 0.2f)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    float lw = size.x * ratio;
    float lh = size.y * ratio;

    float x = topLeft.x, y = topLeft.y;
    float x2 = x + size.x, y2 = y + size.y;

    drawList->AddLine({ x, y }, { x + lw, y }, color, thickness);
    drawList->AddLine({ x, y }, { x, y + lh }, color, thickness);

    drawList->AddLine({ x2, y }, { x2 - lw, y }, color, thickness);
    drawList->AddLine({ x2, y }, { x2, y + lh }, color, thickness);

    drawList->AddLine({ x, y2 }, { x + lw, y2 }, color, thickness);
    drawList->AddLine({ x, y2 }, { x, y2 - lh }, color, thickness);

    drawList->AddLine({ x2, y2 }, { x2 - lw, y2 }, color, thickness);
    drawList->AddLine({ x2, y2 }, { x2, y2 - lh }, color, thickness);
}

// ============================================================
//  PreviewEsp.cpp —— ESP 预览窗
// ------------------------------------------------------------
//  2026-10-18 随透视体系换成 Storm 而重写：预览的是 Storm 的实际画法
//  （半透明填充 + 黑外框 + 黑内框 + 主色框、标签贴右侧），颜色用
//  Storm 语义色。旧的 4 种样式与 24 个可调颜色预览已删除。
// ============================================================

#include "Include.h"
#include "Globals.h"
#include "PreviewESP.h"
#include "Renderer.h"
#include "MenuFonts.h"   // g_fontEsp：预览文字与真实 ESP 同字体（18px 常规雅黑）

// Storm 语义色（与 DrawEsp.cpp 的 StormPaintColor 保持一致）
namespace PrevColor
{
    inline const ImColor Special  (255, 128, 0);
    inline const ImColor Survivor (0, 255, 0);
}

static inline ImColor HalfOf(const ImColor& c)
{
    const ImVec4 v = c;
    return IM_COL32((int)(v.x * 0.5f), (int)(v.y * 0.5f), (int)(v.z * 0.5f), 128);
}

void DrawEspPreviewWindow()
{
    if (!showEspPreview) return;

    ImVec2 fixedSize(150, 250);
    ImGui::SetNextWindowSize(fixedSize);
    ImGui::SetNextWindowSizeConstraints(fixedSize, fixedSize);
    ImGui::Begin("ESP 预览", &showEspPreview, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    // 与真实 ESP 同字体（Gui.Text/CalcTextSize 都吃当前字体）
    if (Neverlus::g_fontEsp)
        ImGui::PushFont(Neverlus::g_fontEsp);

    ImVec2 canvasPos   = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(canvasPos,
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        IM_COL32(40, 40, 40, 255));

    const float boxWidth = 24.0f, boxHeight = 56.0f, spacing = 56.0f;
    const float barW = 4.0f;

    const float boxLeft = canvasPos.x + canvasSize.x * 0.5f - boxWidth * 0.5f;

    // 两个样例：上方 = 特感（橙），下方 = 幸存者（绿）
    struct Sample { float x, y; ImColor color; float hpRatio; const char* hp; const char* dist; };
    const Sample samples[2] = {
        { boxLeft, canvasPos.y + 16.0f,
          PrevColor::Special,  0.70f, "175", "12.5m" },
        { boxLeft, canvasPos.y + 16.0f + boxHeight + spacing,
          PrevColor::Survivor, 0.90f, "90", "5.2m" },
    };

    for (int i = 0; i < 2; ++i)
    {
        const Sample& s = samples[i];
        const ImVec2 tl(s.x, s.y);
        const ImVec2 br(s.x + boxWidth, s.y + boxHeight);
        const ImVec2 center((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);

        // ---- Storm 方框四层 ----
        if (enableBox2D)
        {
            drawList->AddRectFilled(ImVec2(tl.x + 2, tl.y + 2), ImVec2(br.x - 2, br.y - 2),
                                    HalfOf(s.color));
            drawList->AddRect(ImVec2(tl.x - 1, tl.y - 1), ImVec2(br.x + 1, br.y + 1),
                              IM_COL32(0, 0, 0, 255), 1.0f);
            drawList->AddRect(ImVec2(tl.x + 1, tl.y + 1), ImVec2(br.x - 1, br.y - 1),
                              IM_COL32(0, 0, 0, 255), 1.0f);
            drawList->AddRect(tl, br, s.color, 1.0f);
        }

        // ---- 血条（贴左，保留 ours 的双层）----
        if (enableHealthBar)
        {
            const ImVec2 slotTL(s.x - 2.0f - barW, tl.y);
            const ImVec2 slotBR(s.x - 2.0f, br.y);
            drawList->AddRectFilled(slotTL, slotBR, IM_COL32(0, 0, 0, 150));
            const float h = (slotBR.y - slotTL.y) * s.hpRatio;
            drawList->AddRectFilled(ImVec2(slotTL.x + 1, slotBR.y - h),
                                    ImVec2(slotBR.x - 1, slotBR.y), s.color);
        }

        // ---- 血量数字 ----
        if (enableHealthESP)
        {
            const ImVec2 sz = ImGui::CalcTextSize(s.hp);
            drawList->AddText(ImVec2(s.x - 2.0f - barW - 6.0f - sz.x,
                                     center.y - sz.y * 0.5f), s.color, s.hp);
        }

        // ---- 名字标签（Storm 位置：右侧顶部，带底框）----
        if (enableESPName)
        {
            const char* nm = (i == 0) ? "Smoker" : "Nick";
            const ImVec2 sz = ImGui::CalcTextSize(nm);
            const ImVec2 tl2(br.x + 3.0f, tl.y);
            const ImVec2 br2(tl2.x + sz.x + 8.0f, tl2.y + sz.y + 4.0f);

            drawList->AddRectFilled(ImVec2(tl2.x + 2, tl2.y + 2),
                                    ImVec2(br2.x - 2, br2.y - 2), HalfOf(s.color));
            drawList->AddRect(ImVec2(tl2.x - 1, tl2.y - 1),
                              ImVec2(br2.x + 1, br2.y + 1), IM_COL32(0, 0, 0, 255), 1.0f);
            drawList->AddRect(tl2, br2, s.color, 1.0f);
            drawList->AddText(ImVec2(tl2.x + 4.0f, tl2.y + 2.0f), s.color, nm);
        }

        // ---- 距离 ----
        if (enableDistance)
        {
            const ImVec2 sz = ImGui::CalcTextSize(s.dist);
            drawList->AddText(ImVec2(center.x - sz.x * 0.5f, br.y + 2.0f), s.color, s.dist);
        }
    }

    const float sepY = samples[1].y + boxHeight + spacing * 0.35f;
    drawList->AddLine(ImVec2(canvasPos.x + 10, sepY),
                      ImVec2(canvasPos.x + canvasSize.x - 10, sepY),
                      IM_COL32(80, 80, 80, 180), 1.0f);

    ImGui::Dummy(canvasSize);
    if (Neverlus::g_fontEsp)
        ImGui::PopFont();
    ImGui::End();
}
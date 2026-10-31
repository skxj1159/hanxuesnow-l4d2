#pragma once

// ============================================================
// EuiNeo.h —— EUI-NEO 主题的最小保留集（header-only）
// ------------------------------------------------------------
// 菜单视觉已整体迁移到 HanSnowUI.h（寒山雪主题，自带色板与控件），
// 本文件原 770 行控件库（开关/滑条/下拉/色块/按钮/卡片/图标等）
// 已无调用方，全部删除。现仅保留外部初始化链路仍在使用的三个符号：
//   · g_titleFont   —— 24px 中文标题字体（OS-ImGui_Internal 注册，
//                      HanSnowUI::TitleFont() 复用同一字库）
//   · SetTitleFont  —— 注册入口
//   · ApplyStyle    —— DX9 ImGui 初始化时的一次性基线样式
//                      （菜单首帧 DrawMenu→HanSnowUI::EnsureStyle
//                      会以寒山雪色板覆盖为最终值）
// ============================================================

#include "Osimgui/Imgui/imgui.h"
#include "Osimgui/Imgui/imgui_internal.h"

#include <cstdio>

namespace EuiNeo
{
    // ---- 标题字体（24px；由 OS-ImGui_Internal.cpp 初始化时注册）----
    inline ImFont* g_titleFont = nullptr;

    inline void SetTitleFont(ImFont* font) { g_titleFont = font; }

    // ---- 初始基线样式（暗色；仅为菜单首帧前的兜底值）----
    inline void ApplyStyle()
    {
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();

        st.WindowRounding    = 14.0f;
        st.ChildRounding     = 10.0f;
        st.PopupRounding     = 10.0f;
        st.FrameRounding     = 8.0f;
        st.GrabRounding      = 6.0f;
        st.ScrollbarRounding = 8.0f;
        st.TabRounding       = 8.0f;
        st.WindowBorderSize  = 0.0f;
        st.ChildBorderSize   = 0.0f;
        st.FrameBorderSize   = 0.0f;
        st.PopupBorderSize   = 1.0f;
        st.ScrollbarSize     = 8.0f;
        st.GrabMinSize       = 12.0f;
        st.WindowPadding     = ImVec2(16.0f, 16.0f);
        st.FramePadding      = ImVec2(12.0f, 7.0f);
        st.ItemSpacing       = ImVec2(10.0f, 8.0f);
        st.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);

        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg]             = ImVec4(0.102f, 0.102f, 0.122f, 0.980f);   // rgb(26,26,31)
        c[ImGuiCol_PopupBg]              = ImVec4(0.133f, 0.133f, 0.157f, 0.980f);   // rgb(34,34,40)
        c[ImGuiCol_Border]               = ImVec4(1.0f, 1.0f, 1.0f, 0.078f);
        c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_Text]                 = ImVec4(0.933f, 0.933f, 0.949f, 1.0f);     // rgb(238,238,242)
        c[ImGuiCol_TextDisabled]         = ImVec4(0.455f, 0.455f, 0.502f, 1.0f);     // rgb(116,116,128)
        c[ImGuiCol_FrameBg]              = ImVec4(0.188f, 0.188f, 0.220f, 1.0f);     // rgb(48,48,56)
        c[ImGuiCol_FrameBgHovered]       = ImVec4(0.212f, 0.212f, 0.243f, 1.0f);     // rgb(54,54,62)
        c[ImGuiCol_FrameBgActive]        = ImVec4(0.329f, 0.329f, 0.369f, 1.0f);     // rgb(84,84,94)
        c[ImGuiCol_Header]               = ImVec4(0.220f, 0.274f, 0.386f, 1.0f);
        c[ImGuiCol_HeaderHovered]        = ImVec4(0.212f, 0.212f, 0.243f, 1.0f);
        c[ImGuiCol_HeaderActive]         = ImVec4(0.329f, 0.329f, 0.369f, 1.0f);
        c[ImGuiCol_Button]               = ImVec4(0.188f, 0.188f, 0.220f, 1.0f);
        c[ImGuiCol_ButtonHovered]        = ImVec4(0.212f, 0.212f, 0.243f, 1.0f);
        c[ImGuiCol_ButtonActive]         = ImVec4(0.329f, 0.329f, 0.369f, 1.0f);
        c[ImGuiCol_Separator]            = ImVec4(1.0f, 1.0f, 1.0f, 0.078f);
        c[ImGuiCol_SeparatorHovered]     = ImVec4(0.220f, 0.443f, 0.878f, 1.0f);     // rgb(56,113,224)
        c[ImGuiCol_SeparatorActive]      = ImVec4(0.220f, 0.443f, 0.878f, 1.0f);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.302f, 0.302f, 0.333f, 1.0f);     // rgb(77,77,85)
        c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.400f, 0.400f, 0.433f, 1.0f);
        c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.220f, 0.443f, 0.878f, 1.0f);
        c[ImGuiCol_CheckMark]            = ImVec4(0.220f, 0.443f, 0.878f, 1.0f);
        c[ImGuiCol_SliderGrab]           = ImVec4(0.220f, 0.443f, 0.878f, 1.0f);
        c[ImGuiCol_SliderGrabActive]     = ImVec4(0.306f, 0.514f, 0.902f, 1.0f);     // rgb(78,131,230)
    }
}

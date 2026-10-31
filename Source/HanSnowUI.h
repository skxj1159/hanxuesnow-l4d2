#pragma once

// ============================================================
// HanSnowUI.h —— 寒山雪（HanS Snow）皮肤主题与控件库（header-only）
// ------------------------------------------------------------
// 按寒山雪设计规范实现（黑底 + 冰蓝的设计语言）：
//   · 窗口 940×640：标题栏 54 / 顶部横排页签栏 44 / 内容区 / 底栏 46
//   · 卡片：圆角 10、底 #10161F、描边 #1D242E；组头左侧标题 + 右侧计数
//   · 数值一律输入框（无滑块、无范围提示）：InputFloat/InputInt +
//     单位后缀 + 越界 Clamp 回滚
//   · 开关：34×20 胶囊（ImDrawList 自绘，标签右侧紧跟 16px）
//   · 依赖：默认中文字体 18px（msyh 等，OS-ImGui_Internal 初始化）+
//     24px 标题字体（EuiNeo::g_titleFont，同一字库复用）
// 本文件只做视觉与控件，不含任何游戏逻辑。
// ============================================================

#include "Osimgui/Imgui/imgui.h"
#include "Osimgui/Imgui/imgui_internal.h"   // ImDrawList::ChannelsSplit（卡片分层绘制）
#include "EuiNeo.h"                          // 复用 24px 标题字体（EuiNeo::g_titleFont）

#include <cstdio>
#include <cstring>
#include <cmath>

namespace HanSnowUI
{
    // ========================================================
    // 调色板（寒山雪设计 token → IM_COL32）
    // ========================================================
    struct Snow
    {
        ImU32 windowBg;      // 窗口底 #0B0F14
        ImU32 barBg;         // 标题栏 / 页签栏 / 底栏底 #0D131A
        ImU32 cardBg;        // 卡片底 #10161F
        ImU32 cardBorder;    // 卡片描边 #1D242E
        ImU32 primary;       // 冰蓝主色 #6FC3FF
        ImU32 primaryText;   // 主色亮字 #A9DDFF
        ImU32 primaryDim;    // 页签选中底 #132B3F
        ImU32 text;          // 主文字 #E8EEF5
        ImU32 textLabel;     // 行标签 #C6D2DE
        ImU32 textMuted;     // 次级文字 #97A3B4
        ImU32 textFaint;     // 弱化文字 #5E6B7A
        ImU32 track;         // 开关轨道（关）#1C242E
        ImU32 knob;          // 开关旋钮（白）
        ImU32 hover;         // 行悬停 #141C26
        ImU32 active;        // 行按下 #182230
        ImU32 fieldBg;       // 输入框 / 下拉底 #0E141C
        ImU32 fieldBorder;   // 输入框描边 #222B36
        ImU32 popupBg;       // 弹出层 #0E141C
        ImU32 good;          // 成功 / 状态点 #49DE9C
        ImU32 danger;        // 危险 #FF6B6B
        ImU32 sep;           // 分隔线 #1A212B
    };

    inline const Snow& P()
    {
        static const Snow s = [] {
            Snow p{};
            p.windowBg     = IM_COL32(11, 15, 20, 248);
            p.barBg        = IM_COL32(13, 19, 26, 252);
            p.cardBg       = IM_COL32(16, 22, 31, 255);
            p.cardBorder   = IM_COL32(29, 36, 46, 255);
            p.primary      = IM_COL32(111, 195, 255, 255);
            p.primaryText  = IM_COL32(169, 221, 255, 255);
            p.primaryDim   = IM_COL32(19, 43, 63, 255);
            p.text         = IM_COL32(232, 238, 245, 255);
            p.textLabel    = IM_COL32(198, 210, 222, 255);
            p.textMuted    = IM_COL32(151, 163, 180, 255);
            p.textFaint    = IM_COL32(94, 107, 122, 255);
            p.track        = IM_COL32(28, 36, 46, 255);
            p.knob         = IM_COL32(240, 246, 252, 255);
            p.hover        = IM_COL32(20, 28, 38, 255);
            p.active       = IM_COL32(24, 34, 48, 255);
            p.fieldBg      = IM_COL32(14, 20, 28, 255);
            p.fieldBorder  = IM_COL32(34, 43, 54, 255);
            p.popupBg      = IM_COL32(14, 20, 28, 252);
            p.good         = IM_COL32(73, 222, 156, 255);
            p.danger       = IM_COL32(255, 107, 107, 255);
            p.sep          = IM_COL32(26, 33, 43, 255);
            return p;
        }();
        return s;
    }

    inline ImVec4 Col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

    inline int MixChan(int a, int b, float t)
    {
        int v = (int)(a + (b - a) * t + 0.5f);
        return v < 0 ? 0 : (v > 255 ? 255 : v);
    }

    inline ImU32 Mix(ImU32 a, ImU32 b, float t)
    {
        return IM_COL32(
            MixChan((int)(a & 0xFF), (int)(b & 0xFF), t),
            MixChan((int)((a >> 8) & 0xFF), (int)((b >> 8) & 0xFF), t),
            MixChan((int)((a >> 16) & 0xFF), (int)((b >> 16) & 0xFF), t),
            MixChan((int)((a >> 24) & 0xFF), (int)((b >> 24) & 0xFF), t));
    }

    // 取 "文字##id" 的显示部分
    inline const char* LabelText(const char* label)
    {
        if (!label) return "";
        const char* p = strstr(label, "##");
        if (!p) return label;
        static char buf[192];
        size_t n = (size_t)(p - label);
        if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, label, n);
        buf[n] = '\0';
        return buf;
    }

    // ========================================================
    // 全局样式（一次应用；幂等）
    // ========================================================
    inline int g_applied = 0;   // 0=未应用 1=已应用

    inline void EnsureStyle()
    {
        if (g_applied) return;
        g_applied = 1;

        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();

        st.WindowRounding    = 10.0f;
        st.ChildRounding     = 10.0f;
        st.PopupRounding     = 8.0f;
        st.FrameRounding     = 6.0f;
        st.GrabRounding      = 4.0f;
        st.ScrollbarRounding = 6.0f;
        st.WindowBorderSize  = 0.0f;
        st.ChildBorderSize   = 0.0f;
        st.FrameBorderSize   = 0.0f;
        st.PopupBorderSize   = 1.0f;
        st.ScrollbarSize     = 6.0f;
        st.GrabMinSize       = 10.0f;
        st.WindowPadding     = ImVec2(18.0f, 18.0f);
        st.FramePadding      = ImVec2(10.0f, 5.0f);
        st.ItemSpacing       = ImVec2(10.0f, 8.0f);
        st.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);

        const Snow& p = P();
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg]             = Col(p.windowBg);
        c[ImGuiCol_PopupBg]              = Col(p.popupBg);
        c[ImGuiCol_Border]               = Col(p.cardBorder);
        c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_Text]                 = Col(p.text);
        c[ImGuiCol_TextDisabled]         = Col(p.textFaint);
        c[ImGuiCol_FrameBg]              = Col(p.fieldBg);
        c[ImGuiCol_FrameBgHovered]       = Col(p.hover);
        c[ImGuiCol_FrameBgActive]        = Col(p.active);
        c[ImGuiCol_Header]               = Col(p.primaryDim);
        c[ImGuiCol_HeaderHovered]        = Col(p.hover);
        c[ImGuiCol_HeaderActive]         = Col(p.active);
        c[ImGuiCol_Button]               = Col(p.fieldBg);
        c[ImGuiCol_ButtonHovered]        = Col(p.hover);
        c[ImGuiCol_ButtonActive]         = Col(p.active);
        c[ImGuiCol_Separator]            = Col(p.sep);
        c[ImGuiCol_CheckMark]            = Col(p.primary);
        c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab]        = Col(p.track);
        c[ImGuiCol_ScrollbarGrabHovered] = Col(Mix(p.track, p.textMuted, 0.35f));
        c[ImGuiCol_ScrollbarGrabActive]  = Col(p.primary);
    }

    // ========================================================
    // 动画：128 槽线性缓存（零分配），指数趋近
    // ========================================================
    struct AnimSlot { ImGuiID key; float v; };
    inline AnimSlot g_anim[128] = {};

    inline float AnimTo(ImGuiID key, float target, float speed = 16.0f)
    {
        float dt = ImGui::GetIO().DeltaTime;
        if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
        const float k = 1.0f - expf(-speed * dt);

        int freeSlot = -1;
        for (int i = 0; i < 128; ++i)
        {
            if (g_anim[i].key == key)
            {
                float& v = g_anim[i].v;
                v += (target - v) * k;
                if (fabsf(target - v) < 0.0015f) v = target;
                return v;
            }
            if (freeSlot < 0 && g_anim[i].key == 0)
                freeSlot = i;
        }
        if (freeSlot >= 0)
        {
            g_anim[freeSlot].key = key;
            g_anim[freeSlot].v = target;
        }
        return target;
    }

    // 标题字体（24px，EuiNeo 初始化注册；未注册时回退默认字体）
    inline ImFont* TitleFont() { return EuiNeo::g_titleFont; }

    inline void TitleText(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text, float size)
    {
        if (ImFont* f = TitleFont())
            dl->AddText(f, size, pos, col, text);
        else
            dl->AddText(pos, col, text);
    }

    inline float TitleTextWidth(const char* text, float size)
    {
        if (ImFont* f = TitleFont())
            return f->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
        return ImGui::CalcTextSize(text).x;
    }

    // ========================================================
    // Logo：断口圆环 + 双山（寒山雪标记）
    // ========================================================
    inline void Logo(ImDrawList* dl, ImVec2 center, float r, ImU32 col)
    {
        // 外圆环：左上留一个 52° 断口
        const float gap = 0.9076f;                       // 52° in rad
        const float a0  = -1.5707963f + gap * 0.5f;      // 从断口右侧起
        const float a1  = -1.5707963f + 6.2831853f - gap * 0.5f;
        dl->PathArcTo(center, r, a0, a1, 28);
        dl->PathStroke(col, 0, 2.2f);
        // 双山：两座折线山，主峰偏右
        const float s = r * 0.62f;
        const ImVec2 m1[3] = { ImVec2(center.x - s, center.y + s * 0.55f),
                               ImVec2(center.x - s * 0.28f, center.y - s * 0.50f),
                               ImVec2(center.x + s * 0.10f, center.y + s * 0.30f) };
        const ImVec2 m2[3] = { ImVec2(center.x + s * 0.02f, center.y + s * 0.55f),
                               ImVec2(center.x + s * 0.55f, center.y - s * 0.05f),
                               ImVec2(center.x + s, center.y + s * 0.55f) };
        dl->AddPolyline(m1, 3, col, 0, 2.0f);
        dl->AddPolyline(m2, 3, col, 0, 2.0f);
    }

    // ========================================================
    // 线性图标（页签用，16px 网格）
    // ========================================================
    enum IconId
    {
        IconNone = 0,
        IconBox,       // 取景框（视觉 / ESP）
        IconAim,       // 准星（瞄准）
        IconBolt,      // 闪电（战斗）
        IconPalette,   // 调色盘（颜色）
        IconPower,     // 电源（注入控制）
    };

    inline void Icon(int id, ImDrawList* dl, ImVec2 c, float s, ImU32 col)
    {
        const float h = s * 0.5f;
        const float th = 1.6f;
        switch (id)
        {
        case IconBox:
            dl->AddRect(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, 3.0f, 0, th);
            dl->AddCircleFilled(c, 1.8f, col);
            break;
        case IconAim:
            dl->AddCircle(c, h * 0.55f, col, 0, th);
            dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x - h * 0.35f, c.y), col, th);
            dl->AddLine(ImVec2(c.x + h * 0.35f, c.y), ImVec2(c.x + h, c.y), col, th);
            dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y - h * 0.35f), col, th);
            dl->AddLine(ImVec2(c.x, c.y + h * 0.35f), ImVec2(c.x, c.y + h), col, th);
            break;
        case IconBolt:
        {
            const ImVec2 bolt[4] = { ImVec2(c.x + h * 0.25f, c.y - h),
                                     ImVec2(c.x - h * 0.45f, c.y + h * 0.12f),
                                     ImVec2(c.x + h * 0.05f, c.y + h * 0.12f),
                                     ImVec2(c.x - h * 0.25f, c.y + h) };
            dl->AddPolyline(bolt, 4, col, 0, th);
        }
        break;
        case IconPalette:
            dl->AddCircle(c, h * 0.92f, col, 0, th);
            dl->AddCircleFilled(ImVec2(c.x - h * 0.38f, c.y - h * 0.28f), 1.8f, col);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.10f, c.y - h * 0.42f), 1.8f, col);
            dl->AddCircleFilled(ImVec2(c.x + h * 0.40f, c.y + h * 0.10f), 1.8f, col);
            break;
        case IconPower:
        {
            const float a0 = -1.5707963f + 0.62f;
            const float a1 = -1.5707963f + 6.2831853f - 0.62f;
            dl->PathArcTo(c, h * 0.78f, a0, a1, 24);
            dl->PathStroke(col, 0, th);
            dl->AddLine(ImVec2(c.x, c.y - h), ImVec2(c.x, c.y - h * 0.15f), col, th);
        }
        break;
        default:
            break;
        }
    }

    // ========================================================
    // 卡片：圆角 10 分层绘制；组头左侧标题 + 右侧计数
    // ========================================================
    inline constexpr float kCardPad = 18.0f;   // 卡片内边距（左右）
    inline float g_rowInset = 0.0f;

    inline float RowWidth()
    {
        float w = ImGui::GetContentRegionAvail().x - g_rowInset;
        return (w > 1.0f) ? w : 1.0f;
    }

    inline void CardBegin(const char* id, const char* title)
    {
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PushID(id);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();

        if (title && title[0])
        {
            const ImVec2 tp = ImGui::GetCursorScreenPos();
            TitleText(dl, tp, P().text, title, 17.0f);
            const float th = TitleFont() ? 17.0f : ImGui::GetTextLineHeight();
            ImGui::Dummy(ImVec2(1.0f, th));
            ImGui::Dummy(ImVec2(1.0f, 8.0f));
        }
        ImGui::Indent(kCardPad);
        g_rowInset = kCardPad;
    }

    // countText：组头右侧计数（如 "1 / 2"、"6 项"）；可为 nullptr
    inline void CardEnd(const char* countText = nullptr)
    {
        // 组头右侧计数：画在卡片标题基线上（标题绘制时已预留右侧空间，
        // 这里用「卡片顶边 + 6px」对齐组头行）
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::Unindent(kCardPad);
        g_rowInset = 0.0f;
        ImGui::EndGroup();

        ImVec2 mn = ImGui::GetItemRectMin();
        ImVec2 mx = ImGui::GetItemRectMax();
        mn.x -= kCardPad;
        mx.x += kCardPad;
        mn.y -= 14.0f;      // 组头上内边距
        mx.y += 14.0f;      // 组尾下内边距

        const Snow& p = P();
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(mn, mx, p.cardBg, 10.0f);
        dl->AddRect(mn, mx, p.cardBorder, 10.0f);

        if (countText && countText[0])
        {
            const ImVec2 ts = ImGui::CalcTextSize(countText);
            dl->AddText(ImVec2(mx.x - 18.0f - ts.x, mn.y + 5.0f), p.textFaint, countText);
        }
        dl->ChannelsMerge();

        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
    }

    // ========================================================
    // 控件：胶囊开关（34×20；标签左，开关紧跟 16px；整行可点）
    // ========================================================
    inline bool Toggle(const char* label, bool* v)
    {
        const Snow& p = P();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = 30.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();

        bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        if (hovered || active)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), active ? p.active : p.hover, 6.0f);

        const float line = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, LabelText(label));

        // 胶囊（34×20）紧跟标签 16px
        const float tw = 34.0f, thh = 20.0f;
        const float labelW = ImGui::CalcTextSize(LabelText(label)).x;
        ImVec2 t0(pos.x + 8.0f + labelW + 16.0f, pos.y + (h - thh) * 0.5f);
        ImVec2 t1(t0.x + tw, t0.y + thh);

        const float anim = AnimTo(ImGui::GetID(label) ^ 0x9E3779B9u, *v ? 1.0f : 0.0f, 18.0f);
        dl->AddRectFilled(t0, t1, Mix(p.track, p.primary, anim), thh * 0.5f);

        const float margin = 3.0f;
        const float kr = (thh - margin * 2.0f) * 0.5f;
        const float kx = t0.x + margin + kr + anim * (tw - margin * 2.0f - kr * 2.0f);
        const float ky = t0.y + thh * 0.5f;
        dl->AddCircleFilled(ImVec2(kx, ky + 1.0f), kr, IM_COL32(0, 0, 0, 40));
        dl->AddCircleFilled(ImVec2(kx, ky), kr, p.knob);

        if (clicked)
            *v = !*v;
        return clicked;
    }

    // ========================================================
    // 控件：数值滑块行——标签左 + 右侧滑块（带单位后缀）
    // 越界自动 Clamp 回滚
    // ========================================================
    inline bool ValueFloat(const char* label, float* v, float lo, float hi,
                           const char* fmt = "%.2f", const char* unit = "px")
    {
        const Snow& p = P();
        const float w = RowWidth();
        const float h = 28.0f;
        const float line = ImGui::GetTextLineHeight();

        ImVec2 pos = ImGui::GetCursorScreenPos();
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, LabelText(label));
        }

        // 单位文本宽 + 滑块宽
        const float unitW = (unit && unit[0]) ? (ImGui::CalcTextSize(unit).x + 8.0f) : 0.0f;
        const float sliderW = 200.0f;
        const float right = pos.x + w - 8.0f;

        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(ImVec2(right - sliderW - unitW, pos.y));
        ImGui::SetNextItemWidth(sliderW);
        bool changed = ImGui::SliderFloat("##val", v, lo, hi, fmt,
                                          ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        if (unit && unit[0])
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float uy = pos.y + (h - line) * 0.5f;
            dl->AddText(ImVec2(right - ImGui::CalcTextSize(unit).x, uy), p.textFaint, unit);
        }

        // 占位整行高度，保证行悬停区域与文本行一致
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 2.0f));
        ImGui::Dummy(ImVec2(w, 2.0f));
        return changed;
    }

    inline bool ValueInt(const char* label, int* v, int lo, int hi, const char* unit = "ms")
    {
        const Snow& p = P();
        const float w = RowWidth();
        const float h = 28.0f;
        const float line = ImGui::GetTextLineHeight();

        ImVec2 pos = ImGui::GetCursorScreenPos();
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, LabelText(label));
        }

        const float unitW = (unit && unit[0]) ? (ImGui::CalcTextSize(unit).x + 8.0f) : 0.0f;
        const float sliderW = 200.0f;
        const float right = pos.x + w - 8.0f;

        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(ImVec2(right - sliderW - unitW, pos.y));
        ImGui::SetNextItemWidth(sliderW);
        bool changed = ImGui::SliderInt("##val", v, lo, hi, "%d",
                                        ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopID();
        if (unit && unit[0])
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const float uy = pos.y + (h - line) * 0.5f;
            dl->AddText(ImVec2(right - ImGui::CalcTextSize(unit).x, uy), p.textFaint, unit);
        }

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 2.0f));
        ImGui::Dummy(ImVec2(w, 2.0f));
        return changed;
    }

    // ========================================================
    // 控件：分段选择（方框样式 2D / 3D角 / 立体 这类 3~4 段切换）
    // ========================================================
    inline bool Segmented(const char* label, int* cur, const char* const items[], int count)
    {
        const Snow& p = P();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = 30.0f;
        const float line = ImGui::GetTextLineHeight();

        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, LabelText(label));

        // 分段条：右对齐，每段宽度自适应文本
        float segW[8];
        float total = 0.0f;
        for (int i = 0; i < count && i < 8; ++i)
        {
            segW[i] = ImGui::CalcTextSize(items[i]).x + 24.0f;
            total += segW[i];
        }
        float sx = pos.x + w - 8.0f - total;

        bool changed = false;
        const int idx = (*cur >= 0 && *cur < count) ? *cur : 0;
        for (int i = 0; i < count && i < 8; ++i)
        {
            ImVec2 b0(sx, pos.y), b1(sx + segW[i], pos.y + h);
            ImGui::PushID(i);
            bool clicked = ImGui::InvisibleButton("##seg", ImVec2(segW[i], h));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            if (i == idx)
            {
                dl->AddRectFilled(b0, b1, p.primaryDim, 6.0f);
                dl->AddRect(b0, b1, Mix(p.primary, p.cardBorder, 0.35f), 6.0f);
                dl->AddText(ImVec2(b0.x + (segW[i] - ImGui::CalcTextSize(items[i]).x) * 0.5f,
                                   pos.y + (h - line) * 0.5f), p.primaryText, items[i]);
            }
            else
            {
                if (hovered)
                    dl->AddRectFilled(b0, b1, p.hover, 6.0f);
                dl->AddText(ImVec2(b0.x + (segW[i] - ImGui::CalcTextSize(items[i]).x) * 0.5f,
                                   pos.y + (h - line) * 0.5f), p.textMuted, items[i]);
            }
            if (clicked) { *cur = i; changed = true; }
            sx += segW[i];
        }

        ImGui::Dummy(ImVec2(w, h + 4.0f));
        return changed;
    }

    // ========================================================
    // 控件：下拉框（自绘触发框 + 弹出列表）
    // ========================================================
    inline bool Combo(const char* label, int* cur, const char* const items[], int count)
    {
        const Snow& p = P();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = 30.0f;
        const float line = ImGui::GetTextLineHeight();

        ImVec2 pos = ImGui::GetCursorScreenPos();
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, LabelText(label));

        // 下拉框：右侧固定宽 168
        const float boxW = 168.0f;
        ImVec2 b0(pos.x + w - 8.0f - boxW, pos.y);
        ImVec2 b1(b0.x + boxW, b0.y + h);

        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(b0);
        bool clicked = ImGui::InvisibleButton("##combo", ImVec2(boxW, h));
        bool hovered = ImGui::IsItemHovered();
        if (clicked)
            ImGui::OpenPopup("##popup");   // 与 BeginPopup 同 ID（同一 PushID 作用域内）

        dl->AddRectFilled(b0, b1, hovered ? Mix(p.fieldBg, p.hover, 0.6f) : p.fieldBg, 6.0f);
        dl->AddRect(b0, b1, p.fieldBorder, 6.0f);

        const int idx = (*cur >= 0 && *cur < count) ? *cur : 0;
        dl->AddText(ImVec2(b0.x + 12.0f, b0.y + (h - line) * 0.5f), p.text, items[idx]);
        const float ax = b1.x - 16.0f;
        const float ay = b0.y + h * 0.5f;
        dl->AddTriangleFilled(ImVec2(ax - 4.5f, ay - 2.0f), ImVec2(ax + 4.5f, ay - 2.0f),
                              ImVec2(ax, ay + 3.0f), p.textMuted);

        bool changed = false;
        ImGui::SetNextWindowPos(ImVec2(b0.x, b1.y + 4.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(boxW, 0.0f), ImGuiCond_Always);
        if (ImGui::BeginPopup("##popup"))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
            for (int i = 0; i < count; ++i)
            {
                if (ImGui::Selectable(items[i], i == idx, 0, ImVec2(boxW, 26.0f)))
                {
                    *cur = i;
                    changed = true;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h + 4.0f));
        ImGui::Dummy(ImVec2(w, 2.0f));
        return changed;
    }

    // ========================================================
    // 控件：颜色行（标签左 + 十六进制 + 色块 18×18；点击弹出取色器）
    // ========================================================
    inline void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell)
    {
        dl->PushClipRect(a, b, true);
        const int rows = (int)ceilf((b.y - a.y) / cell);
        const int cols = (int)ceilf((b.x - a.x) / cell);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x)
            {
                ImU32 col = ((x + y) & 1) ? IM_COL32(255, 255, 255, 255) : IM_COL32(204, 204, 204, 255);
                ImVec2 c0(a.x + x * cell, a.y + y * cell);
                ImVec2 c1(ImMin(c0.x + cell, b.x), ImMin(c0.y + cell, b.y));
                dl->AddRectFilled(c0, c1, col);
            }
        dl->PopClipRect();
    }

    inline bool ColorRow(const char* id, const char* label, ImColor* color, bool withAlpha)
    {
        const Snow& p = P();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = 28.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();

        bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        if (clicked)
            ImGui::OpenPopup(id);
        if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), p.hover, 6.0f);

        const float line = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(pos.x + 8.0f, pos.y + (h - line) * 0.5f), p.textLabel, label);

        const float sw = 18.0f, sh = 18.0f;
        ImVec2 s0(pos.x + w - sw - 8.0f, pos.y + (h - sh) * 0.5f);
        ImVec2 s1(s0.x + sw, s0.y + sh);

        const ImVec4& c = color->Value;
        char hex[24];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 (int)(c.x * 255.0f + 0.5f), (int)(c.y * 255.0f + 0.5f), (int)(c.z * 255.0f + 0.5f));
        const float hw = ImGui::CalcTextSize(hex).x;
        dl->AddText(ImVec2(s0.x - hw - 12.0f, pos.y + (h - line) * 0.5f), p.textFaint, hex);

        Checker(dl, s0, s1, 6.0f);
        dl->AddRectFilled(s0, s1, ImGui::ColorConvertFloat4ToU32(c), 4.0f);
        dl->AddRect(s0, s1, p.fieldBorder, 4.0f);

        bool changed = false;
        ImVec2 popPos(s0.x - 250.0f, s1.y + 6.0f);
        if (popPos.x < 8.0f) popPos.x = 8.0f;
        ImGui::SetNextWindowPos(popPos, ImGuiCond_Always);
        if (ImGui::BeginPopup(id))
        {
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview;
            flags |= withAlpha
                ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
                : ImGuiColorEditFlags_NoAlpha;
            changed = ImGui::ColorPicker4("##snowPick", (float*)&color->Value, flags);
            ImGui::EndPopup();
        }
        return changed;
    }

    // ========================================================
    // 控件：按钮（主色 / 描边 / 幽灵 / 危险；size.x<=0 占满行宽）
    // ========================================================
    enum SnowButtonStyle
    {
        BtnPrimary = 0,   // 冰蓝底深字（主操作：保存配置）
        BtnOutline,       // 描边
        BtnGhost,         // 幽灵
        BtnDanger,        // 危险
    };

    inline bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, 32.0f), int style = BtnPrimary)
    {
        const Snow& p = P();
        const float w = (size.x > 0.0f) ? size.x : RowWidth();
        const float h = (size.y > 0.0f) ? size.y : 32.0f;
        ImVec2 pos = ImGui::GetCursorScreenPos();

        bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        bool active  = ImGui::IsItemActive();

        ImU32 bg = 0, border = 0, txt = 0;
        switch (style)
        {
        case BtnOutline:
            bg     = active ? p.active : (hovered ? p.hover : IM_COL32(0, 0, 0, 0));
            border = p.fieldBorder;
            txt    = hovered ? p.text : p.textMuted;
            break;
        case BtnGhost:
            bg     = active ? p.active : (hovered ? p.hover : IM_COL32(0, 0, 0, 0));
            txt    = hovered ? p.text : p.textMuted;
            break;
        case BtnDanger:
            bg     = active ? Mix(p.danger, IM_COL32(0, 0, 0, 255), 0.22f)
                            : (hovered ? Mix(p.danger, IM_COL32(255, 255, 255, 255), 0.10f) : Mix(p.danger, IM_COL32(0, 0, 0, 255), 0.12f));
            txt    = IM_COL32(255, 235, 235, 255);
            border = Mix(p.danger, IM_COL32(0, 0, 0, 255), 0.35f);
            break;
        default: // BtnPrimary
            bg     = active ? Mix(p.primary, IM_COL32(0, 0, 0, 255), 0.28f)
                            : (hovered ? Mix(p.primary, IM_COL32(255, 255, 255, 255), 0.14f) : p.primaryDim);
            txt    = p.primaryText;
            border = Mix(p.primary, p.cardBorder, 0.30f);
            break;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 b1(pos.x + w, pos.y + h);
        dl->AddRectFilled(pos, b1, bg, 7.0f);
        if (border)
            dl->AddRect(pos, b1, border, 7.0f);

        const char* disp = LabelText(label);
        const ImVec2 ts = ImGui::CalcTextSize(disp);
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), txt, disp);
        return clicked;
    }

    // ========================================================
    // 文本：说明（次要色自动换行）
    // ========================================================
    inline void Note(const char* text)
    {
        const Snow& p = P();
        ImGui::PushStyleColor(ImGuiCol_Text, Col(p.textMuted));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    inline void NoteFaint(const char* text)
    {
        const Snow& p = P();
        ImGui::PushStyleColor(ImGuiCol_Text, Col(p.textFaint));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // ========================================================
    // 小件：kbd 键帽（底栏快捷键提示）
    // ========================================================
    inline void Kbd(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 textColor, const Snow& p)
    {
        const ImVec2 ts = ImGui::CalcTextSize(text);
        const float padX = 7.0f, h = 22.0f;
        ImVec2 b1(pos.x + ts.x + padX * 2.0f, pos.y + h);
        dl->AddRectFilled(pos, b1, p.fieldBg, 5.0f);
        dl->AddRect(pos, b1, p.fieldBorder, 5.0f);
        dl->AddText(ImVec2(pos.x + padX, pos.y + (h - ts.y) * 0.5f), textColor, text);
    }

    inline float KbdWidth(const char* text)
    {
        return ImGui::CalcTextSize(text).x + 14.0f;
    }
}

#pragma once

#include "Osimgui/Imgui/imgui.h"
#include "Osimgui/Imgui/imgui_internal.h"
#include "MenuFonts.h"
#include "Osimgui/Imgui/imgui_impl_dx9.h"

#include <cstdio>
#include <cstring>
#include <cmath>

namespace Neverlus
{
    // ---- 主题色板 ----
    inline ImU32 PanelBg()    { return IM_COL32(15, 16, 20, 255); }      // 面板底
    inline ImU32 NavBg()      { return IM_COL32(20, 21, 28, 255); }      // 左侧导航
    inline ImU32 CardBg()      { return IM_COL32(23, 24, 31, 255); }      // 卡片
    inline ImU32 CardLine()    { return IM_COL32(36, 38, 47, 255); }      // 卡片描边
    inline ImU32 FieldBg()     { return IM_COL32(30, 31, 40, 255); }      // 输入 / 轨道
    inline ImU32 FieldLine()   { return IM_COL32(44, 46, 58, 255); }
    inline ImU32 Accent()      { return IM_COL32(130, 92, 255, 255); }    // 强调紫
    inline ImU32 AccentSoft()  { return IM_COL32(130, 92, 255, 46); }     // 强调底
    inline ImU32 Ink()         { return IM_COL32(232, 233, 239, 255); }   // 主文字
    inline ImU32 InkLabel()    { return IM_COL32(195, 197, 208, 255); }   // 行标签
    inline ImU32 InkMuted()    { return IM_COL32(122, 125, 140, 255); }   // 次文字
    inline ImU32 InkFaint()    { return IM_COL32(86, 88, 101, 255); }     // 弱文字
    inline ImU32 Good()        { return IM_COL32(73, 222, 156, 255); }
    inline ImU32 Danger()      { return IM_COL32(232, 89, 92, 255); }

    struct Snow
    {
        ImU32 windowBg, barBg, cardBg, cardBorder;
        ImU32 primary, primaryText, primaryDim;
        ImU32 text, textLabel, textMuted, textFaint;
        ImU32 track, knob, hover, active;
        ImU32 fieldBg, fieldBorder, popupBg;
        ImU32 good, danger, sep;
    };
    inline const Snow& P()
    {
        static const Snow s = {
            PanelBg(), NavBg(), CardBg(), CardLine(),
            Accent(), Ink(), AccentSoft(),
            Ink(), InkLabel(), InkMuted(), InkFaint(),
            FieldBg(), IM_COL32(255, 255, 255, 255), IM_COL32(38, 40, 52, 255),
            IM_COL32(48, 50, 64, 255),
            FieldBg(), FieldLine(), IM_COL32(26, 27, 35, 255),
            Good(), Danger(), CardLine()
        };
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

    // 逐帧趋近目标值的缓动（用于开关 / 悬停填充）
    inline float Ease(ImGuiID key, bool on, float rateOn = 14.0f, float rateOff = 9.0f)
    {
        static ImGuiID sKey[64] = {};
        static float   sVal[64] = {};
        const int slot = (int)(key & 63);
        if (sKey[slot] != key) { sKey[slot] = key; sVal[slot] = on ? 1.0f : 0.0f; }

        const float dt = ImMax(1.0f / 240.0f, ImGui::GetIO().DeltaTime);
        const float rate = on ? rateOn : rateOff;
        float v = sVal[slot] + ((on ? 1.0f : 0.0f) - sVal[slot]) * (1.0f - expf(-rate * dt));
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        sVal[slot] = v;
        return v;
    }

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

    // ---- 字体句柄 ----
    // 真正的加载在 MenuFonts.h::SetupMenuFonts()（必须在首帧 NewFrame 之前），
    // 这里只做句柄绑定：g_fontMini 已在那边赋值，g_fontBig / g_fontIcon 统一
    // 复用 16px 粗体——导航图标是纯 ASCII 字母（A/B/C/D/E），不需要单独字形集。
    inline ImFont* g_fontBig    = nullptr;
    inline ImFont* g_fontIcon   = nullptr;
    inline bool    g_fontsReady = false;

    inline constexpr float kRowH   = 34.0f;   // 单行控件高度
    inline constexpr float kRowGap = 2.0f;    // 行间距

    // 兜底：万一 hkEndScene 那次没跑（例如别的注入路径），图集未构建时补一次；
    // 已锁定（NewFrame 后）就跳过，绝不在这里动图集。
    inline void SetupFonts()
    {
        if (g_fontsReady || !ImGui::GetCurrentContext())
            return;
        g_fontsReady = true;

        if (!g_menuFontsReady)
            SetupMenuFonts();

        ImGuiIO& io = ImGui::GetIO();
        if (!io.FontDefault && g_fontBold)
            io.FontDefault = g_fontBold;
        if (!g_fontMini)
            g_fontMini = io.FontDefault;
        g_fontBig  = io.FontDefault;
        g_fontIcon = io.FontDefault;
    }

    inline int g_applied = 0;

    inline void EnsureStyle()
    {
        if (g_applied) return;
        g_applied = 1;
        SetupFonts();

        ImGuiStyle* style = &ImGui::GetStyle();

        style->Alpha             = 1.0f;
        style->WindowRounding    = 12.0f;
        style->WindowPadding     = ImVec2(0.0f, 0.0f);
        style->FramePadding      = ImVec2(8.0f, 5.0f);
        style->ItemSpacing       = ImVec2(8.0f, kRowGap);
        style->ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
        style->FrameRounding     = 8.0f;
        style->GrabRounding      = 8.0f;
        style->ScrollbarSize     = 3.0f;
        style->ScrollbarRounding = 8.0f;
        style->PopupRounding     = 10.0f;
        style->TabRounding       = 8.0f;
        style->GrabMinSize       = 10.0f;
        style->WindowBorderSize  = 0.0f;
        style->ChildBorderSize   = 0.0f;
        style->FrameBorderSize   = 0.0f;
        style->PopupBorderSize   = 1.0f;
        style->ChildRounding     = 10.0f;

        ImVec4* colors = style->Colors;
        colors[ImGuiCol_Text]             = Col(Ink());
        colors[ImGuiCol_TextDisabled]     = Col(InkMuted());
        colors[ImGuiCol_WindowBg]         = Col(PanelBg());
        colors[ImGuiCol_ChildBg]          = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_PopupBg]          = Col(IM_COL32(24, 25, 33, 252));
        colors[ImGuiCol_Border]           = Col(FieldLine());
        colors[ImGuiCol_FrameBg]          = Col(FieldBg());
        colors[ImGuiCol_FrameBgHovered]   = Col(IM_COL32(36, 38, 48, 255));
        colors[ImGuiCol_FrameBgActive]    = Col(IM_COL32(40, 42, 54, 255));
        colors[ImGuiCol_SliderGrab]       = Col(Accent());
        colors[ImGuiCol_SliderGrabActive] = Col(IM_COL32(152, 120, 255, 255));
        colors[ImGuiCol_CheckMark]        = Col(Accent());
        colors[ImGuiCol_Button]           = Col(IM_COL32(34, 36, 46, 255));
        colors[ImGuiCol_ButtonHovered]    = Col(IM_COL32(44, 47, 60, 255));
        colors[ImGuiCol_ButtonActive]     = Col(Accent());
        colors[ImGuiCol_Header]           = Col(AccentSoft());
        colors[ImGuiCol_HeaderHovered]    = Col(IM_COL32(38, 40, 52, 255));
        colors[ImGuiCol_HeaderActive]     = Col(Accent());
        colors[ImGuiCol_Separator]        = Col(CardLine());
        colors[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab]    = Col(IM_COL32(58, 61, 76, 255));
        colors[ImGuiCol_ScrollbarGrabHovered] = Col(IM_COL32(76, 79, 96, 255));
        colors[ImGuiCol_ScrollbarGrabActive]  = Col(Accent());
    }

    // ------------------------------------------------------------
    // 左侧导航项：整宽 × 44 高（图标块 + 标签 + 选中竖条）
    // ------------------------------------------------------------
    // 左侧导航项：148 × 44（设计稿 .navlist width:148 / .navitem height:44）
    // 不能用 GetContentRegionAvail()：导航 child 宽 172、左缩进 12，会算成 160
    inline bool Tab(const char* glyph, const char* label, bool selected)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems)
            return false;

        ImGuiID id = window->GetID(label);
        const ImVec2 pos = window->DC.CursorPos;
        const float  w = 148.0f;
        const float  h = 44.0f;

        const ImRect bb(pos, ImVec2(pos.x + w, pos.y + h));
        ImGui::ItemSize(ImVec2(w, h), 0.0f);
        if (!ImGui::ItemAdd(bb, id))
            return false;

        bool hovered, held;
        const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, ImGuiButtonFlags_None);
        if (hovered || held)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float  t = Ease(id, selected || hovered);

        if (t > 0.01f)
            dl->AddRectFilled(bb.Min, bb.Max,
                              Mix(IM_COL32(0, 0, 0, 0), IM_COL32(31, 33, 44, 255), t), 9.0f);
        if (selected)
            dl->AddRectFilled(ImVec2(bb.Min.x, bb.Min.y + 11.0f),
                              ImVec2(bb.Min.x + 3.0f, bb.Max.y - 11.0f), Accent(), 2.0f);

        // 图标块
        const float is = 28.0f;
        const ImVec2 i0(bb.Min.x + 14.0f, bb.Min.y + (h - is) * 0.5f);
        const ImVec2 i1(i0.x + is, i0.y + is);
        dl->AddRectFilled(i0, i1, selected ? Accent() : IM_COL32(33, 34, 44, 255), 8.0f);

        if (g_fontIcon)
        {
            // 设计稿 .navitem .ic 字号 13：字母居中落在 28×28 圆角块里。
            // 优先用 13px 原生字号的说明字体（16px 缩到 13 会发虚）
            ImFont* f = (g_fontMini ? g_fontMini : g_fontIcon);
            const ImVec2 gs = f->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, glyph);
            dl->AddText(f, 13.0f,
                        ImVec2(i0.x + (is - gs.x) * 0.5f, i0.y + (is - gs.y) * 0.5f),
                        selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(140, 143, 158, 255), glyph);
        }

        dl->AddText(ImVec2(i0.x + is + 12.0f, bb.Min.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                    selected ? Ink() : (hovered ? InkLabel() : InkMuted()), label);
        return pressed;
    }

    inline float g_rowInset = 0.0f;

    // 卡片内可用宽度：与光标 X 无关——这样同一张卡片无论被摆到哪一列，
    // 行控件宽度都一致（两列网格排版的前提）
    inline float g_cardInnerW = 0.0f;

    // ------------------------------------------------------------
    // 行基准 X（关键）
    // ------------------------------------------------------------
    // ImGui 的 ItemSize() 每创建一个 item 就把 DC.CursorPos.x 重置成
    // window->Pos.x + Indent.x —— 也就是正文 child 的最左边（198），
    // 卡片内边距 14 直接丢掉。结果：一张卡的第一行在 219，从第二行起
    // 全部跳到 198，右侧开关/滑条跟着左移，看起来就是"整体没对齐"。
    // 所以所有行控件一律用 g_rowX 作为绘制/热区起点，不接受光标自带的 X。
    inline float g_rowX = 0.0f;

    inline ImVec2 RowPos()
    {
        const ImVec2 c = ImGui::GetCursorScreenPos();
        return ImVec2(g_rowX, c.y);
    }

    // 行末把光标放回「同一行基准 X + 下一行 Y」，避免误差累积
    inline void RowNext(float y, float h)
    {
        ImGui::SetCursorScreenPos(ImVec2(g_rowX, y + h + kRowGap));
    }

    inline float RowWidth()
    {
        if (g_cardInnerW > 1.0f)
            return g_cardInnerW - g_rowInset;
        const float w = ImGui::GetContentRegionAvail().x - g_rowInset;
        return (w > 1.0f) ? w : 1.0f;
    }

    // ------------------------------------------------------------
    // 卡片：DrawList 双通道——内容画在上层，卡片底/描边事后补画在下层
    // ------------------------------------------------------------
    inline constexpr float kCardPad   = 14.0f;   // 卡片左右内边距（.ct / .rows 均 padding 0 14）
    inline constexpr float kCardHeadH = 31.0f;   // 标题区高度：设计稿 .ct height:31（含 1px 底边）
    inline constexpr float kCardPadB  = 13.0f;   // 卡片底部内边距：.rows padding-bottom 13
    inline constexpr float kCardGap   = 14.0f;   // 卡片间距：.card margin-bottom 14

    struct CardFrame {
        ImDrawList* dl;
        ImVec2 min;
        float  width;
        float  titleY;
        float  prevInnerW;
        bool   hasTitle;
    };
    inline CardFrame g_cards[8] = {};
    inline int       g_cardDepth = 0;

    // width > 0 时按指定宽度绘制（两列网格用），否则吃满当前可用宽度
    inline void CardBegin(const char* id, const char* title, float width = 0.0f)
    {
        // 卡片间距完全由 CardEnd 的 kCardPad(14) 承担，这里不再额外顶 4px，
        // 否则网格里两张卡的间隔会变成 18，与设计稿的 14 不一致
        CardFrame& f = g_cards[g_cardDepth++];
        f.dl         = ImGui::GetWindowDrawList();
        f.width      = (width > 1.0f) ? width : ImGui::GetContentRegionAvail().x;
        f.min        = ImGui::GetCursorScreenPos();
        f.titleY     = f.min.y + 9.0f;
        f.prevInnerW = g_cardInnerW;
        f.hasTitle   = (title && title[0]);

        g_cardInnerW = f.width - kCardPad * 2.0f;
        g_rowInset   = 0.0f;
        g_rowX       = f.min.x + kCardPad;   // 本卡所有行的左基准

        f.dl->ChannelsSplit(2);
        f.dl->ChannelsSetCurrent(1);

        ImGui::PushID(id);
        if (f.hasTitle)
        {
            ImGui::SetCursorScreenPos(ImVec2(f.min.x + kCardPad, f.min.y + 9.0f));
            ImGui::PushFont(g_fontMini);
            ImGui::PushStyleColor(ImGuiCol_Text, Col(InkMuted()));
            ImGui::TextUnformatted(title);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            // +7 对应设计稿 .rows 的 padding-top，让首行与标题留出透气空隙
            ImGui::SetCursorScreenPos(ImVec2(f.min.x + kCardPad, f.min.y + kCardHeadH + 7.0f));
        }
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(f.min.x + kCardPad, f.min.y + 12.0f));
        }
    }

    inline void CardEnd(const char* countText = nullptr)
    {
        g_rowInset = 0.0f;
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0.0f, kCardPadB));

        CardFrame f = g_cards[--g_cardDepth];
        g_cardInnerW = f.prevInnerW;

        // 末行控件自带 kRowGap(2)，Dummy 之后又叠了一次 ItemSpacing.y(2)，
        // 所以 mx.y 比「末行底 + 13」多出 4px——减掉才是设计稿的卡片底边
        const ImVec2 mx = ImGui::GetCursorScreenPos();
        const ImVec2 a(f.min.x, f.min.y);
        const ImVec2 b(f.min.x + f.width, mx.y - 4.0f);

        f.dl->ChannelsSetCurrent(0);
        f.dl->AddRectFilled(a, b, CardBg(), 12.0f);
        f.dl->AddRect(a, b, CardLine(), 12.0f, 0, 1.0f);
        if (f.hasTitle)
            f.dl->AddLine(ImVec2(a.x + 12.0f, a.y + kCardHeadH - 0.5f),
                          ImVec2(b.x - 12.0f, a.y + kCardHeadH - 0.5f), CardLine(), 1.0f);

        if (countText && countText[0])
        {
            f.dl->ChannelsSetCurrent(1);
            const ImVec2 ts = g_fontMini ? g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, countText)
                                         : ImGui::CalcTextSize(countText);
            if (g_fontMini)
                f.dl->AddText(g_fontMini, 13.0f, ImVec2(b.x - kCardPad - ts.x, f.titleY),
                              InkFaint(), countText);
            else
                f.dl->AddText(ImVec2(b.x - kCardPad - ts.x, f.titleY), InkFaint(), countText);
        }
        f.dl->ChannelsMerge();

        // 光标停在「卡片底 + 14」，两列网格直接取这个 y 当作下一张卡的顶边
        ImGui::SetCursorScreenPos(ImVec2(f.min.x, b.y + kCardGap));
    }

    // ------------------------------------------------------------
    // 开关行：胶囊滑块
    // ------------------------------------------------------------
    inline bool Toggle(const char* label, bool* v)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = kRowH;
        const ImVec2 pos = RowPos();

        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(pos);          // 热区必须落在行基准 X 上
        const bool clicked = ImGui::InvisibleButton("##tg", ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const ImGuiID id = ImGui::GetID("##tg");
        ImGui::PopID();

        if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 8), 7.0f);

        const float line = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(pos.x, pos.y + (h - line) * 0.5f),
                    (*v || hovered) ? InkLabel() : InkMuted(), LabelText(label));

        const float tw = 38.0f, th = 20.0f;
        const ImVec2 t0(pos.x + w - tw, pos.y + (h - th) * 0.5f);
        const float  t  = Ease(id, *v);
        dl->AddRectFilled(t0, ImVec2(t0.x + tw, t0.y + th),
                          Mix(IM_COL32(48, 50, 63, 255), Accent(), t), th * 0.5f);

        const float kr = th * 0.5f - 3.0f;
        const float kx = t0.x + 3.0f + kr + (tw - kr * 2.0f - 6.0f) * t;
        dl->AddCircleFilled(ImVec2(kx, t0.y + th * 0.5f), kr, IM_COL32(245, 246, 250, 255), 20);

        if (clicked)
            *v = !*v;
        RowNext(pos.y, h);
        return clicked;
    }

    // ------------------------------------------------------------
    // 自绘滑条：4px 细轨道 + 14px 圆球（严格对齐设计稿 .track / .knob）
    // 原生 SliderFloat 的轨道高度由 FramePadding 撑到约 20px，圆球也大，
    // 跟设计稿的细轨道完全是两种观感，所以这里自己画 + 自己算拖拽。
    // ------------------------------------------------------------
    inline bool TrackBar(const char* id, float* v, float lo, float hi,
                         float trackX, float trackW, float centerY)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float trackY = centerY - 2.0f;

        ImGui::PushID(id);
        ImGui::SetCursorScreenPos(ImVec2(trackX - 6.0f, centerY - kRowH * 0.5f));
        ImGui::InvisibleButton("##trk", ImVec2(trackW + 12.0f, kRowH));
        const bool active  = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        bool changed = false;
        if (active)
        {
            float t = (ImGui::GetIO().MousePos.x - trackX) / (trackW > 1.0f ? trackW : 1.0f);
            t = ImClamp(t, 0.0f, 1.0f);
            const float nv = lo + t * (hi - lo);
            if (nv != *v) { *v = nv; changed = true; }
        }

        float t = (hi > lo) ? (*v - lo) / (hi - lo) : 0.0f;
        t = ImClamp(t, 0.0f, 1.0f);
        const float fillW = trackW * t;

        dl->AddRectFilled(ImVec2(trackX, trackY), ImVec2(trackX + trackW, trackY + 4.0f),
                          IM_COL32(42, 44, 55, 255), 2.0f);
        if (fillW > 0.5f)
            dl->AddRectFilled(ImVec2(trackX, trackY), ImVec2(trackX + fillW, trackY + 4.0f),
                              Accent(), 2.0f);

        const ImVec2 kc(trackX + fillW, trackY + 2.0f);
        const ImU32 ring = (hovered || active) ? IM_COL32(255, 255, 255, 70) : IM_COL32(23, 24, 31, 255);
        dl->AddCircleFilled(kc, 7.0f, Accent(), 24);
        dl->AddCircle(kc, 7.0f, ring, 24, 2.0f);
        return changed;
    }

    // 滑条行公共布局：右侧「轨道 | 数值 | 单位」，返回轨道起点
    // 设计稿：.track 123 / .val min-width 26 / .unit min-width 14 / gap 8
    //        → 轨道左边缘距行右端 123+26+14+8+8 = 179
    inline float TrackLayout(float rowW, float* outTrackW, float* outUnitW)
    {
        const float unitW  = 14.0f;
        const float valW   = 26.0f;
        const float trackW = ImMin(123.0f, rowW * 0.46f);
        *outTrackW = trackW;
        *outUnitW  = unitW;
        return rowW - unitW - 8.0f - valW - 8.0f - trackW;
    }

    // ------------------------------------------------------------
    // 数值行：标签 + 细轨道 + 数值 + 单位
    // ------------------------------------------------------------
    inline bool ValueFloat(const char* label, float* v, float lo, float hi,
                           const char* fmt = "%.2f", const char* unit = "px")
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = kRowH;
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 pos = RowPos();
        const float right = pos.x + w;

        dl->AddText(ImVec2(pos.x, pos.y + (h - line) * 0.5f), InkLabel(), LabelText(label));

        float trackW = 0.0f, unitW = 0.0f;
        const float trackOff = TrackLayout(w, &trackW, &unitW);
        const float trackX  = pos.x + trackOff;
        const float centerY = pos.y + h * 0.5f;

        const bool changed = TrackBar(label, v, lo, hi, trackX, trackW, centerY);

        char buf[32];
        snprintf(buf, sizeof(buf), fmt, *v);
        const bool hasUnit = (unit && unit[0]);
        if (g_fontMini)
        {
            const ImVec2 ts = g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, buf);
            dl->AddText(g_fontMini, 13.0f,
                        ImVec2(right - unitW - 8.0f - ts.x, centerY - ts.y * 0.5f), InkMuted(), buf);
            if (hasUnit)
            {
                const ImVec2 us = g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, unit);
                dl->AddText(g_fontMini, 13.0f,
                            ImVec2(right - us.x, centerY - us.y * 0.5f), InkFaint(), unit);
            }
        }
        else
        {
            dl->AddText(ImVec2(right - unitW - 8.0f - ImGui::CalcTextSize(buf).x,
                               pos.y + (h - line) * 0.5f), InkMuted(), buf);
            if (hasUnit)
                dl->AddText(ImVec2(right - ImGui::CalcTextSize(unit).x,
                                   pos.y + (h - line) * 0.5f), InkFaint(), unit);
        }

        RowNext(pos.y, h);
        return changed;
    }

    // ------------------------------------------------------------
    // 整数滑块行：与 ValueFloat 同款布局，但显示取整、写入取整。
    // 2026-10-18 为 Storm 参数面板补的（命令展开数 / 移位预留档 /
    // 准星半径等全是整数滑条）。TrackBar 内部按 float 工作，
    // 这里在读回时四舍五入到整数，避免出现 5.0000001 这类尾数。
    // ------------------------------------------------------------
    inline bool ValueInt(const char* label, int* v, int lo, int hi,
                         const char* unit = "ms")
    {
        float f = static_cast<float>(*v);
        const bool changed = ValueFloat(label, &f, static_cast<float>(lo),
                                         static_cast<float>(hi), "%.0f", unit);
        if (changed)
        {
            int n = static_cast<int>(f + 0.5f);
            if (n < lo) n = lo;
            if (n > hi) n = hi;
            if (n != *v) { *v = n; return true; }
        }
        return false;
    }

    // ------------------------------------------------------------
    // 分段控件：严格照设计稿 .segs / .seg
    //   .segs  background #1E1F28, radius 9, padding 2, gap 2
    //   .seg   font-size 16（正文粗体）, padding 4px 11px, radius 7
    // 段文字必须走 16px：之前压成 13px 是为了硬塞四个段，观感比设计稿小一号；
    // 这里改为先按 11px 内边距排，超宽才逐级收内边距，字号不再动。
    // ------------------------------------------------------------
    inline bool Segmented(const char* label, int* cur, const char* const items[], int count)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float line = ImGui::GetTextLineHeight();
        const ImVec2 pos = RowPos();

        if (count > 8) count = 8;
        if (count <= 0) return false;

        dl->AddText(ImVec2(pos.x, pos.y + (kRowH - line) * 0.5f), InkLabel(), LabelText(label));

        float tw[8] = {};
        for (int i = 0; i < count; ++i)
            tw[i] = ImGui::CalcTextSize(items[i]).x;
        const float labelW = ImGui::CalcTextSize(LabelText(label)).x;

        float padX = 11.0f;
        float segW[8] = {};
        float total = 0.0f;
        for (;;)
        {
            total = 4.0f + (float)(count - 1) * 2.0f;    // .segs padding 2×2 + gap 2
            for (int i = 0; i < count; ++i)
            {
                segW[i] = tw[i] + padX * 2.0f;
                total += segW[i];
            }
            if (w - total >= labelW + 10.0f || padX <= 5.0f)
                break;
            padX -= 2.0f;
        }

        const float segH = 30.0f;                        // .seg 内容高（padding 4 上下）
        const float top  = pos.y + (kRowH - (segH + 4.0f)) * 0.5f;
        float bx = pos.x + w - total + 2.0f;             // .segs 左内边距 2

        bool changed = false;
        const int idx = (*cur >= 0 && *cur < count) ? *cur : 0;
        dl->AddRectFilled(ImVec2(bx - 2.0f, top), ImVec2(bx - 2.0f + total, top + segH + 4.0f),
                          FieldBg(), 9.0f);

        // 段按钮的 ID 必须带 label 作用域：同一张卡里可能有两行分段控件
        // （例如静默卡同时有"注入模式"和"瞄准部位"），只用 PushID(i) 会撞车
        ImGui::PushID(label);
        for (int i = 0; i < count; ++i)
        {
            const ImVec2 b0(bx, top + 2.0f);
            const ImVec2 b1(bx + segW[i], b0.y + segH);
            ImGui::PushID(i);
            // 热区必须显式对齐段按钮的实际位置：InvisibleButton 吃的是当前光标，
            // 不 SetCursorScreenPos 会落在行首（标签上），点按钮位置反而没反应
            ImGui::SetCursorScreenPos(b0);
            const bool clicked = ImGui::InvisibleButton("##seg", ImVec2(segW[i], segH));
            ImGui::PopID();

            if (i == idx)
                dl->AddRectFilled(b0, b1, Accent(), 7.0f);

            const ImU32 tcol = (i == idx) ? IM_COL32(255, 255, 255, 255) : InkMuted();
            dl->AddText(ImVec2(b0.x + (segW[i] - tw[i]) * 0.5f, b0.y + (segH - line) * 0.5f),
                        tcol, items[i]);

            if (clicked) { *cur = i; changed = true; }
            bx += segW[i] + 2.0f;
        }
        ImGui::PopID();

        RowNext(pos.y, kRowH);
        return changed;
    }

    inline void Checker(ImDrawList* dl, ImVec2 a, ImVec2 b, float cell)
    {
        dl->PushClipRect(a, b, true);
        const int rows = (int)ceilf((b.y - a.y) / cell);
        const int cols = (int)ceilf((b.x - a.x) / cell);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < cols; ++x)
            {
                const ImU32 col = ((x + y) & 1) ? IM_COL32(70, 70, 70, 255) : IM_COL32(52, 52, 52, 255);
                const ImVec2 c0(a.x + x * cell, a.y + y * cell);
                const ImVec2 c1(ImMin(c0.x + cell, b.x), ImMin(c0.y + cell, b.y));
                dl->AddRectFilled(c0, c1, col);
            }
        dl->PopClipRect();
    }

    inline bool ColorRow(const char* id, const char* label, ImColor* color, bool withAlpha)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = RowWidth();
        const float h = kRowH;
        const ImVec2 pos = RowPos();

        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        if (clicked)
            ImGui::OpenPopup(id);
        if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), IM_COL32(255, 255, 255, 8), 6.0f);

        const float line = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(pos.x, pos.y + (h - line) * 0.5f), InkLabel(), label);

        const float sw = 18.0f, sh = 18.0f;
        const ImVec2 s0(pos.x + w - sw, pos.y + (h - sh) * 0.5f);
        const ImVec2 s1(s0.x + sw, s0.y + sh);

        const ImVec4& c = color->Value;
        char hex[24];
        snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                 (int)(c.x * 255.0f + 0.5f), (int)(c.y * 255.0f + 0.5f), (int)(c.z * 255.0f + 0.5f));
        // 设计稿 .hex font-size:12 + margin-right:10
        const ImVec2 hsz = g_fontMini ? g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, hex)
                                      : ImGui::CalcTextSize(hex);
        if (g_fontMini)
            dl->AddText(g_fontMini, 13.0f, ImVec2(s0.x - hsz.x - 10.0f, pos.y + (h - hsz.y) * 0.5f),
                        InkFaint(), hex);
        else
            dl->AddText(ImVec2(s0.x - hsz.x - 10.0f, pos.y + (h - line) * 0.5f), InkFaint(), hex);

        Checker(dl, s0, s1, 6.0f);
        dl->AddRectFilled(s0, s1, ImGui::ColorConvertFloat4ToU32(c), 6.0f);
        dl->AddRect(s0, s1, IM_COL32(0, 0, 0, 120), 6.0f, 0, 1.0f);

        bool changed = false;
        ImGui::SetNextWindowPos(ImVec2(ImMax(8.0f, s0.x - 250.0f), s1.y + 6.0f), ImGuiCond_Always);
        if (ImGui::BeginPopup(id))
        {
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview;
            flags |= withAlpha
                ? (ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)
                : ImGuiColorEditFlags_NoAlpha;
            changed = ImGui::ColorPicker4("##pick", (float*)&color->Value, flags);
            ImGui::EndPopup();
        }
        RowNext(pos.y, h);
        return changed;
    }

    enum SnowButtonStyle
    {
        BtnPrimary = 0,
        BtnOutline,
        BtnGhost,
        BtnDanger,
    };

    inline bool Button(const char* label, const ImVec2& size = ImVec2(0.0f, 30.0f), int style = BtnPrimary)
    {
        const float w = (size.x > 0.0f) ? size.x : RowWidth();
        const float h = (size.y > 0.0f) ? size.y : 30.0f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();

        const bool clicked = ImGui::InvisibleButton(label, ImVec2(w, h));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 b1(pos.x + w, pos.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // 设计稿 .btn font-size:13.5 —— 按钮文字比正文小一号，用 13px 说明字体
        const char* txt = LabelText(label);
        const ImVec2 ts = g_fontMini ? g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, txt)
                                     : ImGui::CalcTextSize(txt);
        const ImVec2 tp(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f);
        const auto BtnText = [&](ImU32 col) {
            if (g_fontMini) dl->AddText(g_fontMini, 13.0f, tp, col, txt);
            else            dl->AddText(tp, col, txt);
        };

        if (style == BtnDanger)
        {
            const float t = Ease(ImGui::GetID(label), hovered);
            dl->AddRectFilled(pos, b1, Mix(IM_COL32(58, 28, 32, 255), IM_COL32(96, 36, 40, 255), t), 8.0f);
            dl->AddRect(pos, b1, Mix(IM_COL32(92, 46, 50, 255), Danger(), t), 8.0f, 0, 1.0f);
            BtnText(Mix(IM_COL32(200, 152, 154, 255), IM_COL32(255, 226, 226, 255), t));
        }
        else if (style == BtnPrimary)
        {
            const float t = Ease(ImGui::GetID(label), hovered);
            dl->AddRectFilled(pos, b1, Mix(IM_COL32(108, 74, 220, 255), IM_COL32(140, 104, 255, 255), t), 8.0f);
            BtnText(IM_COL32(255, 255, 255, 255));
        }
        else
        {
            const float t = Ease(ImGui::GetID(label), hovered);
            dl->AddRectFilled(pos, b1, Mix(IM_COL32(0, 0, 0, 0), IM_COL32(38, 40, 52, 255), t), 8.0f);
            dl->AddRect(pos, b1, Mix(FieldLine(), IM_COL32(70, 74, 92, 255), t), 8.0f, 0, 1.0f);
            BtnText(Mix(InkMuted(), Ink(), t));
        }

        ImGui::SetCursorScreenPos(ImVec2(g_rowX, pos.y + h + 2.0f));
        return clicked;
    }

    inline void Note(const char* text)
    {
        // 不能用 Dummy 起手：ItemSize 会把游标 X 重置到窗口行首（##body 左边），
        // 逃出 g_rowX —— 右列卡片的 Note 文字会跑到左列坐标上和别的卡片重叠。
        // 这里显式钉回行基准 X，换行宽度也按行基准算。
        ImGui::SetCursorScreenPos(RowPos());
        ImGui::PushStyleColor(ImGuiCol_Text, Col(InkMuted()));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth() - 2.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        // TextUnformatted 之后游标 X 已被 ItemSize 重置，但后续行控件
        // 一律经 RowPos()/RowNext() 重新取 g_rowX，无需在此补救。
    }

    // ------------------------------------------------------------
    //  [批64] Hint：控件下方的一行小字说明。
    //  与 Note 的区别：
    //    · 用 12px 小字 + 最弱文字色 —— 不抢视觉权重
    //    · 只写【一句】，最长约 20 字。用户明确反感长段解释，
    //      所以只给"看不懂在调什么"的参数配一行，其余一律不加。
    // ------------------------------------------------------------
    inline void Hint(const char* text)
    {
        ImFont* f = g_fontMini ? g_fontMini : ImGui::GetIO().FontDefault;
        const float size = g_fontMini ? 12.0f : ImGui::GetTextLineHeight() * 0.85f;

        const ImVec2 pos = RowPos();
        ImGui::GetWindowDrawList()->AddText(f, size, ImVec2(pos.x, pos.y),
                                            InkFaint(), text);

        const float lineH = f->CalcTextSizeA(size, FLT_MAX, 0.0f, "Ag").y;
        ImGui::SetCursorScreenPos(ImVec2(g_rowX, pos.y + lineH + 2.0f));
    }

    // ------------------------------------------------------------
    //  [批61] 定值行：左边参数名，右边数值，两者基线对齐。
    //  用途：把一套预设的每个数值"钉"在卡片里，用户不用点开滑条
    //  也能看到当前预设长什么样。
    //  matched=true 时数值用强调色（表示当前设置就是这个值）。
    // ------------------------------------------------------------
    inline void PinRow(const char* label, const char* value, bool matched)
    {
        ImGui::SetCursorScreenPos(RowPos());
        const float rowW = RowWidth();

        ImGui::PushStyleColor(ImGuiCol_Text, Col(InkMuted()));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        const ImVec2 vSize = ImGui::CalcTextSize(value);
        const float  labelEnd = ImGui::GetItemRectMax().x;
        // 右对齐；若空间不够就把数值紧跟在标签后（避免重叠）
        float vx = g_rowX + rowW - vSize.x;
        if (vx < labelEnd + 8.0f)
            vx = labelEnd + 8.0f;

        const ImVec2 after = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(vx, after.y - vSize.y));
        ImGui::PushStyleColor(ImGuiCol_Text, Col(matched ? Ink() : InkFaint()));
        ImGui::TextUnformatted(value);
        ImGui::PopStyleColor();
    }

    // 设计稿 .kbd：padding 1px 7px / font-size 11 / 圆角 5
    // 字号跟随说明字体 13px（11 已经小于最小档，硬缩会糊）
    inline float KbdTextW(const char* text)
    {
        return g_fontMini ? g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, text).x
                          : ImGui::CalcTextSize(text).x;
    }

    inline void Kbd(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 textColor, const Snow& p)
    {
        const float tw = KbdTextW(text);
        const float padX = 7.0f, h = 19.0f;
        const ImVec2 b1(pos.x + tw + padX * 2.0f, pos.y + h);
        dl->AddRectFilled(pos, b1, FieldBg(), 5.0f);
        dl->AddRect(pos, b1, FieldLine(), 5.0f, 0, 1.0f);
        if (g_fontMini)
        {
            const float th = g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, text).y;
            dl->AddText(g_fontMini, 13.0f, ImVec2(pos.x + padX, pos.y + (h - th) * 0.5f),
                        textColor, text);
        }
        else
        {
            const ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddText(ImVec2(pos.x + padX, pos.y + (h - ts.y) * 0.5f), textColor, text);
        }
    }

    inline float KbdWidth(const char* text)
    {
        return KbdTextW(text) + 14.0f;
    }
}

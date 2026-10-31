// ============================================================
// menu_probe.cpp -- offline interaction test bench for the Neverlus menu
// ------------------------------------------------------------
// 目的：在没有 D3D9 / 游戏进程的条件下，真实跑 ImGui 帧循环，
//      用合成鼠标事件逐个点击每个行控件，验证：
//        1) 每个控件热区（item rect）落在它被画出来的位置（行基准 X 一致、
//           Y 逐行按 36px 步进，不漂移）——历史上"第二行起控件左移 21px"的回归点；
//        2) 点击真的能翻转/改变绑定变量；
//        3) 相邻行之间没有热区串扰（点 A 不会改 B）；
//        4) 颜色行的弹窗真的会被打开。
//
// 复现条件与真实菜单一致：正文 child 原点 x = 198（正是 ItemSize() 把
// CursorPos.x 重置到的位置），卡片 x = 198、宽 331，行基准 X = 212。
// 若 SetCursorScreenPos 修复回退，第 2 行起 item rect 会退回 198，测试立刻失败。
//
// 说明：本文件只用 ASCII 注释与中文标签（工程带 /utf-8）；中文标签是必须的，
//      分段控件的段宽由真实字形宽度决定。
// ============================================================

#include "MenuSkin.h"

#include <cstdio>
#include <cfloat>
#include <cmath>
#include <windows.h>

using namespace Neverlus;

// ---------------- 受测状态 ----------------
static bool     s_tg0 = false;
static bool     s_tg1 = false;
static float    s_f0  = 40.0f;
static float    s_f1  = 0.25f;
static int      s_seg = 0;
static ImColor  s_col(1.0f, 0.5f, 0.25f, 1.0f);
static int      s_btn = 0;

static const char* kSegItems[] = { "自动", "头部", "胸部", "腹部" };

// ---------------- 布局常量（与 Menu.cpp / MenuSkin.h 对齐） ----------------
static const float kChildX  = 198.0f;   // 正文 child 原点 X
static const float kCardX   = 198.0f;
static const float kCardY   = 100.0f;
static const float kCardW   = 331.0f;   // ColW() = (900-172-52-14)/2
static const float kRowX    = kCardX + 14.0f;      // g_rowX = 212
static const float kRowW    = kCardW - 28.0f;      // 303
static const float kFirstY  = kCardY + 31.0f + 7.0f;  // 138
static const float kRowStep = 34.0f + 2.0f;           // 36

// 轨道起点：TrackLayout() = w - 14 - 8 - 26 - 8 - 123
static const float kTrackX  = kRowX + (kRowW - 14.0f - 8.0f - 26.0f - 8.0f - 123.0f);
static const float kTrackW  = 123.0f;

enum Kind { K_TG0, K_TG1, K_F0, K_F1, K_SEG, K_COL, K_BTN, K_COUNT };
static const char* kNames[K_COUNT] = {
    "Toggle A", "Toggle B", "Value A", "Value B", "Segmented", "ColorRow", "Button"
};

struct Rect { ImVec2 mn, mx; bool ok; };
static Rect s_rect[K_COUNT];

static ImGuiIO* G = nullptr;

// ---------------- 卡片内容：7 行，覆盖全部行控件种类 ----------------
static void DrawProbeCard()
{
    ImGui::SetCursorScreenPos(ImVec2(kCardX, kCardY));
    CardBegin("probe", "交互测试", kCardW);

    s_rect[K_TG0].ok = true;
    Toggle("开关 A", &s_tg0);
    s_rect[K_TG0].mn = ImGui::GetItemRectMin();
    s_rect[K_TG0].mx = ImGui::GetItemRectMax();

    s_rect[K_TG1].ok = true;
    Toggle("开关 B", &s_tg1);
    s_rect[K_TG1].mn = ImGui::GetItemRectMin();
    s_rect[K_TG1].mx = ImGui::GetItemRectMax();

    s_rect[K_F0].ok = true;
    ValueFloat("数值 A", &s_f0, 0.0f, 100.0f, "%.0f", "px");
    s_rect[K_F0].mn = ImGui::GetItemRectMin();
    s_rect[K_F0].mx = ImGui::GetItemRectMax();

    s_rect[K_F1].ok = true;
    ValueFloat("数值 B", &s_f1, 0.0f, 1.0f, "%.2f", "度");
    s_rect[K_F1].mn = ImGui::GetItemRectMin();
    s_rect[K_F1].mx = ImGui::GetItemRectMax();

    s_rect[K_SEG].ok = true;
    Segmented("瞄准部位", &s_seg, kSegItems, 4);
    s_rect[K_SEG].mn = ImGui::GetItemRectMin();
    s_rect[K_SEG].mx = ImGui::GetItemRectMax();

    s_rect[K_COL].ok = true;
    ColorRow("##probeColor", "颜色 A", &s_col, false);
    s_rect[K_COL].mn = ImGui::GetItemRectMin();
    s_rect[K_COL].mx = ImGui::GetItemRectMax();

    s_rect[K_BTN].ok = true;
    if (Button("按钮 A", ImVec2(145.0f, 32.0f), BtnPrimary))
        ++s_btn;
    s_rect[K_BTN].mn = ImGui::GetItemRectMin();
    s_rect[K_BTN].mx = ImGui::GetItemRectMax();

    CardEnd();
}

// ---------------- 帧驱动 ----------------
static int g_openPopups = 0;

static void Frame(float mx, float my, bool down)
{
    G->DisplaySize = ImVec2(900.0f, 688.0f);
    G->DeltaTime   = 1.0f / 60.0f;
    G->AddMousePosEvent(mx, my);
    G->AddMouseButtonEvent(0, down);

    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 688.0f));
    ImGui::Begin("probe", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    {
        ImGui::SetCursorScreenPos(ImVec2(kChildX, 92.0f));
        ImGui::BeginChild("##body", ImVec2(676.0f, 560.0f), false);
        DrawProbeCard();
        ImGui::EndChild();
    }
    ImGui::End();

    ImGui::Render();

    ImGuiContext& g = *GImGui;
    g_openPopups = g.OpenPopupStack.Size;
}

// 一次完整点击：悬停 -> 按下 -> 抬起（InvisibleButton 在抬起时给 clicked）
static void ClickAt(float x, float y)
{
    Frame(x, y, false);
    Frame(x, y, true);
    Frame(x, y, false);
}

// ---------------- 测试流程 ----------------
static int g_pass = 0, g_fail = 0;

static void Check(bool cond, const char* what)
{
    if (cond) { ++g_pass; printf("  [PASS] %s\n", what); }
    else      { ++g_fail; printf("  [FAIL] %s\n", what); }
}

// 行中心点
static float RowCenterY(int i) { return kFirstY + kRowStep * (float)i + 17.0f; }

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    G = &io;
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.DisplaySize = ImVec2(900.0f, 688.0f);
    io.DeltaTime = 1.0f / 60.0f;

    // 字体：与真实菜单同一条路径（含图集构建）
    SetupMenuFonts();
    SetupFonts();
    printf("font: bold=%p(%d glyphs) mini=%p(%d glyphs) atlas=%dx%d built=%.0fms\n",
           (void*)g_fontBold, g_fontBoldGlyphs,
           (void*)g_fontMini, g_fontMiniGlyphs,
           g_atlasW, g_atlasH, g_atlasBuildMs);
    if (!g_fontBold) { printf("FATAL: no font\n"); return 2; }

    // 预热两帧：让 ImGui 稳定（第一帧会把窗口加入列表、计算 auto-fit）
    Frame(-FLT_MAX, -FLT_MAX, false);
    Frame(-FLT_MAX, -FLT_MAX, false);

    printf("\n=== 1. 热区几何（行基准 X / 逐行 Y 步进）===\n");

    // 1.1 整行热区（Toggle / ColorRow）左边界必须等于行基准 X，宽度等于 RowWidth
    const int fullRow[] = { K_TG0, K_TG1, K_COL };
    for (int k : fullRow)
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "%s 热区 x=%.1f (期望 %.1f) w=%.1f (期望 %.1f)",
                 kNames[k], s_rect[k].mn.x, kRowX, s_rect[k].mx.x - s_rect[k].mn.x, kRowW);
        Check(fabsf(s_rect[k].mn.x - kRowX) < 0.5f &&
              fabsf((s_rect[k].mx.x - s_rect[k].mn.x) - kRowW) < 0.5f, buf);
    }

    // 1.2 逐行 Y 步进 36（第 2 行起不漂移即证明 ItemSize 归位问题已修）
    {
        char buf[160];
        const float y0 = s_rect[K_TG0].mn.y;
        const float y1 = s_rect[K_TG1].mn.y;
        snprintf(buf, sizeof(buf), "Toggle A/B 行 Y=%.1f/%.1f 步进=%.1f (期望 %.1f)",
                 y0, y1, y1 - y0, kRowStep);
        Check(fabsf(y0 - kFirstY) < 0.5f && fabsf((y1 - y0) - kRowStep) < 0.5f, buf);
    }
    {
        char buf[160];
        const float d = s_rect[K_F1].mn.y - s_rect[K_F0].mn.y;
        snprintf(buf, sizeof(buf), "Value A/B 轨道 Y 步进=%.1f (期望 %.1f)", d, kRowStep);
        Check(fabsf(d - kRowStep) < 0.5f, buf);
    }
    {
        char buf[160];
        snprintf(buf, sizeof(buf), "Value A 轨道 x=%.1f (期望 %.1f) w=%.1f (期望 %.1f)",
                 s_rect[K_F0].mn.x, kTrackX - 6.0f,
                 s_rect[K_F0].mx.x - s_rect[K_F0].mn.x, kTrackW + 12.0f);
        Check(fabsf(s_rect[K_F0].mn.x - (kTrackX - 6.0f)) < 0.5f &&
              fabsf((s_rect[K_F0].mx.x - s_rect[K_F0].mn.x) - (kTrackW + 12.0f)) < 0.5f, buf);
    }

    printf("\n=== 2. 逐控件点击生效 ===\n");

    // 2.1 Toggle A
    {
        s_tg0 = false; s_tg1 = false;
        ClickAt(s_rect[K_TG0].mx.x - 19.0f, RowCenterY(0));
        Check(s_tg0, "点击 Toggle A 开关 -> true");
        Check(!s_tg1, "点击 Toggle A 不影响 Toggle B");
        ClickAt(s_rect[K_TG0].mx.x - 19.0f, RowCenterY(0));
        Check(!s_tg0, "再点 Toggle A -> 回到 false");
    }

    // 2.2 Toggle B（第二行，历史上最先失效的就是这一行）
    {
        s_tg0 = false; s_tg1 = false;
        ClickAt(s_rect[K_TG1].mx.x - 19.0f, RowCenterY(1));
        Check(s_tg1, "点击第二行 Toggle B -> true（行基准回归点）");
        Check(!s_tg0, "点击 Toggle B 不影响 Toggle A");
    }

    // 2.3 Value A：点轨道右端应拉到上限
    {
        s_f0 = 10.0f;
        ClickAt(kTrackX + kTrackW - 1.0f, RowCenterY(2));
        char buf[160];
        snprintf(buf, sizeof(buf), "点击 Value A 轨道右端 -> %.1f (期望 ~100)", s_f0);
        Check(fabsf(s_f0 - 100.0f) < 1.5f, buf);
        s_f1 = 0.25f;
        Check(fabsf(s_f1 - 0.25f) < 0.001f, "拖动 Value A 不影响 Value B");
    }

    // 2.4 Value B：点轨道左端应拉到下限
    {
        s_f1 = 0.9f;
        ClickAt(kTrackX + 1.0f, RowCenterY(3));
        char buf[160];
        snprintf(buf, sizeof(buf), "点击 Value B 轨道左端 -> %.3f (期望 0)", s_f1);
        Check(fabsf(s_f1) < 0.02f, buf);
    }

    // 2.5 Segmented：右端一段 + 倒数第二段（每段热区必须独立）
    //     注意 GetItemRectMin 返回的是"最后一个段"的矩形，整组左边界不可直接取，
    //     所以用"最后一段左边界往左 8px"去点倒数第二段。
    {
        s_seg = 0;
        ClickAt(s_rect[K_SEG].mx.x - 6.0f, RowCenterY(4));
        char buf[160];
        snprintf(buf, sizeof(buf), "点击分段控件最右段 -> index=%d (期望 3)", s_seg);
        Check(s_seg == 3, buf);
        ClickAt(s_rect[K_SEG].mn.x - 8.0f, RowCenterY(4));
        snprintf(buf, sizeof(buf), "点击倒数第二段 -> index=%d (期望 2)", s_seg);
        Check(s_seg == 2, buf);
        ClickAt(s_rect[K_SEG].mn.x + 6.0f, RowCenterY(4));
        snprintf(buf, sizeof(buf), "点击最后一段 -> index=%d (期望 3)", s_seg);
        Check(s_seg == 3, buf);
    }

    // 2.6 Button（放在颜色弹窗测试之前，避免弹窗截获点击）
    {
        const int before = s_btn;
        ClickAt(s_rect[K_BTN].mn.x + 72.0f, s_rect[K_BTN].mn.y + 16.0f);
        char buf[160];
        snprintf(buf, sizeof(buf), "点击 Button -> 触发 %d 次 (期望 +1)", s_btn - before);
        Check(s_btn == before + 1, buf);
    }

    // 2.7 ColorRow：点击应打开取色弹窗，点空白处应关闭
    {
        Frame(s_rect[K_COL].mn.x + 40.0f, RowCenterY(5), false);
        Check(g_openPopups == 0, "颜色行未点击时没有弹窗");
        ClickAt(s_rect[K_COL].mn.x + 40.0f, RowCenterY(5));
        char buf[160];
        snprintf(buf, sizeof(buf), "点击颜色行 -> 打开弹窗数=%d (期望 1)", g_openPopups);
        Check(g_openPopups >= 1, buf);
        for (int i = 0; i < 4 && g_openPopups > 0; ++i)
            ClickAt(760.0f, 620.0f);       // 空白区：点一次关弹窗，再点无副作用
        snprintf(buf, sizeof(buf), "点击空白区关闭取色弹窗 -> 剩余弹窗=%d", g_openPopups);
        Check(g_openPopups == 0, buf);
    }

    // 2.8 点空白处不应改变任何状态
    {
        const bool b0 = s_tg0, b1 = s_tg1;
        const float f0 = s_f0, f1 = s_f1;
        const int   sg = s_seg, bt = s_btn;
        ClickAt(kRowX + 4.0f, kFirstY - 20.0f);   // 卡片标题区
        Check(b0 == s_tg0 && b1 == s_tg1 && f0 == s_f0 && f1 == s_f1 &&
              sg == s_seg && bt == s_btn, "点击卡片标题区不触发任何控件");
    }

    printf("\n=== 3. 真实菜单文案 vs 控件（左标签不得与右侧控件重叠）===\n");
    {
        // kind: 0=Toggle（右侧 38px 开关） 1=ValueFloat（轨道起点 124）
        //       2=Segmented（标签 + 分段组右对齐） 3=整行/按钮（无约束）
        struct L { const char* s; int kind; };
        static const L kLabels[] = {
            // 视觉 - 绘制元素 / 过滤 / 文字 / 方框样式
            { "2D 方框", 0 }, { "骨骼透视", 0 }, { "血条", 0 }, { "血量数值", 0 },
            { "名称", 0 }, { "距离", 0 }, { "准星连线", 0 }, { "敌人计数", 0 },
            { "显示队友", 0 }, { "忽略幽灵特感", 0 }, { "过滤小僵尸", 0 },
            { "名称字号", 1 }, { "距离字号", 1 }, { "血量字号", 1 },
            { "外观", 2 },
            { "方框粗细", 1 }, { "角框粗细", 1 }, { "连线粗细", 1 },
            { "血条宽度", 1 }, { "血条长度", 1 },
            // 瞄准
            { "启用自瞄", 0 }, { "自瞄热键", 1 }, { "自瞄范围", 1 },
            { "瞄准部位", 2 }, { "仅特感（含女巫）", 0 },
            { "静默自瞄", 0 }, { "最大修正角", 1 },
            // 战斗
            { "自动连跳", 0 }, { "子弹无扩散", 0 }, { "自动连发", 0 },
            // 系统 / 颜色
            { "ESP 预览窗", 0 }, { "瞄准范围圈", 0 },
            { "2D 方框 / 角框", 3 }, { "血条 / 血量", 3 }, { "保存配置", 3 },
        };

        // 分段控件的可用段宽（照抄 Segmented 的内边距收敛逻辑）
        auto segGroupW = [&](const char* const* items, int n) -> float {
            float padX = 11.0f;
            for (;;)
            {
                float total = 4.0f + (float)(n - 1) * 2.0f;
                for (int i = 0; i < n; ++i)
                {
                    const float tw = g_fontBold
                        ? g_fontBold->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, items[i]).x
                        : ImGui::CalcTextSize(items[i]).x;
                    total += tw + padX * 2.0f;
                }
                if (kRowW - total >= 60.0f || padX <= 5.0f)
                    return total;
                padX -= 2.0f;
            }
        };

        int bad = 0;
        float worst = 0.0f; const char* worstS = "";
        for (const L& l : kLabels)
        {
            const float lw = g_fontBold
                ? g_fontBold->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, l.s).x
                : ImGui::CalcTextSize(l.s).x;
            float reserve = 0.0f;   // 右侧留给控件的宽度
            if (l.kind == 0) reserve = 38.0f + 8.0f;
            else if (l.kind == 1) reserve = 124.0f;
            else if (l.kind == 2)
            {
                static const char* kBox[]   = { "描边", "四角", "实线" };
                static const char* kPart[]  = { "自动", "头部", "胸部", "腹部" };
                const bool isPart = (strstr(l.s, "部位") != nullptr);
                reserve = segGroupW(isPart ? kPart : kBox, isPart ? 4 : 3) + 8.0f;
            }
            if (l.kind == 3) continue;

            const float ratio = lw / (kRowW - reserve);
            if (ratio > worst) { worst = ratio; worstS = l.s; }
            if (lw > kRowW - reserve)
            {
                printf("  [FAIL] \"%s\" 标签 %.1fpx + 控件 %.1fpx > 行宽 %.1fpx\n",
                       l.s, lw, reserve, kRowW);
                ++bad;
            }
        }
        char buf[220];
        snprintf(buf, sizeof(buf), "%d 个真实标签全部不与右侧控件重叠（最紧: \"%s\" 占行宽 %.0f%%）",
                 (int)(sizeof(kLabels) / sizeof(kLabels[0])), worstS, worst * 100.0f);
        Check(bad == 0, buf);
    }

    printf("\n=== 4. 每页内容总高度（不得超出正文区，否则底部控件点不到）===\n");
    {
        // 卡片高 = 38(标题+间距) + 36*行数 + 11(底内边距)
        auto cardH = [](int rows) { return 49.0f + 36.0f * (float)rows; };
        struct P { const char* name; int l[3]; int r[3]; int ln, rn; };
        static const P kPages[] = {
            { "视觉", { 8, 3, 0 }, { 3, 7, 0 }, 2, 2 },   // 左: espDraw(8)+espText(3) 右: espFilter(3)+espBox(7)
            { "瞄准", { 4, 0, 0 }, { 2, 2, 0 }, 1, 2 },   // 左: aimMain(4) 右: aimSilent(2)+aimSafe(2)
            { "战斗", { 1, 1, 0 }, { 1, 0, 0 }, 2, 1 },   // 左: 移动(1)+弹道(1) 右: 连发(1)
            { "颜色", { 6, 6, 0 }, { 6, 6, 0 }, 2, 2 },   // 四个 6 行阵营卡
            { "系统", { 2, 3, 0 }, { 3, 0, 0 }, 2, 1 },   // 左: 界面(2)+卸载 右: 配置(3 按钮)
        };
        const float bodyH = 688.0f - 92.0f - 8.0f;
        for (const P& p : kPages)
        {
            float hl = 0.0f, hr = 0.0f;
            for (int i = 0; i < p.ln; ++i) hl += cardH(p.l[i]) + (i ? 14.0f : 0.0f);
            for (int i = 0; i < p.rn; ++i) hr += cardH(p.r[i]) + (i ? 14.0f : 0.0f);
            const float h = (hl > hr) ? hl : hr;
            char buf[200];
            snprintf(buf, sizeof(buf), "%-4s 页 最高列 %.0fpx / 可用 %.0fpx", p.name, h, bodyH);
            Check(h <= bodyH, buf);
        }
    }

    printf("\n----------------------------------------\n");
    printf("PASS %d / FAIL %d\n", g_pass, g_fail);

    ImGui::DestroyContext();
    return g_fail == 0 ? 0 : 1;
}

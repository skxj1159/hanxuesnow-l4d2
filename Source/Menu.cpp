#include "Include.h"
#include "Menu.h"

#include "MenuSkin.h"
#include "Config/Config.h"    // 瞄准页参数直连 g_Config（批3 菜单化）
#include "Config/CfgFile.h"   // [批60] cfg 保存 / 导入 / 分享
#include "Features/Aimbot.h"  // Aimbot::RenderFovRing（FOV 范围圈）

#include <cstdio>
#include <vector>
#include <shellapi.h>

#ifdef OSIMGUI_INTERNAL
#include "PayloadControl.h"
#endif

using namespace Neverlus;

// ============================================================
// 层级与尺寸
// ------------------------------------------------------------
//   ┌───────────────────────────────────────────────┐
//   │ L4D2  logo                                    │  ← 左侧导航 172
//   │ 视觉 / 瞄准 / 战斗 / 颜色 / 系统               │
//   │───────────────────────────────────────────────│
//   │ 页标题 + 一句说明                              │  ← 页头 92
//   │ ┌────────────┐ ┌────────────┐                 │
//   │ │ 卡片        │ │ 卡片        │                │  ← 两列网格
//   │ └────────────┘ └────────────┘                 │
//   │───────────────────────────────────────────────│
//   │ ● 状态                              INS 关闭   │  ← 页脚 46
//   └───────────────────────────────────────────────┘
//
// 重构要点（相对上一版）：
//   · 取消「页 → 子标签 → 卡片」三级结构，改为「页 → 两列卡片」两级：
//     子标签把设置切得太碎（有的页只有一个开关），点两次才能到目标项；
//   · 取消右侧常驻状态栏（它占掉 160px 正文宽度，是"挤"的直接原因），
//     状态/配置/界面开关分别并入页脚与系统页；
//   · 正文由 408px 扩到 676px，行高 30→34，卡片内边距 12→14，
//     标题与内容之间加分隔线——靠留白和分组取代"堆在一起"。
// ============================================================
static constexpr float kWinW  = 900.0f;
static constexpr float kWinH  = 688.0f;
static constexpr float kNavW  = 172.0f;
static constexpr float kPad   = 26.0f;
static constexpr float kHeadH = 92.0f;
static constexpr float kColGap = 14.0f;

static float BodyW() { return kWinW - kNavW - kPad * 2.0f; }
static float ColW()  { return (BodyW() - kColGap) * 0.5f; }

// 菜单鼠标：经 CHLClient 虚表调 IN_DeactivateMouse/IN_ActivateMouse
// 停用引擎鼠标（SDL 方案在 Windows 版不生效）；运行时校验四个槽位同源
// 转发 thunk 才启用，失败退化为仅软件光标。
#ifdef OSIMGUI_INTERNAL
namespace {
    struct MenuMouseState {
        bool  resolved      = false;
        bool  available     = false;
        void* chl           = nullptr;
        void* fnActivate   = nullptr;   // vtable[13]
        void* fnDeactivate = nullptr;   // vtable[14]
        bool  weDeactivated = false;
    };
    MenuMouseState g_menuMouse;

    using VoidFn = void(__thiscall*)(void*);

    void* GetClientInterface()
    {
        // [批153] 统一加锁获取（CreateInterface 不可重入）
        return GetModuleInterfaceLocked("client.dll", "VClient016");
    }

    uintptr_t ThunkGlobalRef(const unsigned char* p, size_t len,
                             uintptr_t lo, uintptr_t hi)
    {
        for (size_t i = 0; i + 5 <= len; ++i)
        {
            uintptr_t imm = 0;
            if (p[i] == 0xA1)
                imm = *(const uint32_t*)(p + i + 1);
            else if (p[i] == 0x8B && (p[i + 1] & 0xC7) == 0x05)
                imm = *(const uint32_t*)(p + i + 2);
            else
                continue;
            if (imm >= lo && imm < hi)
                return imm;
        }
        return 0;
    }

    void MenuMouseResolve()
    {
        if (g_menuMouse.resolved)
            return;
        g_menuMouse.resolved = true;

        const HMODULE client = ::GetModuleHandleA("client.dll");
        void* chl = GetClientInterface();
        if (!client || !chl)
            return;

        const auto vtable = *reinterpret_cast<void***>(chl);
        if (!vtable)
            return;

        const uintptr_t modLo = reinterpret_cast<uintptr_t>(client);
        const uintptr_t modHi = modLo + 0x4000000;

        void* slots[4] = { vtable[13], vtable[14], vtable[15], vtable[16] };
        uintptr_t refs[4] = {};
        int inMod = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uintptr_t fn = reinterpret_cast<uintptr_t>(slots[i]);
            if (fn < modLo || fn >= modHi)
                break;
            ++inMod;
            __try
            {
                refs[i] = ThunkGlobalRef(reinterpret_cast<const unsigned char*>(fn),
                                         0x18, modLo, modHi);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { refs[i] = 0; }
        }
        if (inMod < 4)
            return;

        uintptr_t g = 0;
        int best = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (!refs[i]) continue;
            int c = 0;
            for (int k = 0; k < 4; ++k)
                if (refs[k] == refs[i]) ++c;
            if (c > best) { best = c; g = refs[i]; }
        }
        if (best < 3 || !g)
            return;

        g_menuMouse.chl          = chl;
        g_menuMouse.fnActivate   = slots[0];
        g_menuMouse.fnDeactivate = slots[1];
        g_menuMouse.available    = true;
    }
}

void MenuMouseUnlock(bool unlock)
{
    MenuMouseResolve();
    if (g_menuMouse.available)
    {
        if (unlock && !g_menuMouse.weDeactivated)
        {
            reinterpret_cast<VoidFn>(g_menuMouse.fnDeactivate)(g_menuMouse.chl);
            g_menuMouse.weDeactivated = true;
        }
        else if (!unlock && g_menuMouse.weDeactivated)
        {
            reinterpret_cast<VoidFn>(g_menuMouse.fnActivate)(g_menuMouse.chl);
            g_menuMouse.weDeactivated = false;
        }
    }
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().MouseDrawCursor = unlock;

    // ---- SDL2 直调（L4D2 官方输入层，主路径）----
    // 特征法猜引擎锁鼠函数是脆弱的（available=false 时整条链失效）。
    // L4D2 用 SDL 管输入：释放相对鼠标模式 = 引擎不再吃鼠标转视角，
    // 光标回到窗口绝对位置 → ImGui 可控。恢复时反向。
    static HMODULE s_sdl = nullptr;
    if (!s_sdl)
        s_sdl = ::GetModuleHandleA("SDL2.dll");
    if (s_sdl)
    {
        using SetRelFn  = int (__cdecl*)(int);
        using ShowCurFn = int (__cdecl*)(int);
        const auto setRel = reinterpret_cast<SetRelFn>(
            ::GetProcAddress(s_sdl, "SDL_SetRelativeMouseMode"));
        const auto showCur = reinterpret_cast<ShowCurFn>(
            ::GetProcAddress(s_sdl, "SDL_ShowCursor"));

        //  [批52·修复"关卡中菜单点不动"]
        //  原实现在【关闭菜单】时也调 setRel(1) / showCur(0)。问题：
        //  关卡进行中引擎自己就在相对鼠标模式里，我们再去写一次
        //  SDL 状态会与引擎的输入状态打架 —— 表现是菜单能打开，
        //  但鼠标完全点不动，只有进 ESC 菜单（引擎交还光标）才行。
        //  正确做法：关闭时【只交还光标显示权】，绝不主动改
        //  SDL 的鼠标模式 —— 那是引擎的地盘，它想开就开着。
        if (unlock)
        {
            if (setRel)  setRel(0);
            if (showCur) showCur(1);
        }
        else
        {
            // 关闭：只显示光标（让用户看得见），不碰相对模式
            if (showCur) showCur(1);
        }
    }
}

void MenuMouseFrameGuard()
{
    // SDL 每帧重申：引擎可能每帧重设相对鼠标模式（否则菜单开着开着
    // 鼠标又被游戏吃回去）。菜单开着时每帧 SetRelativeMouseMode(0)。
    if (g_menuMouse.available && g_menuMouse.weDeactivated)
        reinterpret_cast<VoidFn>(g_menuMouse.fnDeactivate)(g_menuMouse.chl);

    static HMODULE s_sdl = nullptr;
    if (!s_sdl)
        s_sdl = ::GetModuleHandleA("SDL2.dll");
    if (s_sdl && menuOpen)
    {
        using SetRelFn = int (__cdecl*)(int);
        const auto setRel = reinterpret_cast<SetRelFn>(
            ::GetProcAddress(s_sdl, "SDL_SetRelativeMouseMode"));
        if (setRel)
            setRel(0);
    }
}
#else
void MenuMouseUnlock(bool) {}
void MenuMouseFrameGuard() {}
#endif

void HandleMenuToggle() {
    static bool wasPressed = false;
    bool isPressed = GetAsyncKeyState(VK_INSERT) & 0x8000;
    if (isPressed && !wasPressed)
        menuOpen = !menuOpen;
    wasPressed = isPressed;

#ifdef OSIMGUI_INTERNAL
    static bool cursorReleased = false;
    //  [批52] 不再用 cursorPrevShowing 决定恢复后的光标可见性。
    //  原逻辑：进菜单时若光标本是隐藏的（关卡里引擎会藏），关闭菜单
    //  就再 ShowCursor(FALSE) 藏回去 —— 结果"菜单关着、光标也看不见"，
    //  用户自然点不动任何东西。关卡中要能用鼠标开关，就必须保证
    //  光标【可见】。只记"是否已经解锁过"，不再记可见性。
    if (menuOpen && !cursorReleased) {
        cursorReleased = true;
        // Win32 层的光标显示计数器是【累加式】的：引擎每帧可能调用
        // ShowCursor(FALSE) 让它变负数。所以打开菜单时用 while 循环
        // 把它抬到"确定可见"，而不是只调一次。
        while (::ShowCursor(TRUE) < 0) {}
        MenuMouseUnlock(true);
    } else if (!menuOpen && cursorReleased) {
        cursorReleased = false;
        // 关闭时【不隐藏光标】—— 关卡中要能继续用鼠标操作。
        // 引擎自己会在下一帧按需再藏，这里不跟它抢。
        MenuMouseUnlock(false);
    }
    if (menuOpen)
    {
        ::ClipCursor(nullptr);
        MenuMouseFrameGuard();
    }
#endif
}

// ============================================================
// 页面定义（扁平导航，无子标签）
// ------------------------------------------------------------
//  2026-10-18：随 Storm 全部功能移植，从 5 页重排为 7 页。
//  重排依据是 Storm 仓库真实的模块划分（33 个 hpp/cpp），
//  不是凭感觉加页：
//    · 原「瞄准」只承载自瞄 → 拆出「过检测」（Storm 的反作弊
//      主体是 Extra Commands / Sequence Shift / 时钟校正三件套，
//      与选靶逻辑无关，混在一页会互相淹没）
//    · 原「战斗」只有 3 个开关 → 并入 Perform_Trace 的完整伤害
//      模型后升格为独立一页
//    · 原「移动」混在战斗里 → 独立成页（Storm 有 8 个移动/物理
//      模块：Set_Move_Type / Process_Movement / Estimate_Velocity
//      / Update_Animations / Spawn_Grenade / Play_Footstep …）
//    · 新增「准星」页 —— Storm 的 Draw_Crosshair 是一套带 5 个
//      参数的旋转十字，原菜单完全没有对应位置
//    · 原「颜色」页取消：Storm 配色是三档硬编码语义色、不可调，
//      单独占一页只有一个只读图例，信息密度太低 → 并入「视觉」页
// ============================================================
namespace {
    struct PageDef {
        const char* glyph;
        const char* name;
        const char* desc;
    };

    const PageDef kPages[] = {
        { "A", "视觉",   "v1018C ｜ 绘制元素与语义色图例" },
        { "B", "瞄准",   "Legit + 微静默 ｜ 参数实时生效" },
        { "C", "移动",   "自动连跳 + 自动连发 ｜ 受检测阈值约束" },
        { "D", "系统",   "配置保存与注入卸载" },
    };
    constexpr int kPageCount = IM_ARRAYSIZE(kPages);
}

// ============================================================
// 两列网格：每列独立累计高度，卡片按指定列摆放
// ============================================================
namespace {
    struct GridState { float x0; float top; float y[2]; };
    GridState g_grid{};

    void GridBegin()
    {
        g_grid.x0  = ImGui::GetCursorScreenPos().x;
        g_grid.top = ImGui::GetCursorScreenPos().y;
        g_grid.y[0] = g_grid.top;
        g_grid.y[1] = g_grid.top;
    }

    void GridEnd()
    {
        const float bottom = ImMax(g_grid.y[0], g_grid.y[1]);
        ImGui::SetCursorScreenPos(ImVec2(g_grid.x0, bottom));
        ImGui::Dummy(ImVec2(1.0f, 4.0f));
    }

    template <typename Fn>
    void GridCard(int col, const char* id, const char* title, Fn&& body,
                  const char* count = nullptr)
    {
        const float x = g_grid.x0 + (float)col * (ColW() + kColGap);
        ImGui::SetCursorScreenPos(ImVec2(x, g_grid.y[col]));
        CardBegin(id, title, ColW());
        body();
        CardEnd(count);
        g_grid.y[col] = ImGui::GetCursorScreenPos().y;
    }
}

// ============================================================
// 左侧导航
// ============================================================
static void DrawNavBar(int& page)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();

    // logo 块
    // logo 块：.badge 36×36 圆角 10 / 字号 15；.nm 16px 粗体；.sub 11px
    const ImVec2 l0(origin.x + 18.0f, origin.y + 20.0f);
    const ImVec2 l1(l0.x + 36.0f, l0.y + 36.0f);
    dl->AddRectFilled(l0, l1, Accent(), 10.0f);
    if (g_fontBig)
    {
        const ImVec2 gs = g_fontBig->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "L4");
        dl->AddText(g_fontBig, 15.0f,
                    ImVec2(l0.x + (36.0f - gs.x) * 0.5f, l0.y + (36.0f - gs.y) * 0.5f),
                    IM_COL32(255, 255, 255, 255), "L4");
    }
    if (g_fontMini)
    {
        if (ImFont* fName = ImGui::GetIO().FontDefault)
            dl->AddText(fName, 16.0f, ImVec2(l0.x + 48.0f, l0.y + 2.0f), Ink(), "L4D2");
        dl->AddText(g_fontMini, 11.0f, ImVec2(l0.x + 48.0f, l0.y + 23.0f), InkFaint(), "INTERNAL");
    }

    ImGui::SetCursorPos(ImVec2(12.0f, 82.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 3.0f));
    ImGui::BeginGroup();
    for (int i = 0; i < kPageCount; ++i)
    {
        ImGui::PushID(i);
        if (Tab(kPages[i].glyph, kPages[i].name, page == i))
            page = i;
        ImGui::PopID();
    }
    ImGui::EndGroup();
    ImGui::PopStyleVar();
}

// ============================================================
// 页头：标题 + 一句说明
// ------------------------------------------------------------
// 一律用「窗口原点 + 绝对偏移」画，不走 SetCursorPos/TextUnformatted：
// 后者相对的是窗口内容区原点，会被滚动、游标残留和 child 复位影响，
// 之前标题就是这样被顶下去、还和描述叠在一起。x 一定要加 kNavW。
// ============================================================
// 颜色行：左侧可点击色块（弹出拾色器）+ 右侧标签（照 KeyBindRow 行模式）
static void ColorRow(const char* label, ImColor& col)
{
    // [批111·死代码] 删除了 `const float w = RowWidth();` —— 无人引用
    //（W4 的 C4189 报出）。行宽由 RowPos()/RowWidth() 各自按需取，
    // 这里算出来却不用，只会让人以为色块宽度受它约束。
    const float h = kRowH;
    const ImVec2 p = RowPos();      // 行基准 X：不吃光标自带的 X

    const float line = ImGui::GetTextLineHeight();
    const ImVec2 swPos(p.x + 2.0f, p.y + (h - 20.0f) * 0.5f);
    ImGui::SetCursorScreenPos(swPos);
    ImGui::PushID(label);
    ImGui::ColorButton(label, col.Value,
                       ImGuiColorEditFlags_NoTooltip, ImVec2(20.0f, 20.0f));
    if (ImGui::IsItemClicked())
        ImGui::OpenPopup(label);
    if (ImGui::BeginPopup(label))
    {
        ImGui::ColorPicker4("##picker", &col.Value.x,
                            ImGuiColorEditFlags_AlphaBar
                            | ImGuiColorEditFlags_NoSidePreview);
        ImGui::EndPopup();
    }
    ImGui::PopID();

    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 32.0f, p.y + (h - line) * 0.5f),
                                        Ink(), label);
    RowNext(p.y, h);
}

static void DrawPageHeader(const PageDef& p)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont*  fTitle = ImGui::GetIO().FontDefault;
    const ImVec2 pos = ImGui::GetWindowPos();
    const float  x   = pos.x + kNavW + kPad;

    // .head{top:24} → 竖条 top+2 高 19；标题 16px 行高 20；描述 margin-top 5
    dl->AddRectFilled(ImVec2(x, pos.y + 26.0f), ImVec2(x + 3.0f, pos.y + 45.0f), Accent(), 2.0f);

    if (fTitle)
        dl->AddText(fTitle, 16.0f, ImVec2(x + 12.0f, pos.y + 24.0f), Ink(), p.name);
    if (g_fontMini)
        dl->AddText(g_fontMini, 13.0f, ImVec2(x + 12.0f, pos.y + 49.0f), InkFaint(), p.desc);

    // 右上角「关闭 INS」：.hint{right:0;top:4}，与页头同基线
    if (g_fontMini)
    {
        const char* hint = "关闭";
        const ImVec2 hs = g_fontMini->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, hint);
        const float right = pos.x + kWinW - kPad;
        const float kbdY  = pos.y + 28.0f;
        dl->AddText(g_fontMini, 13.0f,
                    ImVec2(right - hs.x, kbdY + (19.0f - hs.y) * 0.5f), InkFaint(), hint);
        const float kw = KbdWidth("INS");
        Kbd(dl, ImVec2(right - hs.x - 8.0f - kw, kbdY), "INS", InkMuted(), P());
    }
}

// ============================================================
// 背景：静态渐变底 + 双色光晕 + 细网格
// ------------------------------------------------------------
// 不依赖任何外部图片资源（注入环境里纹理加载失败会直接黑屏，
// 用 DrawList 画最稳）。光晕用同心圆叠出径向衰减，网格只铺正文区。
// ============================================================
static void DrawBackdrop(ImDrawList* dl, const ImVec2& pos)
{
    const ImVec2 bodyMin(pos.x + kNavW, pos.y);
    const ImVec2 bodyMax(pos.x + kWinW, pos.y + kWinH);

    dl->PushClipRect(bodyMin, bodyMax, true);

    // 左上淡紫光晕
    for (int i = 9; i >= 1; --i)
    {
        const float r = 78.0f * (float)i;
        const int   a = (int)(7.0f * (float)(10 - i) / 9.0f * 2.0f);
        dl->AddCircleFilled(ImVec2(pos.x + kNavW + 150.0f, pos.y + 70.0f), r,
                            IM_COL32(130, 92, 255, a), 64);
    }
    // 右下淡蓝光晕
    for (int i = 9; i >= 1; --i)
    {
        const float r = 96.0f * (float)i;
        const int   a = (int)(6.0f * (float)(10 - i) / 9.0f * 2.0f);
        dl->AddCircleFilled(ImVec2(pos.x + kWinW - 90.0f, pos.y + kWinH - 60.0f), r,
                            IM_COL32(78, 132, 235, a), 64);
    }

    // 48px 细网格（alpha 4，几乎只提供质感）
    for (float x = bodyMin.x; x < bodyMax.x; x += 48.0f)
        dl->AddLine(ImVec2(x, bodyMin.y), ImVec2(x, bodyMax.y), IM_COL32(255, 255, 255, 4));
    for (float y = bodyMin.y; y < bodyMax.y; y += 48.0f)
        dl->AddLine(ImVec2(bodyMin.x, y), ImVec2(bodyMax.x, y), IM_COL32(255, 255, 255, 4));

    dl->PopClipRect();
}

// ============================================================
//  Toast —— 右下角浮层提示（全局共享，任意页可触发）
// ------------------------------------------------------------
//  为什么单独抽一层而不是写死在系统页：
//   保存 / 读取 / 卸载 / 未来的预设切换都需要同一套"右下角浮出、
//   自动消失"的反馈。写在页面里会导致每个调用点各画一遍。
//
//  绘制位置用【屏幕坐标】而非窗口游标：菜单正文在 child 里带滚动，
//  游标坐标会随滚动/复位变化，用它画浮层会飘。
// ============================================================
namespace
{
    enum class ToastKind { Info, Success, Warn };

    struct ToastState
    {
        std::string text;
        DWORD       bornMs = 0;      // 出现时刻（0 = 无 toast）
        ToastKind   kind   = ToastKind::Info;
        DWORD       lifeMs = 2200;   // 存活时长
    };
    ToastState g_toast;

    void ShowToast(const char* text, ToastKind kind = ToastKind::Success,
                   DWORD lifeMs = 2200)
    {
        g_toast.text   = text;
        g_toast.kind   = kind;
        g_toast.bornMs = GetTickCount();
        g_toast.lifeMs = lifeMs;
    }

    void DrawToast()
    {
        if (!g_toast.bornMs)
            return;
        const DWORD now = GetTickCount();
        const DWORD age = now - g_toast.bornMs;
        if (age >= g_toast.lifeMs)
        {
            g_toast = ToastState{};
            return;
        }

        // 进出场淡入淡出：头 12% 淡入，尾 22% 淡出
        float alpha = 1.0f;
        const float inMs  = (float)g_toast.lifeMs * 0.12f;
        const float outMs = (float)g_toast.lifeMs * 0.22f;
        if ((float)age < inMs)
            alpha = (float)age / inMs;
        else if ((float)(age + (DWORD)outMs) > (float)g_toast.lifeMs)
            alpha = (float)(g_toast.lifeMs - age) / outMs;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos();

        // 轻微上浮（进出场各 6px），比纯淡入更有"弹出"感
        const float rise = (float)age / (float)g_toast.lifeMs * 6.0f;
        const float padX = 14.0f, padY = 10.0f;
        const float fontSz = 13.0f;

        ImFont* f = g_fontMini ? g_fontMini : ImGui::GetIO().FontDefault;
        const ImVec2 ts = f ? f->CalcTextSizeA(fontSz, FLT_MAX, 0.0f,
                                               g_toast.text.c_str())
                            : ImVec2(120.0f, fontSz);

        const float w = ts.x + padX * 2.0f;
        const float h = ts.y + padY * 2.0f;
        const ImVec2 p1(wp.x + kWinW - w - 20.0f - rise,
                        wp.y + kWinH - h - 20.0f - rise);
        const ImVec2 p0(p1.x - w, p1.y - h);

        // 底板
        dl->AddRectFilled(p0, p1,
                          ImColor(22, 26, 34, (int)(236 * alpha)), 8.0f);
        // 左侧状态色条：绿=成功 蓝=信息 橙=警告
        ImU32 bar = IM_COL32(90, 200, 130, (int)(255 * alpha));
        if (g_toast.kind == ToastKind::Info) bar = IM_COL32(96, 165, 250, (int)(255 * alpha));
        if (g_toast.kind == ToastKind::Warn) bar = IM_COL32(245, 180, 90,  (int)(255 * alpha));
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p0.x + 3.0f, p1.y),
                          bar, 8.0f, ImDrawFlags_RoundCornersLeft);

        if (f)
            dl->AddText(f, fontSz,
                        ImVec2(p1.x - ts.x - padX,
                               p1.y - ts.y - padY),
                        ImColor(238, 242, 248, (int)(255 * alpha)),
                        g_toast.text.c_str());
    }
}

// ============================================================
// 菜单主体
// ------------------------------------------------------------
// 页面实现体统一放在本函数【之后】（文件下半部分，按页聚拢），
// 这里只做前向声明 —— switch 里的调用点要早于定义。
// ============================================================
void DrawPageVisual();
void DrawPageAim();
void DrawPageMovement();
void DrawPageSystem();

void DrawMenu() {
    EnsureStyle();

#ifdef OSIMGUI_INTERNAL
    // 字体一次性诊断：中文范围没生效的话整屏就是 ???，这里把实际路径打出来
    static bool s_fontLogged = false;
    if (!s_fontLogged)
    {
        s_fontLogged = true;
        PayloadLog("[menu] 字体 粗体=%s(%.0fpx/%d字) 常规=%s(%.0fpx/%d字) 中文=%s 图集=%dx%d Built=%d 耗时=%.0fms",
                   g_fontBoldPath[0] ? g_fontBoldPath : "(缺失)",
                   g_fontBold ? g_fontBold->FontSize : 0.0f, g_fontBoldGlyphs,
                   g_fontMiniPath[0] ? g_fontMiniPath : "(缺失)",
                   g_fontMini ? g_fontMini->FontSize : 0.0f, g_fontMiniGlyphs,
                   g_fontCjkOk ? "OK" : "未生效(会显示???)",
                   g_atlasW, g_atlasH, g_atlasBuilt ? 1 : 0, g_atlasBuildMs);
    }
#endif

    static int page = 0;
    if (page >= kPageCount)
        page = 0;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2((disp.x - kWinW) * 0.5f, (disp.y - kWinH) * 0.5f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(kWinW, kWinH), ImGuiCond_Always);

    ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoDecoration);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetWindowPos();

    // 面板
    dl->AddRectFilled(pos, ImVec2(pos.x + kWinW, pos.y + kWinH), PanelBg(), 12.0f);
    DrawBackdrop(dl, pos);
    dl->AddRectFilled(pos, ImVec2(pos.x + kNavW, pos.y + kWinH), NavBg(), 12.0f,
                      ImDrawFlags_RoundCornersLeft);
    dl->AddLine(ImVec2(pos.x + kNavW + 0.5f, pos.y + 14.0f),
                ImVec2(pos.x + kNavW + 0.5f, pos.y + kWinH - 14.0f), CardLine(), 1.0f);

    ImGui::BeginChild("##nav", ImVec2(kNavW, kWinH), false, ImGuiWindowFlags_NoScrollbar);
    DrawNavBar(page);
    ImGui::EndChild();

    DrawPageHeader(kPages[page]);

    // 正文：可滚动（内容超出窗口高度时才出现细滚动条）
    ImGui::SetCursorPos(ImVec2(kNavW + kPad, kHeadH));
    ImGui::BeginChild("##body", ImVec2(BodyW(), kWinH - kHeadH - 22.0f), false);
    {
        GridBegin();
        switch (page)
        {
        case 0: DrawPageVisual();   break;
        case 1: DrawPageAim();      break;
        case 2: DrawPageMovement(); break;
        case 3: DrawPageSystem();   break;
        default: break;
        }
        GridEnd();
    }
    ImGui::EndChild();

    // Toast 必须画在 EndChild 之后：这样它相对 child 的滚动不敏感。
    DrawToast();

    ImGui::End();
}

// ============================================================
// 视觉页：绘制元素 / 过滤 / 文字 / 方框样式
// ============================================================
static char* CntText(int on, int total)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", on, total);
    return buf;
}

// ============================================================
//  语义色图例（纯文本行，不改任何配置）
// ------------------------------------------------------------
//  Storm Paint.hpp:205-238 的配色是三档硬编码语义色、不可调：
//    特感 255,128,0 / 普通小僵尸 255,0,0 / 坦克·女巫 0,128,255
//    幸存者 0,255,0 / 场景道具 255,255,255 / 幽灵态 128,128,128
//  原独立「颜色」页只放这一张图例，信息密度太低，2026-10-18 并入
//  视觉页。这里刻意不提供任何改动入口 —— 避免出现
//  「菜单里改了颜色但实际不生效」的假开关。
// ============================================================
namespace {
    void StormLegendRow(const char* label, ImColor c)
    {
        // 不能用 Dummy + GetCursorScreenPos 起手：ItemSize 会把游标 X 重置到
        // 正文窗口行首，图例整列跑到左列坐标上与其它卡片重叠。
        // 一律 RowPos()（行基准 g_rowX）定位，行距 22 显式推进。
        // 另外此前的 Gui.RectangleFilled/Gui.Text 走背景 DrawList，
        // 会被菜单面板底色盖住 —— 图例必须画在窗口 DrawList 上。
        const ImVec2 at = RowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImGui::PushID(label);

        // 色块
        dl->AddRectFilled(ImVec2(at.x, at.y + 4.0f), ImVec2(at.x + 28.0f, at.y + 18.0f), c);
        dl->AddRect(ImVec2(at.x, at.y + 4.0f), ImVec2(at.x + 28.0f, at.y + 18.0f),
                    IM_COL32(0, 0, 0, 180), 1.0f);

        // 色块右侧印同色系文字，保证深色块上也读得出
        if (g_fontMini)
            dl->AddText(g_fontMini, 14.0f, ImVec2(at.x + 36.0f, at.y + 2.0f), c, label);
        else
            dl->AddText(ImVec2(at.x + 36.0f, at.y + 2.0f), c, label);

        ImGui::PopID();

        // 行距 22px（沿用原 Dummy 节奏），游标钉回行基准 X
        ImGui::SetCursorScreenPos(ImVec2(g_rowX, at.y + 22.0f));
    }
}

void DrawPageVisual()
{
    // 2026-10-18：随透视体系换成 Storm 而精简。保留项= 方框 / 右侧名字 /
    // 血条 / 血量 / 距离 / 队友显示 / 道具透视；Storm 带来的新项 = 道具距离截断。
    const int onMain = (enableBox2D ? 1 : 0) + (enableESPName ? 1 : 0) +
                       (enableHealthBar ? 1 : 0) + (enableHealthESP ? 1 : 0) +
                       (enableDistance ? 1 : 0);
    GridCard(0, "espDraw", "绘制元素", [] {
        Toggle("方框", &enableBox2D);
        Toggle("名称", &enableESPName);
        Toggle("血条", &enableHealthBar);
        Toggle("血量数值", &enableHealthESP);
        Toggle("距离", &enableDistance);
    }, CntText(onMain, 5));

    const int onFilter = (showAllies ? 1 : 0) + (filterCommonInfected ? 1 : 0);
    GridCard(1, "espFilter", "过滤", [] {
        Toggle("显示队友", &showAllies);
        Toggle("过滤普通僵尸", &filterCommonInfected);
    }, CntText(onFilter, 2));



    // Storm Paint.hpp:80-191 的框外扩量（原为硬编码 2px）
    GridCard(0, "espBox", "框与血条样式", [] {
        ValueFloat("框外扩", &espBoxPadding, 0.0f, 8.0f, "%.0f", "px");
        ValueInt("填充透明度", &espFillAlpha, 0, 255, "");
        ValueFloat("血条宽度", &healthBarWidth, 2.0f, 12.0f, "%.1f", "px");
        ValueFloat("血条长度", &healthBarHeightRatio, 0.2f, 1.0f, "%.2f", "");
    });

    // 方框颜色（可自定义；掩体/玻璃后 = 「不可见时」色）
    // ⚠ 必须走 RowPos/RowNext 行定位惯例：卡片内 ImGui 控件"新行"会回
    //   窗口原点（跑列+字体重合的根因），每行都要显式 SetCursorScreenPos。
    GridCard(1, "espColor", "方框颜色", [] {
        ColorRow("普通僵尸", g_colInfected);
        ColorRow("特感",     g_colSpecial);
        ColorRow("坦克",     g_colTank);
        ColorRow("女巫",     g_colWitch);
        ColorRow("幸存者",   g_colSurvivor);
        ColorRow("不可见时", g_colHidden);
        Note("点击色块改色；「不可见时」= 被不可穿透物遮挡时的框色");
    });

    GridCard(1, "espText", "文字", [] {
        ValueFloat("名称字号", &nameTextSize, 8.0f, 32.0f, "%.0f", "px");
        ValueFloat("距离字号", &distanceTextSize, 8.0f, 32.0f, "%.0f", "px");
        ValueFloat("血量字号", &healthTextSize, 8.0f, 32.0f, "%.0f", "px");
    });

    // 原独立「颜色」页（2026-10-18 取消）并入本页右列第二行：
    // Storm 配色是三档硬编码语义色、不可调，单占一页只有一个只读图例，
    // 信息密度太低。合并后既保留了"这个颜色代表谁"的说明，
    // 又不会让人误以为改了能生效 —— 所以全部为纯文本行，无改动入口。
    GridCard(1, "espLegend", "语义色图例（不可调）", [] {
        StormLegendRow("特感",        ImColor(255, 128, 0));
        StormLegendRow("普通小僵尸",  ImColor(255, 0, 0));
        StormLegendRow("坦克 / 女巫", ImColor(0, 128, 255));
        StormLegendRow("幸存者",      ImColor(0, 255, 0));
        StormLegendRow("场景道具",    ImColor(255, 255, 255));
        StormLegendRow("幽灵态",      ImColor(128, 128, 128));
    });
}

// ============================================================
// 瞄准页：自瞄 / 安全 / 静默
// ============================================================
static bool g_aimKeyListening = false;

// 热键按钮固定尺寸：文案在「鼠标侧键 1 / V / 未绑定 / 按下任意键…」之间
// 切换时宽度不跳动，右边缘与其它行的控件对齐（行内右留 2px）
static constexpr float kKeyBtnW = 150.0f;
static constexpr float kKeyBtnH = 30.0f;

static const char* VkDisplayName(int vk, char* buf, size_t bufSize)
{
    switch (vk)
    {
    case 0:            return "未绑定";
    case VK_LBUTTON:   return "鼠标左键";
    case VK_RBUTTON:   return "鼠标右键";
    case VK_MBUTTON:   return "鼠标中键";
    case VK_XBUTTON1:  return "鼠标侧键 1";
    case VK_XBUTTON2:  return "鼠标侧键 2";
    case VK_BACK:      return "退格";
    case VK_TAB:       return "Tab";
    case VK_RETURN:    return "回车";
    case VK_SHIFT:     return "Shift";
    case VK_CONTROL:   return "Ctrl";
    case VK_MENU:      return "Alt";
    case VK_PAUSE:     return "Pause";
    case VK_CAPITAL:   return "CapsLock";
    case VK_ESCAPE:    return "Esc";
    case VK_SPACE:     return "空格";
    case VK_PRIOR:     return "PageUp";
    case VK_NEXT:      return "PageDown";
    case VK_END:       return "End";
    case VK_HOME:      return "Home";
    case VK_LEFT:      return "方向键 ←";
    case VK_UP:        return "方向键 ↑";
    case VK_RIGHT:     return "方向键 →";
    case VK_DOWN:      return "方向键 ↓";
    case VK_INSERT:    return "Insert";
    case VK_DELETE:    return "Delete";
    case VK_LSHIFT:    return "左 Shift";
    case VK_RSHIFT:    return "右 Shift";
    case VK_LCONTROL:  return "左 Ctrl";
    case VK_RCONTROL:  return "右 Ctrl";
    case VK_LMENU:     return "左 Alt";
    case VK_RMENU:     return "右 Alt";
    case VK_MULTIPLY:  return "小键盘 *";
    case VK_ADD:       return "小键盘 +";
    case VK_SUBTRACT:  return "小键盘 -";
    case VK_DECIMAL:   return "小键盘 .";
    case VK_DIVIDE:    return "小键盘 /";
    case VK_NUMLOCK:   return "NumLock";
    case VK_SCROLL:    return "ScrollLock";
    default:           break;
    }

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
    {
        snprintf(buf, bufSize, "%c", static_cast<char>(vk));
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        snprintf(buf, bufSize, "F%d", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
    {
        snprintf(buf, bufSize, "小键盘 %d", vk - VK_NUMPAD0);
        return buf;
    }
    snprintf(buf, bufSize, "键码 %d", vk);
    return buf;
}

// 热键绑定器：pKey 指向要写入的按键变量，idSuffix 保证两个按钮 id 不冲突
static int* g_aimKeyTarget = nullptr;

static void DrawAimKeyBinder(int* pKey, const char* idSuffix)
{
    static bool preDown[256] = {};

    if (!g_aimKeyListening)
    {
        char nameBuf[32];
        const char* keyName = VkDisplayName(*pKey, nameBuf, sizeof(nameBuf));
        char label[80];
        snprintf(label, sizeof(label), "%s##aimKeyBtn%s", keyName, idSuffix);

        if (Button(label, ImVec2(kKeyBtnW, kKeyBtnH), BtnOutline))
        {
            g_aimKeyListening = true;
            g_aimKeyTarget    = pKey;
            for (int vk = 1; vk <= 254; ++vk)
                preDown[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("左键绑定按键，右键清除绑定");
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                *pKey = 0;
        }
        return;
    }

    // 监听中：只让真正处于监听态的那个按钮画出"按下任意键…"
    char listeningLabel[80];
    snprintf(listeningLabel, sizeof(listeningLabel), "按下任意键…##aimKeyBtn%s", idSuffix);
    Button(listeningLabel, ImVec2(kKeyBtnW, kKeyBtnH), BtnPrimary);

    if (g_aimKeyTarget != pKey)
        return;                       // 另一个绑定器在监听，本行不抢键

    bool done = false;
    for (int vk = 1; vk <= 254 && !done; ++vk)
    {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!down)
        {
            preDown[vk] = false;
            continue;
        }
        if (preDown[vk])
            continue;
        if (vk == VK_ESCAPE)
            done = true;
        else if (vk == VK_INSERT)
            continue;
        else
        {
            *pKey = vk;
            done = true;
        }
    }
    if (done)
    {
        g_aimKeyListening = false;
        g_aimKeyTarget    = nullptr;
    }
}

// 标签 + 右侧控件的通用行（热键绑定用）
static void KeyBindRow(const char* label, void (*draw)())
{
    const float w = RowWidth();
    const float h = kRowH;
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 p = RowPos();      // 行基准 X：不吃光标自带的 X

    ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 2.0f, p.y + (h - line) * 0.5f),
                                        InkLabel(), label);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - kKeyBtnW - 2.0f, p.y + (h - kKeyBtnH) * 0.5f));
    draw();
    RowNext(p.y, h);
}

// ============================================================
//  [批61] 拟人预设（Human-like preset）
// ------------------------------------------------------------
//  设计目标（用户原话：像人类、不抢鼠标、拉枪正常）：
//
//   1) 【不抢鼠标】—— 靠三个旋钮配合，而不是把辅助关掉：
//      · deadzone 0.25°  ：只要准星已经在目标附近，辅助就完全不出手。
//        这是"抢鼠标"感的最主要来源：一点点偏差就被持续修正，
//        手会明显感到鼠标被"吸住"。死区把这种微修正彻底关掉。
//      · FOV 不在预设里   ：选靶锥由用户自己定（个人手感偏好的核心，
//        任何"推荐值"都不该覆盖它）。FOV 越小越不抢手，FOV 越大越
//        容易替人甩枪 —— 这个尺度交给用户。
//      · smoothFactor 0.82：每 tick 只走剩余距离的 18%（66tick 下约
//        0.09s 收敛一半）。够快跟上目标，又不会"啪"一下粘上去。
//
//   2) 【拉枪正常】—— 保留人类可达的跟随能力：
//      · reactionMs 140~240：真人从看到目标到动手就是 150~250ms。
//        设成 0 会让辅助在你自己动手之前抢先发力，手感变"自动"。
//      · jitter 0.04°    ：静止时轻微手抖；移动时自动关闭（代码内建）。
//      · aimPart 4 自动  ：头可见打头，否则打胸 —— 和真人选择一致。
//      · distWeight 0.35 ：偏向离准星近的目标，避免"跳过近处打远处"。
//      · maxDistance 45m ：真人在 60m 外的命中率本来就低，锁远处反而假。
//
//   3) 反检测角度：这套参数不改变任何"服务端可见"的位移形状
//      （平滑系数控制的是每 tick 步长，仍在 4.5°/tick 的反检测上限内）。
//
//  ⚠ 数值只写在这里一次：卡片显示与应用按钮都读同一份，
//    避免"卡片显示 0.82、实际应用 0.75"这种漂移。
// ============================================================
namespace
{
    struct HumanPreset
    {
        // ⚠ [批63] 预设【不含选靶范围类参数】—— 用户明确要求自己定：
        //   · FOV       选靶锥
        //   · 最大距离   多远还锁
        //   · 选靶权重   距离与屏幕角距的配比
        //   这三个是"打多远、锁多广"的个人手感偏好的核心，
        //   任何推荐值都不该覆盖；而且菜单里必须有对应滑条能改。
        int   reactionMs   = 140;     // 反应延迟下限
        int   reactionMax  = 240;     // 反应延迟上限
        float smooth       = 0.82f;   // 平滑系数（越大越慢）
        float jitter       = 0.04f;   // 微抖动（度）
        float deadzone     = 0.25f;   // 死区（度）—— 不抢鼠标的关键
        int   aimPart      = 4;       // 4 = 自动（头优先）
        bool  hitVerify    = true;    // 只锁可见目标
        bool  recoilComp   = true;    // 后坐力补偿
    };

    const HumanPreset& HumanPresetRef()
    {
        static const HumanPreset kPreset;
        return kPreset;
    }

    void ApplyHumanPreset()
    {
        const HumanPreset& p = HumanPresetRef();
        // FOV / 最大距离 / 选靶权重 都不在这里：它们是"打多远、锁多广"
        // 的个人手感，由用户在自瞄核心卡里自己调，预设一律不覆盖。
        g_Config.aim.aimPart        = p.aimPart;
        g_Config.aim.hitVerify      = p.hitVerify;
        g_Config.aim.recoilComp     = p.recoilComp;

        g_Config.humanize.enabled      = true;
        g_Config.humanize.smoothFactor = p.smooth;
        g_Config.humanize.reactionMs   = p.reactionMs;
        g_Config.humanize.reactionMsMax= p.reactionMax;
        g_Config.humanize.jitterAngle  = p.jitter;
        g_Config.humanize.deadzone     = p.deadzone;
    }

    // 当前设置是否与预设一致（逐项比较，用于数值行着色）
    bool SameF(float a, float b) { return fabsf(a - b) < 0.001f; }
}

void DrawPageAim()
{
    // ============================================================
    //  瞄准页（Legit + PSilent）—— 参数直连 g_Config，实时生效
    // ------------------------------------------------------------
    //  排版铁律：全部控件走 MenuSkin 行体系（RowPos/SetCursorScreenPos
    //  + RowNext），零 Dummy 起手 —— 复用视觉页验证过的控件。
    // ============================================================
    GridCard(0, "aimCore", "自瞄核心", [] {
        Toggle("自瞄", &g_Config.aim.enabled);
        KeyBindRow("触发键", [] {
            DrawAimKeyBinder(&g_Config.aim.key, "aim");
        });
        Toggle("FOV 范围圈", &g_Config.aim.showFov);
        Toggle("微静默", &g_Config.aim.silent);      // [批31] 独立功能
        ValueFloat("静默 FOV", &g_Config.aim.silentFov, 1.0f, 15.0f, "%.0f", "°");
        Hint("目标进这个锥内静默才动手");
        Segmented("瞄准部位", &g_Config.aim.aimPart,
                  [] {
                      static const char* kParts[] = { "头", "颈", "胸", "腹", "自动" };
                      return kParts;
                  }(), 5);
        ValueFloat("FOV", &g_Config.aim.fov, 1.0f, 15.0f, "%.1f", "°");

        // [批64] 选靶范围的两个参数交给用户自己调 —— 原来它们被
        //   "拟人预设"固定成 45m / 0.35，菜单里又没有入口，
        //   等于改不了。预设已不再覆盖这两项。
        ValueFloat("最大距离", &g_Config.aim.maxDistance, 5.0f, 150.0f, "%.0f", "m");
        ValueFloat("选靶权重", &g_Config.aim.distWeight, 0.0f, 1.0f, "%.2f", "");
        Hint("权重0=只看准星远近，越大越优先近处目标");
    });

    // ---- [批61] 拟人预设卡：把每个数值钉在页面上 ----
    GridCard(0, "aimPreset", "拟人预设", [] {
        const HumanPreset& p = HumanPresetRef();

        if (Button("应用拟人预设", ImVec2(150.0f, 32.0f), BtnPrimary))
        {
            ApplyHumanPreset();
            ShowToast("已应用拟人预设", ToastKind::Success);
            PayloadLog("[menu] 应用拟人预设：平滑=%.2f 反应=%d~%dms "
                       "抖动=%.2f 死区=%.2f（FOV 未改动，保持用户值 %.1f）",
                       p.smooth, p.reactionMs, p.reactionMax,
                       p.jitter, p.deadzone, g_Config.aim.fov);
        }

        char buf[48];

        snprintf(buf, sizeof(buf), "%d ~ %d ms", p.reactionMs, p.reactionMax);
        PinRow("反应延迟", buf,
               g_Config.humanize.reactionMs == p.reactionMs &&
               g_Config.humanize.reactionMsMax == p.reactionMax);

        snprintf(buf, sizeof(buf), "%.2f", p.smooth);
        PinRow("平滑系数", buf, SameF(g_Config.humanize.smoothFactor, p.smooth));

        snprintf(buf, sizeof(buf), "%.2f°", p.jitter);
        PinRow("微抖动", buf, SameF(g_Config.humanize.jitterAngle, p.jitter));

        snprintf(buf, sizeof(buf), "%.2f°", p.deadzone);
        PinRow("死区", buf, SameF(g_Config.humanize.deadzone, p.deadzone));

        PinRow("瞄准部位", "自动（头优先）", g_Config.aim.aimPart == p.aimPart);
        PinRow("可见验证", p.hitVerify ? "开" : "关",
               g_Config.aim.hitVerify == p.hitVerify);
        PinRow("后坐力补偿", p.recoilComp ? "开" : "关",
               g_Config.aim.recoilComp == p.recoilComp);
    });

    GridCard(1, "aimHumanize", "拟人化", [] {
        ValueFloat("平滑系数", &g_Config.humanize.smoothFactor, 0.05f, 0.95f, "%.2f", "");
        Hint("越大越慢，0.82≈每tick走18%");
        ValueInt("反应下限", &g_Config.humanize.reactionMs, 0, 400, "ms");
        ValueInt("反应上限", &g_Config.humanize.reactionMsMax, 0, 600, "ms");
        ValueFloat("微抖动", &g_Config.humanize.jitterAngle, 0.0f, 0.5f, "%.2f", "°");
        ValueFloat("死区", &g_Config.humanize.deadzone, 0.0f, 2.0f, "%.2f", "°");
        Hint("准星已在目标上时不修正的范围");
    });
}

void DrawPageMovement()
{
    // ============================================================
    //  移动页（自动连跳 / 自动连发）—— [批46]
    // ------------------------------------------------------------
    //  两个功能【刻意不暴露参数】给用户（连跳）：
    //  落地延迟、按住时长、跳过概率都是反检测常量。开放成滑条等于
    //  邀请用户把参数调到"更快更准"，那正好是检测算法数的东西。
    //  连发只暴露间隔，且下限被硬钳在反检测红线之上。
    // ============================================================
    GridCard(0, "mvBhop", "自动连跳", [] {
        Toggle("自动连跳", &g_Config.combat.bhop);
    });

    GridCard(1, "mvFire", "自动连点", [] {
        Toggle("自动连点", &g_Config.combat.autoFire);
        Hint("按住下面的键 = 替你按左键");
        KeyBindRow("连点键", [] {
            DrawAimKeyBinder(&g_Config.combat.autoFireKey, "af");
        });
        ValueInt("连点间隔", &g_Config.combat.autoFireMs,
                 ConfigCombat::kAutoFireMsMin, ConfigCombat::kAutoFireMsMax, "ms");
        Hint("低于52ms会被判连发脚本，已硬性限制");

        // [批77] 去后坐力挪进这张卡。
        //   原来单独一张「画面」卡，实测用户根本没找到/没点上，
        //   日志里连一行都没有 —— 功能没开却被当成"无效"。
        //   放进同卡后，开关与连点永远一起可见。
        Toggle("移除画面抖动", &g_Config.combat.noRecoilVis);
        Hint("只稳屏幕（渲染层去抖），输入角与准星回弹保留");
    });
}

void DrawPageSystem()
{
    // [批47] 修复"保存配置无效"：本工程有三套互相独立的持久化路径，
    //   ① EspConfig    → esp_config.json（ESP 全部字段）
    //   ② AimConfig    → aim_config.ini （aim + humanize）
    //   ③ ConfigMgr    → config.json    （其余全部：combat/anti/crosshair/
    //                                   aim.fov 等 combat.bhop/autoFire…）
    //  旧代码只调了 ①②，③ 从未被菜单触发 —— 于是 combat/anti/crosshair
    //  这几十个字段改了永远不落盘，重启即丢。表现就是"保存了但没反应"。
    //  三套缺任何一套都是静默丢数据，所以这里必须全调。
    GridCard(1, "sysCfg", "配置 (cfg)", [] {
        const float bw = 145.0f;

        // ---- 保存 ----
        // 对外行为是 cfg（可读、可分享）；内部三路仍然落盘，
        // 否则 ESP 那些不走 cfg 的字段会丢。两者不冲突：
        // cfg 是"人看的那份"，json/ini 是"机器那份"。
        if (Button("保存 cfg", ImVec2(bw, 32.0f), BtnPrimary))
        {
            EspConfigSaveNow();                        // ESP（json）
            const bool ok2 = Aimbot::AimConfigSave();  // 自瞄（ini）
            const bool ok3 = ConfigMgr::TrySaveNow();  // 其余（json）

            std::string err;
            const bool okc = CfgFile::Save(CfgFile::DefaultPath(), &err);
            PayloadLog("[menu] 配置已保存 cfg=%s（ESP/自瞄=%s 全局=%s）%s",
                       okc ? "OK" : "失败", ok2 ? "OK" : "失败",
                       ok3 ? "OK" : "失败", okc ? "" : err.c_str());
            if (okc)
                ShowToast("已保存 hanxuesnow.cfg", ToastKind::Success);
            else
                ShowToast("cfg 写入失败，详见日志", ToastKind::Warn, 3200);
        }

        ImGui::SameLine(0.0f, 8.0f);
        if (Button("打开目录", ImVec2(110.0f, 32.0f), BtnOutline))
        {
            // 分享给别人 = 把这个目录里的 .cfg 发过去
            ShellExecuteA(nullptr, "open", CfgFile::Dir().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }

        // ---- [批65] 一键读取默认 cfg ----
        //  之前只能"在列表里选中 → 确认导入"，用户手改完自己的
        //  hanxuesnow.cfg 之后没有直达入口，观感就是"菜单不读 cfg"。
        if (Button("读取 cfg", ImVec2(110.0f, 30.0f), BtnOutline))
        {
            int applied = 0, ignored = 0;
            std::string err;
            const bool ok = CfgFile::Load(CfgFile::DefaultPath(),
                                          &applied, &ignored, &err);
            PayloadLog("[menu] 读取默认 cfg：%s 应用 %d 项，忽略 %d 项%s",
                       ok ? "OK" : "失败", applied, ignored,
                       ok ? "" : err.c_str());
            if (ok)
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "已读取 %d 项", applied);
                ShowToast(msg, ToastKind::Success);
            }
            else
            {
                ShowToast("没有 hanxuesnow.cfg", ToastKind::Warn, 2800);
            }
        }

        // ---- 导入别人的 cfg ----
        static std::vector<std::string> s_files;
        static float s_nextScan = 0.0f;
        static bool  s_scanned  = false;
        static int   s_sel = -1;
        static bool  s_confirm = false;

        // ⚠ [批62 修复] 原来写的是 `if (s_files.empty() || now - last > 2)`
        //   —— 目录为空时 s_files 永远是空，条件恒真 → **每一帧**都执行
        //   FindFirstFile。菜单打开时每帧一次磁盘枚举，直接掉帧。
        //   改成纯时间驱动：只在到点（或首次）扫描，与结果是否为空无关。
        // [批99] GetTime() 返回 double → 显式窄化（消掉 C4244）
        const float nowSec = static_cast<float>(ImGui::GetTime());
        if (!s_scanned || nowSec >= s_nextScan)
        {
            s_files   = CfgFile::List();
            s_scanned = true;
            s_nextScan = nowSec + 2.0f;          // 2s 一次，跟手且不浪费
            if (s_sel >= (int)s_files.size()) s_sel = -1;
        }

        if (s_files.empty())
        {
            Note("把别人的 .cfg 放进上面这个目录即可导入");
        }
        else
        {
            for (int i = 0; i < (int)s_files.size(); ++i)
            {
                const bool sel = (s_sel == i);
                if (ImGui::Selectable(s_files[i].c_str(), sel))
                {
                    s_sel = i;
                    s_confirm = false;
                }
            }

            if (s_sel >= 0 && s_sel < (int)s_files.size())
            {
                if (!s_confirm)
                {
                    if (Button("导入选中", ImVec2(110.0f, 30.0f), BtnOutline))
                        s_confirm = true;
                }
                else
                {
                    Note("导入会用该 cfg 覆盖当前玩法设置");
                    if (Button("确认导入", ImVec2(88.0f, 30.0f), BtnDanger))
                    {
                        const std::string path =
                            CfgFile::Dir() + "\\" + s_files[s_sel];
                        int applied = 0, ignored = 0;
                        std::string err;
                        const bool ok = CfgFile::Load(path, &applied, &ignored, &err);
                        PayloadLog("[menu] cfg 导入 %s：应用 %d 项，忽略 %d 项%s",
                                   s_files[s_sel].c_str(), applied, ignored,
                                   ok ? "" : "（打开失败）");
                        if (ok)
                            ShowToast("cfg 已导入", ToastKind::Success);
                        else
                            ShowToast("cfg 打开失败", ToastKind::Warn, 3000);
                        s_confirm = false;
                        s_sel = -1;
                    }
                    ImGui::SameLine(0.0f, 8.0f);
                    if (Button("取消", ImVec2(70.0f, 30.0f), BtnOutline))
                        s_confirm = false;
                }
            }
        }
    });

#ifdef OSIMGUI_INTERNAL
    GridCard(0, "sysUnload", "卸载", [] {
        static bool confirmUnload = false;
        if (!confirmUnload) {
            if (Button("卸载注入", ImVec2(150.0f, 32.0f), BtnDanger))
                confirmUnload = true;
        } else {
            Note("确认卸载？");
            if (Button("确认", ImVec2(88.0f, 30.0f), BtnDanger)) {
                PayloadLog("[menu] 卸载注入：停止全部功能、保存配置并移除渲染挂钩...");
                PayloadRequestUnload();
            }
            ImGui::SameLine(0.0f, 8.0f);
            if (Button("取消", ImVec2(88.0f, 30.0f), BtnOutline))
                confirmUnload = false;
        }
    });
#endif

    // ============================================================
    //  [批60] 免责声明 —— 用户要求加回系统页
    // ============================================================
    GridCard(0, "sysAbout", "免责声明", [] {
        Note("寒山雪 Hanshanxue · v1.0");
        Note("本工具由 AI 辅助生成与维护，仅供内部技术研究与个人测试。");
        Note("⚠ 请勿用于商业分发、公开传播或破坏游戏公平性的场景。");
        Note("⚠ 使用第三方工具可能违反游戏服务条款，风险自负。");
        Note("作者不对任何封号、数据损坏或衍生纠纷负责。");
    });
}

void DrawCallBack() {
    try {
#ifdef OSIMGUI_INTERNAL
    if (Gui.Window.hWnd) {
        RECT rc{};
        if (GetClientRect(Gui.Window.hWnd, &rc))
            Gui.Window.Size = Vec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
    }
#endif
    Neverlus::SetupFonts();

    HandleMenuToggle();
    if (!menuOpen)
        g_aimKeyListening = false;

    EspConfigTick();


    // 帧数据：渲染线程直读实体列表（零 RPM、零快照年龄）
    LiveFrameBegin();
    FrameCacheBegin();
    BoneFrameBegin();
    HitboxFrameBegin();
    EspFrameBegin();

    // [批30] 骨骼缓存刷新（渲染线程）：引擎骨骼取数只在这里发生——
    // 游戏线程绝不碰引擎骨骼函数（召唤瞬间崩溃的根治）。
    Aimbot::RefreshBonesForFrame();

    // 2026-10-18：自瞄选靶已并入 CreateMove 侧的 Storm 管线
    // （StormAim.cpp 直接消费上面 LiveFrameBegin 发布的渲染帧快照，
    //   不再需要渲染线程→游戏线程的候选通道 AimBridge）。

    if (menuOpen)
        DrawMenu();

    // 2026-10-18：透视已整体换成 Storm 体系（DrawStormEsp 内部自己消费
    // enableBox2D / enableESPName / enableHealthBar / enableHealthESP /
    // enableDistance 这五个开关，并负责"从远到近"的绘制顺序）。
    // ESP 文字统一用 g_fontEsp（18px 常规雅黑 = 原 OS-ImGui 默认字体）：
    // Gui.Text 走 ImGui::GetFont()，不推字体就会变成菜单的 16px 粗体。
    if (enableBox2D || enableESPName || enableHealthBar ||
        enableHealthESP || enableDistance)
    {
        if (g_fontEsp)
            ImGui::PushFont(g_fontEsp);
        DrawStormEsp();
        if (g_fontEsp)
            ImGui::PopFont();
    }

    // FOV 范围圈（自瞄反馈，渲染线程安全：只读 g_Config + DisplaySize）
    Aimbot::RenderFovRing();

    DrawEspPreviewWindow();

    }
    catch (...) {
        static DWORD s_eLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_eLog > 5000)
        {
            s_eLog = t;
            PayloadLog("[hooks] 渲染帧异常已拦截（5s 节流）");
        }
    }
}

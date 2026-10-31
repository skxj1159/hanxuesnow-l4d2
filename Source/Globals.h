#pragma once

#include "include.h"
#include <mutex>     // [批153] 统一接口获取锁
#include <atomic>

extern bool menuOpen;

extern HANDLE hProcess;
extern uintptr_t clientBase;
extern uintptr_t engineBase;
extern uintptr_t localPlayer;

// ============================================================
//  透视配置（2026-10-18 全量替换为 Storm Paint.hpp 体系）
// ------------------------------------------------------------
//  Storm 只有一个方框 + 一个右侧名字标签，配色是三档硬编码语义色。
//  保留 ours 的三项：血条 / 血量数字 / 距离。
//  已删除：boxStyle（4 种样式）、enableSnapLine（断头台连线）、
//  enableSkeletonEsp、enableEnemyCounter、24 个可调 ImColor、
//  ignoreGhostSpecials / filterCommonInfected（收录口径改由
//  IdentifierSystem + Storm 语义统一决定）。
// ============================================================

extern bool showAllies;        // 显示队友（幸存者）
extern bool enableBox2D;       // Storm 方框
extern bool enableHealthBar;   // 血条（ours 保留）
extern bool enableHealthESP;   // 血量数字（ours 保留）
extern bool enableDistance;    // 距离文本（ours 保留）
extern bool enableESPName;     // 右侧名字标签（Storm 位置）
// 道具透视已整体拆除（2026-10-18，enableEquipment/equipmentDistance 随之删除，
// 菜单视觉页留占位卡片，待重建后恢复）

// ---- 保留 ours 的样式参数（只服务血条与文字；方框已固定为 Storm 画法）----
inline float healthBarWidth       = 4.0f;   // 血条宽度（px）
inline float healthBarHeightRatio = 1.0f;   // 血条高 = 方框高 × 比例
inline float nameTextSize         = 16.0f;  // 名字字号（px）
inline float distanceTextSize     = 16.0f;  // 距离字号（px）
inline float healthTextSize       = 16.0f;  // 血量文字字号（px）

extern bool showEspPreview;

// ---- Storm 语义参数 ----
// 方框外扩（px）。原 stormcfg::stormEspPadding，自瞄体系拆除后收编为视觉参数。
inline float espBoxPadding = 2.0f;

// 方框填充透明度（0~255，0 = 不显示填充，只有描边框）
inline int espFillAlpha = 128;

// ---- 方框颜色（可自定义，EspConfig colors 节持久化）----
inline ImColor g_colInfected (255,   0,   0, 255);  // 普通小僵尸
inline ImColor g_colSpecial  (255, 128,   0, 255);  // 特感
inline ImColor g_colTank     (  0, 128, 255, 255);  // 坦克
inline ImColor g_colWitch    (255, 105, 180, 255);  // 女巫
inline ImColor g_colSurvivor (  0, 255,   0, 255);  // 幸存者
inline ImColor g_colHidden   (120, 120, 120, 255);  // 不可见（掩体/玻璃后）

// 可见性开关（掩体判定：EngineTrace + 材质穿透白名单；玻璃=不可见）
inline bool enableVisibility = true;

// 过滤普通僵尸（sid=Infected 不画框； Tank/女巫/特感不受影响）
inline bool filterCommonInfected = true;

// 本地玩家实体索引（LoopEntity 维护）
inline int localPlayerIndex = -1;

// [批116] g_viewFovDeg 已删除：它由 OverrideView 自定位采样，实测会采到
//   随视角/跳跃变化的字段（FOV 圈一大一小、跳跃缩小）。FOV 圈现在直接
//   由视图矩阵行范数求垂直 FOV，精确且稳定（见 RenderFovRing）。

// [批125] 崩溃心跳：后台线程每秒输出 phase/ent。
//   __fastfail(0xc0000409) 不会走 UEF，但日志每行都 fflush —— 崩溃前
//   最后一条 [hb] 就是现场：phase 表明死在哪个阶段，ent 是最后处理的实体。
inline volatile LONG g_hbPhase = 0;   // 0=空闲 1/2/3=CreateMove 阶段 10=取骨循环 20=OverrideView
inline volatile LONG g_hbEnt   = -1;  // 取骨循环最后一个实体索引

// [批146] 普通感染者（common infected）的实体 vtable：
//   渲染线程分类时遇到 sid==kInfected 就发布一次；游戏线程的射线过滤器
//   据此判断"射线碰到的这个实体是不是小僵尸"→ 命中就当作透明（穿透）。
//   指针宽度在 x86 上读写天然原子；没采到就是 nullptr（过滤器不生效）。
inline void* g_commonInfectedVtable = nullptr;

// [批146] 取骨就绪度：渲染线程每帧发布，游戏线程据此在"大面积未就绪"
//   （切图 / 实体刚生成）时整帧跳过自瞄，避免几十个候选逐个失败。
inline volatile LONG g_boneNotReady   = 0;   // 本轮未就绪实体数
inline volatile LONG g_boneCandidates = 0;   // 本轮候选实体数

// [批74] 自动连点宏本 tick 是否在"替玩家按左键"。
//   Movement 每 tick 写，Aimbot 读。
//   为什么需要它：自瞄的触发键门控读的是 GetAsyncKeyState（真实按键），
//   而宏是通过 cmd->buttons 发 IN_ATTACK，真实左键并没有按下 ——
//   结果就是"宏在开枪，自瞄却认为你没按触发键"。
//   放全局而不是让 Aimbot include Movement：瞄准模块不该依赖操作模块，
//   一个 bool 足够，还能避免头文件成环。
inline bool g_autoFirePressing = false;

// ============================================================
//  [批105/批107] 游戏窗口状态查询（切窗口闪退修复共用）
// ------------------------------------------------------------
//  Alt+Tab / 最小化时，引擎可能重建渲染与动画状态。此时我们拿到的是
//  半成品数据（实测 punch 估计值飙到 220°），写进去 = 崩。
//  渲染线程与骨骼刷新都要先问一句这个。
//
//  [批107·严重设计错误修正]
//  原实现靠 `inline HWND g_gameWndShared` 由 dllmain 赋值。这里有
//  两个致命问题：
//    ① dllmain.cpp 根本没 include Globals.h → 它写的那份和 Hooks/Bone
//       读的那份不是同一个对象（ODR 陷阱）→ 读侧永远是 nullptr
//    ② 门控一旦恒为 true，**所有功能层全被静默跳过** —— 表现就是
//       "连跳失效、自瞄不动"，而且日志里一个字都没有（因为门控在
//       最前面就 return 了）。这正是批105 交付后用户报"连跳失效"
//       的根因，不是按住式连跳本身有问题。
//
//  修法：不再依赖任何共享变量（那是 ODR 陷阱）。判据只用
//  GetForegroundWindow + 归属校验：前台窗口属于本进程 → 可用；
//  不属于本进程（Alt+Tab 走了）或被最小化/隐藏 → 不可用。
//  相比 FindWindowA("SDL_app")，这个办法还有个好处：不需要知道
//  引擎的窗口类名（Source 各作/各平台都不一样），也不会误匹配到
//  库自己创建的临时窗口（dllmain 里 FindGameWindow 特意做了
//  "面积最大 + 与 Gui.Window.hWnd 一致"双重确认，正是这个坑）。
// ============================================================
inline bool GameWindowUnavailable()
{
    const DWORD self = ::GetCurrentProcessId();
    const HWND  fg   = ::GetForegroundWindow();

    // 前台窗口不属于本进程 = 玩家切到别的程序了 → 引擎状态可能正在重建
    DWORD fgPid = 0;
    if (fg)
        ::GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid != self)
        return true;                                // 切到别的程序了

    // 属于本进程：再排除最小化 / 不可见（Alt+Tab 不一定会换前台窗口，
    // 但最小化一定换）
    if (!fg || ::IsIconic(fg) || !::IsWindowVisible(fg))
        return true;

    return false;
}

// ============================================================
//  [批100] punch 字段定位结果 —— 跨线程共享
// ------------------------------------------------------------
//  分工：
//    · OverrideView（渲染线程）拿得到 view->angles 与引擎视角角，
//      用两者的差（= punch 估计）去反向定位 m_Local.m_vecPunchAngle。
//      定位成功后把偏移写到这里。
//    · CreateMove（游戏线程）读偏移拿到引擎权威 punch，用
//      SetViewAngles 把视角设成「当前视角 - punch」——
//      这是引擎接口（已验证槽位 20），不依赖 ViewSetup 的 angles 偏移。
//  offset 相对 localPlayer；-1 = 未锁定（此时回退 OverrideView 估算）。
// ============================================================
inline int g_punchOffset = -1;
inline int g_punchSign   = 0;      // 0=未定 1=减 -1=加（符号自检结果）

// ============================================================
//  [批153] 引擎接口获取的唯一安全入口
// ------------------------------------------------------------
//  引擎的 CreateInterface 内部是【遍历接口链表 + 字符串比较】，不可重入。
//  我们有两个线程会取接口（渲染线程：ESP/DrawEsp/OverrideView；
//  游戏线程：自瞄可见性/视角），旧的 `static bool s_tried` 裸标志会让
//  两边同时进去 → 崩在 VCRUNTIME 的 strcmp/memcpy（access=null）。
//  这里用一个 inline 函数的函数局部 static 互斥体 —— C++11 保证
//  inline 函数的静态局部变量全程序唯一，跨 TU 也是同一把锁。
// ============================================================
inline void* GetModuleInterfaceLocked(const char* moduleName, const char* iface)
{
    static std::mutex s_mx;
    std::lock_guard<std::mutex> lk(s_mx);
    const HMODULE mod = ::GetModuleHandleA(moduleName);
    if (!mod)
        return nullptr;
    using CreateInterfaceFn = void* (*)(const char*, int*);
    const auto fn = reinterpret_cast<CreateInterfaceFn>(
        ::GetProcAddress(mod, "CreateInterface"));
    return fn ? fn(iface, nullptr) : nullptr;
}

// ============================================================
//  Hooks/Hooks.cpp —— 挂钩层装配（**只做触发，不写任何逻辑**）
// ------------------------------------------------------------
//  【本文件职责】
//    · 解析 ClientMode（签名法，本机/本游戏版本验证过）：
//        "89 04 B5 ? ? ? ? E8"  →  clientMode = **(void***)(sig + 3)
//    · MinHook 挂 ClientMode::CreateMove（虚表槽 27，L4D2 双源验证）
//    · Hooked_CreateMove 内部只做两件事：
//        ① 调原函数（游戏先把 cmd 填充完）
//        ② Aimbot::CreateMove(cmd)   ← 功能逻辑全部在 Features 层
//  · 本文件不出现任何 if/循环/内存读/角度数学 —— 契约见 Hooks.h
//
//  【签名与槽位的出处】
//    签名与槽 27 在本机 + 本游戏版本实测验证过
//    （备份 _removed_aim_20261018/Aim/AimCore2.cpp:658-721）。
//    公开 L4D2 内部项目（Panguins/L4D2_Internal、l4d2-internal-base）
//    同样挂 ClientMode 槽 27，签名 `bool(float, CUserCmd*)`。
// ============================================================

#include "Hooks.h"
#include "../Features/Aimbot.h"
#include "../Features/Movement.h"
#include "../SDK/Cmd.h"
#include "../PayloadControl.h"
#include "../Minhook/MinHook.h"
#include "../Config/Config.h"     // [批84] g_Config.combat.noRecoilVis
#include "../Features/ViewPunch.h" // [批85] angles 定位（纯函数，可离线测试）
#include "../Features/PunchProbe.h" // [批99] m_Local.m_vecPunchAngle 反向定位
#include "../BoneSystem.h"         // [批99] BoneSysLocalPlayerPtr()
#include "../Globals.h"          // [批88] g_autoFirePressing（宏代按左键）

#include <Windows.h>
#include <cstring>
#include <cmath>
#include <intrin.h>     // [批113] _ReturnAddress()

namespace
{
    // ---- 原函数 trampoline ----
    using CreateMove_t = bool(__fastcall*)(void*, void*, float, void*);
    CreateMove_t g_origCreateMove = nullptr;

    // ---- 状态 ----
    uintptr_t g_clientBase  = 0;
    size_t    g_clientSize  = 0;
    uintptr_t g_cmTarget    = 0;     // ClientMode::CreateMove 目标地址
    bool      g_hookInstalled = false;

    // ---- 模块尺寸（PE 头解析，签名扫描边界）----
    bool ResolveClientModule()
    {
        const HMODULE h = ::GetModuleHandleA("client.dll");
        if (!h)
            return false;
        g_clientBase = reinterpret_cast<uintptr_t>(h);

        // DOS → NT → SizeOfImage
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_clientBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            g_clientBase + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;
        g_clientSize = nt->OptionalHeader.SizeOfImage;
        return g_clientSize > 0x1000;
    }

    bool InClientRange(uintptr_t addr)
    {
        return addr >= g_clientBase && addr < g_clientBase + g_clientSize;
    }

    // 字节模式扫描（'?' = 通配；形如 "89 04 B5 ? ? ? ? E8"）
    uintptr_t FindPattern(const char* pattern)
    {
        // 预解析
        struct Byte { unsigned char v; bool wild; };
        Byte bytes[32] = {};
        int  count = 0;
        const char* p = pattern;
        while (*p && count < 32)
        {
            if (*p == ' ') { ++p; continue; }
            if (*p == '?') { bytes[count].wild = true; ++count; ++p; continue; }
            char hex[3] = { p[0], p[1], 0 };
            bytes[count].v    = (unsigned char)strtoul(hex, nullptr, 16);
            bytes[count].wild = false;
            ++count;
            p += 2;
        }
        if (count == 0)
            return 0;

        const unsigned char* base = reinterpret_cast<const unsigned char*>(g_clientBase);
        const size_t last = g_clientSize - count;
        for (size_t off = 0; off < last; ++off)
        {
            bool match = true;
            for (int i = 0; i < count; ++i)
            {
                if (bytes[i].wild)
                    continue;
                if (base[off + i] != bytes[i].v) { match = false; break; }
            }
            if (match)
                return g_clientBase + off;
        }
        return 0;
    }

    bool IsUserPtr(uintptr_t addr)
    {
        return addr > 0x10000 && addr < 0x7FFF0000;
    }

    // ============================================================
    //  [批84] 移除画面后坐力 —— 按开源实现改走 OverrideView
    // ------------------------------------------------------------
    //  资料依据（三个独立来源指向同一做法）：
    //    · Panguins/L4D2_Internal（L4D2 内部作弊）
    //        hooks::override_view = clientmode vtable 槽 19
    //    · UnknownCheats《CSGO:NoVisRecoil》
    //        在 FRAME_RENDER_START 保存 punch→置 0→调原函数→恢复
    //    · GMod 讨论串
    //        "in OverrideView just do setup->angles -= m_vecPunchAngle()"
    //
    //  共同点：**在视角计算阶段扣掉 punch**，而不是去改实体内存。
    //  这也解释了为什么我们前面几次"写 m_Local"会崩 —— 写的是实体内存，
    //  而正确做法只需要改这一帧的 ViewSetup。
    //
    //  本实现不需要知道 m_Local 偏移，也不需要知道 CViewSetup 的字段偏移：
    //    · punch 抵消的等价形式 = 把 angles 直接设成"玩家真正瞄的角度"
    //      （cmd->viewangles 在 CreateMove 里已由自瞄写好）
    //    · angles 在 ViewSetup 里的位置用【自定位】找：在前 256 字节内
    //      扫连续 3 个 float，要求 pitch∈[-89.5,89.5]、yaw∈[-180.5,180.5]，
    //      且与 cmd 角度最接近（<30°）。找不到就什么都不改。
    //  纯内存读 + 改这一帧的临时结构，不写玩家实体 → 不可能再因它闪退。
    // ============================================================
    typedef void(__fastcall* OverrideView_t)(void*, void*, void*);
    OverrideView_t g_origOverrideView = nullptr;
    uintptr_t      g_ovTarget         = 0;

    // [批94] 这三个全局原本是"渲染阶段的瞄准角参照"，现已改为读引擎
    //   视角角（ViewEng().Read）—— 静默模式下 cmd 角不是画面该在的方向，
    //   拿它当参照会把"输入↔目标"的大角度算进 punch。它们随之成为死代码。
    //   保留：CreateMove 每次有效 tick 打时间戳 + 开火时刻。
    bool  g_lastCmdValid     = false;
    DWORD g_lastCmdTick      = 0;      // [批85] 角度新鲜度（菜单/暂停时不动视图）
    DWORD g_lastAttackTick   = 0;      // [批88] 最近一次开火时刻（后坐力窗口）

    // ============================================================
    //  [批113] 一次性探针：定位"引擎写 punch"的代码位置
    // ------------------------------------------------------------
    //  目的：为"挂最终视角计算函数"做准备。punch 字段偏移是我们运行时
    //  锁定出来的（唯一可信锚点），因此在 client.dll 代码段里扫描
    //  "位移量恰好等于该偏移的 movss 指令"——命中的就是 ViewPunch /
    //  SetPunchAngle 一类函数的字段访问点（写点=store，读点=load）。
    //  只读扫描：不 hook、不改代码，最坏情况是没有输出。
    // ============================================================
    void ProbePunchRefs(int punchOff)
    {
        HMODULE h = GetModuleHandleA("client.dll");
        if (!h)
        {
            PayloadLog("[probe] client.dll 未加载，无法定位 punch 写入点");
            return;
        }
        const unsigned char* base = reinterpret_cast<const unsigned char*>(h);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return;

        const size_t codeRva = nt->OptionalHeader.BaseOfCode;
        const size_t codeLen = nt->OptionalHeader.SizeOfCode;
        int hits = 0;
        for (size_t i = 0; i + 8 <= codeLen && hits < 12; ++i)
        {
            const unsigned char* q = base + codeRva + i;
            // movss xmm, m32 (F3 0F 10 /r) 或 movss m32, xmm (F3 0F 11 /r)
            if (q[0] != 0xF3 || q[1] != 0x0F ||
                (q[2] != 0x10 && q[2] != 0x11))
                continue;
            const unsigned char modrm = q[3];
            if ((modrm & 0xC0) != 0x80)          // 只认 mod=10（disp32）
                continue;
            const int rm = modrm & 7;
            if (rm == 4 || rm == 5)              // SIB / EIP 相对，不是 [reg+disp32]
                continue;
            int disp = 0;
            std::memcpy(&disp, q + 4, 4);
            if (disp != punchOff && disp != punchOff + 4 && disp != punchOff + 8)
                continue;
            ++hits;
            PayloadLog("[probe] client.dll+0x%X movss %s [reg+0x%X]"
                       "（punch 字段访问点 %d）",
                       (unsigned)(codeRva + i),
                       q[2] == 0x10 ? "load" : "store", disp, hits);
        }
        if (hits == 0)
            PayloadLog("[probe] client.dll 代码段无直接 movss [reg+0x%X] 引用"
                       "（punch 可能经 lea/指针间接访问）", punchOff);
    }

    void __fastcall Hooked_OverrideView(void* ecx, void* edx, void* view)
    {
        // [批113] 记录调用者（client.dll 内偏移）—— 为"挂最终视角函数"做调用链定位。
        //   只记一次；OverrideView 由 CViewRender 调用，返回值地址即渲染侧调用点。
        {
            static bool s_callerLogged = false;
            if (!s_callerLogged)
            {
                s_callerLogged = true;
                HMODULE hc = GetModuleHandleA("client.dll");
                const uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
                if (hc && ra > reinterpret_cast<uintptr_t>(hc))
                    PayloadLog("[probe] OverrideView 调用者 = client.dll+0x%X",
                               (unsigned)(ra - reinterpret_cast<uintptr_t>(hc)));
                else
                    PayloadLog("[probe] OverrideView 调用者 = 0x%X（不在 client.dll）",
                               (unsigned)ra);
            }
        }

        // ------------------------------------------------------------------
        //  [批85] 顺序：必须先让引擎把 ViewSetup 填完，再改 angles。
        //      ClientMode::OverrideView 的职责就是填充 ViewSetup，
        //      "先改→再调原函数"会被原函数覆盖，等于没改。
        // ------------------------------------------------------------------
        g_origOverrideView(ecx, edx, view);

        // ============================================================
        //  [批109·删除] 失焦 / 最小化门控 —— 从 OverrideView 移除
        // ------------------------------------------------------------
        //  批105 在这里加了 `if (GameWindowUnavailable()) return;`，
        //  当时的动机是"切窗口时引擎会传半成品 view"。但它有两个
        //  致命问题，本批实测确认：
        //
        //  ① **误伤率极高**：`GetForegroundWindow()` 的归属判据在
        //     窗口化/无边框模式下极不稳定（IME 窗口、任务栏、桌面、
        //     鼠标划过都会让 fgPid 变成别的进程）。实测本次会话
        //     `[bone-gate] 取骨被拦：窗口失焦` 触发了 36 次（3s 节流
        //     ≈ 至少 108 秒被拦），而用户全程在正常游戏。
        //     这个门控在 OverrideView 最前面 return，等于
        //     **去后坐力整段代码从不执行** → 用户报"移除画面抖动没用了"。
        //
        //  ② **它是冗余的**：真正保护崩溃的是下面批105 加的
        //     【45° 荒谬值熔断】。实测那次崩溃的现场是
        //     `est=220.07°` —— 早已被熔断拦下，根本轮不到这个门控。
        //
        //  保留的部分：
        //    · 45° 熔断（见下）—— 这是真正有效的防线，且不依赖
        //      任何外部状态，不会误伤。
        //    · 骨骼刷新里的失焦门控（BoneSysRefreshEntities）——
        //      那里直调引擎 SetupBones，风险等级完全不同，必须拦。
        // ============================================================

        // ============================================================
        //  [批116] FOV 采样已删除
        // ------------------------------------------------------------
        //  旧实现在这里用 FindFovOffset 自定位 view->fov 并写 g_viewFovDeg，
        //  供 FOV 圈算半径。实测该偏移会采到随视角/跳跃变化的字段
        //  （圈会一大一小、跳跃缩小）—— 已改为在 RenderFovRing 里直接用
        //  视图矩阵行范数求垂直 FOV（精确且稳定），不再依赖此采样。
        // ============================================================

        if (!g_Config.combat.noRecoilVis || !view) return;
        if (!g_lastCmdValid) return;

        // 角度必须是刚算出来的；否则在菜单/暂停/切图，视图与 cmd 无关
        if (GetTickCount() - g_lastCmdTick > 250) return;

        __try
        {
            unsigned char* base = reinterpret_cast<unsigned char*>(view);

            // ============================================================
            //  [批94] 参照角改为【引擎视角角】——静默模式从此兼容
            // ------------------------------------------------------------
            //  旧实现拿 cmd 角度当参照：
            //    · 平滑模式 OK（cmd ≈ 瞄准角）
            //    · 静默模式崩（cmd = 目标角，差值里混进"输入↔目标"的大角度）
            //  引擎视角角（ViewEng().Read）两种模式下都等于"画面应在哪"，
            //  是估算 punch 的唯一正确参照。
            // ============================================================
            float eng[3] = { 0.0f, 0.0f, 0.0f };
            if (!Aimbot::ReadViewAngles(eng)) return;

            // 只在偏移失效时重新定位（每帧重定位会导致候选跳动 → 画面抖）
            static int   s_off = -1;
            static int   s_offBad = 0;

            if (s_off < 0)
            {
                // ============================================================
                //  [批98] 优先信任已知偏移 72 —— 修"去后坐力失效"
                // ------------------------------------------------------------
                //  实测日志出现 `angles@+0xC4，误差 0.00°`：0xC4 处存着与
                //  引擎视角角【完全相等】的值。真正的 view->angles 应该等于
                //  「引擎视角角 + 视觉 punch」，误差 = punch（几度）；而误差
                //  恰好 0.00 说明 0xC4 是引擎视角角的一份副本（改它不影响
                //  渲染）→ 去后坐力看起来"生效"但画面纹丝不动。
                //
                //  自定位的判据是"与参照角最接近"，天生会挑中这种副本。
                //  改为：已知偏移 72 只要【范围合理】且【与 eng 的差在 punch
                //  量级内（≤15°）】就直接用它，不再让自定位抢走。
                //  玩家正对正前方（角度全零）也成立 —— 这里用 PlausibleAny
                //  而不是 Plausible，正是为了允许全零。
                // ============================================================
                const float* a72 = reinterpret_cast<const float*>(
                    base + ViewPunch::kKnownAnglesOffset);
                if (ViewPunch::PlausibleAny(a72))
                {
                    const float e72 = ViewPunch::AngleError(a72, eng);
                    if (e72 <= 15.0f)
                    {
                        s_off = ViewPunch::kKnownAnglesOffset;
                        s_offBad = 0;
                        PayloadLog("[move] 去后坐力生效（angles@+0x%X 已知位，"
                                   "与引擎角差 %.2f° = punch 量级）"
                                   "—— 只改本帧视口，不写实体内存",
                                   ViewPunch::kKnownAnglesOffset, e72);
                    }
                }

                if (s_off < 0)                        // 72 不可用 → 自定位兜底
                {
                    float err = 0.0f;
                    const int off = ViewPunch::FindAnglesOffset(base, eng,
                                                                30.0f, &err);
                    if (off < 0) return;              // 没定位到：什么都不改
                    s_off = off;
                    s_offBad = 0;
                    PayloadLog("[move] 去后坐力生效（angles@+0x%X 自定位，"
                               "误差 %.2f°）—— 只改本帧视口，不写实体内存",
                               off, err);
                }
            }

            float* a = reinterpret_cast<float*>(base + s_off);

            // 已锁定的偏移只需要"范围合理"（正对前方时 (0,0,0) 是合法值）
            if (!ViewPunch::PlausibleAny(a))
            {
                if (++s_offBad >= 3) s_off = -1;     // 结构变了：重新定位
                return;
            }
            s_offBad = 0;

            const float d0 = a[0] - eng[0];
            const float d1 = a[1] - eng[1];
            const float d2 = a[2] - eng[2];
            const float obs[3] = { d0, d1, d2 };

            constexpr float kMaxPunch = 15.0f;   // punch 量级上限
            constexpr float kDecay    = 0.50f;   // 非修正态：每帧衰减 50%
            constexpr DWORD kCombatMs = 500;     // 开火后 500ms 内算"有后坐力"
            constexpr float kSteadyDeg = 1.2f;   // 视角移动阈值（度/tick）

            // ============================================================
            //  [批105] 荒谬值熔断 —— 第二道防线（不依赖窗口检测）
            // ------------------------------------------------------------
            //  PlausibleAny 只保证"在合法角度范围内"，但 220° 这个量级
            //  的 yaw 差仍能通过它（|yaw| ≤ 180.5）。而 punch 真实上限
            //  是 15°。差值超过 45° 只有一个来源：view 结构是半成品。
            //  此时【既不修正、也不探测、也不累加滤波估计】——
            //  直接返回，且把已锁定的偏移作废（结构变了，该重新定位）。
            // ============================================================
            if (!std::isfinite(d0) || !std::isfinite(d1) || !std::isfinite(d2) ||
                std::fabs(d0) > 45.0f || std::fabs(d1) > 45.0f ||
                std::fabs(d2) > 45.0f)
            {
                s_off = -1;                        // 结构变了：下次重新定位
                return;
            }

            const bool combat = (g_lastAttackTick != 0) &&
                                (GetTickCount() - g_lastAttackTick < kCombatMs);

            // ============================================================
            //  [批100] 引擎角变化量必须【无条件】更新，且放在 punch 直读
            //  路径的 return 之前。
            //  批99 把它留在后面 → 一旦 punch 字段锁定并 return，
            //  这个 static 就永远停在上一次的值；等哪天退回估算路径，
            //  第一帧算出的 engMove 是"很久以前到现在"的总位移 →
            //  必然 >1.2° → 被判成"正在转视角" → 白白丢几帧修正。
            //  （真实教训：批94 的 FOV 采样也犯过同类"放在 return 之后"。）
            // ============================================================
            static float s_prevEng[3] = { 0.0f, 0.0f, 0.0f };
            float dyE = eng[1] - s_prevEng[1];
            while (dyE > 180.0f)  dyE -= 360.0f;
            while (dyE < -180.0f) dyE += 360.0f;
            const float engMove =
                (std::fabs(eng[0] - s_prevEng[0]) > std::fabs(dyE))
                    ? std::fabs(eng[0] - s_prevEng[0])
                    : std::fabs(dyE);
            s_prevEng[0] = eng[0];
            s_prevEng[1] = eng[1];
            s_prevEng[2] = eng[2];

            // ============================================================
            //  [批99] 优先走【引擎权威 punch 值】—— 彻底消除抖动
            // ------------------------------------------------------------
            //  估算路径（view->angles - 引擎角）天生带噪声：
            //    · 渲染帧与 tick 不同步，同一 tick 内多个渲染帧的 obs 不一致
            //    · combat/steady 门控在边界反复切换 → 修正量突变 → 抖
            //  引擎里本来就有权威值 CBasePlayer::m_Local.m_vecPunchAngle。
            //  netvar 解析不出来（0x1204 落在 m_flFOVRate），但可以用
            //  【已有估计值反向定位】：在 m_Local 区间扫三连 float，
            //  与 obs 在 1° 内相等的那个就是 punch。三次不同时刻
            //  落在同一偏移才锁定（单次命中可能是巧合）。
            //  锁定后每帧直接读它相减 —— 无门控、无滤波、无滞后。
            //  只读实体内存，绝不写（错地址写过一次闪退两次，教训）。
            // ============================================================
            static int   s_punchOff  = -1;   // 锁定的 punch 偏移（相对实体）
            static int   s_candOff   = -1;   // 当前候选偏移
            static int   s_candHits  = 0;    // 候选连续命中次数
            static DWORD s_probeAt   = 0;
            static bool  s_punchLog  = false;

            const uintptr_t lp = localPlayer ? localPlayer
                                             : BoneSysLocalPlayerPtr();

            if (s_punchOff < 0 && lp && combat &&
                GetTickCount() - s_probeAt > 200)
            {
                const float mag = std::fabs(d0) + std::fabs(d1) + std::fabs(d2);
                if (mag > 0.3f)      // [批104] 1.0° → 0.3°：
                    //                  单发/点射武器的 punch 常在 0.3~0.9°，
                    //                  1.0 的门槛让它永远探不到
                {
                    s_probeAt = GetTickCount();
                    int cands[PunchProbe::kMaxCandidates] = {};
                    int n = 0;
                    __try
                    {
                        n = PunchProbe::Scan(
                            reinterpret_cast<const unsigned char*>(lp),
                            obs, 0.0f, cands, PunchProbe::kMaxCandidates);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }

                    if (n == 1)                  // 唯一候选才计票
                    {
                        if (cands[0] == s_candOff)
                        {
                            if (++s_candHits >= 3)
                            {
                                s_punchOff = s_candOff;
                                if (!s_punchLog)
                                {
                                    s_punchLog = true;
                                    PayloadLog("[move] punch 字段已锁定 "
                                               "m_Local+0x%X（引擎权威值）"
                                               "—— 去后坐力改用直读，不再估算",
                                               s_punchOff);
                                    // [批113] 锁定的偏移同时是代码扫描锚点：
                                    //   输出引用该字段的 movss 指令位置，
                                    //   为 hook 最终视角函数提供依据。
                                    ProbePunchRefs(s_punchOff);
                                }
                            }
                        }
                        else
                        {
                            s_candOff  = cands[0];
                            s_candHits = 1;
                        }
                    }
                    else
                    {
                        // [批104] 探测失败必须留痕 —— 原来只有"锁定"会打日志，
                        // 失败时一个字都不留，导致"去后坐力没效果"完全查不出
                        // 是没探到（多候选/零候选）还是根本没探（mag/combat）。
                        static DWORD s_failLog = 0;
                        const DWORD nowF = GetTickCount();
                        if (nowF - s_failLog > 3000)
                        {
                            s_failLog = nowF;
                            PayloadLog("[move] punch 探测：候选=%d（est=%.2f°）"
                                       " —— 0=没扫到，多个=有歧义，均不锁定",
                                       n, mag);
                        }
                        s_candOff  = -1;         // 多个候选/零候选：不投票
                        s_candHits = 0;
                    }
                }
                else if (combat)        // 开了火但 punch 幅度没到门槛
                {
                    // [批104] 同理：门槛以下也要留痕（3s 节流）。
                    // 静默失败是这次"去后坐力没效果"查不出原因的另一半。
                    static DWORD s_smallLog = 0;
                    const DWORD nowS = GetTickCount();
                    if (nowS - s_smallLog > 3000)
                    {
                        s_smallLog = nowS;
                        PayloadLog("[move] punch 探测：est=%.2f° 未达门槛 0.3°"
                                   "（估算路径仍在用）", mag);
                    }
                }
            }
            else if (s_punchOff < 0 && g_Config.combat.noRecoilVis && lp)
            {
                // 三个前置条件（off<0 / lp / combat）任一不满足时，
                // 每 5s 记一次到底卡在哪一个。
                static DWORD s_gateLog = 0;
                const DWORD nowG = GetTickCount();
                if (nowG - s_gateLog > 5000)
                {
                    s_gateLog = nowG;
                    const char* why = !lp          ? "无本地玩家指针"
                                    : !combat      ? "开火窗口外（500ms 内未见开火）"
                                                   : "探测节流中";
                    PayloadLog("[move] punch 未探测：%s"
                               "（估算路径仍在用）", why);
                }
            }

            // ---- 权威值路径 ----
            // [批100] 锁定后【本帧不改 ViewSetup】，只把偏移/符号共享出去，
            //   由 CreateMove 用 SetViewAngles 应用。
            //   为什么换过去：SetViewAngles 是已验证的引擎槽位（读回校验
            //   通过），不依赖 ViewSetup 里 angles 的偏移猜测；而改
            //   ViewSetup 一旦偏移选错（0xC4 那次）就完全无效。
            //   两边只能二选一，否则 punch 会被抵消两次 → 反向抖动。
            if (s_punchOff >= 0 && lp)
            {
                float p[3] = { 0.0f, 0.0f, 0.0f };
                bool okRead = false;
                __try
                {
                    p[0] = *reinterpret_cast<const float*>(
                        reinterpret_cast<const unsigned char*>(lp) + s_punchOff);
                    p[1] = *reinterpret_cast<const float*>(
                        reinterpret_cast<const unsigned char*>(lp) + s_punchOff + 4);
                    p[2] = *reinterpret_cast<const float*>(
                        reinterpret_cast<const unsigned char*>(lp) + s_punchOff + 8);
                    okRead = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { okRead = false; }

                if (okRead && PunchProbe::StillValid(p))
                {
                    // --------------------------------------------------
                    //  [批99] 符号自检：真 punch 满足 |a - p - eng| < |a - eng|
                    //    即"减掉 p 之后更贴近引擎角"。若反过来（加更近），
                    //    说明这份客户端里 punch 的符号与惯例相反 —— 自动
                    //    改成相加。不做这一步，符号错会变成"加倍抖动"。
                    //    只判定一次（首次成功读取时），之后固定。
                    // --------------------------------------------------
                    if (g_punchSign == 0)
                    {
                        const float eM = std::fabs(a[0] - p[0] - eng[0]) +
                                         std::fabs(a[1] - p[1] - eng[1]);
                        const float eP = std::fabs(a[0] + p[0] - eng[0]) +
                                         std::fabs(a[1] + p[1] - eng[1]);
                        g_punchSign = (eM <= eP) ? 1 : -1;
                        PayloadLog("[move] punch 符号判定：%s（减=%.2f° 加=%.2f°）",
                                   g_punchSign > 0 ? "相减" : "相加", eM, eP);
                    }
                    // ============================================================
                    //  [批115] 渲染层去抖：用【锁定的权威 punch】直接减 ViewSetup
                    // ------------------------------------------------------------
                    //  实测日志转折点：估算路径改 ViewSetup 时用户能看到效果；
                    //  punch 锁定后改走 SetViewAngles，效果消失 —— 说明
                    //  "改最终渲染视角有效，改输入角对本地屏幕无效"。
                    //  这里把两者优点合并：权威 punch 值 + 渲染层减法，
                    //  无估算、无门控、无滤波。输入角保持不动（准星会回弹）。
                    // ============================================================
                    if (g_Config.combat.noRecoilVis)
                    {
                        if (ViewPunch::PlausibleAny(a) && ViewPunch::ValidDelta(p))
                        {
                            const float k = (g_punchSign >= 0) ? 1.0f : -1.0f;
                            a[0] -= k * p[0];
                            a[1] -= k * p[1];
                            a[2] -= k * p[2];

                            static DWORD s_renderLog = 0;
                            const DWORD nowR = GetTickCount();
                            if (nowR - s_renderLog > 2000)
                            {
                                s_renderLog = nowR;
                                PayloadLog("[move] 去后坐力=渲染层："
                                           "punch@0x%X=(%.2f,%.2f,%.2f) "
                                           "已减到 ViewSetup(angles@0x%X)",
                                           s_punchOff, p[0], p[1], p[2], s_off);
                            }
                        }
                    }

                    g_punchOffset = s_punchOff;   // 交给 CreateMove 应用
                    return;                       // 本帧不碰 ViewSetup
                }
                s_punchOff = -1;                  // 读不出来/范围异常：退回估算
                g_punchOffset = -1;
            }

            // ============================================================
            //  [批94] "稳准"判据也改用引擎视角角
            // ------------------------------------------------------------
            //  原先用 cmd 角的变化量：静默模式下 cmd 在目标间跳变，
            //  会被误判成"正在转视角" → 永远不修正。
            //  引擎视角角才反映真实的鼠标运动（静默时 = 输入角）。
            // ============================================================
            //  engMove 已在上面无条件算好（批100），这里只用它判"稳准"
            // ============================================================
            const bool steady = engMove < kSteadyDeg;

            // [批113] 估算路径状态（2s）：说明"为什么没修正"。
            //   combat=0 → 不在开火窗口（500ms）；steady=0 → 引擎角移动过快；
            //   两者都 1 却仍抖 → 估算值/偏移本身有问题。
            {
                static DWORD s_estLog = 0;
                const DWORD nowE = GetTickCount();
                if (nowE - s_estLog > 2000)
                {
                    s_estLog = nowE;
                    PayloadLog("[move] 去后坐力=估算路径：combat=%d steady=%d "
                               "engMove=%.2f obs=(%.2f,%.2f,%.2f) off=0x%X",
                               combat ? 1 : 0, steady ? 1 : 0, engMove,
                               d0, d1, d2, s_off);
                }
            }

            // ============================================================
            //  稳定态直接用观测值，不做任何追赶（见批92 的机制分析：
            //  punch 快速衰减时滤波滞后 → 减多了 → 上下跳）
            // ============================================================
            static float s_est[3] = { 0.0f, 0.0f, 0.0f };

            if (combat && steady)
            {
                if (ViewPunch::ValidDelta(obs) &&
                    std::fabs(d0) < kMaxPunch &&
                    std::fabs(d1) < kMaxPunch &&
                    std::fabs(d2) < kMaxPunch)
                {
                    s_est[0] = d0; s_est[1] = d1; s_est[2] = d2;
                }
            }
            else
            {
                s_est[0] -= s_est[0] * kDecay;
                s_est[1] -= s_est[1] * kDecay;
                s_est[2] -= s_est[2] * kDecay;
            }

            // 修正量小到看不出来时就完全不写
            if (std::fabs(s_est[0]) > 0.02f || std::fabs(s_est[1]) > 0.02f ||
                std::fabs(s_est[2]) > 0.02f)
            {
                a[0] -= s_est[0];
                a[1] -= s_est[1];
                a[2] -= s_est[2];
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            /* 结构不对：本帧跳过，绝不影响游戏 */
        }
    }

    // ============================================================
    //  Hooked_CreateMove —— 契约：调原函数 + 一行功能透传
    // ============================================================
    bool __fastcall Hooked_CreateMove(void* ecx, void* edx,
                                      float inputSample, void* pCmd)
    {
        // 先让游戏把 cmd 填充完（原函数在前，改写在后才能生效）
        const bool original = g_origCreateMove(ecx, edx, inputSample, pCmd);

        // [诊断] 一次性：证明 detour 真的被引擎调用
        static bool s_tickLogged = false;
        if (!s_tickLogged)
        {
            s_tickLogged = true;
            PayloadLog("[aim] tick 链路 OK（Hooked_CreateMove 到达）");
        }

        // ============================================================
        //  [批62] CreateMove 链耗时探针
        // ------------------------------------------------------------
        //  卡顿排查不能靠猜"是不是日志/是不是 netvar"。这里直接量本链
        //  在游戏线程上的耗时（Aimbot + Movement 两段），每 5 秒最多报
        //  一次，且【只在超过阈值时】才写日志 —— 正常帧零 I/O 开销。
        //  判读：
        //    · 峰值 < 0.5ms  → 本链不是卡顿源，去查渲染/骨骼那条线
        //    · 峰值 1~5ms    → 每帧都在超预算，会明显掉帧
        //    · 偶发 10ms+    → 换目标/骨骼重取触发的尖峰
        //  用 QPC 而不是 GetTickCount：后者 15.6ms 粒度，量不出这个量级。
        // ============================================================
        LARGE_INTEGER qpc0{};
        QueryPerformanceCounter(&qpc0);

        // ============================================================
        //  [批105·切窗口闪退修复] 失焦 / 最小化 → 本 tick 全部跳过
        // ------------------------------------------------------------
        //  切窗口那一瞬引擎正在重建渲染与动画状态（可能伴随 DirectX
        //  设备重置）。此时写 cmd 视角 / 取骨 / 改视口全是危险操作 ——
        //  实测就是"卡死后自动关闭"。恢复前台后 CreateMove 照常，
        //  玩家感知只是切回来的一两帧没有辅助。
        //
        //  注意：cmd 本身照常透传（不 return 整个函数），只跳过我们
        //  的功能层 —— 引擎的 CreateMove 已经在前面跑完了。
        //
        //  实现放在 try 内部而不是用 if 包 try：C++ 不允许 try 块被
        //  条件语句包住（C++ 语法硬限制），所以门控写进 try 的第一行。
        // ============================================================
        try
        {
            Aimbot::CreateMove(reinterpret_cast<sdk::CUserCmd*>(pCmd));
        }
        catch (...)
        {
            static DWORD s_eLog = 0;
            const DWORD t = GetTickCount();
            if (t - s_eLog > 5000)
            {
                s_eLog = t;
                PayloadLog("[hooks] CreateMove 异常已拦截（5s 节流）");
            }
        }

        // [批46] 自动连跳 / 自动连发（独立模块 Movement）
        //  · 与 Aimbot 平级、互不依赖；各自独立 try/catch
        //  · 顺序【必须在 Aimbot 之后】：Aimbot 末尾会按静默架构
        //    覆写 cmd 视角与移动修正，两边都只动 buttons/viewangles，
        //    互不重叠，先谁后谁都不影响正确性；放后面是为了让瞄准
        //    的最终状态不被这里再改一遍。
        try
        {
            Movement::CreateMove(reinterpret_cast<sdk::CUserCmd*>(pCmd));
        }
        catch (...)
        {
            static DWORD s_mLog = 0;
            const DWORD t = GetTickCount();
            if (t - s_mLog > 5000)
            {
                s_mLog = t;
                PayloadLog("[hooks] Movement 异常已拦截（5s 节流）");
            }
        }

        // ============================================================
        //  [批116] 去后坐力只走渲染层（OverrideView）
        // ------------------------------------------------------------
        //  原 CreateMove 末尾的 Aimbot::ApplyNoRecoilVis()（SetViewAngles
        //  输入层补偿）已删除。实测日志转折点证明：改 ViewSetup 有效，
        //  改 SetViewAngles 对本地屏幕无效 —— 留在这里只会误导。
        // ============================================================

        // [批94] 记录本 tick 有效性 + 开火时刻。
        //   后坐力只在这之后的一小段时间存在；ViewSetup 的修正必须限定在
        //   这个窗口内，否则会去修"本来就没有的 punch"。
        //   （原先这里还缓存 cmd 角度供 OverrideView 当参照，已随批94 删除 ——
        //     参照角改为引擎视角角，cmd 角在静默模式下方向不对。）
        {
            const sdk::CUserCmd* c = reinterpret_cast<const sdk::CUserCmd*>(pCmd);
            if (c && c->command_number)
            {
                g_lastCmdTick  = GetTickCount();
                g_lastCmdValid = true;

                // [批88] 记录开火时刻。
                if ((c->buttons & sdk::Button::IN_ATTACK) || g_autoFirePressing)
                    g_lastAttackTick = g_lastCmdTick;
            }
        }

        // [批36] 静默架构：本 tick 写过 cmd 角度 → 返回 false，
        //  引擎不把 cmd 角应用到视角（视角 = 我们同步的输入角/平滑角）
        // ---- [批62] 结束计时并做 5s 汇总 ----
        {
            LARGE_INTEGER qpc1{}, freq{};
            QueryPerformanceCounter(&qpc1);
            QueryPerformanceFrequency(&freq);
            const double ms = (double)(qpc1.QuadPart - qpc0.QuadPart) * 1000.0
                            / (double)freq.QuadPart;

            static double  s_maxMs   = 0.0;
            static double  s_sumMs   = 0.0;
            static int     s_n       = 0;
            static int     s_over    = 0;      // 超 1ms 的帧数
            static DWORD   s_logAt   = 0;

            ++s_n;
            s_sumMs += ms;
            if (ms > s_maxMs) s_maxMs = ms;
            if (ms > 1.0) ++s_over;

            const DWORD now = GetTickCount();
            if (now - s_logAt >= 5000)
            {
                const double avg = s_n ? s_sumMs / s_n : 0.0;
                // 只在真的慢时才落盘：正常时一行都不写，避免探针本身造成卡顿
                if (s_maxMs > 0.5)
                {
                    PayloadLog("[perf] CreateMove 链 平均=%.3fms 峰值=%.3fms "
                               "超1ms=%d/%d 帧", avg, s_maxMs, s_over, s_n);
                }
                s_logAt = now;
                s_maxMs = 0.0; s_sumMs = 0.0; s_n = 0; s_over = 0;
            }
        }

        return Aimbot::WroteCmdAngles() ? false : original;
    }
} // namespace

// ============================================================
//  对外接口（Hooks.h）
// ============================================================
bool Hooks::Init()
{
    static bool s_done = false;
    if (s_done)
        return g_hookInstalled;
    s_done = true;

    if (!ResolveClientModule())
    {
        PayloadLog("[hooks] ERR: client.dll 模块/PE 解析失败");
        return false;
    }

    // ---- ClientMode 全局指针（签名法，本机验证过）----
    const uintptr_t sig = FindPattern("89 04 B5 ? ? ? ? E8");
    if (!sig)
    {
        PayloadLog("[hooks] ERR: ClientMode 签名未找到");
        return false;
    }

    void* clientMode = nullptr;
    __try { clientMode = **(reinterpret_cast<void***>(sig + 3)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { clientMode = nullptr; }
    if (!IsUserPtr(reinterpret_cast<uintptr_t>(clientMode)))
    {
        PayloadLog("[hooks] ERR: ClientMode 指针无效 0x%X", (unsigned)(uintptr_t)clientMode);
        return false;
    }

    void** vtable = nullptr;
    __try { vtable = *reinterpret_cast<void***>(clientMode); }
    __except (EXCEPTION_EXECUTE_HANDLER) { vtable = nullptr; }
    if (!vtable)
    {
        PayloadLog("[hooks] ERR: ClientMode vtable 无效");
        return false;
    }

    void* pCreateMove = vtable[27];                 // 槽 27（L4D2 双源验证）
    if (!pCreateMove || !InClientRange(reinterpret_cast<uintptr_t>(pCreateMove)))
    {
        PayloadLog("[hooks] ERR: vtable[27] 不在 client.dll 内（0x%X）",
                   (unsigned)(uintptr_t)pCreateMove);
        return false;
    }

    // ---- MinHook 挂接（OS-ImGui 已初始化过 MH 则跳过 Initialize）----
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        PayloadLog("[hooks] ERR: MH_Initialize 失败（%d）", (int)st);
        return false;
    }

    st = MH_CreateHook(pCreateMove,
                       reinterpret_cast<LPVOID>(&Hooked_CreateMove),
                       reinterpret_cast<LPVOID*>(&g_origCreateMove));
    if (st != MH_OK)
    {
        PayloadLog("[hooks] ERR: MH_CreateHook 失败（%d）", (int)st);
        return false;
    }

    st = MH_EnableHook(pCreateMove);
    if (st != MH_OK)
    {
        PayloadLog("[hooks] ERR: MH_EnableHook 失败（%d）", (int)st);
        MH_RemoveHook(pCreateMove);
        return false;
    }

    g_cmTarget      = reinterpret_cast<uintptr_t>(pCreateMove);
    g_hookInstalled = true;
    PayloadLog("[hooks] CreateMove 已挂：clientMode=0x%X target=0x%X",
               (unsigned)(uintptr_t)clientMode, (unsigned)g_cmTarget);

    // ============================================================
    //  [批84] OverrideView（槽 19）—— 移除画面后坐力的正确挂点
    // ------------------------------------------------------------
    //  槽位出处：Panguins/L4D2_Internal 的 override_view.cpp 里
    //      get_original<...>( 19 )        // ClientMode vtable
    //  与 CreateMove 槽 27 同源（本项目已实测 27 有效）。
    //  失败不影响主功能，只打一条 WARN。
    // ============================================================
    {
        void* pOv = nullptr;
        __try { pOv = vtable[19]; }
        __except (EXCEPTION_EXECUTE_HANDLER) { pOv = nullptr; }

        if (pOv && InClientRange(reinterpret_cast<uintptr_t>(pOv)))
        {
            MH_STATUS s2 = MH_CreateHook(pOv,
                                         reinterpret_cast<LPVOID>(&Hooked_OverrideView),
                                         reinterpret_cast<LPVOID*>(&g_origOverrideView));
            if (s2 == MH_OK && MH_EnableHook(pOv) == MH_OK)
            {
                g_ovTarget = reinterpret_cast<uintptr_t>(pOv);
                PayloadLog("[hooks] OverrideView 已挂（槽 19，target=0x%X）",
                           (unsigned)g_ovTarget);
            }
            else
            {
                PayloadLog("[hooks] WARN: OverrideView 挂钩失败（%d），"
                           "移除后坐力不可用", (int)s2);
            }
        }
        else
        {
            PayloadLog("[hooks] WARN: vtable[19] 不在 client.dll 内，"
                       "跳过 OverrideView（移除后坐力不可用）");
        }
    }

    return true;
}

void Hooks::Shutdown()
{
    if (!g_hookInstalled)
        return;
    MH_DisableHook(reinterpret_cast<LPVOID>(g_cmTarget));
    MH_RemoveHook(reinterpret_cast<LPVOID>(g_cmTarget));
    MH_Uninitialize();                                // [批44] 顺手收尾
    g_hookInstalled = false;
    g_origCreateMove = nullptr;
    PayloadLog("[hooks] CreateMove 已卸下");
}

Hooks::Status Hooks::GetStatus()
{
    Status s;
    s.createMove  = g_hookInstalled;
    s.target      = g_cmTarget;
    // trampoline 语义上就是原函数指针
    s.trampoline  = reinterpret_cast<uintptr_t>(g_origCreateMove);
    return s;
}

bool Hooks::Ready()
{
    return g_hookInstalled;
}

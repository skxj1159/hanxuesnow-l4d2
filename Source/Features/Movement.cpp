// ============================================================
//  Features/Movement.cpp —— 自动连跳 / 自动连发
// ------------------------------------------------------------
//  【模块契约】
//   · 只写 CUserCmd::buttons 的 IN_JUMP / IN_ATTACK 两位
//   · 不碰 viewangles / forwardmove / sidemove → 与自瞄的平滑、
//     静默、移动修正完全正交
//   · 不读不写任何检测/规避状态 → 与既有逻辑零耦合
//   · 无动态分配、无引擎 vcall、无文件 I/O（除节流日志）
//
//  【阈值全部来自反检测判据，不是拍脑袋】
//  依据《连跳与连发检测完全解读》：
//
//   连跳（Lilac Bhop）：数"落地即起跳"的**上升沿**。
//     每次命中后开 0.3 秒窗口，窗口内再跳 → 整轮作废(bhop_reset)；
//     超窗才计 perfect_bhops。满 MIN(7/10) 记一次 detection，
//     满 MAX(20) 立即 Ban。
//     ⚠ JUMP 缓冲（按住时长 > JUMP+MIN 就放过）是唯一豁免，
//       但默认 Medium 档 JUMP=-1（关闭）。所以不能依赖它。
//     → 对策：① 落地后加随机延迟（人不可能 0 tick 反应）
//            ② 跳跃键【按住】若干 tick（既像滚轮，也喂 JUMP 缓冲）
//            ③ 按概率跳过个别跳（真人不会每一跳都接上）
//
//   连发（Lilac Macro）：一秒滑动窗口内 IN_ATTACK **上升沿** >= 20 次。
//     → 对策：上升沿间隔 >= 52ms（1000/20 = 50ms 为红线，留 2ms 余量）
//     SMAC AutoFire 更宽松（连发+松开都算、阈值 tickrate/3、且
//     连续 2 tick 无边沿即清零），按下沿间隔 >=52ms 同样安全。
// ============================================================

#include "Movement.h"
#include "../Config/Config.h"
#include "../Globals.h"          // [批62] localPlayerIndex（药品保护读弹匣）
#include "../PayloadControl.h"
#include "../SDK/Entity.h"
#include "../SDK/NetVars.h"
#include "../SDK/Offsets.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Movement
{
namespace
{
    // ---- 引擎 FL_ONGROUND（Source 标准位）----
    constexpr int FL_ONGROUND = 1;

    // ============================================================
    //  参数硬边界（不可被配置突破 —— 这些是检测红线，不是偏好）
    //  下限/上限统一取 ConfigCombat 的公开常量，【不在这里另抄一份】：
    //  两处各写一个数字，迟早会只改一处，然后菜单与实际行为对不上。
    // ============================================================
    constexpr int kFireDelayMinMs = ConfigCombat::kAutoFireMsMin;   // 52
    constexpr int kFireDelayMaxMs = ConfigCombat::kAutoFireMsMax;   // 400
    constexpr int kFireDefaultMs  = 60;   // 16.7 次/秒，稳在 20 以下

    // [批55] IN_ATTACK 每次触发后连续保持的 tick 数。
    //   2 是下限而不是可调参数：Lilac 的 Autoshoot 判据要数开枪点
    //   前一 tick / 开枪 tick / 后一 tick 三格里有几格按着开火键，
    //   只有 1 格才会被判。保持 2 tick 后最少命中 2 格，判据失效。
    //   设成 1 就等于把判据原样喂给它，不能降。
    // [批55 → 批60] 这两个常量保留作历史边界说明：
    //   ① 三格窗口判据要求射击瞬间前后必须有按下 tick —— 新方案用
    //      "松开 1 tick → 立即按住"天然满足（窗口 = 松开/按下/按下，tmp=2）。
    //   ② =1 就等于把判据原样喂给它，任何方案都不能让按下只存在 1 tick。
    //   注：批60 起不再用"固定保持 2 tick"，改为"整段按住、每周期松 1 tick"，
    //       见 TickAutoFire 顶部注释（旧的 2 tick 窗口 30ms 短于步枪射速
    //       周期 100ms，导致武器开火检查时正处于松开状态 = 连发不生效）。
    constexpr int kFireHoldTicks  = 2;   // 仅供判据说明，运行时不直接使用

    // ============================================================
    //  [批103] 连跳时序常量 —— 全部删空
    // ------------------------------------------------------------
    //  按住式连跳不需要任何时序常量：玩家按住空格，我们保持该位，
    //  引擎自己在落地帧起跳。没有 delay、没有 hold、没有链长、
    //  没有"主动释放"，因此也没有可被人为调慢的环节（用户实测
    //  delay=1 仍"卡脚、跳不流畅"，就是这个等待造成的）。
    //
    //  安全性改由"零上升沿"保证（见 TickBhop 上方注释）：
    //  Lilac 的 perfect 只在上升沿 + FL_ONGROUND 时递增，而按住时
    //  电平恒 1 → 永不递增 → 三档预设全部免疫。
    // ============================================================

    struct State
    {
        // ---- 连跳（[批103] 按住式：只留统计用字段，时序字段全删）----
        bool  wasOnGround   = false;   // 上一 tick 是否在地面
        int   landTick      = -1;      // 落地后的 tick 计数（-1 = 空中/未在地面）
        bool  seeded        = false;   // 随机种子是否播种

        // ---- 连发 ----
        DWORD lastFireMs     = 0;      // 上一次触发 IN_ATTACK 的墙钟时刻
        bool  firePrimed     = false;  // 是否已完成一次"按下-松开"
        int   delayMs        = kFireDefaultMs;
        bool  releaseTick    = false;  // 本 tick 是否要松开（制造下降沿）
        const char* block    = nullptr;// 最近一次被哪道门控拦下
                                       //   （nullptr = 正常出脉冲）

        // 只保留日志在用的两个计数。曾经的 breakCount 只写不读、
        // groundTicks 从未赋值也从未读取 —— 死字段已删。
        int   jumpCount   = 0;    // 本会话累计连跳次数（仅日志用）
        int   fireCount   = 0;    // 本会话累计连发次数（仅日志用）
    };

    State g;

    // [批111·死代码] 删除了 `RandRange(lo, hi)` —— 全项目零调用
    //（唯一定义在 108 行）。它是批101"学习连跳"被删后遗留的随机化
    //  助手；连跳改为按住式后不再需要任何延迟随机。
    //  若将来重做连跳并要加随机间隔，重新引入即可（rand() 无状态、
    //  无副作用，不存在"必须现在留着"的依赖）。

    // [批103] PickJumpDelay 及其时序常量已删除 —— 按住式连跳没有延迟。

    // ============================================================
    //  本地玩家地址（缓存，零成本）
    // ------------------------------------------------------------
    //  [批48·卡死修复] 原实现每 tick 调 EntityList::LocalPlayer()，
    //  它会【构造 CBaseEntity + 跑四重 IsValid 防线】——每条防线都是
    //  SEH 包裹的内存读（m_addr / vptr / netable 各一次），而本函数
    //  跑在 CreateMove 的游戏线程关键路径上，每 tick 都做。
    //  这是典型的"用重量级接口做轻量级查询"：一旦某次读落在刚被
    //  释放的页上，SEH 展开本身在 CreateMove 里是危险的（引擎正在
    //  此刻推进 tick）。实测表现为偶发"未响应"。
    //
    //  改为：dwLocalPlayer 指针本身【每 tick 读一次】并缓存地址，
    //  之后只做 1 次 m_fFlags 读。指针有效性由 0 值天然兜底。
    // ============================================================
    uintptr_t LocalPlayerAddr()
    {
        const HMODULE client = ::GetModuleHandleA("client.dll");
        if (!client)
            return 0;
        // 0 = 主菜单 / 加载中 —— 指针为空天然兜底，无需额外校验
        const uintptr_t addr = (uintptr_t)client + off::dwLocalPlayer;
        return Mem::Read<uintptr_t>(addr, (uintptr_t)0);
    }

    bool OnGround()
    {
        const uintptr_t lp = LocalPlayerAddr();
        if (!lp)
            return false;                             // 未进对局 → 不跳
        // m_fFlags：NetVars 未解析此字段（RecvTable 里属 DT_BasePlayer），
        // 直接用 SDK 硬编码偏移（0xF0，见 SDK/Offsets.h）。读不到时
        // 按 "不在地面" 处理 —— 保守方向是【不跳】。
        // 注意 Mem / off 是【全局】命名空间（不在 sdk 内）。
        const int flags = Mem::Read<int>(lp + off::m_fFlags, 0);
        return (flags & FL_ONGROUND) != 0;
    }


    // ============================================================
    //  自动连跳
    // ------------------------------------------------------------
    //  [批47] 触发 = 【按住空格】，不是无条件自动。
    //  无条件自动意味着键盘上根本没这个输入却凭空出现完美落地起跳
    //  序列 —— 那是最不像人的模式；按住才生效则与"玩家自己连按空格"
    //  在服务端看来一致。
    //
    //  [批48·条件①对策] Lilac 判定完美跳的第一条件是上升沿。真人按住
    //  空格 = 电平恒 1 = 全程只有 1 个上升沿；而自动连跳需要每次落地
    //  都产生新上升沿，两者天然冲突。解法即用户所说：空中【主动释放】
    //  IN_JUMP，落地那一刻才【重新置位】——服务端看到的是
    // ============================================================
    //  [批103·按住式连跳] —— 全部机制重写，删掉 delay/上升沿/断链
    // ------------------------------------------------------------
    //  为什么换掉"空中释放 + 落地重按"：
    //    用户实测那套"卡脚、跳不流畅"。根因是结构性：它必须等
    //    delay（至少 1 tick = 33ms）才能起跳，而真人按住空格时
    //    引擎是在【落地那一帧自己起跳】，零延迟。我们为了造上升沿
    //    反而把这条最顺的路径堵死了。
    //
    //  为什么按住式反而最安全（Lilac 逐行核对过）：
    //    lilac_bhop_check 的 perfect 计数【只在上行沿 + FL_ONGROUND
    //    时递增】（源码 49 行 `buttons & IN_JUMP && !(last & IN_JUMP)`）。
    //    一直按住 = 电平恒 1 = 全程零上升沿 → perfect_bhops 永远
    //    停在 reset 后的 -1 → check_bhop_min/max 的阈值永远够不着。
    //    另外 jump_ticks 会随按住时长持续累积，Lilac 的 JUMP 缓冲
    //    豁免（`jump_ticks > JUMP + MIN`）也会自然满足。
    //    → 三档预设（LOW/MEDIUM/HIGH）全部免疫，且延迟为 0。
    //
    //  之前批48 认为"必须造上升沿"是错的：那只在需要【精确控制
    //  起跳时刻】时成立，而我们并不需要 —— 让引擎自己起跳更准。
    // ============================================================
    void TickBhop(sdk::CUserCmd* cmd, bool grounded)
    {
        (void)grounded;                 // 按住式不关心落地沿
        const bool userHolding = (cmd->buttons & sdk::Button::IN_JUMP) != 0;

        // ---- 玩家没按住空格 → 本模块完全不介入 ----
        if (!userHolding)
        {
            g.landTick    = -1;
            g.wasOnGround = grounded;
            return;
        }

        // ---- 按住即保持：引擎自己判断何时起跳（FL_ONGROUND + IN_JUMP）----
        // 这里唯一要做的，是保证这一位真的送出去（某些分支里
        // cmd->buttons 会被其它模块改写），以及清掉已废止的旧状态。
        cmd->buttons |= sdk::Button::IN_JUMP;

        // 统计仍按"落地→离地"计数，只用于日志（不再驱动任何按键逻辑）
        if (grounded)
        {
            if (!g.wasOnGround)
            {
                g.landTick = 0;
                ++g.jumpCount;
            }
            else
            {
                ++g.landTick;
            }
        }
        else
        {
            g.landTick = -1;
        }
        g.wasOnGround = grounded;
    }

    // ============================================================
    //  自动连发
    // ------------------------------------------------------------
    //  [批51·设计回退] 退回最初的"按住即连发"，只单独处理药品。
    //
    //  为什么放弃"白名单只对单发枪械生效"：
    //    这条路要求可靠识别武器类型，而 RTTI 类名在【武器实体】上
    //    三轮都没读通（批49 CBaseEntity 两道闸 → 批50 堆上实体
    //    放宽后又发现 COL+0x0C 在本机存的是【绝对地址】而非 RVA，
    //    见 [move-RTTI] 埋点：tdRva=0x6A9A1C3C 远大于任何模块基址）。
    //    在一个读不出来的字段上做白名单 = 全部武器都失配。
    //    用户实测"连狙、手枪全都没效果"，说明这条路走不通。
    //
    //  新判据只回答一个问题：【现在是不是在用药品？】
    //  靠的是 L4D2 的硬事实：吃药必须【按住 IN_USE(E)】打开药品栏，
    //  此时左键是"选药位"而非"开枪"。所以只要 IN_USE 被按住，
    //  本模块就完全不介入 —— 这条判据不依赖任何武器类型识别。
    //
    //  【为什么这样不会误伤开枪】
    //    · 开枪时用户不会同时按住 E（E 是蹲/起身/交互键）
    //    · 即便同时按住，代价只是"这一刻不连发"，不产生故障
    //    · 反向：吃药时绝不会误接管 → 药品安全，这是硬需求
    //
    //  【为什么"接管 ATTACK 位"这个核心不能动】
    //    检测只数【上升沿】。用户按住不放 = 电平恒 1 = 全程只有 1 个
    //    上升沿 = 半自动武器只出 1 枪。同 tick 内先 set 再 clear 是
    //    空操作，永远产生不了新边沿。必须"先清后置"造出真实的
    //    下降沿 + 上升沿。
    // ============================================================
    // ============================================================
    //  [批60] 连发脉冲：改成"长时间按住 + 每周期松开 1 tick"
    // ------------------------------------------------------------
    //  旧实现（批55）：按住 2 tick → 松开直到间隔到 → 再按 2 tick。
    //  占空比只有约 50%，这是"连发没效果"的真正原因：
    //    · 全自动武器要求开火瞬间 IN_ATTACK 处于按下状态；
    //      而它每 ~100ms 检查一次（步枪射速），我们的按下窗口只有
    //      2 tick ≈ 30ms —— 检查时刻大概率落在【松开】区间，
    //      于是这一发被引擎跳过。看起来就是"点一下只响一发"。
    //
    //  新实现：默认一直按住，只在每个连发周期松开【恰好 1 tick】。
    //    · 全自动：占空比 ~85%，引擎几乎总能看到按下 → 按武器射速连射
    //    · 半自动：那 1 tick 的松开创出下降沿，下一 tick 按下即上升沿
    //      → 每周期稳定击发一次
    //    · Lilac AUTOSHOOT：射击发生在"松开 tick 的下一 tick"，
    //      三格窗口 = [松开, 按下, 按下]，tmp = 2 ≠ 1 → 判据不成立
    //    · Lilac Macro：上升沿间隔 = delayMs + 1 tick ≈ 68ms
    //      → 14.7 次/秒 < 20 次/秒红线
    //
    //  ⚠ 为什么不能"一直按住不松开"：半自动武器只在上升沿击发，
    //    一直按住 = 只响第一发。必须保留那 1 tick 的松开。
    // ============================================================
    // ============================================================
    //  [批73] 自动连点 = 宏
    // ------------------------------------------------------------
    //  需求：绑定一个键，按住它就等于"替玩家按左键"。
    //  与旧实现的区别：
    //    · 旧：必须先按住左键，模块在 LMB 基础上加脉冲
    //    · 新：只要按住【绑定键】，不看左键状态，模块自己发
    //      IN_ATTACK 的按下/松开序列 —— 真正的宏
    //
    //  脉冲形状：默认一直按住，每 delayMs 松开恰好 1 tick。
    //    · 半自动：那 1 tick 的松开创出下降沿，下一 tick 按下即上升沿
    //      → 每周期击发一次
    //    · Lilac AUTOSHOOT 三格窗口 = [松开, 按下, 按下] → tmp=2，不成立
    //    · Lilac Macro 上升沿间隔 = delayMs + 1 tick ≈ 68ms
    //      → 14.7 次/秒 < 20 次/秒红线
    //
    //  保护不变：换弹 / 按 E 用物品 / 无弹匣（药丸等）都不接管。
    // ============================================================
    void TickAutoFire(sdk::CUserCmd* cmd)
    {
        const int key = g_Config.combat.autoFireKey;

        // 触发源：绑了键就按键状态；key==0 时退回"按住左键"（兼容旧行为）
        const bool active = (key != 0)
            ? ((GetAsyncKeyState(key) & 0x8000) != 0)
            : ((cmd->buttons & sdk::Button::IN_ATTACK) != 0);

        // [批74] 广播给自瞄：宏正在替玩家按左键。
        //   自瞄的触发键若是左键，应当认为"左键被按下了"，
        //   否则会出现"宏在连射、自瞄不接管"的割裂。
        g_autoFirePressing = active && g_Config.combat.autoFire;

        if (!active)
        {
            g.block       = "未按住";
            g.firePrimed  = false;
            g.releaseTick = false;
            return;
        }
        if (cmd->buttons & sdk::Button::IN_RELOAD)
        {
            g.block       = "换弹";
            g.releaseTick = false;
            return;
        }

        // 药品保护：按住 E = 正在使用物品/打开药品栏
        if (cmd->buttons & sdk::Button::IN_USE)
        {
            g.block       = "用物品";
            g.firePrimed  = false;
            g.releaseTick = false;
            return;
        }

        // 无弹匣物品（药丸/医疗包/除颤器/近战/投掷）不接管。
        //  只有【曾读到正数弹匣】才相信这个偏移：偏移读偏时门控自动失效，
        //  宁可少管，也不让连点静默消失（这是之前反复踩过的坑）。
        {
            static bool s_clipProven = false;
            const sdk::CBaseEntity me = sdk::EntityList::Get(localPlayerIndex);
            const int clip = me.IsValid() ? me.GetActiveWeaponClip()
                                          : sdk::CBaseEntity::kClipUnknown;
            if (clip > 0) s_clipProven = true;
            if (s_clipProven && clip == sdk::CBaseEntity::kClipNoClip)
            {
                g.block       = "无弹匣";
                g.firePrimed  = false;
                g.releaseTick = false;
                return;
            }
        }

        const DWORD now = GetTickCount();

        // 松开 tick：制造下降沿（玩家自己按着左键也一并清掉，否则没有边沿）
        if (g.releaseTick)
        {
            g.releaseTick = false;
            cmd->buttons &= ~sdk::Button::IN_ATTACK;
            g.lastFireMs  = now;
            g.block       = nullptr;
            ++g.fireCount;
            return;
        }

        // 其余 tick：按住
        cmd->buttons |= sdk::Button::IN_ATTACK;
        g.block = nullptr;

        if (!g.firePrimed)
        {
            g.firePrimed = true;
            g.lastFireMs = now;
            return;
        }
        if (now - g.lastFireMs >= (DWORD)g.delayMs)
            g.releaseTick = true;
    }


} // namespace

// ============================================================
//  [批71] 移除画面后坐力
// ------------------------------------------------------------
//  Source 的视角抖动来自玩家实体的 punch 角（m_vecPunchAngle /
//  m_aimPunchAngle，NetVars 已按三个候选名解析）。
//  客户端每帧把它叠加到相机上 → 开枪时屏幕往上跳。把它每 tick 写 0，
//  屏幕就稳住了。
//
//  三个必须守住的边界：
//    ① 只影响视觉：弹道/伤害由服务器算，我们没碰 cmd->viewangles，
//       所以不会改变命中逻辑（也不构成"无后坐力外挂"的弹道特征）。
//    ② 写之前必须验数：偏移若解析错，12 字节会盖到别的字段上。
//       先读回来判断是不是"合理的小角度"，不合理就一个字都不写。
//    ③ 偏移没解析到（=0）直接不做事，绝不猜地址。
// ============================================================
namespace
{
    // ============================================================
    //  [批73] 移除画面后坐力（修"开关开了但屏幕照样抖"）
    // ------------------------------------------------------------
    //  上一版只写了玩家实体上的 m_vecPunchAngle —— 那是网络同步副本。
    //  L4D2 真正叠加到视口的是 m_Local.m_vecPunchAngle（本地独占表），
    //  所以写错一份的表现就是：开关有效、日志正常、屏幕照抖。
    //
    //  现在两份都写：
    //    · m_Local 那份 = 相机抖动来源（主目标）
    //    · 实体那份    = 给依赖它的其它逻辑保持一致（顺带）
    //  每份独立验数，任何一份读到离谱值就单独停用它，不影响另一份。
    // ============================================================
    // ============================================================
    //  [批83] punch 差分取证：用"开火变非零、停火后回零"认定
    // ------------------------------------------------------------
    //  教训：给未验证的地址写 0 会直接闪退（0x1234 实测就是这么崩的）。
    //  所以写入前必须先【认定】目标，认定依据是 punch 的物理特征：
    //    ① 静止时是 0
    //    ② 开火期间变成非零（几度以内）
    //    ③ 停火后 2 秒内自己衰减回 0
    //  三个条件同时满足才认定，然后才允许写。认定不了就完全不写。
    //
    //  扫描范围沿用 0x1100~0x1C00（m_Local 所在段），只挑静止时为 0
    //  的 12 字节窗口，避免把常驻状态字段（蹲伏计时、下落速度）误认。
    // ============================================================
    struct PunchCand
    {
        uintptr_t rel;
        DWORD     seenMs;      // 最近一次非零的时刻
        float     peak;        // 观测到的最大幅度（几度）
        bool      decayed;     // 停火后已回零
    };




} // namespace


// ============================================================
//  对外入口
// ============================================================
void CreateMove(sdk::CUserCmd* cmd)
{
    if (!cmd || !cmd->command_number || !cmd->tick_count)
        return;

    const ConfigCombat& c = g_Config.combat;

    // 随机源播种（一次性）
    if (!g.seeded)
    {
        g.seeded = true;
        srand((unsigned)GetTickCount());
    }

    // ---- 配置钳位：红线不可突破 ----
    // 运行时兜底（第三层）：菜单滑条、Config::Load、ApplyPreset
    // 都已钳过，这里是最后一道 —— 因为这三处未来都可能新增入口，
    // 而本模块是唯一真正决定发射节奏的地方。
    g.delayMs = c.autoFireMs;
    if (g.delayMs < kFireDelayMinMs) g.delayMs = kFireDelayMinMs;
    if (g.delayMs > kFireDelayMaxMs) g.delayMs = kFireDelayMaxMs;

    // ---- 连跳（纯开关；按住空格才生效）----
    if (c.bhop)
        TickBhop(cmd, OnGround());
    else
    {
        // 关闭时清空状态，避免下次开启时用陈旧的落地序列
        g.wasOnGround = false;
        g.landTick    = -1;
    }

    // ---- 连发（按住开火键即连发；按住 E 用药时不介入）----
    if (c.autoFire)
        TickAutoFire(cmd);
    else
    {
        g.firePrimed = false;
        g.block      = "连发未开启";
    }

    // ============================================================
    //  [批66] 门控诊断：原因【变化】时立刻打一行
    // ------------------------------------------------------------
    //  旧版 60s 才打一次，而且只说"累积 fire=N"，看不出为什么没开火。
    //  连发失效时只能靠猜是哪道门控挡的 —— 这就是"每次修复都像在
    //  重新试错"的根源。现在任何门控状态变化都会留下一条明确记录：
    //      [move] 拦=换弹 / 无弹匣 / 用物品 / 未按住 ...（nullptr = 放行）
    //  边沿触发 + 500ms 节流，正常对局不会刷屏。
    //
    //  [批94 修复] 上面原有一行悬空的 `if (c.noRecoilVis)`（批71 删掉本文件
    //  里的去后坐力实现后遗留）。它没有分号也没有独立语句体，于是把【下面
    //  整个诊断块】吞成了自己的函数体 → 只有开着"移除后坐力"时诊断才
    //  打印，关掉时静默。表现为"连发没反应但一条原因都没有"。
    //  去后坐力现已在 OverrideView 实现（渲染期），与本文件无关，此处删除。
    // ============================================================
    {
        static const char* s_lastBlock = nullptr;
        static bool        s_haveBlock = false;
        static DWORD       s_blockAt   = 0;
        const DWORD t = GetTickCount();

        if (!s_haveBlock || g.block != s_lastBlock)
        {
            if (t - s_blockAt >= 500 || !s_haveBlock)
            {
                s_haveBlock = true;
                s_blockAt   = t;
                s_lastBlock = g.block;
                PayloadLog("[move] 连发门控：%s",
                           g.block ? g.block : "放行（正在出脉冲）");
            }
        }
    }

    // ---- 节流诊断（60s）----
    static DWORD s_log = 0;
    const DWORD now = GetTickCount();
    if (now - s_log > 60000)
    {
        s_log = now;
        if (c.bhop || c.autoFire)
        {
            PayloadLog("[move] bhop=%d autoFire=%d 间隔=%dms ｜ "
                       "累计 jump=%d fire=%d ｜ 当前拦=%s",
                       c.bhop ? 1 : 0, c.autoFire ? 1 : 0, g.delayMs,
                       g.jumpCount, g.fireCount,
                       g.block ? g.block : "无");
        }
    }
}


} // namespace Movement
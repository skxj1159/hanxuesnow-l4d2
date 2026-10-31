#pragma once
// ============================================================
//  Config/Config.h —— 全局配置（ESP / 自瞄 / 拟人化 / 静默）
// ------------------------------------------------------------
//  【分层铁律】Config 是**纯数据 + 纯声明**，不包含任何游戏逻辑，
//  更不include Features/ 或 SDK/ 的任何东西。它只依赖标准库。
//
//  【循环依赖处理 —— 本文件是依赖图的第二个枢纽】
//  Config 被 Features（Aimbot / ESP）大量使用，如果 Config 反过来
//  include Features，就会成环。
//  解法：Config **只 include 标准库**，任何游戏类型都不认识。
//  需要实体信息？由Features 层自己 include SDK/Entity.h。
//
//  【为什么用单一g_Config 而不是每个功能一组全局变量】
//  现状是aimcfg::stormOn / enableBox2D / …散落各处，
//  改一个功能要翻三个文件，且持久化表与运行时变量容易漂移。
//  收成一个结构体后：
//    · 持久化 = 遍历结构体，天然不会漏
//    · 功能之间取配置只经一个符号
// ============================================================

#include <cstdint>
#include <Windows.h>

// ============================================================
//  分组结构体
// ============================================================

// ------------------------------------------------------------
//  ESP 绘制
// ------------------------------------------------------------
struct ConfigESP
{
    bool  box          = true;    // 方框
    bool  name         = true;    // 头顶名字
    bool  healthBar    = true;    // 血条
    bool  healthText   = true;    // 血量数值
    bool  distance     = true;    // 距离
    bool  boxFill      = false;   // 半透明填充
    bool  showAllies   = true;    // 显示队友
    bool  equipment    = true;    // 场景道具透视

    float boxPadding   = 2.0f;    // 框外扩（px）
    float healthBarW   = 4.0f;
    float nameSize     = 16.0f;
    float distanceSize = 12.0f;
    float healthSize   = 18.0f;
    float equipmentDist = 60.0f;  // 米，<=0 不限制

    // 命中盒来源：1 = 命中盒并集，0 = 骨骼 AABB，-1 = 自动降级
    int   geometrySource = -1;
};

// ------------------------------------------------------------
//  自瞄
// ------------------------------------------------------------
struct ConfigAimbot
{
    bool  enabled      = false;
    int   key          = VK_XBUTTON1;   // 触发键，0 = 常开
    float fov          = 4.0f;          // [Legit] FOV 圆锥（度），建议 3~5
    bool  showFov      = true;          // FOV 范围圈（屏中心，辅助确认范围）
    bool  silent       = false;         // [批31] 微静默（独立功能：只写 cmd，画面不动）
    float silentFov    = 8.0f;          // [批31] 静默生效范围（1~15 度，菜单可调）
    float distWeight   = 0.5f;          // [批4] 选靶权重：3D 距离分量（0=纯屏幕角距）
    int   aimPart      = 4;             // [批13] 瞄准部位：0头 1颈 2胸 3腹 4自动（屏幕最近可见部位）
    bool  friendlyFire = false;         // [批4] 是否允许锁定队友
    int   part         = 0;             // [批10] 瞄准部位：0=头 1=胸 2=臂 3=腿
                                        //   （RazeHack type 编码：头 0/1 兼容、
                                        //    胸/臂/腿按 type&~1 配对过滤）
    float maxDistance  = 60.0f;         // 米
    bool  hitboxTable  = true;          // 用硬编码命中盒索引表
    bool  priority     = true;          // 被抓住的队友优先
    bool  hitVerify    = true;          // 可见性验证
    bool  recoilComp   = true;          // 后坐力补偿
    bool  showRange    = false;         // 画范围圈

    // [批150·发布版] 日志级别：0 = 完整（开发排障），1 = 精简
    //   （发布版只留 启动 / 崩溃记录 / 心跳 三类日志）
    int   logLevel     = 0;
    // [批150] studiohdr 探测开关：1 = 开（排障用），0 = 关
    //   发布版默认 0 —— 这条链已实证走不通（引擎 .text 混淆），留着纯烧 CPU。
    int   studioHdrProbe = 1;
};

// ------------------------------------------------------------
//  静默（silent）—— 视角不跟手，只改CUserCmd
// ------------------------------------------------------------
struct ConfigSilent
{
    bool  enabled      = false;
    float fov          = 180.0f;   // 静默范围（度）
    bool  autoFire     = false;    // 静默自动开火
};

// ------------------------------------------------------------
//  拟人化 —— 让瞄准行为像人
// ------------------------------------------------------------
struct ConfigHumanize
{
    bool  enabled          = true;
    int   reactionMs      = 100;    // 反应延迟下限（ms）——legit 建议 80~250
    int   reactionMsMax   = 200;    // 反应延迟上限（ms），随机取值
    float smoothFactor    = 0.75f;  // [Legit] 越大越慢：每 tick 走剩余距离的 (1-S)
                                    //   0.75 ≈ Osiris smooth 4；0.95 很慢
    float jitterAngle     = 0.05f;  // [Legit] 微抖动幅度（度）——模拟手颤
    float deadzone        = 0.0f;   // 死区（度）
    bool  strafe          = false;  // 完美移动（±450 横移 + yaw 反补偿）
    bool  moveFix         = false;  // 反制 Anti-Aim
};

// ------------------------------------------------------------
//  射击增强 / 移动辅助
// ------------------------------------------------------------
//  bhop     自动连跳（纯开关，无参数——落地延迟与按住时长是内部
//           反检测常量，不开放给用户，避免"调参调到被检测"）
//  autoFire 自动连发（按住即连发，仅半自动武器有意义）
//  autoFireMs 连发间隔。硬边界由 Movement 模块强制：>=52ms
//           （1000/20，Lilac Macro 的"一秒 20 次上升沿"红线），
//           菜单与配置都只能在这个区间内取值。
struct ConfigCombat
{
    bool  noSpread       = true;   // 子弹无扩散
    bool  bhop           = false;  // 自动连跳
    bool  autoFire       = false;  // 自动连点（宏）
    int   autoFireKey    = 0;      // [批73] 宏触发键：按住它 = 替玩家按左键
                                   //   0 = 未绑定（退回"按住左键"旧行为）
    int   autoFireMs     = 60;     // 连点间隔 ms（钳位到 [52, 400]）
                                   //   只对"按住只出一发"的单发武器生效：
                                   //   程序按弹匣变化自动判定武器类型，
                                   //   连发武器（步枪/连狙等）纯按住不干预。
    bool  penetration    = false;  // 完整伤害模型
    float penetrationDmg = 12.0f;  // 穿透伤害阈值
    bool  noRecoilVis    = false;  // [批71] 移除画面抖动（唯一开关，批116 收口）：
                                   //   渲染层去抖 —— OverrideView 里用锁定的
                                   //   权威 punch 减最终视角（屏幕不抖、准星仍回弹）；
                                   //   punch 未锁定时退回估算路径。
                                   //   [批116] 输入层(SetViewAngles)与源头清零两个
                                   //   实验模式已删除：实测对本地屏幕无效/被重写。

    // 连发间隔的公开边界（供菜单滑条与配置加载共用，避免两处各写一份）
    static constexpr int kAutoFireMsMin = 52;
    static constexpr int kAutoFireMsMax = 400;

    // 配置加载/预设后的统一钳位——菜单滑条只防"手滑"，
    // 真正的红线由这里 + Movement::CreateMove 双重保证
    void Clamp()
    {
        if (autoFireMs < kAutoFireMsMin) autoFireMs = kAutoFireMsMin;
        if (autoFireMs > kAutoFireMsMax) autoFireMs = kAutoFireMsMax;
    }
};

// ------------------------------------------------------------
//  过检测 —— 时间基准 / 序列号 / 时钟校正
// ------------------------------------------------------------
struct ConfigAntiCheat
{
    bool  extraCommands   = true;
    int   extraCount      = 5;     // ≤14
    int   extraAction     = 10;
    bool  interpolateExtra = true;
    bool  seqShift        = true;
    int   seqReserve      = 0;
    bool  noClockCorr     = true;
};

// ------------------------------------------------------------
//  准星
// ------------------------------------------------------------
struct ConfigCrosshair
{
    bool  enabled    = false;
    int   radius    = 8;
    int   rotationR = 24;
    int   segments  = 12;
    int   iterations= 16;
    float speed     = 540.0f;   // 度/秒
};

// ============================================================
//  主配置
// ============================================================
struct Config
{
    ConfigESP      esp;
    ConfigAimbot   aim;
    ConfigSilent   silent;
    ConfigHumanize humanize;
    ConfigCombat   combat;
    ConfigAntiCheat anti;
    ConfigCrosshair crosshair;

    // ---- 界面 ----
    bool  menuOpen      = false;
    bool  espPreview    = false;

    // ---- 元信息（不持久化，只用于诊断）----
    int   lastTargetSid = -1;   // 上一 tick 命中的 Storm Identifier
};

// ============================================================
//  全局配置实例
// ------------------------------------------------------------
//  extern 声明在头，定义在 Config.cpp —— 这样任何 TU 都能改它，
//  且链接器保证全程序只有一份实例。
// ============================================================
extern Config g_Config;

// ============================================================
//  运行模式（ApplyPreset 切换的预设）
// ------------------------------------------------------------
enum class Preset
{
    Default,      // 全开，标准距离
    Legit,        // 拟人化优先：小 FOV、强平滑、随机延迟
    Aggressive,   // 大范围、无平滑、开伤害模型
    Silent,       // 全静默
};

namespace ConfigMgr
{
    // 切换预设：一次改完整个 g_Config
    void ApplyPreset(Preset p);
    const char* PresetName(Preset p);

    // ---- 持久化 ----
    bool Save();
    bool Load();
    // [批111·死代码] 删除了 `void Tick();  // 热加载轮询（渲染线程每帧调）`
    //   —— 全项目零调用，那条注释与事实相反（没有任何地方每帧调它），
    //   定义体也已删除。注释描述一个不存在的行为比没有注释更糟。
    void SaveNow();       // 立即同步保存（卸载/退出前）
                          // [批52] 返回 bool：菜单要用它给出真实的
                          // 成功/失败提示（原来恒 void → 只能无脑
                          // 显示"已保存"，写盘失败也照样报成功）
    bool TrySaveNow();    // = SaveNow 的带返回值版本

    // ---- 热键 ----
    void RegisterHotkeys();
}

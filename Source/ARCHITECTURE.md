// ============================================================
//  ARCHITECTURE.md —— 目标工程结构与依赖规则
//  （本文件放在仓库里，防止半年后自己把分层破坏掉）
// ============================================================

================================================================
 一、目录树
================================================================

Source/
│
├── dll_entry.cpp                 ← DLL 入口（对应"根目录 main.cpp"）
├── main.cpp                      ← 注入器入口（独立 exe 工程，勿混）
│
├── SDK/                          【L1】只读环境层，零功能逻辑
│   ├── Offsets.h                 全局偏移 + 特征码 + Mem:: 安全读写
│   ├── Vector.h                  Vector / QAngle / Matrix3x4 / Matrix4x4
│   ├── Cmd.h                     CUserCmd 字段布局薄壳（零依赖）
│   ├── Interfaces.h / .cpp       全局接口指针 + GetInterface 原语
│   ├── NetVars.h   / .cpp        RecvTable 动态偏移解析
│   └── Entity.h    / .cpp        CBaseEntity + 实体列表 + 本帧快照
│
├── Config/                       【L0】纯数据，零游戏逻辑
│   ├── Config.h / .cpp           struct Config + extern g_Config
│   │├── ConfigESP                方框/血条/名字/距离/道具
│   │  ├── ConfigAimbot           自瞄开关与参数
│   │  ├── ConfigSilent           静默
│   │  ├── ConfigHumanize         拟人化（延迟/平滑/抖动/死区/Strafe）
│   │  ├── ConfigCombat           无扩散/连跳/连发/伤害模型
│   │     ├── ConfigAntiCheat     过检测三件套
│   │     └── ConfigCrosshair     Storm 旋转十字
│   │  └── ApplyPreset()          模式切换
│   │
│   ├── Render/                   【L2】绘制封装
│   │   ├── Render2D.h / .cpp     DrawBox / DrawText / DrawHealthBar
│   │  └── Render3D.h / .cpp      OBB / 骨骼 / 世界→屏幕投影
│   │
│   ├── Features/                 【L3】功能逻辑
│   │  ├── ESP.h    / .cpp        ESP::Render()
│   │  └── Aimbot.h / .cpp        Aimbot::CreateMove()
│   │
│   └── Hooks/                    【L4】只做触发，零逻辑
│       └── Hooks.h / .cpp        MinHook 挂载/卸载
│           ├── Hooked_PaintTraverse()  → 只调 ESP::Render()
│           └── Hooked_CreateMove()      → 只调 Aimbot::CreateMove()


================================================================
 二、依赖图（有向无环 —— 编号即拓扑序）
================================================================

                    ┌─────────────┐
       L5           │  dll_entry  │
                    └──────┬──────┘
                           │
                    ┌──────▼──────┐
       L4           │    Hooks    │  只做触发
                    └──────┬──────┘
                           │
          ┌────────────────┴────────────────┐
┌─────────▼─────────┐              ┌───────▼────────┐
│      Features     │              │     Render     │
│  ESP / Aimbot     │──────────────│  2D / 3D       │
└─────────┬─────────┘              └───────┬────────┘
          │                                │
    ┌─────┴──────┬─────────────┐            │
┌───▼───┐  ┌───▼────┐  ┌─────▼─────┐      │
│ Config│  │  SDK   │  │   SDK     │      │
│  (L0) │  │Entity  │  │Interfaces │      │
└───┬───┘  └───┬────┘  │ NetVars   │      │
    │          │       │  (L1)     │      │
    └──────────┴───────┴───────────┘      │
                       ▲                  │
                       └──────────────────┘
                    （Render3D → SDK，单向）

【规则】边只能从低编号指向高编号。任何反向 include 都是架构腐化。

【验证】12 个头文件各自独立 TU 编译 + 链接，exit=0。
       复现：把每个头包成独立 .cpp 丢进一个工程，只 #include 它。


================================================================
 三、循环依赖的三个具体解法
================================================================

问题 1 ── 接口类型互相引用成团
─────────────────────────────────────────────
  IVEngineClient 里有 IEngineTrace 的方法，IEngineTrace 里要
  CTraceFilter…，引擎 SDK 之间互相 include 成一团。

  解法：Interfaces.h 对所有接口类型只做**前向声明**
        （struct IVEngineClient;），指针类型不需要完整定义。
        要调方法的那一层自己 include 引擎 SDK 并
        reinterpret_cast。结果：Interfaces.h 依赖集合为空，
        它是依赖图的根。

问题 2 ── CUserCmd 会把引擎 SDK 拖进 Features
─────────────────────────────────────────────
  Features/Aimbot 需要 CUserCmd（要改 viewangles / buttons），
  而 CUserCmd 属于引擎 SDK；Features 又被 Hooks include，
  Hooks 的CreateMove 签名里也有引擎类型 → SDK ↔ Features 成环。

  解法：SDK/Cmd.h 只抄**字段布局**，不 include 任何引擎头。
        且 Aimbot::CreateMove 的入参用 void* 接收，
        内部再 reinterpret_cast。
        结果：Features 层彻底不认识任何引擎类型。

问题 3 ── Config 既是数据又被 Features 依赖
─────────────────────────────────────────────
  Config 被 Aimbot/ESP 大量读，若 Config 反过来 include
  Features/C++Engine/… 任何一项就成环。

  解法：Config.h 只 include <cstdint> 与 <Windows.h>，
        **不认识任何游戏类型**。需要实体信息时由
        Features 层自己 include SDK/Entity.h（单向）。

  额外收益：Config 变成可被独立单测的纯 POD —— 写个测试
        include Config.h 就能验证 ApplyPreset 的行为，
        不需要拖整个游戏环境。


================================================================
 四、Hooks 层的硬性约束（最重要的一条）
================================================================

  Hooked_PaintTraverse()  内部只允许出现：  ESP::Render();
  Hooked_CreateMove()      内部只允许出现：  Aimbot::CreateMove(pCmd, sampleTime);

  任何 if / 循环 / 读内存 / 算角度出现在这两个函数里，
  都意味着功能逻辑与触发时机重新耦合 —— 那就等于回到旧结构：
  AimCore2.cpp 783 行里只有几十行是"钩子该做的事"，
  其余全是本该属于 Aimbot / Config 的逻辑。

  Code Review 时先看这两个函数，再看别的。


================================================================
 五、两处必须写进代码的现实约束
================================================================

1. 字节↔米换算只有一个入口
   L4D2 1 米 ≈ 52.49 世界单位。
   走ESP::DistanceMeters()，不要在别处自己乘除。
   （踩过：阈值直接平方，60 米当成 1.1 米，差 52 倍）

2. 易漂移字段一律走 NetVars，硬编码只做兜底
   踩过：Offsets::m_lifeState = 0x147 在当前 client.dll 上
   对**活着的**感染者读出 1/2，于是"lifeState!=0 就拒绝"
   把所有感染者全拒掉 —— 表现是「透视有框但自瞄毫无反应」，
   不崩溃、不报错，极难定位。
   NetVars::Check() 会把"哪些解析到了、哪些回退了"打出来，
   排查时先看这一行。


================================================================
 六、与现状的三点差异（不能想当然）
================================================================

1. 绘制后端：目标 ISurface，实际 OS-ImGui
   注入环境下纹理加载失败会黑屏/变黑块；项目已有自绘 ImGui
   框架。Render2D.h 做的是**接口形态对齐、实现后端可换** ——
   现在转发 OS-ImGui，将来换 ISurface 只改这一个文件。

2. 挂钩方式：目标 MinHook，实际 vtable[27] 手写跳板
   CreateMove 是 client.dll 内部虚表槽，手写跳板够用且少一个依赖。
   Hooks.h 的 Status 结构预留了 trampoline 字段，两种方案形状一致。

3. 入口文件名：目标 main.cpp，实际 dll_entry.cpp
   本仓库 main.cpp 已被注入器（l4d2.exe）占用。

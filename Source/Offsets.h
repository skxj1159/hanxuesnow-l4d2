#pragma once
#include <cstdint>

namespace Offsets
{
	// Offsets
	constexpr uintptr_t dwLocalPlayer = 0x726BD8; // Localplayer
	constexpr uintptr_t m_vecOrigin = 0x124;
	constexpr uintptr_t dwViewMatrix = 0x601FDC;
	constexpr uintptr_t viewMatrixInnerOffset = 0x2E4;
	constexpr uintptr_t dwEntityList = 0x73A574;
	constexpr uintptr_t m_iTeamNum = 0xE4; // Client.dll
	constexpr uintptr_t m_iHealth = 0xEC;
	constexpr uintptr_t m_fFlags = 0xF0;
	constexpr uintptr_t m_zombieClass = 0x1C90;
	// Class DT_TerrorPlayer
	// ⚠ 下面两个硬编码偏移在当前 client.dll 上已**失效**（2026-10-18 实测）：
	//   m_lifeState=0x147 对**活着的**感染者读出 1/2 → LiveEsp 收录门槛
	//   与自瞄硬门槛把所有感染者全拒 → 表现是"透视有框但自瞄毫无反应"。
	//   已改为走 RecvTable 动态解析（AimCore2::ResolveNetvars），
	//   解析结果发布到下面的运行时槽，由 LiveEsp 优先使用；
	//   动态槽为 0 时才回退到这里的硬编码值（保持旧行为，便于对比）。
	inline constexpr uintptr_t m_lifeState = 0x147;
	inline constexpr uintptr_t m_isGhost = 0x1C9A; // bool，==1 = 幽灵（未出生）状态（静态验证）

	// ---- 运行时 netvar 槽（由 AimCore2::ResolveNetvars 填充，0 = 未解析）----
	//   LiveEsp 在渲染线程读这两个槽。只在初始化阶段写一次、之后只读，
	//   属于「先同步后发布」的模式，不需要原子操作。
	inline uintptr_t rtLifeState = 0;
	inline uintptr_t rtIsGhost   = 0;

	// 取生效偏移：优先动态解析值，失败才用硬编码
	inline uintptr_t LifeState()
	{
		return rtLifeState ? rtLifeState : m_lifeState;
	 }
	inline uintptr_t IsGhost()
	{
		return rtIsGhost ? rtIsGhost : m_isGhost;
	 }

	// ---- 实体识别 / 移动预判（功能3、5；静态分析验证）----
	// 类主虚表 RVA：对 client.dll（2026-07 版）按 RTTI→COL→vtable 链路解析所得。
	// 实体对象首 DWORD（vptr）即指向所属类的虚表；C_Witch 与 C_Infected 各自
	// 持有独立虚表，可精确区分女巫与普通感染者（不依赖模型名字符串）。
	// 客户端更新后若判定失效，需重新解析核对。
	inline constexpr uintptr_t kWitchVtableRva  = 0x60C61C; // C_Witch 主虚表
	inline constexpr uintptr_t kCommonVtableRva = 0x60BC24; // C_Infected 主虚表
	inline constexpr uintptr_t kPlayerVtableRva = 0x5B6494; // 幸存者实测 vptr（C_TerrorPlayer 基类）

	// ---- 特感 / 坦克主虚表（2026-10-18 实机解析）----
	// 重要：L4D2 客户端给**每一种特感都派生了独立类**（C_Smoker / C_Boomer /
	// C_Hunter / C_Spitter / C_Jockey / C_Charger / C_Tank），每个派生类有自己
	// 的主虚表，与幸存者的 C_TerrorPlayer(0x5B6494) 都不同。
	// 这正是「vptr 必须等于本地玩家 vptr」这条判据会整片漏掉特感的根因。
	// 解析链路：RTTI 类型名字符串 ← RTTICompleteObjectLocator(+0x0C)
	//          ← vtable[-1]；同一类会命中 5 张表，取 COL.mdisp==0 的那张即主虚表。
	// 已与实机取样交叉验证：Boomer=0x60D454、Hunter=0x60E1F4、Jockey=0x60F664、
	// Spitter=0x60EF94、Witch=0x60C61C、Infected=0x60BC24 全部一致。
	// 客户端更新后若特感又"消失"，重跑 C:\Temp\wb_verify\probe_rtti.py 校准。
	inline constexpr uintptr_t kSmokerVtableRva  = 0x60DB24; // C_Smoker
	inline constexpr uintptr_t kBoomerVtableRva  = 0x60D454; // C_Boomer
	inline constexpr uintptr_t kHunterVtableRva  = 0x60E1F4; // C_Hunter
	inline constexpr uintptr_t kSpitterVtableRva = 0x60EF94; // C_Spitter
	inline constexpr uintptr_t kJockeyVtableRva  = 0x60F664; // C_Jockey
	inline constexpr uintptr_t kChargerVtableRva = 0x60E8C4; // C_Charger
	inline constexpr uintptr_t kTankVtableRva    = 0x60CD8C; // C_Tank

	// 眼位偏移（C_BasePlayer::m_vecViewOffset）—— 真玩家恒为 (0,0,~62)。
	// 用作"这确实是一个玩家实体"的结构判据：武器/投掷物/道具的同一位置
	// 是别的字段，凑不出「x、y≈0 且 z 在 30~90」这种组合。
	inline constexpr uintptr_t m_vecViewOffset = 0xF4;

	// 目标当前速度（float3；[0/1/2] 位于 0x100/0x104/0x108）——移动预判数据源
	inline constexpr uintptr_t m_vecVelocity = 0x100;

	// ---- 骨骼矩阵（骨骼自瞄 / 骨骼透视数据源）----
	// C_BaseAnimating 骨骼矩阵（matrix3x4_t 数组，每骨 0x30 字节，位置在
	// +0x0C/+0x1C/+0x2C）。公开资料口径（Guided Hacking L4D2 实测）：实体
	// +0x6C0。此值仅作为 BoneSystem 自适应发现的**初始候选**：指针/内联
	// 两种解释均失败时，运行时扫描实体内存窗口自动发现真实偏移并缓存，
	// 客户端更新后无需改码。
	inline constexpr uintptr_t m_dwBoneMatrix = 0x6C0;
}

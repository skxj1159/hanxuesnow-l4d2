#pragma once
// ============================================================
// BoneSystem —— 骨骼数据读取（外部 RPM）与骨架/骨骼自瞄共用模块
// ------------------------------------------------------------
// 数据源：C_BaseAnimating 骨骼矩阵（matrix3x4_t 数组，每骨 0x30 字节，
//   位置位于每骨矩阵的 +0x0C / +0x1C / +0x2C 三列）。
// 骨骼索引：L4D2 全模型 ValveBiped 真实布局（.mdl 骨骼名表实测确认，
//   详见下方 enum 注释）。
// 偏移健壮性（自瞄重构核心）：骨骼矩阵偏移**自适应发现**——先按公开口径
// （实体+0x6C0）的指针/内联两种解释尝试，失败则在实体内存窗口内扫描
// "像指针"的值并按三骨合理性校验，命中即缓存偏移全局复用（每帧至多
// 扫描一个实体）。客户端更新后无需改代码即可自愈；全程失败时调用方
// 回退旧的身高比例估算法（AimBridge/DrawSkeleton）。
// 帧缓存：每渲染帧 BoneFrameBegin() 递增世代号，帧内按实体索引缓存
//   （ESP 骨架绘制与自瞄选靶共用，同一实体一帧只读一次）。
// ============================================================

#include "Include.h"

// ValveBiped 骨骼索引（L4D2 实测：hunter/boomer/smoker/witch/ceda/幸存者
// 模型 .mdl 骨骼名表逐一解析确认，全模型一致）：
//   0 骨盆，1~4 左腿，5~8 右腿，9~12 脊柱链（Spine/Spine1/Spine2/Spine4），
//   13 颈，14 头，15~18 左臂，34~37 右臂。
// 注意：CS 系列的 "6=头" 在 L4D2 是右小腿——硬照 CS 索引会让自瞄打脚。
enum L4D2Bone {
    Bone_Pelvis  = 0,
    Bone_L_Thigh = 1,  Bone_L_Calf = 2,  Bone_L_Foot = 3,  Bone_L_Toe0 = 4,
    Bone_R_Thigh = 5,  Bone_R_Calf = 6,  Bone_R_Foot = 7,  Bone_R_Toe0 = 8,
    Bone_Spine   = 9,
    Bone_Spine1  = 10,  // 上腹部（自瞄"腹部"候选）
    Bone_Spine2  = 11,
    Bone_Spine4  = 12,  // 上胸
    Bone_Neck    = 13,
    Bone_Head    = 14,
    Bone_L_Clavicle = 15, Bone_L_UpperArm = 16, Bone_L_Forearm = 17, Bone_L_Hand = 18,
    Bone_R_Clavicle = 34, Bone_R_UpperArm = 35, Bone_R_Forearm = 36, Bone_R_Hand = 37,
    Bone_Count_  = 38,   // 骨架绘制/缓存覆盖的骨骼数（0..37）
};

// 单实体骨骼快照（帧内缓存复用）
struct BoneSnapshot {
    Vec3 pos[Bone_Count_];
    bool valid[Bone_Count_];
    bool ok;   // 头/颈/骨盆等关键骨骼有效（骨架与骨骼自瞄可用）
};

// 每渲染帧调用一次（DrawCallBack，FrameCacheBegin 之后）：骨骼帧缓存进入新帧
void BoneFrameBegin();

// 读取实体骨骼快照（帧内按实体索引缓存，同一实体一帧只读一次 RPM）；
// 失败时返回 false（out.ok=false，调用方回退身高比例估算）
bool GetEntityBones(int entIndex, const Vec3& origin, BoneSnapshot& out);

// 取实体骨骼矩阵数组基址（每骨 0x30 字节，matrix3x4_t）。
// 与 GetEntityBones 共用同一份自适应偏移缓存；缓存偏移失效时会在实体
// 窗口内做一次"矩阵像不像"的扫描（每帧至多一个实体），命中即写回缓存。
// origin 用于校验矩阵平移列是否落在实体附近（防止把别处的矩阵数组认成骨骼）。
// 失败返回 0 —— 调用方（HitboxSystem）须降级到眼高比例兜底。
// 注意：不要求头/颈/骨盆齐备，因此比 GetEntityBones 宽松，且不依赖
// "骨架透视开关是否打开"（那条路径只在绘制骨架时才会调 GetEntityBones）。
uintptr_t GetEntityBoneMatrixBase(uintptr_t ent, const Vec3& origin);

// ============================================================
//  [批109] 统一的实体地址解析 —— 骨骼缓存的键必须两端同源
// ------------------------------------------------------------
//  为什么必须导出：
//    g_boneStore 的键是【实体地址】。取骨端用 BoneSystem::GetEntAddr
//    （client + dwEntityList + idx*0x10）写入，读骨骼端却用
//    sdk::CBaseEntity::Address()（构造时可能被显式传入别的地址）。
//    两者不一致时，缓存永远查不到 → GetEntityBoneMatrixBase 返回 0
//    → 日志报"骨骼矩阵不可用" → 自瞄退到兜底点 = 【瞄准空气】。
//
//  用法：读取侧一律改用本函数拿地址，保证与取骨端【逐位相同】。
//  这是唯一能保证命中的办法 —— 不去猜 SDK 侧地址是怎么来的。
// ============================================================
uintptr_t BoneSysEntityAddr(int index);

// [批15] 引擎 vcall 链助手（RazeHack 实证布局）
//  GetModelInfoIface  ：VModelInfoClient004（回退 003），进程内一次解析
//  GetEntityStudioHdr ：networkable 槽8 GetModel → modelinfo 槽30
//                       GetStudiomodel → studiohdr_t*（IDST 校验）
//  两者全程活体门控 + SEH，失效/异常返回 0
uintptr_t GetModelInfoIface();
uintptr_t GetEntityStudioHdr(uintptr_t ent, uintptr_t modelInfo);

// [批19] 引擎 curtime 注入（CreateMove 每 tick 调；未注入时 SetupBones 关闭）
void BoneSysSetCurtime(float t);

// [批30] 渲染线程每帧刷新骨骼缓存（引擎骨骼取数只发生在渲染线程；
//  连续出现 ≥3 帧的实体才取 = 召唤瞬间不碰未就绪的 animstate）
void BoneSysRefreshEntities(const int* entIndices, int count);

// [批95] 读引擎 dwLocalPlayer（client.dll + 偏移）。
//   自瞄的骨骼刷新门控用此函数，不再依赖透视模块设的 localPlayer 全局 ——
//   透视延迟启动或某些路径没走时 localPlayer=0 → 骨骼刷新被挡 →
//   自瞄全走兜底 Origin+64 → 打空气。
uintptr_t BoneSysLocalPlayerPtr();

// 骨架绘制线段表（ValveBiped 链路；DrawSkeleton 共用，返回段数）
struct BoneSegment { unsigned char a, b; };
const BoneSegment* GetSkeletonSegments(int& count);

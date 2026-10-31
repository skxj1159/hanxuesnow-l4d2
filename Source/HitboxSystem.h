#pragma once
// ============================================================
//  HitboxSystem —— 打击盒（hitbox）瞄准点
// ------------------------------------------------------------
//  为什么不再用骨骼原点：
//    骨骼矩阵给的是**关节原点**，遇到自定义/替换模型时，关节原点可能被
//    摆到模型脚下甚至地板以下（作者重定向骨骼、动画未对齐、模型缩放为 0
//    等），自瞄就会"直接打地板"。而 hitbox 是模型自带的碰撞体，由
//    studiohdr 声明、带 group（头/胸/腹…），几何中心天然在体表内部，
//    与动画姿态无关，稳定性远高于骨骼原点。
//
//  数据链路（实机核对过，见 .cpp 顶部注释）：
//    实体 + m_nModelIndex → IVModelInfoClient::GetModel(索引) → model_t
//         → model_t 内的 studiohdr_t*
//         → hitboxset (numhitboxsets / hitboxsetindex @ hdr+0xAC / +0xB0)
//         → mstudiohitboxset_t (numhitboxes / hitboxindex)
//         → mstudiobbox_t (bone / group / bbmin / bbmax，步长 0x30)
//         → 中心 (bbmin+bbmax)*0.5
//         → hitbox->bone 的 matrix3x4 变换到世界坐标
//
//  为什么必须走 modelinfo：客户端实体里**根本没有** studiohdr 指针
//  （0x12000 字节逐 dword 扫描为 0 命中），唯一能找到的一处是
//  第一人称手臂视图模型，拿它算命中盒会把点算到手臂上。
//
//  防呆校验（关键）：拿到世界坐标后必须与实体原点比对，
//  落在地板下或离脚底不到 15 单位的一律废弃，降级到下一个部位。
// ============================================================

#include "Include.h"

// 三个可瞄部位（顺序即降级顺序：头 → 胸 → 腹）
struct HitboxParts
{
    Vec3 head;            // HITGROUP_HEAD  中心世界坐标
    Vec3 chest;           // HITGROUP_CHEST 中心世界坐标
    Vec3 stomach;         // HITGROUP_STOMACH 中心世界坐标
    bool okHead    = false;   // 该部位命中且通过防呆校验
    bool okChest   = false;
    bool okStomach = false;
    bool any       = false;   // 至少一个部位可用
    int  nboxes    = 0;       // 本模型命中盒总数（诊断）
    int  set       = -1;      // 实际使用的 hitbox set 序号（诊断）
};

// 初始化（AimCore2 解析出 IVModelInfoClient 与 m_nModelIndex 偏移后调用）
//   modelInfo：IVModelInfoClient 接口指针（可为空 → 全程走眼高兜底）
//   modelIndexOffset：实体上 m_nModelIndex 的偏移（0 = 未解析到）
void HitboxInit(void* modelInfo, uintptr_t modelIndexOffset);

// modelinfo 与模型索引偏移是否都就绪
bool HitboxReady();

// 每渲染帧调用一次（与 BoneFrameBegin 同一处）：命中盒帧缓存进入新帧
void HitboxFrameBegin();

// 读取实体的头/胸/腹打击盒中心（帧内按实体索引缓存，同一实体一帧只解析一次）
//   origin：实体世界原点（m_vecOrigin，玩家在脚底）—— 防呆校验的基准
//   返回 false 表示本实体没有任何可用命中盒，调用方应走眼高比例兜底
bool GetEntityHitboxParts(int entIndex, const Vec3& origin, HitboxParts& out);

// [批32] 通用部位解析（模型无关，可复用）
//  · 动态遍历 CStudioHdr 的 hitbox 列表；头部判定 = 绑定骨骼【骨名】
//    含 head，回退 = 几何（模型空间高度区间）
//  · 局部中心 → VectorTransform(bone→world 矩阵) → 世界坐标（一次变换）
//  · 失败返回 false 并给出原因（模型未加载 / hitbox 列表为空 / 无匹配）
enum HitboxPartKind
{
    HBP_Head = 0, HBP_Neck, HBP_Chest, HBP_Stomach, HBP_Leg, HBP_Count
};
bool GetEntityPartPoint(int entIndex, const Vec3& origin, int partKind,
                        Vec3& outWorld, const char** outFail);

// [批40·修复P1-2] 骨名表步长解析（每 hdr 一次探测并缓存）——
//  供 Aimbot::ResolvePartBones 复用，避免各处自带一份全局 stride
int HitboxGetBoneStride(uintptr_t hdr);

// ============================================================
//  命中盒云（ESP 几何来源，Storm Paint.hpp 同款思路）
// ------------------------------------------------------------
//  为什么不用骨骼点 AABB：骨骼点是关节原点，只有点没有"面"，横躺/扑击/
// 骑乘时 AABB 会明显大于实际可见轮廓；而命中盒是模型自带的 OBB，8 个角
// 本身就是可见模型的边界近似，投影后逐盒画框能贴合姿态。
//  做法与 Storm 一致：bbmin/bbmax 经该盒绑定骨骼的 matrix3x4 变换出 8 个
//  世界角点 → 逐角 WorldToScreen → 逐盒连成屏幕线框。
// ============================================================

// 单个命中盒的世界 OBB（8 角已变换到世界空间）
struct HitboxOBB
{
    Vec3 corner[8];     // 世界坐标，索引位序：bit0=x  bit1=y  bit2=z（1 取 hi）
    Vec3 center;        // 世界中心（诊断 / 自瞄复用）
    int  group = 0;     // HITGROUP_*，0 表示未解析到
    bool ok = false;
};

// 单实体命中盒云（帧内缓存；同一实体一帧只解析一次）
struct HitboxCloud
{
    static constexpr int kMaxBoxes = 32;   // 与解析上限一致（hitbox 数 > 32 视为非法）
    HitboxOBB box[kMaxBoxes];
    int  count = 0;      // 有效盒数
    bool any    = false;  // 至少一个盒通过校验
    bool done   = false;  // 本帧已解析过（即使 any=false 也是"真的没有"）
};

// 取实体命中盒云（帧内缓存）。返回 any；false 表示解析链路不可用
// （模型头未取到 / 骨基址为 0），此时不能与"解析了但全被过滤"混淆。
bool GetEntityHitboxCloud(int entIndex, const Vec3& origin, HitboxCloud& out);

// ============================================================
//  模型名（分类判据的唯一权威来源）
// ------------------------------------------------------------
//  为什么必须有这个：客户端实体里**没有** model_t 指针。曾经用
//  `entity + 0x10` 当 model_t* 直接读字符串，那块内存根本不是模型
//  头 —— 所有"按模型名分类"的判断都在读垃圾，结果是特感分类长期
//  只靠硬编码虚表 RVA 撑着，一漂移就整类不显示。
//  正确链路（HitboxSystem 早已实机验证）：entity + m_nModelIndex
//  → IVModelInfoClient::GetModel() → model_t，其 +0 即 szName[64]。
//  与命中盒解析共用模型头查找与帧缓存，额外开销≈0。
// ============================================================

// 取实体模型名（形如 "models/infected/boomer.mdl"）。
//   origin：实体世界原点（骨基址解析需要，与命中盒同一套缓存）
//   out/cap：输出缓冲与容量；保证 NUL 结尾
//   返回 false 表示链路不可用（modelInfo 未就绪 / 模型索引无效）
bool GetEntityModelName(int entIndex, const Vec3& origin, char* out, int cap);

// ============================================================
//  Storm 式瞄准点验证：按类枚举候选命中盒
// ------------------------------------------------------------
//  Storm Copy_Command.hpp 的做法是给每个职业硬编码一个盒索引
//  （{270,10}=烟鬼第 10 盒 …），只打那一个盒；打不中就换下一个目标。
//  我们不照抄那张表（盒序是版本相关的，且我们已有 HITGROUP 语义），
//  而是【同类内枚举全部盒】：从最接近准心的盒开始逐个试，
//  由调用方做可见性验证。这样既拿到了"多盒候选"的好处，
//  又不依赖任何硬编码盒号。
//
//  按 HITGROUP 排序（头 → 胸 → 腹 → 四肢），头部优先。
// ============================================================

// 从实体的全部命中盒里，挑出体积最大的 N 个盒中心（世界坐标），
//   按【头部优先 → 体积降序】排序写入 out。
//   origin：实体世界原点（防呆基准）
//   maxOut：输出容量（建议 6~8）
// 返回写入数量；0 = 无可用命中盒（调用方回退眼高估算）
int GetEntityAimPoints(int entIndex, const Vec3& origin, Vec3* out, int maxOut);

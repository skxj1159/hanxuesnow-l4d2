#pragma once
// ============================================================
//  IdentifierSystem —— Storm 式实体识别（Get_Identifier 等价实现）
// ------------------------------------------------------------
//  2026-10-18 移植自 Storm（qwertyuiop3/Storm）Estimate_Velocity.hpp:8-111。
//
//  Storm 的做法：直读客户端的 **serverclass 网络类 ID**，绕开虚表 RVA：
//      Identifier = *(__int32*)( *Get_Identifier_Type(*(void**)(Entity+8) + 4)()
//                      + 20 );
//  其中 Get_Identifier_Type 取自 client.dll 的**混淆导出表**（调用点藏在
//  实体 +8 的虚表里，再间接一层才拿到 +20 处的类 ID）。
//  这些偏移是 Storm 作者对自己那份 client.dll 逆向出来的，**不能照搬**：
//  直接抄会读到垃圾、整类实体静默消失。
//
//  本模块取的是**同一条语义、不同取数路径**：
//      客户端模型索引(m_nModelIndex, netvar)
//        → IVModelInfoClient::GetModel(idx)
//        → model_t.szName[64]（内联字符数组，注意不是指针）
//        → 关键字分类 → Storm 的 Identifier 常量
//  再叠加 Storm 判定里真正起作用的那几条**数值判据**（存活位、
//  "eath" 动画排除、感染者按 zombieClass 反查等），语义与它一致。
//
//  为什么这条链可靠：
//  · 模型名是**资产路径**（models/infected/xxx.mdl），跨版本稳定；
//    而虚表 RVA / 混淆表会随版本漂移，漂了就整类漏画。
//  · 上一轮"boomer 不显示"的根因就是把 entity+0x10 当 model_t* 读（客户端
//    实体里根本没有 model_t 指针），导致分类空转。现已走通 GetModel。
//
//  ID 语义（与 Storm 表格逐一对齐，不要随意改）：
//    0=Boomer   13=Rock      73=Aid      99=Charger   232=Survivor
//    263=Hunter 264=Infected 265=Jockey  270=Smoker   272=Spitter
//    276=Tank 277=Witch      256=Ammo 105=Adrenaline 109=Defibrillator 121=Pain
//  注意与我们的 zombieClass 不同：Boomer 是 0 不是 2，Charger 是 99 不是 6，
//  普通小僵尸是 264 特感是 0/263/265/270/272 —— 这正是 Storm 的排序键。
// ============================================================

#include "Include.h"

// ---- Storm Identifier 常量（务必与 Paint.hpp / Copy_Command.hpp 的表一致）----
namespace sid
{
    constexpr int Boomer        = 0;
    constexpr int Rock          = 13;
    constexpr int Aid           = 73;
    constexpr int Charger       = 99;
    constexpr int Adrenaline    = 105;
    constexpr int Defibrillator = 109;
    constexpr int Pain          = 121;
    constexpr int Survivor      = 232;
    constexpr int Ammo          = 256;
    constexpr int Hunter        = 263;
    constexpr int Infected      = 264;
    constexpr int Jockey        = 265;
    constexpr int Smoker        = 270;
    constexpr int Spitter       = 272;
    constexpr int Tank          = 276;
    constexpr int Witch         = 277;
}

// 一个被识别出来的实体（Storm Target_Structure 的等价物 + 我们需要的附加字段）
struct StormTarget
{
    int      id          = -1;   // 带符号：>0 敌人/幸存者，<0 场景道具，-1 未识别
    uintptr_t self       = 0;    // 实体指针
    int      team        = 0;    // Storm 会把道具改写成 1；我们不写实体，只在本地记录
    float    distanceSq  = 0.0f; // 到本地玩家的**平方**距离（Storm 排序不开根号）
    int      tickNumber  = 0;    // 插值 tick 序号（自瞄回写 tick_count 用）
    int      zombieClass = 0;    // 我们原有的类号（1..8），保留给自瞄沿用
    int      survivorId  = -1;   // 幸存者角色 ID 0..7（entity+7308），-1 非幸存者
    bool     ghost       = false; // 幽灵态（未出生）
};

// 识别一个实体。
//   modelName / szName：模型名（可为空，此时只靠数值判据）
//   out：输出
// 返回 out.id：>=0 敌人或幸存者；<0 道具（-73 / -105 …）；-1 未识别
int StormIdentify(uintptr_t ent, const char* modelName,
                  int localTeam, const Vec3& localPos, const Vec3& entPos,
                  int zombieClass, StormTarget& out);

// 从模型名直接映射到 Storm Identifier（不查实体）。识别失败返回 -1。
int IdentifierFromModelName(const char* modelName);

// Storm 的 zombieClass → Identifier 反查表（感染者专用）。
// 对齐 Estimate_Velocity.hpp:48-63：{1:270 Smoker, 2:0 Boomer, 3:263 Hunter,
// 4:272 Spitter, 5:265 Jockey, 6:99 Charger, 8:276 Tank}
int IdentifierFromZombieClass(int zombieClass);

// Storm 的装备子类 → 道具 Identifier（对齐 Estimate_Velocity.hpp:83-94）。
//   12→Aid(73)  15→Pain(121)  23→Adrenaline(105)  24→Defibrillator(109)
int EquipmentIdentifier(int subtype);
#pragma once
// ============================================================
//  SDK/Entity.h —— CBaseEntity（**只读环境层**）
// ------------------------------------------------------------
//  【分层铁律】本层**只读游戏数据**，绝不含任何功能逻辑：
//  没有开关判断、没有过滤规则、没有绘制/瞄准决策。
//  "要不要画这个实体""要不要打这个实体"是 Features 层的事。
//
//  【循环依赖处理 —— 本层是整个依赖图的枢纽】
//  依赖方向：
//      Offsets.h  ┐
//      Vector.h   ┼─→ Entity.h ─→（向上）Config.h / Features.h
//      Interfaces.h┤
//      NetVars.h  ┘
//  Entity.h **绝不** include Config.h 或 Features/*。
//  若某个 Features 层的数据结构（如 EntityInfo / EntityState）
//  想复用，Features 反过来 include 本文件即可（单向）。
//
//  【快照机制的归属】
//  渲染线程与游戏线程都要读实体，但两者不能共用同一个指针：
//    · 渲染线程（EndScene）：每帧重建
//    · 游戏线程（CreateMove）：每 tick 重建
//  旧的跨线程候选通道（AimBridge）已被删掉，改为**同帧快照**
//  共享：LiveFrameBegin 发布，GetFrameSnapshot 消费。
//  这个机制放SDK 层（本文件），因为它纯粹是"数据从哪来"。
// ============================================================

#include <cstdint>
#include <vector>
#include <mutex>
#include <utility>
#include <functional>

#include "Offsets.h"
#include "Vector.h"

namespace sdk
{
// [批36] 地图加载/切换检测（dwLocalPlayer==0）：加载期实体处于半初始化
// 状态，任何引擎方法调用都可能空指针 —— 所有实体访问入口先过这道门。
bool IsGameLoading();

    class CBaseEntity;

    // ============================================================
    //  网络类名分类（外部现成 ESP 的 m_ClassID 思路移植）
    // ------------------------------------------------------------
    //  实体 +8 的 IClientNetworkable 子对象虚表槽 1 = GetClientClass
    //  （Storm 对本版 client.dll 逆向验证过的槽位）→ ClientClass
    //  → m_pNetworkName（server class 名）。
    //  为什么这条链是分类主路：模型名分类依赖 IVModelInfo，本机
    //  VModelInfoClient004 常态拿不到（日志 5 次初始化全 0x0）→
    //  模型名链路断 → 分类空转。网络类名只依赖 VClient016（拿得到），
    //  且 server class 名跨版本比模型名还稳。
    //  ⚠ L4D2 的 server class 名没有 C 前缀："Witch"/"Tank"/"Infected"，
    //    幸存者 bot 是 "SurvivorBot"，人类玩家才是 "CTerrorPlayer"。
    // ============================================================
    bool GetNetClassName(uintptr_t ent, char* out, int cap);   // 失败返回 false
    bool NetClassNameToSid(const char* name, int& outSid);     // 未映射返回 false

    // ============================================================
    //  实体分类（Storm Identifier 语义）
    // ------------------------------------------------------------
    //  取值与 Storm 的 Extended_Interface 体系一致，
    //  保证"画出来的"和"打的"是同一套分类。
    //  负值 = 场景道具（-73 石头 / -105 肾上腺素 / -109 除颤器 …）
    //  -1 = 未识别
    // ============================================================
    namespace sid
    {
        inline constexpr int kBoomer     = 0;
        inline constexpr int kRock       = 13;
        inline constexpr int kAid        = 73;
        inline constexpr int kCharger    = 99;
        inline constexpr int kAdrenaline = 105;
        inline constexpr int kDefib      = 109;
        inline constexpr int kPain        = 121;
        inline constexpr int kSurvivor   = 232;
        inline constexpr int kAmmo       = 256;
        inline constexpr int kHunter     = 263;
        inline constexpr int kInfected   = 264;
        inline constexpr int kJockey     = 265;
        inline constexpr int kSmoker     = 270;
        inline constexpr int kSpitter    = 272;
        inline constexpr int kTank       = 276;
        inline constexpr int kWitch      = 277;
    }

    // ============================================================
    //  实体数据快照（同一帧内渲染线程写、游戏线程读）
    // ============================================================
    struct EntityInfo
    {
        sdk::Vector position{};
        sdk::Vector velocity{};
        int  team         = 0;
        int  health       = 0;
        int  index        = -1;
        int  stormId      = -1;   // 见上方 sid 说明
        bool isSurvivor   = false;
        bool isSpecial    = false; // 特感（含 Tank / Witch）
        bool isProp       = false; // 场景道具
    };

    struct EntityState
    {
        int  lifeState  = 0;
        bool isGhost    = false;  // 特感未出生
        bool valid      = false;
    };

    using EntityPair = std::pair<EntityInfo, EntityState>;

    // ============================================================
    //  命中盒
    // ============================================================
    struct Hitbox
    {
        bool  ok        = false;
        int   index     = -1;
        int   group     = 0;    // [批5] HITGROUP_*（1=头）——跨模型稳定的部位判据
        sdk::Vector center{};
        sdk::Vector min{};      // 世界包围 min（corner[0]）
        sdk::Vector max{};      // 世界包围 max（corner[7]）
        float radius    = 0.0f;
    };

    struct HitboxCloud
    {
        static constexpr int kMaxBoxes = 24;
        Hitbox box[kMaxBoxes]{};
        int    count = 0;
    };

    struct Bone
    {
        bool  ok    = false;
        int   index = -1;
        sdk::Vector pos{};
    };

    struct BoneCloud
    {
        static constexpr int kMaxBones = 128;
        Bone bone[kMaxBones]{};
        int  count = 0;
    };

    // ============================================================
    //  CBaseEntity —— 只读句柄
    // ------------------------------------------------------------
    //  刻意做成**值语义的小对象**而不是指针：构造一次、拷贝传递，
    //  内部只存 entity index +指针，任何时候读都走 SEH 保护。
    //  不缓存字段值 —— 注入环境下实体指针随时可能失效，
    //  缓存下来的血量/位置会在实体销毁后变成悬垂数据。
    // ============================================================
    class CBaseEntity
    {
    public:
        CBaseEntity() = default;
        explicit CBaseEntity(int index, uintptr_t addr = 0);

        // ---- 基础 ----
        bool        IsValid() const;
        bool        IsDormant() const;  // 休眠实体防线（槽位未校准前默认放行，见 .cpp）
        int         Index()   const { return m_index; }
        uintptr_t   Address() const { return m_addr; }
        const char* ClassName() const;      // 走 RTTI，需 m_addr 有效
        bool        IsPlayer() const;

        // ---- 基础字段（全部走 NetVars 解析优先、硬编码兜底）----
        sdk::Vector GetOrigin()   const;
        sdk::Vector GetVelocity() const;
        int         GetHealth()   const;
        int         GetTeam()     const;
        int         GetLifeState() const;
        bool        IsGhost()     const;
        sdk::Vector GetViewOffset() const;

        // ---- 分类 ----
        // Storm Identifier：m_nModelIndex → IVModelInfoClient::GetModel
        // → model_t.szName[64] → 关键字匹配。
        // ⚠ 不要走 Storm 原来的 Get_Identifier 混淆导出表路径
        //   （*(int*)(*Get_Identifier_Type(*(void**)(Entity+8)+4)()+20)）
        //   —— 那是 Storm 对自己 client.dll 逆向的导出，读出来是垃圾。
        int         GetStormId() const;
        int         GetZombieClass() const;   // 保留项：m_zombieClass
        bool        IsSurvivor() const;
        bool        IsSpecial()  const;
        bool        IsTank()     const;
        bool        IsWitch()    const;
        bool        IsProp()     const;      // stormId < 0

        // ---- 几何 ----
        bool        GetHitboxCloud(HitboxCloud& out) const;
        bool        GetBoneCloud(BoneCloud& out) const;
        bool        GetWorldBounds(sdk::Vector& min, sdk::Vector& max) const;

        // ---- 武器 ----
        uintptr_t   GetActiveWeapon() const;
        int         GetWeaponId() const;

        // [批62] 当前武器的弹匣数。
        //   返回值三态，调用方必须区分：
        //     >= 0       真实弹匣数（0 = 空仓）
        //     kClipNoClip 武器本来就没有弹匣（药丸/医疗包/除颤器/近战）
        //     kClipUnknown 偏移没解析到 / 武器实体无效 —— 调用方不要据此下结论
        // ⚠ 绝对不要把"未知"和"无弹匣"混成一个值：前者是读不到，
        //   后者是确定的事实，混用会让药品保护在解析失败时误判。
        static constexpr int kClipNoClip  = -1;
        static constexpr int kClipUnknown = -32768;
        int GetActiveWeaponClip() const;

        // ---- 眼位/枪位（自瞄与ESP 共用，必须同口径）----
        sdk::Vector GetEyePos() const;
        sdk::Vector GetShootPos(const sdk::QAngle& viewAngles) const;

        // ---- 生存救援优先级（Storm Priority）----
        //   m_hMyRecipient != -1 → 正被烟鬼抓住
        //   m_nClass != 0→ 被特感控制
        bool        IsBeingChased() const;
        bool        IsBeingControlled() const;

        // ---- 无效实体统一给"空"----
        static const CBaseEntity Invalid;

    private:
        // 统一的安全读：走 Mem::Read（SEH 保护）
        template <typename T>
        T Read(uintptr_t off, T fallback = T{}) const;

        int       m_index = -1;
        uintptr_t m_addr  = 0;
    };

    // ============================================================
    //  实体列表遍历（渲染线程每帧调用）
    // ============================================================
    namespace EntityList
    {
        // 初始化指针表（一次性）
        bool Init();

        // 遍历所有实体，调用方在 lambda 里决定收不收录。
        //   visit(const CBaseEntity&) → 返回 false 可提前终止。
        // 用途：LiveFrameBegin 的实体收集。
        void ForEach(const std::function<bool(const CBaseEntity&)>& visit);

        // 按索引取实体（自瞄命中盒链路按索引走）
        CBaseEntity Get(int index);

        // 本地玩家
        CBaseEntity LocalPlayer();
        uintptr_t  LocalPlayerAddr();

        int  MaxEntities();
    }

    // ============================================================
    //  本帧快照（渲染线程发布 → 游戏线程消费）
    // ------------------------------------------------------------
    //  这就是替代旧 AimBridge 的机制：不再是"渲染线程算好候选发给
    //  游戏线程"，而是**渲染线程发布完整实体列表**，游戏线程自己选靶。
    //  好处：分类语义天然同源，不会出现"透视是特感、自瞄当小僵尸"。
    // ============================================================
    namespace Snapshot
    {
        // 每渲染帧调一次：进程内直读（零 RPM、零快照年龄）并发布
        void Publish();

        // 消费者取本帧快照（游戏线程 / 渲染线程都可读）
        const std::vector<EntityPair>& Get();

        // 上一帧是否有效（判断 CreateMove 时快照是否已就绪）
        bool IsFresh();

        // 清空（卸载路径）
        void Reset();
    }
} // namespace sdk

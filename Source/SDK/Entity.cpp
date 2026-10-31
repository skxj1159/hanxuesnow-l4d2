// ============================================================
//  SDK/Entity.cpp —— CBaseEntity / EntityList / Snapshot 实现
// ------------------------------------------------------------
//  【本批（第一批：底层与配置）的职责】
//    1) CBaseEntity 全部方法：SEH 安全读 + NetVars 优先 / 硬编码兜底
//    2) IsDormant 空指针防线（虚表槽位校准前默认禁用，见下）
//    3) ClassName：MSVC x86 RTTI 链（vftable[-1] → COL → TypeDescriptor）
//    4) GetStormId：模型名分类 + zombieClass 反查兜底
//    5) EntityList 遍历：单槽直读（与 LiveEsp/LoopEntity 同源公式）
//    6) Snapshot::Publish：委托 LiveFrameBegin（已实战验证的直读链路）
//       + 旧类型 → 新类型转换 + 双缓冲发布
//
//  【过渡期桥接说明】
//  本文件 include 旧系统头（Include.h 全家桶）换取三个函数签名：
//    LiveFrameBegin / GetFrameSnapshot / GetEntityHitboxCloud /
//    GetEntityModelName / GetEntityBones / GetEntityWorldBounds /
//    IdentifierFromModelName / IdentifierFromZombieClass
//  这些是已实战验证的链路，剥离≠重写。迁移全部完成、旧系统删除后，
//  这批 include 与"过渡期桥接"区块整体清掉，Entity.cpp 只认 SDK/ 头。
//
//  【与旧 Offsets.h 的命名空间隔离】
//  旧 Source/Offsets.h 与本 SDK/Offsets.h 都是偏移表，若同名会发生
//  同一 TU 重定义（旧表 m_vecVelocity=0x100，新表=0x14C，数值也不同，
//  绝不能合并）。SDK 侧已改名为 namespace off —— Entity.cpp 里
//  off::xxx 是 SDK 表，Offsets::xxx 是旧表。
// ============================================================

#include "Entity.h"
#include "NetVars.h"
#include "Interfaces.h"

// ---- 过渡期桥接：旧系统头（迁移完成后整体删除）----
// 新旧同名类型在本文件并存，规则：
//   非限定名 EntityInfo / EntityState / HitboxCloud / Vec3 = 旧系统
//   sdk::EntityInfo / sdk::HitboxCloud / sdk::Vector      = 新 SDK
#include "../Include.h"
#include "../IdentifierSystem.h"
#include "../EntityBounds.h"
#include "../Netvar.h"
#include "../PayloadControl.h"   // NetClassDiag 诊断日志

#include <atomic>
#include <cstring>

namespace sdk
{
    // ============================================================
    //  内部工具
    // ============================================================

    // client.dll 基址（缓存一次；换图/重注入前进程不会卸载主模块）
    static uintptr_t ClientBase()
    {
        static uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        return base;
    }

    // ---- 新旧向量转换（旧 Vec3 是 OS-ImGui 类型，桥接用）----
    static sdk::Vector ToSdk(const Vec3& v) { return { v.x, v.y, v.z }; }
    static Vec3 ToOld(const sdk::Vector& v) { return Vec3(v.x, v.y, v.z); }

    // ============================================================
    //  网络类名分类（分类主路，见 Entity.h 的设计说明）
    // ============================================================

    // 分档诊断：每档只打一条（定位"网络类名为什么全失败"用，
    // 根因修掉后这些日志自然不再出现）
    static void NetClassDiag(const char* stage, uintptr_t v1 = 0, uintptr_t v2 = 0)
    {
        static bool s_done[10] = {};
        for (int i = 0; i < 10; ++i)
        {
            if (!s_done[i])
            {
                s_done[i] = true;
                PayloadLog("[netclass-diag] %s v1=0x%X v2=0x%X",
                           stage, (unsigned)v1, (unsigned)v2);
                return;
            }
        }
    }

    // [批36] 地图加载/切换检测
    bool IsGameLoading()
    {
        const uintptr_t client = (uintptr_t)::GetModuleHandleA("client.dll");
        if (!client)
            return true;
        uintptr_t lp = 0;
        if (!Mem::ReadBlock(client + off::dwLocalPlayer, &lp, sizeof(lp)))
            return true;
        return lp == 0;
    }

    bool GetNetClassName(uintptr_t ent, char* out, int cap)
    {
        if (IsGameLoading())                       // [批36] 加载期不碰实体
        {
            if (out && cap > 0) out[0] = 0;
            return false;
        }
        if (!ent || !out || cap <= 0) return false;
        out[0] = '\0';

        // IClientNetworkable 子对象虚表在实体 +8（C++ 多继承布局），
        // 其虚表槽 1 = GetClientClass（Storm 逆向验证，本版 client.dll 可信）。
        // ⚠ Mem::Read 只有值版：返回值才是读到的内容，不存在"引用写入"
        const uintptr_t vtable = Mem::Read<uintptr_t>(ent + 8);
        if (!vtable)
        {
            NetClassDiag("vtable-read-fail", ent);
            return false;
        }
        const uintptr_t fn = Mem::Read<uintptr_t>(vtable + sizeof(uintptr_t));
        if (!fn)
        {
            NetClassDiag("slot1-null", vtable);
            return false;
        }

        // 虚调用必须 SEH：刚析构的槽位调进来就是野指针
        ::netvar::ClientClassX* cc = nullptr;
        __try {
            cc = reinterpret_cast<::netvar::ClientClassX* (__thiscall*)(uintptr_t)>(fn)(ent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            NetClassDiag("seh-exception", fn);
            return false;
        }
        if (!cc)
        {
            NetClassDiag("cc-null", fn);
            return false;
        }

        // ClientClassX.m_pNetworkName @ +0x08（与 Storm 读 +0x14=ClassID 的
        // 布局互相吻合，本作已校准；不是 2013 SDK 的 +0x10）
        const char* name = Mem::Read<const char*>(
            reinterpret_cast<uintptr_t>(cc) + 0x08);
        if (!name)
        {
            NetClassDiag("name-ptr-null", (uintptr_t)cc);
            return false;
        }

        // 字符串校验：ASCII 可打印、长度合理。server class 名是引擎
        // 启动时注册的静态串，不该出现任何异常
        char buf[64];
        if (!Mem::ReadBlock(reinterpret_cast<uintptr_t>(name), buf, sizeof(buf) - 1))
        {
            NetClassDiag("name-read-fail", (uintptr_t)name);
            return false;
        }
        buf[sizeof(buf) - 1] = '\0';
        size_t len = std::strlen(buf);
        if (len == 0 || len >= 48)
        {
            NetClassDiag("name-bad-len", (uintptr_t)name, len);
            return false;
        }
        for (size_t i = 0; i < len; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(buf[i]);
            if (c < 0x20 || c > 0x7E)
            {
                // 打出前 8 字节现场，判断是偏移错还是校验过严
                NetClassDiag("name-garbage", (uintptr_t)name,
                             (uintptr_t)buf[0] | ((uintptr_t)buf[1] << 8)
                             | ((uintptr_t)buf[2] << 16) | ((uintptr_t)buf[3] << 24));
                return false;
            }
        }

        if (len >= static_cast<size_t>(cap)) len = static_cast<size_t>(cap) - 1;
        std::memcpy(out, buf, len);
        out[len] = '\0';
        return true;
    }

    bool NetClassNameToSid(const char* name, int& outSid)
    {
        if (!name || !name[0]) return false;
        // L4D2 server class 名（_stricmp，注册名跨版本稳定）。
        // 道具类第一版只放高置信的 TankRock；其余道具类（武器/药包/油桶…）
        // 等实机日志 [esp-net] 把类名列表跑出来再补，宁可落回原链也不猜。
        static const struct { const char* name; int sid; } kMap[] =
        {
            { "CTerrorPlayer", sid::kSurvivor },
            { "SurvivorBot",   sid::kSurvivor },
            { "Smoker",        sid::kSmoker },
            { "Boomer",        sid::kBoomer },
            { "Hunter",        sid::kHunter },
            { "Spitter",       sid::kSpitter },
            { "Jockey",        sid::kJockey },
            { "Charger",       sid::kCharger },
            { "Tank",          sid::kTank },
            { "Witch",         sid::kWitch },
            { "Infected",      sid::kInfected },
            { "TankRock",      -sid::kRock },   // 石头投掷物（自瞄排除项）
        };
        for (const auto& e : kMap)
        {
            if (_stricmp(name, e.name) == 0)
            {
                outSid = e.sid;
                return true;
            }
        }
        return false;
    }

    // ============================================================
    //  CBaseEntity
    // ============================================================

    const CBaseEntity CBaseEntity::Invalid{};

    CBaseEntity::CBaseEntity(int index, uintptr_t addr)
        : m_index(index), m_addr(addr)
    {
        if (m_index < 0)
        {
            m_index = -1;
            m_addr  = 0;
            return;
        }
        // 没给地址 → 按实体表单槽公式解析（与 LiveEsp::FillEntityTable
        // / LoopEntity::GetEntity 同源：槽基址 = clientBase + dwEntityList，
        // 槽位 = 基址 + index*0x10。dwEntityList 指向的地址已含 4096 实体偏移）
        if (!m_addr)
        {
            const uintptr_t base = ClientBase();
            if (base)
                m_addr = Mem::Read<uintptr_t>(
                    base + off::dwEntityList
                         + static_cast<uintptr_t>(m_index) * off::kEntityListStride);
        }
    }

    // CBaseEntity::Read —— 统一走 Mem::Read（SEH）
    template <typename T>
    T CBaseEntity::Read(uintptr_t rel, T fallback) const
    {
        if (!m_addr) return fallback;
        return Mem::Read<T>(m_addr + rel, fallback);
    }

    // ------------------------------------------------------------
    //  基础
    // ------------------------------------------------------------

    bool CBaseEntity::IsValid() const
    {
        // [批4 排查] 四重有效性防线：
        //   ① 索引范围（1..MAX_EDICTS 语义；本表 1024 槽，上界放宽到 edict 上限）
        //   ② 实体指针非空（m_addr）
        //   ③ vptr 落在 client.dll 内（活体客户端实体最便宜的证据）
        //   ④ Networkable 子对象指针（实体+8）非空 —— 拦截析构后残留的
        //      "幽灵实体"：vptr 残留但 Networkable 已释放的槽位
        if (m_index < 0 || m_index > 4095 || !m_addr)
            return false;
        // vptr 落在 client.dll 内 = "这是活着的客户端实体"最便宜的证据。
        // 正在析构的槽位 / 已释放指针在此被拦，后面任何读都安全得多。
        // （范围口径与 LiveEsp 的头部预筛一致）
        const uintptr_t base = ClientBase();
        if (!base) return false;
        const uintptr_t vptr = Mem::Read<uintptr_t>(m_addr);
        if (!(vptr >= base && vptr < base + 0x8000000u))
            return false;
        const uintptr_t netable = Mem::Read<uintptr_t>(m_addr + 8);
        return netable != 0;
    }

    const char* CBaseEntity::ClassName() const
    {
        // MSVC x86 RTTI 链（全部读走 SEH，任何一环失败给空串）：
        //   *(entity)            = vftable 指针
        //   vftable[-1]（-4 字节）= CompleteObjectLocator*
        //   COL + 0x0C           = TypeDescriptor 的 RVA（相对模块基址）
        //   TypeDescriptor + 0x08 = decorated name（".?AVCJockey@@"）
        // 返回的指针直接指向 client.dll 只读数据段，进程内长期有效。
        if (!IsValid()) return "";
        const uintptr_t base = ClientBase();
        const uintptr_t vptr = Mem::Read<uintptr_t>(m_addr);
        if (vptr < base + sizeof(uintptr_t) || vptr >= base + 0x8000000u)
            return "";
        const uintptr_t col = Mem::Read<uintptr_t>(vptr - sizeof(uintptr_t));
        if (col < base || col >= base + 0x8000000u)
            return "";
        const uint32_t tdRva = Mem::Read<uint32_t>(col + 0x0C);
        if (!tdRva) return "";
        // 首字节探测一下再返回指针，防 TypeDescriptor RVA 漂到不可读页
        const uintptr_t td = base + tdRva;
        if (Mem::Read<char>(td + 0x08) == '\0') return "";
        return reinterpret_cast<const char*>(td + 0x08);
    }

    bool CBaseEntity::IsPlayer() const
    {
        const char* n = ClassName();
        return n && std::strstr(n, "Player") != nullptr;   // .?AVCTerrorPlayer…
    }

    // ------------------------------------------------------------
    //  IsDormant —— 空指针防线之二（用户硬要求）
    // ------------------------------------------------------------
    //  休眠实体（服务器端超出 PVS）字段是陈旧数据，收进快照会画出
    //  "鬼影框"、打不准。判据 = IClientNetworkable 虚表上的 IsDormant。
    //
    //  ⚠ 该虚表槽位在本版本 client.dll 上【尚未校准】。槽位常量 = 0
    //  表示禁用（不拦），此时主防线是：
    //    ① IsValid() 的 vptr 范围校验（已验证）
    //    ② LiveEsp 收录的 lifeState/health 硬门槛（已验证）
    //  校准方法（做一次即可）：
    //    IDA 打开 client.dll → 找 CBaseEntity::IsDormant 的实现
    //    （读 m_bDormant 标志位返回）→ 看它在本类虚表的序号 →
    //    写进 kDormantVtableSlot。校准后本函数自动生效，无需改逻辑。
    // ------------------------------------------------------------
    static constexpr int kDormantVtableSlot = 0;   // 0 = 禁用（未校准）

    bool CBaseEntity::IsDormant() const
    {
        // 【语义澄清（批4 排查）】休眠 = 实体离开 PVS、网络数据冻结——
        //   指针仍有效，但 position/health 等字段是【最后一次可见时的旧值】。
        //   休眠 ≠ "当前不可见"，也 ≠ "不存在"。
        //   因此：休眠实体【不得参与瞄准点计算/选靶】（位置可能是旧的，
        //   打过去就是空气），但绘制层可用最后已知位置做半透明提示。
        // 休眠判据 = IClientNetworkable 虚表上的 IsDormant。
        // 槽位未校准（常量=0）时直接放行，主防线是
        //   ① IsValid() 的 vptr 范围校验（已验证）
        //   ② LiveEsp 收录的 lifeState/health 硬门槛（已验证）
        const int slot = kDormantVtableSlot;
        if (slot <= 0 || !m_addr)
            return false;
        // 虚表槽调用：x86 thiscall。任何一步读失败都按"休眠"处理
        // （保守拒绝），绝不把读不到的实体放进快照。
        const uintptr_t vptr = Mem::Read<uintptr_t>(m_addr);
        if (!vptr) return true;
        const uintptr_t fn = Mem::Read<uintptr_t>(vptr + slot * sizeof(uintptr_t));
        if (!fn) return true;
        bool dormant = false;
        __try {
            dormant = reinterpret_cast<bool(__thiscall*)(uintptr_t)>(fn)(m_addr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return true;   // 调用异常 = 实体状态不可信
        }
        return dormant;
    }

    // ------------------------------------------------------------
    //  基础字段（NetVars 优先、硬编码兜底、读失败给保守值）
    // ------------------------------------------------------------

    sdk::Vector CBaseEntity::GetOrigin() const
    {
        return Read<sdk::Vector>(off::m_vecOrigin, sdk::Vector{});
    }

    sdk::Vector CBaseEntity::GetVelocity() const
    {
        return Read<sdk::Vector>(off::m_vecVelocity, sdk::Vector{});
    }

    int CBaseEntity::GetHealth() const
    {
        return Read<int>(off::m_iHealth, 0);
    }

    int CBaseEntity::GetTeam() const
    {
        return Read<int>(off::m_iTeamNum, 0);
    }

    int CBaseEntity::GetLifeState() const
    {
        // ★ 失效过的字段：解析优先，兜底硬编码，再兜底"读不到=死亡"
        //（保守拒绝比误收安全——误收会把幽灵/尸体画进透视、打上目标）
        uintptr_t rel = NetVars::LifeState();
        if (!rel) rel = off::m_lifeState;
        return Read<int>(rel, 1);
    }

    bool CBaseEntity::IsGhost() const
    {
        uintptr_t rel = NetVars::IsGhost();
        if (!rel) rel = off::m_isGhost;
        return Read<uint8_t>(rel, 0) != 0;
    }

    sdk::Vector CBaseEntity::GetViewOffset() const
    {
        uintptr_t rel = NetVars::ViewOffset();
        if (!rel) rel = off::m_vecViewOffset;
        return Read<sdk::Vector>(rel, sdk::Vector{});
    }

    // ------------------------------------------------------------
    //  分类
    // ------------------------------------------------------------

    int CBaseEntity::GetStormId() const
    {
        if (!IsValid()) return -1;
        // 主路：网络类名（引擎官方 server class，不依赖 modelInfo）。
        // 本机 VModelInfoClient004 常态拿不到，模型名链路断 —— 这条
        // 主路是把"分类空转"救回来的关键（外部现成 ESP 的同款思路）。
        char netName[64];
        int nid = 0;
        if (GetNetClassName(m_addr, netName, sizeof(netName)) &&
            NetClassNameToSid(netName, nid))
            return nid;
        // 次路：模型名关键字（modelInfo 可用时才走得到）。
        // GetEntityModelName 与命中盒解析共用帧缓存，额外开销≈0。
        char name[64] = {};
        const Vec3 org = ToOld(GetOrigin());
        if (GetEntityModelName(m_index, org, name, sizeof(name)))
        {
            const int id = IdentifierFromModelName(name);
            if (id != -1)          // -1 = 未识别；0(Boomer)/负值(道具)都是合法结果
                return id;
        }
        // 兜底：zombieClass（1..8）→ Storm Identifier 反查表
        const int zc = GetZombieClass();
        if (zc > 0)
            return IdentifierFromZombieClass(zc);
        return -1;
    }

    int CBaseEntity::GetZombieClass() const
    {
        // zombieClass 未进 NetVars 解析表（仅感染者语义），硬编码直读
        return Read<int>(off::m_zombieClass, 0);
    }

    bool CBaseEntity::IsSurvivor() const { return GetStormId() == sid::kSurvivor; }
    bool CBaseEntity::IsTank()     const { return GetStormId() == sid::kTank; }
    bool CBaseEntity::IsWitch()    const { return GetStormId() == sid::kWitch; }

    bool CBaseEntity::IsSpecial() const
    {
        switch (GetStormId())
        {
        case sid::kBoomer: case sid::kHunter: case sid::kJockey:
        case sid::kSmoker: case sid::kSpitter: case sid::kCharger:
        case sid::kTank:   case sid::kWitch:
            return true;
        default:
            return false;
        }
    }

    bool CBaseEntity::IsProp() const
    {
        // 道具 Identifier 为负（-73 石头 / -105 肾上腺素 / -109 除颤器 …）
        // -1 是"未识别"，不算道具
        return GetStormId() <= -2;
    }

    // ------------------------------------------------------------
    //  几何（委托现有系统 + 新旧类型转换）
    // ------------------------------------------------------------

    bool CBaseEntity::GetHitboxCloud(HitboxCloud& out) const
    {
        out.count = 0;
        if (!IsValid()) return false;
        if (IsDormant()) return false;      // 休眠实体的骨骼/命中盒是陈旧帧

        // 旧类型（kMaxBoxes=32）→ 新类型（kMaxBoxes=24），限定名区分
        ::HitboxCloud old{};
        const Vec3 org = ToOld(GetOrigin());
        if (!GetEntityHitboxCloud(m_index, org, old) || old.count <= 0)
            return false;

        const int n = old.count < HitboxCloud::kMaxBoxes
                    ? old.count : HitboxCloud::kMaxBoxes;
        for (int i = 0; i < n; ++i)
        {
            const ::HitboxOBB& b = old.box[i];
            Hitbox& o = out.box[i];
            o.ok     = b.ok;
            o.index  = i;
            o.group  = b.group;     // [批5] HITGROUP_* 透传——打击部位判据
            o.center = ToSdk(b.center);
            o.min    = ToSdk(b.corner[0]);
            o.max    = ToSdk(b.corner[7]);
            // OBB 没有统一半径，给 center→corner 的长度供包围球近似
            const sdk::Vector d = ToSdk(b.corner[0]) - o.center;
            o.radius = d.Length();
        }
        out.count = n;
        return true;
    }

    bool CBaseEntity::GetBoneCloud(BoneCloud& out) const
    {
        out.count = 0;
        if (!IsValid()) return false;
        if (IsDormant()) return false;

        ::BoneSnapshot old{};
        const Vec3 org = ToOld(GetOrigin());
        if (!GetEntityBones(m_index, org, old) || !old.ok)
            return false;

        // ValveBiped 实际 38 骨（Bone_Count_），新云容量 128 富余
        static_assert(Bone_Count_ <= BoneCloud::kMaxBones,
                      "L4D2 骨骼数超出 BoneCloud 容量");
        for (int i = 0; i < Bone_Count_; ++i)
        {
            Bone& b = out.bone[i];
            b.ok    = old.valid[i];
            b.index = i;
            b.pos   = ToSdk(old.pos[i]);
        }
        out.count = Bone_Count_;
        return true;
    }

    bool CBaseEntity::GetWorldBounds(sdk::Vector& mn, sdk::Vector& mx) const
    {
        if (!IsValid()) return false;
        // 旧接口要一份旧 EntityInfo 做防呆（origin 基准 / 体型分类）
        ::EntityInfo info{};
        info.position    = ToOld(GetOrigin());
        info.team        = GetTeam();
        info.health      = GetHealth();
        info.zombieClass = GetZombieClass();
        info.index       = m_index;
        Vec3 omin, omax;
        if (!GetEntityWorldBounds(m_addr, info, omin, omax))
            return false;
        mn = ToSdk(omin);
        mx = ToSdk(omax);
        return true;
    }

    // ------------------------------------------------------------
    //  武器
    // ------------------------------------------------------------

    uintptr_t CBaseEntity::GetActiveWeapon() const
    {
        if (!IsValid()) return 0;
        uintptr_t rel = NetVars::ActiveWeapon();
        if (!rel) rel = off::m_hActiveWeapon;
        // m_hActiveWeapon 是 EHANDLE：低 13 位实体索引 + 高位序号
        const uint32_t handle = Read<uint32_t>(rel, 0xFFFFFFFFu);
        if (handle == 0xFFFFFFFFu || handle == 0)
            return 0;
        const int idx = static_cast<int>(handle & 0x1FFF);
        if (idx < 0 || idx >= off::kMaxEntities)
            return 0;
        const CBaseEntity weapon(idx);
        return weapon.IsValid() ? weapon.Address() : 0;
    }

    int CBaseEntity::GetWeaponId() const
    {
        const uintptr_t w = GetActiveWeapon();
        if (!w) return 0;
        // m_iWeaponId 只在 NetVars 解析表里（DT_TerrorWeapon），无硬编码兜底
        const uintptr_t rel = NetVars::WeaponId();
        if (!rel) return 0;
        return Mem::Read<int>(w + rel, 0);
    }

    int CBaseEntity::GetActiveWeaponClip() const
    {
        // 偏移没解析到 → 未知。绝不能返回 0：调用方会把"读不到"
        // 当成"确定没有弹匣"，从而在药品保护里误判。
        const uintptr_t rel = NetVars::Clip1();
        if (!rel) return kClipUnknown;

        const uintptr_t w = GetActiveWeapon();
        if (!w) return kClipUnknown;

        // sanity：真实弹匣数不可能是极端值。读出离谱数字说明偏移错了
        // （撞到别的字段），此时同样按未知处理，不参与判断。
        const int clip = Mem::Read<int>(w + rel, kClipUnknown);
        if (clip < -1 || clip > 5000) return kClipUnknown;
        return clip;
    }

    // ------------------------------------------------------------
    //  眼位 / 枪位（自瞄与 ESP 共用口径）
    // ------------------------------------------------------------

    sdk::Vector CBaseEntity::GetEyePos() const
    {
        return GetOrigin() + GetViewOffset();
    }

    sdk::Vector CBaseEntity::GetShootPos(const sdk::QAngle& /*viewAngles*/) const
    {
        // 当前版本与眼位同口径。punchAngle 后坐力补偿由 Aimbot 层按
        // g_Config.aim.recoilComp 决定加不加 —— SDK 层不做功能决策
        //（分层铁律：本层没有开关判断）。
        return GetEyePos();
    }

    // ------------------------------------------------------------
    //  生存救援优先级
    // ------------------------------------------------------------

    bool CBaseEntity::IsBeingChased() const
    {
        if (!IsValid()) return false;
        uintptr_t rel = NetVars::MyRecipient();
        if (!rel) rel = off::m_hMyRecipient;
        return Read<int32_t>(rel, -1) != -1;
    }

    bool CBaseEntity::IsBeingControlled() const
    {
        if (!IsValid()) return false;
        // m_nClass 只在 NetVars 解析表里；解析失败 → 恒 false（诚实降级，
        // 不猜偏移——猜错的后果是把普通幸存者当"被控制"乱救）
        const uintptr_t rel = NetVars::ClassId();
        if (!rel) return false;
        return Read<int>(rel, 0) != 0;
    }

    // ============================================================
    //  EntityList
    // ============================================================

    namespace EntityList
    {
        bool Init()
        {
            const uintptr_t base = ClientBase();
            if (!base) return false;
            // 槽基址页必须可读（单参值版读一次即可，值无关）
            return Mem::Read<uintptr_t>(base + off::dwEntityList) != 0;
        }

        void ForEach(const std::function<bool(const CBaseEntity&)>& visit)
        {
            if (!visit) return;
            for (int i = 0; i < off::kMaxEntities; ++i)
            {
                const CBaseEntity e(i);
                // 【空指针防线】槽为空 / vptr 越界 / 休眠 一律跳过
                if (!e.IsValid())  continue;
                if (e.IsDormant()) continue;
                if (!visit(e)) break;   // 调用方要求提前终止
            }
        }

        CBaseEntity Get(int index)
        {
            if (index < 0 || index >= off::kMaxEntities)
                return CBaseEntity::Invalid;
            const CBaseEntity e(index);
            return e.IsValid() ? e : CBaseEntity::Invalid;
        }

        CBaseEntity LocalPlayer()
        {
            // [修正] 旧实现 `CBaseEntity e(LocalPlayerAddr())` 把【地址】
            // 当成【index】传参（uintptr_t → int 隐式截断，addr=0）→
            // IsValid 的 !m_addr 检查恒 false → 永远返回 Invalid。
            // 修正：指针取自 Globals（LiveEsp/LoopEntity 每帧直读维护），
            // 表槽位 localPlayerIndex 与本表同源（同一 dwEntityList 槽位
            // 公式），两者齐备才构造。
            if (!localPlayer || localPlayerIndex < 0)
                return CBaseEntity::Invalid;
            const CBaseEntity e(localPlayerIndex, localPlayer);
            return e.IsValid() ? e : CBaseEntity::Invalid;
        }

        uintptr_t LocalPlayerAddr()
        {
            const uintptr_t base = ClientBase();
            if (!base) return 0;
            // 每次直读（换图自动归零，不缓存陈旧指针）
            return Mem::Read<uintptr_t>(base + off::dwLocalPlayer);
        }

        int MaxEntities() { return off::kMaxEntities; }
    }

    // ============================================================
    //  Snapshot —— 渲染线程发布 / 游戏线程消费（双缓冲，零锁读）
    // ------------------------------------------------------------
    //  Publish（渲染帧）：LiveFrameBegin 直读 → 转换 → 写后备缓冲 → 翻面
    //  Get（游戏 tick）： 读当前正面，永不与写半程重叠
    //  游戏线程最多读到"上一帧"的快照（一帧延迟，语义等同旧 AimBridge
    //  的跨线程候选通道，但分类同源——这是本次重构的核心收益）。
    // ============================================================

    namespace Snapshot
    {
        static std::vector<EntityPair> g_buf[2];
        static std::atomic<int>        g_front{ 0 };
        static std::atomic<DWORD>      g_lastPublishTick{ 0 };

        void Publish()
        {
            // 权威直读链路（旧系统，已实战验证：快照里所有实体都出自它）
            LiveFrameBegin();

            std::vector<EntityPair> tmp;
            {
                // 旧快照读取持锁（g_entitiesMutex 的契约：跨线程读自行加锁）
                std::lock_guard<std::mutex> lock(g_entitiesMutex);
                const auto& old = GetFrameSnapshot();
                tmp.reserve(old.size());
                for (const auto& pr : old)
                {
                    const ::EntityInfo&  oi = pr.first;
                    const ::EntityState& os = pr.second;

                    sdk::EntityInfo ni;
                    ni.position   = ToSdk(oi.position);
                    ni.velocity   = ToSdk(oi.velocity);
                    ni.team       = oi.team;
                    ni.health     = os.health;
                    ni.index      = oi.index;
                    ni.stormId    = oi.stormId;
                    ni.isSurvivor = (oi.stormId == sid::kSurvivor);
                    ni.isProp     = (oi.stormId <= -2);
                    ni.isSpecial  = false;
                    switch (oi.stormId)
                    {
                    case sid::kBoomer: case sid::kHunter: case sid::kJockey:
                    case sid::kSmoker: case sid::kSpitter: case sid::kCharger:
                    case sid::kTank:   case sid::kWitch:
                        ni.isSpecial = true;
                        break;
                    default:
                        break;
                    }

                    sdk::EntityState ns;
                    ns.lifeState = os.lifeState;
                    ns.isGhost   = os.isGhost;
                    ns.valid     = true;

                    tmp.emplace_back(ni, ns);
                }
            }

            const int back = g_front.load(std::memory_order_relaxed) ^ 1;
            g_buf[back] = std::move(tmp);
            g_front.store(back, std::memory_order_release);
            g_lastPublishTick.store(GetTickCount(), std::memory_order_relaxed);
        }

        const std::vector<EntityPair>& Get()
        {
            return g_buf[g_front.load(std::memory_order_acquire)];
        }

        bool IsFresh()
        {
            const DWORD t = g_lastPublishTick.load(std::memory_order_relaxed);
            return t != 0 && GetTickCount() - t < 1000;   // 1 秒内的快照算新鲜
        }

        void Reset()
        {
            g_buf[0].clear();
            g_buf[1].clear();
            g_lastPublishTick.store(0, std::memory_order_relaxed);
        }
    }
} // namespace sdk

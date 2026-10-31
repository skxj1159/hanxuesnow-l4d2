#include "Include.h"

#include "HitboxSystem.h"     // GetEntityModelName（Storm 分类的权威模型名来源）
#include "IdentifierSystem.h" // StormIdentify（唯一的实体分类来源）
#include "PayloadControl.h"   // PayloadLog（诊断日志）
#include "Netvar.h"           // 判活三件套偏移解析（m_usSolidFlags/m_nSequence）
#include "SDK/Entity.h"       // sdk::GetNetClassName / NetClassNameToSid（分类主路）

#include <cstring>
#include <string>
#include <unordered_set>

// LoopEntity.cpp 定义（LiveFrameBegin 与扫描线程共用同一存储：
// 后台线程降级为指针表维护，快照唯一权威写入方 = 渲染线程）
extern std::mutex g_entitiesMutex;
extern std::vector<std::pair<EntityInfo, EntityState>> g_entities;

namespace
{
    // 实体块整读尺寸与降级（与 LoopEntity 同口径）
    // [批172] actor 块读 0x2000 → 0x800：0x800 覆盖全部热字段
    //   （m_vecOrigin 0x124 / m_vecVelocity 0x100 / m_iHealth 0xEC /
    //     m_lifeState 0x147 / m_nModelIndex …）；0x1C90/0x1C9A/0x1084/7308
    //   等尾部字段由 TailInt/TailByte 的单点补读兜底（原有机制）。
    //   65 实体 × 60fps：8KB→0.8KB，内存拷贝从 ~31MB/s 降到 ~3MB/s。
    constexpr size_t kEntityBlockSize = 0x800;
    constexpr size_t kBlockFallbacks[] = { 0x400, 0x200 };

    // 道具（WorldItem / Tank 投掷石）只需读头部一小块：
    // m_vecOrigin=0x124 / m_iHealth=0xEC / m_lifeState=0x147 都在 0x200 内，
    // 而 m_isGhost=0x1C9A 对道具无意义，直接填 false。
    constexpr size_t kPropBlockSize = 0x200;

    // 实体指针表（1024 槽 × 0x10）
    constexpr int    kMaxEntitySlots    = 1024;
    constexpr size_t kEntityTableStride = 0x10;
    constexpr size_t kEntityTableBytes  = kMaxEntitySlots * kEntityTableStride;

    // 渲染线程工作缓冲（EndScene 单线程访问，静态零分配）
    uint8_t  g_table[kEntityTableBytes];
    uint8_t  g_block[kEntityBlockSize];

    // ---- 进程内安全读（SEH 兜底：实体帧间释放/未提交页不崩）----
    inline bool SafeCopy(void* dst, const void* src, size_t n)
    {
        __try
        {
            std::memcpy(dst, src, n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    template <typename T>
    inline T BlockField(const uint8_t* block, uintptr_t offset)
    {
        T v{};
        std::memcpy(&v, block + offset, sizeof(T));
        return v;
    }

    // 实体表填充：整块读最快，但整块失败（数组尾部跨未提交页）时不能直接
    // 降级成"只读前 N 字节"——那样 index 超过 N/16 的实体会整片消失，
    // 表现为特感（常落在高槽位）进得了快照却没有框。
    // 这里按 4KB 分块读，失败的块再逐槽单点读，保证 1024 槽全覆盖。
    void FillEntityTable(uintptr_t listBase)
    {
        constexpr size_t kChunk = 0x1000;
        for (size_t off = 0; off < kEntityTableBytes; off += kChunk)
        {
            const size_t n = (kEntityTableBytes - off < kChunk)
                           ? (kEntityTableBytes - off) : kChunk;
            if (SafeCopy(g_table + off, reinterpret_cast<const void*>(listBase + off), n))
                continue;

            for (size_t s = 0; s + kEntityTableStride <= n; s += kEntityTableStride)
            {
                uintptr_t ent = 0;
                if (!SafeCopy(&ent, reinterpret_cast<const void*>(listBase + off + s), sizeof(ent)))
                    ent = 0;         // 该槽确实读不到：清零，不沿用上一帧的陈旧指针
                std::memcpy(g_table + off + s, &ent, sizeof(ent));
            }
        }
    }

    // ---- Storm Identifier → 我们的 zombieClass（自瞄沿用的 1..8 语义）----
    // 注意与 Storm 的 Identifier 数值不同：Boomer 是 sid 0 但 zombieClass 2，
    // Charger 是 sid 99 但 zombieClass 6。女巫/小僵尸/幸存者统一给 0
    // （IsTank 用 zombieClass==7 && hp>1000 区分，女巫给 0 更安全）。
    int ZombieClassFromSid(int id)
    {
        switch (id)
        {
        case sid::Smoker:    return 1;
        case sid::Boomer:    return 2;
        case sid::Hunter:    return 3;
        case sid::Spitter:   return 4;
        case sid::Jockey:    return 5;
        case sid::Charger:   return 6;
        case sid::Tank:      return 8;
        case sid::Witch:     return 0;   // C_Witch 无 zombieClass 语义
        case sid::Infected:  return 0;   // 普通小僵尸
        case sid::Survivor:  return 0;
        default:             return 0;
        }
    }

    // 装备实体没有"存活/血量"语义，统一给固定占位，避免下游按血量算比例
    const int kPropHealth = 1;
}

// ============================================================
//  LiveFrameBegin：渲染帧实体直读（唯一权威快照写入方）
// ------------------------------------------------------------
//  2026-10-18 分类链路换成 Storm 的 Get_Identifier 语义
//  （见 IdentifierSystem.cpp）：模型名 → Storm Identifier，
//  幸存者被控制成特感时再按 m_zombieClass 反查一次。
//
//  删掉的三套旧判据（都是为绕开虚表 RVA 漂移而加的补丁，
//  分类不再依赖 vptr 后全部失去意义）：
//   ① IsWitchByVptr / IsCommonByVptr / SpecialClassFromVptr（7 个虚表 RVA）
//   ② SpecialClassFromModelName（与 IdentifierSystem 重复的一套关键字表）
//   ③ 类号防抖缓存 s_clsCache + 结构体兜底 playerStruct/plausibleSpecial
//      （原本用来救 ① 漏掉的特感；现在分类由模型名+zombieClass 决定，
//        且下面保留了 lifeState/health 硬门槛，误收武器投掷物的路径已断）
// ============================================================
// [批146] 读取实体的 vtable（客户端类的虚表指针）。
//   __try 必须放在【没有析构函数】的函数里，否则 MSVC 报 C2712
//   （"无法在要求对象展开的函数中使用 __try"）—— 这是本项目的既有约定。
void* ReadEntityVtable(uintptr_t ent)
{
    void* vt = nullptr;
    __try { vt = *(void**)ent; }
    __except (EXCEPTION_EXECUTE_HANDLER) { vt = nullptr; }
    return vt;
}

void LiveFrameBegin()
{
    // ---- HitboxSystem 初始化（懒，一次性）----
    // ⚠ 自瞄体系拆除（AimCore2.cpp）时把 HitboxInit 的调用点一起删了，
    //   导致 modelInfo 恒 null → 命中盒云 + 模型名链全灭（[esp-model] 全 <none>
    //   的真正根因）。这里补回：VModelInfoClient004（磁盘 client.dll 静态确认存在）
    //   + netvar 解析 m_nModelIndex。
    static bool s_hitboxInit = false;
    if (!s_hitboxInit)
    {
        s_hitboxInit = true;
        // ⚠ VModelInfoClient 是【引擎接口】，注册在 engine.dll（Source 惯例），
        //   从 client.dll 拿永远 null —— 磁盘扫描确认两个模块都有该名字符串，
        //   但注册表只在 engine。
        HMODULE engine = GetModuleHandleA("engine.dll");
        if (engine)
        {
            void* modelInfo = netvar::GetInterface(engine, "VModelInfoClient004");
            uintptr_t modelIdxOff = 0;
            if (netvar::ClientClassX* head = netvar::GetAllClasses())
                modelIdxOff = netvar::OffsetInClass(head, "CBasePlayer", "m_nModelIndex");
            HitboxInit(modelInfo, modelIdxOff);
            PayloadLog("[hitbox-init] modelInfo=%p modelIndex=0x%X (from engine.dll)",
                       modelInfo, (unsigned)modelIdxOff);
        }
    }

    // 本地玩家指针：每帧直读（换图自动归零）
    uintptr_t localPtr = 0;
    if (clientBase)
        SafeCopy(&localPtr, reinterpret_cast<const void*>(
                                clientBase + Offsets::dwLocalPlayer),
                 sizeof(localPtr));
    if (!localPtr)
    {
        localPlayer = 0;
        localPlayerIndex = -1;
        std::lock_guard<std::mutex> lock(g_entitiesMutex);
        g_entities.clear();
        return;
    }
    if (!localPlayer)
    {
        localPlayer = localPtr;
        PayloadLog("[live] 本地玩家已获取：0x%llX（渲染帧直读链路）",
            (unsigned long long)localPtr);
    }
    localPlayer = localPtr;

    // 本地玩家位置：Storm 的距离排序/道具距离截断都要用
    Vec3 localPos{};
    {
        Vec3 p{};
        if (SafeCopy(&p, reinterpret_cast<const void*>(localPlayer + Offsets::m_vecOrigin),
                     sizeof(p)) && isfinite(p.x) && isfinite(p.y) && isfinite(p.z))
            localPos = p;
    }

    // 实体指针表：分块拷贝（失败块逐槽兜底），保证高槽位实体不被整表降级抹掉
    FillEntityTable(clientBase + Offsets::dwEntityList);

    std::vector<std::pair<EntityInfo, EntityState>> snapshot;
    snapshot.reserve(64);

    static unsigned char s_diagSeen[256] = {};   // 未收录诊断去重位图（2048 位）
    static int           s_tblNonZero   = 0;     // 实体表非零槽位数
    static int           s_vptrInMod    = 0;     // vptr 落在 client.dll 内的实体数
    static int           s_teamOkCount  = 0;     // 阵营 2/3 的实体数
    static int           s_propCount    = 0;     // 收录进来的场景道具数
    s_tblNonZero = 0;
    s_vptrInMod  = 0;
    s_teamOkCount = 0;
    s_propCount   = 0;

    for (int i = 0; i < kMaxEntitySlots; ++i)
    {
        uintptr_t ent = 0;
        std::memcpy(&ent, g_table + (size_t)i * kEntityTableStride, sizeof(ent));
        if (!ent) continue;
        ++s_tblNonZero;
        if (ent == localPlayer)
        {
            localPlayerIndex = i;   // 自瞄桥接 / 目标索引校验使用
            continue;
        }

        // ---- 头部预筛（0x100 直读）：vptr 可信 + 阵营 2/3 才值得整读 ----
        // vptr 落在 client.dll 内是"这确实是个客户端实体"最便宜的证据；
        // 正在析构的槽位 / 已释放的指针会在此被拦掉（后面 GetModel 也会失败）。
        uint8_t head[0x100];
        if (!SafeCopy(head, reinterpret_cast<const void*>(ent), sizeof(head)))
            continue;

        uintptr_t vptr = 0;
        std::memcpy(&vptr, head, sizeof(vptr));
        if (vptr < clientBase || vptr >= clientBase + 0x8000000u)
            continue;   // vptr 不在 client.dll 内：非游戏实体（含正在析构的槽位）
        ++s_vptrInMod;

        int team = 0;
        std::memcpy(&team, head + Offsets::m_iTeamNum, sizeof(team));

        // 眼位偏移（0xF4..0x100 正好落在已读的 0x100 头部内，不必额外读）
        Vec3 viewOff;
        std::memcpy(&viewOff, head + Offsets::m_vecViewOffset, sizeof(viewOff));
        (void)viewOff;   // 旧结构体兜底判据已删，保留读取以维持头部预读语义

        const bool isActor = (team == 2 || team == 3);
        if (isActor) ++s_teamOkCount;

        // ---- 整块读取 ----
        // 玩家读 0x2000（m_zombieClass=0x1C90 / m_isGhost=0x1C9A 在尾部）；
        // 阵营 0/1 的实体只可能是场景道具，读 0x200 就够，省掉整帧开销。
        const size_t want = isActor ? kEntityBlockSize : kPropBlockSize;
        size_t got = 0;
        if (SafeCopy(g_block, reinterpret_cast<const void*>(ent), want))
        {
            got = want;
        }
        else if (isActor)
        {
            for (size_t sz : kBlockFallbacks)
            {
                std::memset(g_block, 0, kEntityBlockSize);
                if (SafeCopy(g_block, reinterpret_cast<const void*>(ent), sz))
                {
                    got = sz;
                    break;
                }
            }
        }
        if (!got)
            continue;   // 实体正在被释放：本帧跳过

        EntityInfo info;
        info.position     = BlockField<Vec3>(g_block, Offsets::m_vecOrigin);
        info.velocity     = BlockField<Vec3>(g_block, Offsets::m_vecVelocity);
        info.team         = team;
        info.health       = BlockField<int>(g_block, Offsets::m_iHealth);
        info.index        = i;
        info.type         = EntityType::Unknown;

        // ---- 尾部字段：超出实际读到的字节数时单点补读 ----
        // 0x2000 整块读取一旦降级（实体跨未提交页 → 退到 0x1000/0x800），
        // m_zombieClass(0x1C90) / m_isGhost(0x1C9A) 全为 0：特感会被
        // 分类成小僵尸。这两个字段因此单独兜底读一次。
        const auto TailInt = [&](uintptr_t off) -> int
        {
            int v = 0;
            if (got >= off + sizeof(int))
                v = BlockField<int>(g_block, off);
            else
                SafeCopy(&v, reinterpret_cast<const void*>(ent + off), sizeof(int));
            return v;
        };
        const auto TailByte = [&](uintptr_t off) -> BYTE
        {
            BYTE v = 0;
            if (got >= off + 1)
                v = BlockField<BYTE>(g_block, off);
            else
                SafeCopy(&v, reinterpret_cast<const void*>(ent + off), 1);
            return v;
        };

        // ---- 判活三件套偏移（一次性解析；外部现成 ESP 的判活思路）----
        // m_usSolidFlags(DT_BaseEntity) / m_nSequence(DT_BaseAnimating)。
        // 第一版只采数：四元组日志落 payload.log，实机跑一次看死人/
        // 幽灵时各字段的实际值，再定拦截规则 —— 不拍脑袋写规则。
        static uintptr_t s_sfOff = 0, s_seqOff = 0;
        static bool s_aliveProbe = false;
        if (!s_aliveProbe)
        {
            s_aliveProbe = true;
            netvar::ClientClassX* head = netvar::GetAllClasses();
            if (head)
            {
                s_sfOff  = netvar::OffsetInTable(head, "DT_BaseEntity", "m_usSolidFlags");
                s_seqOff = netvar::OffsetInTable(head, "DT_BaseAnimating", "m_nSequence");
                PayloadLog("[alive-probe] m_usSolidFlags=0x%X m_nSequence=0x%X",
                           (unsigned)s_sfOff, (unsigned)s_seqOff);
            }
        }

        if (!isfinite(info.position.x) || !isfinite(info.position.y) ||
            !isfinite(info.position.z) ||
            (fabsf(info.position.x) > 1e7f ||
             fabsf(info.position.y) > 1e7f ||
             fabsf(info.position.z) > 1e7f))
            continue;   // 位置是垃圾值（未初始化 / 半写状态）：不是可渲染实体

        // ---- 模型名（仅诊断需要时懒取）----
        // [批172] GetEntityModelName 要走 IVModelInfoClient 接口调用，
        //   原来每实体每帧都取（65 实体 = ~3900 次接口调用/秒），
        //   而它只服务两条"每类只打一次"的诊断日志 —— 改为懒取。
        char modelName[64] = {};
        bool  modelNameOk = false;
        const auto FetchModelName = [&]() -> bool
        {
            if (!modelNameOk)
                modelNameOk = GetEntityModelName(i, info.position, modelName,
                                                 (int)sizeof(modelName));
            return modelNameOk;
        };

        const int rawClass = isActor ? TailInt(Offsets::m_zombieClass) : 0;

        // ---- 分类唯一权威：网络类名白名单（unknowncheats 共识做法）----
        // 只收录类名命中白名单（CTerrorPlayer/SurvivorBot/Infected/特感 7 类/
        // Witch/TankRock）的实体；其余（投掷物 MolotovProjectile/PipeBombProjectile、
        // 机关、基类 CBaseCombatCharacter、mod 实体）一律不收录。
        //
        // ⚠ 不再落回 zombieClass 反查 / StormIdentify：投掷物的 0x1C90 字段
        //   是垃圾值，反查会被误判成特感 —— 这正是"特感框画在投掷物上 /
        //   画在地图外"的根因。分类错 = 宁可不画。
        // [批172] 分类缓存：网络类名→sid 在实体生命周期内不变。
        //   键 =（索引, vtable）：vtable 变了（索引被另一类实体复用）才重新解析。
        //   命中时省掉 GetNetClassName 的 vtable 读 + 引擎 vcall + 字符串校验
        //   （65 实体 × 60fps ≈ 3900 次引擎调用/秒 → ~65 次/秒）。
        static uintptr_t s_sidVt[kMaxEntitySlots]  = {};
        static int       s_sidId[kMaxEntitySlots]  = {};   // -2=未缓存 -1=白名单外
        struct SidCacheInit { SidCacheInit() { for (int k = 0; k < kMaxEntitySlots; ++k) s_sidId[k] = -2; } };
        static SidCacheInit s_sidCacheInit;

        char netName[64] = {};
        int  sidId = -2;
        uintptr_t entVt = 0;
        if (!SafeCopy(&entVt, reinterpret_cast<const void*>(ent), sizeof(entVt)))
            entVt = 0;
        if (entVt != 0 && i >= 0 && i < kMaxEntitySlots &&
            s_sidVt[i] == entVt && s_sidId[i] != -2)
        {
            sidId = s_sidId[i];                 // 缓存命中（含白名单外 -1）
            // [批173·修复] 缓存命中也必须检查白名单结果！
            //   批172 只在"缓存未命中"分支里 continue，导致白名单外的实体
            //   （投掷物/武器/机关…）命中缓存后照样被收录 → 快照从 65 暴涨
            //   到 745 → 取骨在垃圾实体上反复失败 → 熔断器常驻 → 骨骼全灭
            //   （实测 16232 行 bone-guard 熔断日志）。
            if (sidId < 0)
                continue;
        }
        else
        {
            const bool ok = sdk::GetNetClassName(ent, netName, (int)sizeof(netName));
            sidId = -1;
            if (ok && sdk::NetClassNameToSid(netName, sidId))
            {
                if (entVt != 0 && i >= 0 && i < kMaxEntitySlots)
                {
                    s_sidVt[i] = entVt;
                    s_sidId[i] = sidId;
                }
            }
            else
            {
                if (entVt != 0 && i >= 0 && i < kMaxEntitySlots)
                {
                    s_sidVt[i] = entVt;
                    s_sidId[i] = -1;            // 缓存负结果：不再重复解析
                }
                // 白名单外：一次性记录类名（扩白名单用），然后静默丢弃
                if (ok)
                {
                    static std::unordered_set<std::string> s_loggedNet;
                    if (s_loggedNet.size() < 64 && s_loggedNet.insert(netName).second)
                        PayloadLog("[esp-net] 白名单外类名 \"%s\"（不收录）", netName);
                }
                continue;
            }
        }
        const bool netNameOk = true;             // 走到这里必然有 sid

        const bool isProp = (sidId < 0);
        const int  sidAbs = isProp ? -sidId : sidId;

        // [批146] 发布普通感染者的实体 vtable。
        //   游戏线程的射线过滤器据此把"小僵尸"当作透明 —— 它们挤在特感
        //   前面时会把特感的可见性判定挡掉（"僵尸明明在掩体外却不瞄准"）。
        //   vtable 属于客户端类，所有普通感染者共用同一个指针。
        if (sidAbs == sdk::sid::kInfected && g_commonInfectedVtable == nullptr)
        {
            if (void* vt = ReadEntityVtable(ent))
                g_commonInfectedVtable = vt;
        }

        // ---- 模型名诊断（一次性打表，便于确认"某类不显示"卡在哪一环）----
        if (isActor)
        {
            static std::unordered_set<std::string> s_loggedNames;
            // [批172] 模型名懒取：只在"可能打新日志"时才走引擎接口
            if (s_loggedNames.size() < 64)
            {
                FetchModelName();
                const std::string key = modelNameOk ? std::string(modelName)
                                                    : std::string("<none>");
                if (s_loggedNames.insert(key).second)
                {
                    PayloadLog("[esp-model] %-46s → sid=%d class=%d 血量=%d",
                               key.c_str(), sidId, rawClass, info.health);
                }
            }
        }

        if (isProp)
        {
            // 道具：team 统一改写成 1（与 Storm 一致，下游一看就知道不是玩家），
            // 血量给固定占位，幽灵态为 false。距离截断在 DrawEsp 里做。
            ++s_propCount;
            info.team       = 1;
            info.health     = kPropHealth;
            info.zombieClass = 0;
            info.velocity   = Vec3(0.0f, 0.0f, 0.0f);
            info.stormId    = sidId;          // 负值 = 道具（自瞄侧据此排除）

            EntityState st;
            st.health    = kPropHealth;
            st.lifeState = 0;
            st.isGhost   = false;
            snapshot.push_back({ info, st });
            continue;
        }

        // ---- 玩家：把 Storm Identifier 翻译回我们的 type / zombieClass ----
        info.stormId   = sidId;
        info.zombieClass = ZombieClassFromSid(sidAbs);

        // 幸存者角色 ID：bot 的 GetPlayerInfo name 为空（L4D2 bot 不写
        // player_info），角色名全靠这个 ID 查表（Coach/Rochelle/Ellis…）。
        // netvar 动态解析（多候选名）优先，硬编码 7308 兜底——与 lifeState
        // 同一策略：易漂移字段不裸奔。
        if (sidAbs == sid::Survivor)
        {
            static uintptr_t s_svidOff = 0;
            static bool s_svidDone = false;
            if (!s_svidDone)
            {
                s_svidDone = true;
                if (netvar::ClientClassX* head = netvar::GetAllClasses())
                {
                    static const char* kSvidNames[] = {
                        "m_survivorCharacter", "m_nSurvivorCharacter",
                        "m_characterType", "m_nCharacterType",
                    };
                    for (const char* n : kSvidNames)
                    {
                        s_svidOff = netvar::OffsetInTable(head, "DT_TerrorPlayer", n);
                        if (s_svidOff) break;
                    }
                }
                PayloadLog("[survivor-id] netvar 偏移=0x%X（0 = 解析失败，用硬编码 7308）",
                           (unsigned)s_svidOff);
            }
            const uintptr_t off = s_svidOff ? s_svidOff : 7308;
            int v = TailInt(off);
            if (v < 0 || v > 7)
            {
                // 越界（未同步/mod）：试一次硬编码兜底
                v = TailInt(7308);
                if (v < 0 || v > 7) v = -1;
            }
            info.survivorId = v;
        }

        if (sidAbs == sid::Witch)
            info.type = EntityType::Witch;
        else if (sidAbs == sid::Infected)
            info.type = EntityType::Common;

        // 被控制成特感的幸存者：模型名仍是 survivor，但 zombieClass 变了。
        // StormIdentify 内部已按 zombieClass 反查过一次（sid 会变成对应特感），
        // 上面 ZombieClassFromSid 因此能拿到正确的 1..6/8。
        if (rawClass >= 1 && rawClass <= 8)
            info.zombieClass = rawClass;

        EntityState state;
        state.health    = info.health;
        // Offsets::LifeState()/IsGhost() 优先用 RecvTable 动态解析值，
        // 为 0 才回退硬编码（硬编码已知在本机失效，见 Offsets.h 注释）。
        state.lifeState = TailByte(Offsets::LifeState());
        state.isGhost   = (TailByte(Offsets::IsGhost()) == 1);

        // ---- 判活四元组诊断（按网络类名去重，每类只打一次）----
        // "死人/幽灵也画"的规则校准数据源：实机对比活/死实体的
        // seq / solid 差异，再决定把哪条写进收录门槛。
        if (isActor)
        {
            static std::unordered_set<std::string> s_loggedAlive;
            // [批172] netNameOk 在分类缓存后恒为 true（否则已 continue），
            //   原 modelName 回退是死代码；缓存命中时 netName 为空 → "<cached>"。
            const std::string akey = netName[0] ? std::string(netName)
                                               : std::string("<cached>");
            if (s_loggedAlive.size() < 64 && s_loggedAlive.insert(akey).second)
            {
                unsigned solidFlags = 0; int seq = -1;
                if (s_sfOff)
                    SafeCopy(&solidFlags, reinterpret_cast<const void*>(ent + s_sfOff), sizeof(solidFlags));
                if (s_seqOff)
                    SafeCopy(&seq, reinterpret_cast<const void*>(ent + s_seqOff), sizeof(seq));
                PayloadLog("[esp-alive] %-20s life=%d hp=%d ghost=%d seq=%d solid=0x%X",
                           akey.c_str(), (int)state.lifeState, info.health,
                           (int)state.isGhost, seq, solidFlags);
            }
        }

        // ---- 收录门槛 ----
        // lifeState==0 是唯一权威判据。实测（训练场）：Hunter 潜行/Witch/
        // 玩家特感等实体 life=0（活）但 hp 长时间为 0（网络同步间隙），
        // 旧门槛 "hp<=0 一票否决" 会把活实体全拒 → "潜行/骑乘不显示"。
        // hp 只用于血条显示；脏道具（燃瓶 team=3）靠 lifeState=1/2 挡住，
        // 这道防线不变。
        if (state.lifeState != 0)
        {
            const int key = (info.zombieClass & 15) | ((state.lifeState & 3) << 4)
                          | ((state.isGhost ? 1 : 0) << 6) | ((info.health > 0 ? 1 : 0) << 7);
            const int bi = key >> 3, bit = 1 << (key & 7);
            if (!(s_diagSeen[bi] & bit))
            {
                s_diagSeen[bi] |= (unsigned char)bit;
                PayloadLog("[esp] 未收录 idx=%d sid=%d type=%d life=%d ghost=%d hp=%d pos=%.0f,%.0f,%.0f",
                           i, sidAbs, (int)info.type,
                           (int)state.lifeState, (int)state.isGhost, info.health,
                           info.position.x, info.position.y, info.position.z);
            }
            continue;
        }

        // ---- 骑乘场景诊断（Jockey 专用，限流 ~3 秒）----
        // 复现"骑在幸存者头上时不显示"：对比骑乘前后本实体的字段变化
        if (sidAbs == sid::Jockey)
        {
            static unsigned s_jk = 0;
            if ((s_jk++ % 90u) == 0)
                PayloadLog("[jockey] idx=%d life=%d hp=%d ghost=%d team=%d "
                           "pos=%.0f,%.0f,%.0f vel=%.0f,%.0f,%.0f",
                           i, (int)state.lifeState, info.health, (int)state.isGhost,
                           info.team,
                           info.position.x, info.position.y, info.position.z,
                           info.velocity.x, info.velocity.y, info.velocity.z);
        }

        snapshot.push_back({ info, state });
    }

    // ---- 汇总诊断：每 ~3 秒输出一次快照里的分类分布 ----
    // 按 Storm Identifier 计数（不再是 zombieClass，口径与绘制层一致）
    {
        static unsigned s_tick = 0;
        if ((s_tick++ % 180u) == 0)
        {
            // 全量 dump：定位"世界外框 / 血条联动 / 骑乘不显示"的确切实体
            for (const auto& [info, state] : snapshot)
            {
                PayloadLog("[esp-dump] idx=%-4d sid=%-4d team=%d hp=%-5d life=%d ghost=%d svid=%d pos=%.0f,%.0f,%.0f",
                           info.index, info.stormId, info.team, info.health,
                           (int)state.lifeState, (int)state.isGhost, info.survivorId,
                           info.position.x, info.position.y, info.position.z);
            }
            int nSurvivor = 0, nCommon = 0, nSpecial = 0, nTank = 0,
                nWitch = 0, nProp = 0, ghost = 0;
            for (const auto& [info, state] : snapshot)
            {
                if (info.team == 1) { ++nProp; continue; }
                if (state.isGhost)  ++ghost;
                if (info.team == 2)             { ++nSurvivor; continue; }
                if (info.type == EntityType::Common) ++nCommon;
                else if (info.type == EntityType::Witch) ++nWitch;
                else if (info.zombieClass == 8)        ++nTank;
                else if (info.zombieClass >= 1 && info.zombieClass <= 6) ++nSpecial;
            }
            PayloadLog("[esp] 快照 %d ｜ 幸存者=%d 特感=%d 小僵尸=%d 女巫=%d 坦克=%d 道具=%d 幽灵=%d"
                       " ｜ 表=%d vptr=%d team=%d",
                       (int)snapshot.size(), nSurvivor, nSpecial, nCommon,
                       nWitch, nTank, nProp, ghost,
                       s_tblNonZero, s_vptrInMod, s_teamOkCount);
        }
    }

    // 原子替换：同帧后续 GetFrameSnapshot()（ESP 绘制 + 自瞄选靶）即刻生效。
    // 渲染线程是唯一权威写入方；扫描线程降级后不再写快照
    std::lock_guard<std::mutex> lock(g_entitiesMutex);
    g_entities.swap(snapshot);
}
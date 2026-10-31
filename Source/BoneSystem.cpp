// ============================================================
// BoneSystem —— 骨骼数据读取（[批11·重写] SetupBones 直调，唯一权威来源）
// ------------------------------------------------------------
// 参考 RazeHack（L4D2 实证可用外挂）§7.1：
//     ent->SetupBones(buf, 128, 0x7FF00, curtime)
// 引擎自己计算骨骼矩阵写入调用方缓冲——零偏移依赖，永不漂移。
// 旧"实体内存窗口扫描骨基址"方案（TryBoneBase / MatrixNearOrigin /
// 偏移缓存 / 帧槽竞争）全部移除——它们依赖的头/颈/骨盆索引约定在
// 当前版本不成立，且偏移漂移导致 [bone-diag] 恒挂。
// 帧缓存：每渲染帧 BoneFrameBegin() 递增世代号，帧内按实体索引缓存
//   （ESP 骨架绘制与自瞄选靶共用，同一实体一帧只读一次）。
// ============================================================

 #include "BoneSystem.h"
#include "PayloadControl.h"
#include "Offsets.h"
#include "SDK/NetVars.h"
#include "Netvar.h"              // [批32] netvar::GetInterface（VModelInfoClient 在 engine.dll）   // [批32] netvar::GetInterface（engine.dll 取 VModelInfoClient）   // [批25] ModelIndex netvar
#include "Globals.h"
#include "Config/Config.h"   // [批150] 发布开关：studioHdrProbe             // [批105] GameWindowUnavailable（切窗口失焦门控）

#include <cstring>
#include <unordered_map>

namespace
{
    constexpr size_t kMatrixStride = 0x30;            // matrix3x4_t = 48 字节

    // 进程内 SEH 读
    template <typename T>
    bool SafeRead(uintptr_t addr, T& out)
    {
        __try
        {
            std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // 单骨位置合理性（相对实体原点，容差 320u）
    inline bool SaneBonePos(const Vec3& p, const Vec3& origin)
    {
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z))
            return false;
        return fabsf(p.x - origin.x) < 320.0f &&
               fabsf(p.y - origin.y) < 320.0f &&
               p.z - origin.z > -320.0f && p.z - origin.z < 320.0f;
    }

    // [批111·死代码] 删除了 MultiBoneSane()（原 53-77 行）—— 全项目零调用。
    //  它是批5 的"多骨验证"，当时用于 SetupBones 探针的输出校验；
    //  批40 改成在 BoneSysRefreshEntities 里直接校验矩阵内容后，
    //  这个独立函数就没人调了。留着会让人误以为存在两套校验。

    // [批35] 本地玩家指针（用于关卡加载门控）
    inline uintptr_t BoneSysLocalPlayer()
    {
        const uintptr_t client = (uintptr_t)::GetModuleHandleA("client.dll");
        if (!client)
            return 0;
        uintptr_t lp = 0;
        SafeRead(client + Offsets::dwLocalPlayer, lp);
        return lp;
    }

    // [批40·修复P0-1] 模型索引负缓存 —— 读写必须共用同一张表。
    //  旧实现两个函数各自持有一份 static map：Mark 写入的失败记录
    //  Cached 永远读不到 → 负缓存完全失效 → 每帧都在对同一个坏索引
    //  撞引擎 GetModel（空指针异常刷屏 + 性能损耗）。
    SRWLOCK                        g_negLock = SRWLOCK_INIT;

    // ============================================================
    //  [批108] 负缓存键必须是 (mi, ent) 复合键，不是单独的 mi
    // ------------------------------------------------------------
    //  沿革（前两次都错，代价分别是"拖黑全场"和"卡死"）：
    //   ① 批40：键 = mi。同一 mi 被多个实体共享（玩家 + 4 特感同模型）
    //      → 一个坏实体把全场拖黑 2 秒 → 自瞄周期性"头↔空气"。
    //   ② 批106：键仍是 mi，值里加 owner，只拦 owner 自己。
    //      → 同模型的其它实体【每帧】都要重跑完整解析。而一次解析 =
    //      槽8 vcall + IDST 扫描 64 次 SafeRead + FindModelPtrByScan
    //      **252 次 SafeRead**；大量 SafeRead 读未映射页会触发 SEH，
    //      开销比普通读高一两个数量级。每帧 N 实体 × 每 tick
    //      → 主线程被拖死 = 【游戏未响应】。而且 g_negCache[mi]= 是
    //      【覆盖写】，多实体失败互相冲掉 owner，负缓存等于没做。
    //
    //  正解：键 = (mi<<32 | ent)，每个 (模型,实体) 组合各自记账。
    //      各自失败各自被缓存 → 性能保护完整，且互不牵连。
    // ============================================================
    inline uint64_t NegKey(int mi, uintptr_t ent)
    {
        return ((uint64_t)(uint32_t)mi << 32) | (uint64_t)(uint32_t)ent;
    }
    std::unordered_map<uint64_t, DWORD> g_negCache;   // (mi,ent) → 失败时刻

    // 该 (mi, ent) 组合是否处于冷却期
    bool ModelIndexNegCached(int mi, uintptr_t ent)
    {
        AcquireSRWLockShared(&g_negLock);
        const auto it = g_negCache.find(NegKey(mi, ent));
        const bool bad = (it != g_negCache.end()) &&
                         (GetTickCount() - it->second < 500);
        ReleaseSRWLockShared(&g_negLock);
        return bad;
    }

    [[maybe_unused]] void MarkModelIndexBad(int mi, uintptr_t ent)  // [批168] 仅排障
    {
        AcquireSRWLockExclusive(&g_negLock);
        g_negCache[NegKey(mi, ent)] = GetTickCount();
        if (g_negCache.size() > 8192)
            g_negCache.clear();
        ReleaseSRWLockExclusive(&g_negLock);
    }

    // ============================================================
    //  [批37·性能] studiohdr 正缓存（modelIndex → studiohdr）
    // ------------------------------------------------------------
    //  [批111·根因修复] 缓存必须由 Get/Put 【共用同一份】。
    //  原来 s_lock 与 s_cache 都写成函数内 static：
    //      HdrCacheGet 里的 s_cache  和  HdrCachePut 里的 s_cache
    //  是【两个各自独立的空 map】（static 的存储期绑定在各自函数），
    //  锁也同样是两个互不同步的锁。于是：
    //      Put 把结果写进 A  →  Get 永远去查 B  → 永远 miss
    //  即【正缓存 100% 失效】，每帧每个实体都要重跑一次
    //  GetEntityStudioHdr 的完整解析（槽8 vcall + IDST 扫描
    //  64 次 SafeRead + FindModelPtrByScan 252 次 SafeRead
    //  ≈ 316 次 __try/__except 读）。这正是"游戏未响应"的老源头，
    //  也是"取骨骼有延迟/先锁空气后锁头"的直接原因。
    //
    //  为什么一直没被发现：负缓存（g_negCache）是【文件作用域】的，
    //  写得对；正缓存这个 bug 藏在"看起来有缓存"的表象下，日志上
    //  与"缓存未命中"完全一样，看不出来。
    //  与批105/批110 同源：同名 static 分散在不同作用域 = 两份存储。
    // ============================================================
    SRWLOCK                        g_hdrLock = SRWLOCK_INIT;
    std::unordered_map<int, uintptr_t> g_hdrCache;   // modelIndex → studiohdr*

    uintptr_t HdrCacheGet(int mi)
    {
        AcquireSRWLockShared(&g_hdrLock);
        const auto it = g_hdrCache.find(mi);
        const uintptr_t r = (it != g_hdrCache.end()) ? it->second : 0;
        ReleaseSRWLockShared(&g_hdrLock);
        return r;
    }

    void HdrCachePut(int mi, uintptr_t hdr)
    {
        AcquireSRWLockExclusive(&g_hdrLock);
        // [批111·内存泄漏] 原上限 512：特感模型约 40 种，永远打不满，
        //   所以一旦某局的服务器/资源包给出异常大的 mi 区间
        //   （合法范围 0..65535），缓存会一路涨到 6.5 万条且永不淘汰。
        //   改为 4096，与 g_negCache 同一量级。
        if (g_hdrCache.size() > 4096)
            g_hdrCache.clear();
        g_hdrCache[mi] = hdr;
        ReleaseSRWLockExclusive(&g_hdrLock);
    }

    // ============================================================
    //  [批41·根因修复②] 实体地址解析 —— 必须是【静态数组】语义
    // ------------------------------------------------------------
    //  旧代码：list = Read(client + dwEntityList)  → 把 dwEntityList
    //  当成"指向实体指针数组的指针变量"，再读 list + idx*0x10。
    //  实测该处是 client.dll 内的【静态数组】（LiveEsp::FillEntityTable
    //  与 SDK::CBaseEntity 构造都按 base + idx*0x10 直接读槽位）：
    //  于是第一次读拿到的是 entity[0]（通常为空）→ 第二次读地址 0
    //  → GetEntAddr 恒返回 0 → 每帧每个实体都走"空实体"跳过
    //  → 骨骼缓存永远为空（与 curtime 死锁叠加，双重断链）。
    // ============================================================
    inline uintptr_t GetEntAddr(int entIndex)
    {
        if (entIndex < 0 || entIndex > 4095)
            return 0;                                 // 表 1024 槽，放宽到 edict 上限
        const uintptr_t client = (uintptr_t)::GetModuleHandleA("client.dll");
        if (!client) return 0;
        uintptr_t ent = 0;
        SafeRead(client + Offsets::dwEntityList
                        + (uintptr_t)entIndex * 0x10u, ent);   // 槽步长 0x10
        return ent;
    }

    // 帧缓存：实体索引 → 快照（本帧已读实体零开销）
    struct BoneCacheEntry
    {
        BoneSnapshot snap;
        bool         resolved;
        unsigned     frame;
    };
    unsigned g_frame = 0;
    std::unordered_map<int, BoneCacheEntry> g_cache;

    // [批19] 引擎 curtime 估计（游戏线程 CreateMove 注入；BoneSystem 只读）
    float g_curtimeEst = 0.0f;

    // ============================================================
    //  [批30] 骨骼缓存（渲染线程写 / 游戏线程只读）
    // ------------------------------------------------------------
    //  架构修正（召唤 smoker 必崩的根因）：骨骼【只在渲染线程】取——
    //  引擎刚在渲染期算完骨骼、animstate 完整，且不在 CreateMove 的
    //  tick 临界区；游戏线程绝不调用任何引擎骨骼函数，只读缓存。
    //  稳定门槛：实体在快照中连续出现 ≥3 帧才取（spawn 初期跳过）。
    // ============================================================
    struct BoneStoreEntry
    {
        float m[128 * 12] = {};
        DWORD stampMs     = 0;
    };
    std::unordered_map<uintptr_t, BoneStoreEntry> g_boneStore;
    SRWLOCK            g_storeLock = SRWLOCK_INIT;
    std::unordered_map<int, int> g_appearCount;        // 实体索引 → 连续出现帧数
    SRWLOCK            g_appearLock = SRWLOCK_INIT;   // [批33] 出现计数加锁

    // ============================================================
    //  [批11] ReadBoneArray —— 骨骼矩阵唯一来源 = SetupBones 直调
    // ============================================================
    bool ReadBoneArray(uintptr_t ent, const Vec3& origin, BoneSnapshot& out)
    {
        const uintptr_t base = GetEntityBoneMatrixBase(ent, origin);
        if (!base)
            return false;

        std::memset(&out, 0, sizeof(out));
        for (int b = 0; b < Bone_Count_; ++b)
        {
            const uintptr_t ma = base + (uintptr_t)b * kMatrixStride;
            // [批40·修复P3-4] 一次 SEH 读整根骨的三列平移
            //  （旧代码 3 次 SafeRead = 每骨 3 次 SEH 开销 ×38 骨）
            float x = 0, y = 0, z = 0;
            __try
            {
                x = *reinterpret_cast<const float*>(ma + 0x0C);
                y = *reinterpret_cast<const float*>(ma + 0x1C);
                z = *reinterpret_cast<const float*>(ma + 0x2C);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;                             // 单骨异常：跳过该骨
            }
            out.pos[b]   = Vec3(x, y, z);
            out.valid[b] = SaneBonePos(out.pos[b], origin);
        }
        out.ok = true;                                // 矩阵源权威
        return true;
    }
} // namespace

// [批95] 公共版本：自瞄骨骼刷新门控用此函数自主读 dwLocalPlayer，
//   不再依赖透视模块设的 localPlayer 全局变量。
//   透视延迟启动或某些路径没走时 localPlayer 永远 0 → 骨骼刷新被
//   门控挡住 → 自瞄全走兜底 Origin+64 → 打空气。
uintptr_t BoneSysLocalPlayerPtr()
{
    const uintptr_t client = (uintptr_t)::GetModuleHandleA("client.dll");
    if (!client)
        return 0;
    uintptr_t lp = 0;
    SafeRead(client + Offsets::dwLocalPlayer, lp);
    return lp;
}

// [批19] 引擎 curtime 注入：CreateMove 每 tick 调
// （tick_count × interval_per_tick 自校准；未注入时 SetupBones 直调关闭）
void BoneSysSetCurtime(float t)
{
    if (t > 0.0f)
        g_curtimeEst = t;
}

// 每渲染帧调用一次（DrawCallBack，FrameCacheBegin 之后）：骨骼帧缓存进入新帧
void BoneFrameBegin()
{
    ++g_frame;
    if (g_cache.size() > 512)                         // 防御：换图等异常增长时收缩
        g_cache.clear();
}

// ============================================================
//  [批11·重写] GetEntityBoneMatrixBase —— SetupBones 直调
// ------------------------------------------------------------
//  结构：IClientUnknown 槽 2 = GetClientRenderable → renderable vtable
//        → SetupBones 槽位探针（3..40，宽掩码 0x7FF00 全骨写出，
//          输出过 6 骨验证者即中）→ 槽位缓存（每会话一次探针）
//  每次调用：SetupBones 刷新静态矩阵缓冲 → 返回缓冲地址。
//  探针/调用全程 SEH；输出必须过 6 骨验证（假阳性槽位无法蒙混）。
// ============================================================
namespace { bool NetableAlive(uintptr_t ent, uintptr_t& netable, uintptr_t& nvt); }

// ============================================================
//  [批12·重写] GetEntityBoneMatrixBase —— SetupBones 直调（精确槽位版）
// ------------------------------------------------------------
//  槽位来源：RazeHack 逆向伪代码 GetBonePos_100247C0（L4D2 实证）：
//      this  = 实体地址 + 4   （IClientNetworkable 子对象）
//      vtable= *(this)，槽 13（字节偏移 52）
//      调用  = SetupBones(buf, 128, 0x7FF00, curtime)
//  与旧探针方案的区别：【零探测】——槽位是逆向精确值，不猜不试，
//  无闪退风险；输出仍过 6 骨验证（防实体异常状态）。
// ============================================================
// ============================================================
//  [批30·重做] 骨骼获取：渲染线程独占取骨 + 游戏线程只读缓存
// ------------------------------------------------------------
//  FetchBonesRaw()  —— 仅【渲染线程】调用（引擎刚在渲染期算完骨骼，
//                      animstate 完整；不在 CreateMove 的 tick 临界区）
//  GetEntityBoneMatrixBase() —— 【游戏线程】只读缓存副本，
//                      绝不调用任何引擎骨骼函数（召唤瞬间崩溃的根治）
//  BoneSysRefreshEntities()  —— 渲染线程每帧刷新：连续出现 ≥3 帧
//                      的实体才取骨骼（spawn 初期跳过 = 崩溃高发期）
// ============================================================
namespace
{
    // why 输出（0=成功）：
    //   1=curtime 未注入   2=networkable 虚表读失败  3=活体门控拒绝
    //   4=槽 13 函数指针无效  5=SetupBones 返回 false 或 AV
    //   6=输出验证失败（NaN / 骨间距异常）
    bool FetchBonesRaw(uintptr_t ent, float* outM, int* why = nullptr)
    {
        if (why) *why = 0;
        if (ent < 0x10000u || !outM)
        {
            if (why) *why = 2;
            return false;
        }
        if (g_curtimeEst <= 0.0f)
        {
            if (why) *why = 1;
            return false;                             // curtime 未注入：宁缺勿卡
        }

        const uintptr_t netable = ent + 4;            // IClientNetworkable 子对象
        uintptr_t nvt = 0;
        if (!SafeRead(netable, nvt) || nvt < 0x10000u || nvt >= 0x7FFF0000u)
        {
            if (why) *why = 2;
            return false;
        }

        {   // [批18] 活体门控：vtable 必在 client.dll 模块内（纯数据校验）
            uintptr_t na = 0, nv = 0;
            if (!NetableAlive(ent, na, nv))
            {
                if (why) *why = 3;
                return false;
            }
        }

        uintptr_t fn = 0;
        if (!SafeRead(nvt + 52u, fn) || fn < 0x10000u || fn >= 0x7FFF0000u)
        {
            if (why) *why = 4;
            return false;                             // 槽 13 = SetupBones
        }

        using SB_t = bool (__fastcall*)(void*, void*, float (*)[12], int, int, float);
        static SRWLOCK s_sbLock = SRWLOCK_INIT;
        bool ret = false;
        AcquireSRWLockExclusive(&s_sbLock);
        __try
        {
            ret = ((SB_t)(void*)fn)((void*)netable, nullptr,
                                    reinterpret_cast<float (*)[12]>(outM),
                                    128, 0x7FF00, g_curtimeEst);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            ret = false;
        }
        ReleaseSRWLockExclusive(&s_sbLock);

        // [批41] 返回值只作提示，不作判据：RazeHack 的逆向伪代码里这个槽位
        //  返回的是地址（不是 bool）；若某些模型/实体的实现走 void 分支，
        //  强判 false 会让我们永远拿不到骨骼。真判据 = 下面的矩阵内容验证。
        const bool retOk = ret;

        // [批40·修复P2-3] 输出验证：前 6 骨必须有限且【彼此靠近】
        //  （一副真实骨架的前 6 骨互相间距 < 200u；半初始化/垃圾矩阵
        //   会出现一根骨飞出 —— 旧代码只查 isfinite，注释与代码不符）
        bool valid = true;
        {
            const float* root = outM + 0 * 12;
            const float rx = root[3], ry = root[7], rz = root[11];
            if (!isfinite(rx) || !isfinite(ry) || !isfinite(rz))
                valid = false;
            for (int b = 1; valid && b <= 5; ++b)
            {
                const float* m = outM + b * 12;
                const float px = m[3], py = m[7], pz = m[11];
                if (!isfinite(px) || !isfinite(py) || !isfinite(pz))
                {
                    valid = false;
                    break;
                }
                const float dx = px - rx, dy = py - ry, dz = pz - rz;
                if (dx * dx + dy * dy + dz * dz > 200.0f * 200.0f)
                {
                    valid = false;                    // 骨间距异常：弃用
                    break;
                }
            }
        }
        if (!valid)
        {
            if (why) *why = retOk ? 6 : 5;
            return false;
        }
        if (!retOk)
        {
            // 矩阵有效但返回 false：记录一次（10s 节流），说明返回值语义不符
            static DWORD s_abiLog = 0;
            const DWORD nowA = GetTickCount();
            if (nowA - s_abiLog > 10000)
            {
                s_abiLog = nowA;
                PayloadLog("[bone-abi] SetupBones 返回 false 但矩阵有效 —— "
                           "返回值不参与判据（沿用矩阵验证）");
            }
        }
        return true;
    }
}


// 游戏线程只读缓存（线程局部副本 + 共享锁拷贝，杜绝撕裂）
uintptr_t GetEntityBoneMatrixBase(uintptr_t ent, const Vec3& origin)
{
    (void)origin;
    if (ent < 0x10000u)
        return 0;

    // [批112] TTL 250ms → 50ms；[批123] 50 → 80ms：
    //   轮转后每个实体约每 3 帧刷新一次（60fps ≈ 50ms），50ms 正好卡在
    //   边界上会频繁 miss；80ms 容忍一帧掉拍，仍远小于旧 250ms。
    static thread_local float s_readBuf[128 * 12];
    bool ok = false;
    AcquireSRWLockShared(&g_storeLock);
    const auto it = g_boneStore.find(ent);
    if (it != g_boneStore.end() && GetTickCount() - it->second.stampMs <= 80)
    {
        std::memcpy(s_readBuf, it->second.m, sizeof(s_readBuf));
        ok = true;
    }
    ReleaseSRWLockShared(&g_storeLock);
    return ok ? (uintptr_t)s_readBuf : 0;
}

// ============================================================
//  [批109] 统一的实体地址解析（见 BoneSystem.h 的说明）
// ------------------------------------------------------------
//  与上面的 GetEntAddr 同源。存在的唯一理由：让"读骨骼"侧也能用
//  与"取骨"侧【完全相同】的算法拿到地址，从而保证 g_boneStore 命中。
// ============================================================
uintptr_t BoneSysEntityAddr(int index)
{
    return GetEntAddr(index);
}

// ============================================================
//  [批33] 渲染线程：每帧刷新骨骼缓存
//  · 地图切换检测（dwLocalPlayer 变化/归零）→ 清空跨地图状态
//    （实体地址与索引会被复用，旧状态会套到新实体上 = 悬垂/野指针）
//  · 每帧直调预算（SetupBones 不是廉价调用，关卡加载期实体暴增时
//    无上限调用 = 引擎侧崩溃）
//  · 实体基础校验（vtable 必在 client.dll 模块内 = 存活槽位）
//  · 出现计数加锁（防止与其它线程并发操作 unordered_map）
// ============================================================
void BoneSysRefreshEntities(const int* entIndices, int count)
{
    // [批98] 门控诊断：静默 return 是"整场取骨 0 次却查不出原因"的根源。
    //   任何一道门挡住都留一条 3s 节流记录，直接指出卡在哪。
    auto GateLog = [](const char* why, unsigned a = 0, unsigned b = 0)
    {
        static DWORD s_gateLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_gateLog < 3000)
            return;
        s_gateLog = t;
        PayloadLog("[bone-gate] 取骨被拦：%s（a=0x%X b=0x%X）", why, a, b);
    };

    if (!entIndices || count <= 0)
    {
        GateLog("无候选实体", (unsigned)(entIndices ? 1 : 0), (unsigned)count);
        return;
    }

    // ============================================================
    //  [批105·切窗口闪退修复] 失焦 / 最小化 → 本帧不取骨
    // ------------------------------------------------------------
    //  批72 已经踩过同构的坑（切图瞬间清缓存后仍直调 SetupBones →
    //  游戏卡死 + ucrtbase 0xc0000409）。切窗口是同一类事件的另一个
    //  触发方式：Alt+Tab 时引擎可能重建渲染/动画状态，实体正在被
    //  改写；此时直调引擎函数 = 铁律里的"失效实体直调引擎 vcall"。
    //  取骨不是每帧必需（缓存有 250ms 有效窗口），停几帧无感知。
    // ============================================================
    {
        if (GameWindowUnavailable())
        {
            GateLog("窗口失焦（切窗口/最小化）", 0, 0);
            return;
        }
    }

    // ---- 地图切换检测：本地玩家指针变化 → 清空跨地图状态 ----
    {
        static uintptr_t s_lastLocal = 0;
        // [批118] 变化防抖：指针必须【稳定 300ms】才认定为换图。
        //   实战中 localPlayer 可能被引擎短暂改写（重生/观察者切换/
        //   0 与真实值之间抖动），逐帧 `lp != s_lastLocal` 会反复
        //   清缓存 + 进冷却，自瞄被反复打断。防抖后只认真正的换图。
        static uintptr_t s_pendingLocal = 0;
        static DWORD     s_pendingSince = 0;
        uintptr_t lp = 0;
        const uintptr_t client = (uintptr_t)::GetModuleHandleA("client.dll");
        if (client)
            SafeRead(client + Offsets::dwLocalPlayer, lp);   // 每帧现读真实值
        static DWORD s_graceUntil = 0;
        // [批118] 冷却的硬上限基准 + 上限触发后的静默期
        static DWORD s_graceStart = 0;
        static DWORD s_graceBlocked = 0;
        // [批170] 最近一次确认换图的时刻 —— 硬上限在换图后要放宽
        static DWORD s_lastMapChangeAt = 0;

        const DWORD nowTick = GetTickCount();
        if (lp != s_lastLocal)
        {
            if (lp != s_pendingLocal)
            {
                s_pendingLocal = lp;
                s_pendingSince = nowTick;
            }
            const bool stable = (s_pendingSince != 0) &&
                                (nowTick - s_pendingSince >= 300);
            if (stable)
            {
                int nStore = 0, nAppear = 0, nNeg = 0, nHdr = 0;
                AcquireSRWLockExclusive(&g_storeLock);
                nStore = (int)g_boneStore.size();
                g_boneStore.clear();
                ReleaseSRWLockExclusive(&g_storeLock);
                {
                    AcquireSRWLockExclusive(&g_appearLock);
                    nAppear = (int)g_appearCount.size();
                    g_appearCount.clear();
                    ReleaseSRWLockExclusive(&g_appearLock);
                }
                // [批118] 模型缓存必须跟骨骼缓存一起清：
                //   · 负缓存键含实体地址，旧地图的失败记录会污染新地图；
                //   · 正缓存键是 mi，换图后 mi 可能被复用到【不同模型】上，
                //     不清会拿旧 studiohdr 去算新模型的 hitbox（错位）。
                {
                    AcquireSRWLockExclusive(&g_negLock);
                    nNeg = (int)g_negCache.size();
                    g_negCache.clear();
                    ReleaseSRWLockExclusive(&g_negLock);
                    AcquireSRWLockExclusive(&g_hdrLock);
                    nHdr = (int)g_hdrCache.size();
                    g_hdrCache.clear();
                    ReleaseSRWLockExclusive(&g_hdrLock);
                }
                PayloadLog("[bone-reset] 地图切换（local %p→%p，稳定 %ums）："
                           "清空骨骼缓存 store=%d appear=%d neg=%d hdr=%d",
                           (void*)s_lastLocal, (void*)lp,
                           (unsigned)(nowTick - s_pendingSince),
                           nStore, nAppear, nNeg, nHdr);
                s_lastLocal    = lp;
                s_graceUntil   = nowTick + 1500;   // [批72] 开始冷却
                s_graceStart   = nowTick;          // [批118] 硬上限起点
                s_lastMapChangeAt = nowTick;       // [批170] 记录换图时刻
                s_pendingLocal = 0;
                s_pendingSince = 0;
            }
            // 未稳定：本帧不认定换图（不清缓存、不进冷却），继续走门控
        }
        else
        {
            s_pendingLocal = 0;
            s_pendingSince = 0;
        }

        // ============================================================
        //  [批72] 地图切换冷却 —— 修"切图瞬间卡死后自动关闭"
        // ------------------------------------------------------------
        //  实测崩溃链：
        //      [bone-reset] 地图切换（local 00000000→53EFD3D0）
        //      [bone-abi] SetupBones 返回 false 但矩阵有效
        //      → 游戏卡死 → WER: ucrtbase 0xc0000409 + tier0 0x40000015
        //
        //  原因：清空缓存只清了【我们的表】，同一帧下面仍然拿着渲染快照里的
        //  实体索引去直调引擎 SetupBones。而切图时引擎正在释放/重建实体，
        //  这些索引指向的内存已经失效 —— 正是铁律里"失效实体直调引擎
        //  vcall = 闪退"。vtable 校验只能挡住"槽位已被复用"的情况，
        //  挡不住"内存正在被引擎改写"的中间态。
        //
        //  对策：切图后 1.5 秒内完全不取骨；本地玩家为空（主菜单/加载中）
        //  时也不取。宁可这一秒半没有 ESP 骨骼，也不碰正在变动的内存。
        // ============================================================
        {
            const DWORD now = GetTickCount();

            // ============================================================
            //  [批118] 冷却硬上限（2 秒）：任何冷却都不许无限续期
            // ------------------------------------------------------------
            //  旧账（实战日志）：curtime 非单调 → "curtime 倒退"每 3 秒
            //  重新触发一次 2 秒冷却 → 骨骼整场不刷新 → 自瞄全 -1。
            //  批117 把 curtime 改成单调累加后根因已除；这里再加一道
            //  兜底：一轮冷却从 s_graceStart 起最多 2 秒，到点强制放行，
            //  随后 10 秒内不再接受新的冷却请求（防止同一故障反复咬住）。
            //  取舍：宁可极小概率踩到半初始化实体，也不能让自瞄永久失效。
            // ============================================================
            //  [批170] 硬上限在换图后放宽到 10 秒 + curtime 未起来不放行
            //  ------------------------------------------------------------
            //  10-25 静默闪退实锤：换图冷却被 2s 硬上限解除 → 还在加载的
            //  半初始化实体（ent=4）上直调 SetupBones → 引擎内部崩 →
            //  tier0 Error() → 静默退出（hb 最后一条 phase=10 ent=4）。
            //  批118 当时赌的是"极小概率踩到半初始化实体"，实测不是小概率
            //  —— 每次换图都会撞。自瞄静默几秒远好过闪退。
            {
                const DWORD sinceMap = now - s_lastMapChangeAt;
                const DWORD capMs    = (sinceMap < 10000u) ? 10000u : 2000u;
                if (s_graceStart != 0 && now >= s_graceStart + capMs &&
                    g_curtimeEst > 0.01f)
                {
                    s_graceUntil = 0;
                    s_graceStart = 0;
                    s_graceBlocked = now + 10000;
                    PayloadLog("[bone-gate] 冷却达到硬上限（%lus%s）：强制恢复取骨"
                               "（之后 10s 内不再因 curtime/换图进冷却）",
                               (unsigned long)(capMs / 1000u),
                               sinceMap < 10000u ? "，换图后放宽" : "");
                }
            }

            // [批82] curtime 归零/倒退 = 正在加载新关卡。
            //   实测崩溃日志里这一行是前兆：
            //     候选=17 成功=0 跳过=17 | 未就绪=17 | curtime=0.00
            //   本地玩家指针已经有效，但引擎时间还没起来、实体正在重建；
            //   此时调 SetupBones 会踩到半初始化的骨骼/模型数据
            //   （WER: ucrtbase 0xc0000409 + tier0 0x40000015）。
            //   只靠"本地玩家为空"挡不住这种情况，必须看时间。
            static float s_lastCt = -1.0f;
            const float ct = g_curtimeEst;
            // [批117] 阈值 0.5s → 2.0s：curtime 已改为单调累加，正常时
            //   不会回退；仍留 2s 容差只认真实的关卡加载（tick 基线重建）。
            const bool ctBad = (ct <= 0.01f ||
                                (s_lastCt > 0.0f && ct < s_lastCt - 2.0f));
            if (ctBad && now >= s_graceBlocked)
            {
                if (s_graceStart == 0)
                    s_graceStart = now;
                DWORD until = now + 2000;
                const DWORD cap = s_graceStart + 2000;   // 硬上限
                if (until > cap)
                    until = cap;
                s_graceUntil = until;
                GateLog(ct <= 0.01f ? "curtime 未注入（CreateMove 没跑？）"
                                    : "curtime 倒退（换图中）",
                        (unsigned)(ct * 1000.0f),
                        (unsigned)(s_lastCt * 1000.0f));
                s_lastCt = ct;
                return;
            }
            s_lastCt = ct;

            // [批118] 静默期放行也要留痕：否则"硬上限在起作用"和
            //   "逻辑没跑"在日志上一样（静默门控必须留痕）。
            if (ctBad && now < s_graceBlocked)
            {
                GateLog("curtime 异常但处于硬上限静默期（继续取骨）",
                        (unsigned)(ct * 1000.0f),
                        (unsigned)(s_graceBlocked - now));
            }

            if (!lp)
            {
                if (s_graceStart == 0)
                    s_graceStart = now;
                DWORD until = now + 500;        // 主菜单/加载中：持续后延
                const DWORD cap = s_graceStart + 2000;
                if (until > cap)
                    until = cap;
                s_graceUntil = until;
                GateLog("localPlayer 为空", (unsigned)client, 0);
                return;
            }
            if (now < s_graceUntil)
            {
                // [批118] 打印"剩余毫秒"而不是时间戳，避免被误读成指针
                GateLog("切图冷却中", (unsigned)(s_graceUntil - now),
                        (unsigned)lp);
                return;                         // 切图冷却中：不调引擎
            }
        }
    }

    // [批41] 耗时测量：SetupBones 直调是渲染线程上的同步引擎调用，
    //  curtime 修好之后这条路径【第一次真正执行】——必须量出它的毫秒数，
    //  否则 FPS 掉下来只能瞎猜（预算 24 次/帧 × 单次成本 = 最坏帧耗时）。
    static LARGE_INTEGER s_freq = {};
    if (!s_freq.QuadPart) ::QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER t0{}, t1{};
    ::QueryPerformanceCounter(&t0);

    // [批152] 每帧直调预算 24 → 12：SetupBones 是渲染线程上的同步引擎调用，
    //   而且实体处于坏状态时会在引擎内部 AV（崩溃日志 43 次同一形状：
    //   engine→materialsystem→shaderapidx9→我们→client.dll→AV）。
    //   减半直调 = 触发概率减半，骨骼缓存靠轮转+TTL 兜住，不影响精度。
    constexpr int kMaxFetchPerFrame = 12;   // [批33/批152] 每帧最多 12 次直调
    // [批41] 逐门计数器：整场"成功=0"时必须能一眼看出卡在哪一道门
    int fetched = 0, skipped = 0;
    int skEarly = 0, skNull = 0, skGate = 0, skFetch = 0;
    // [批161] 高速跳过恢复（兜底锚点已修为盒内按 sid 加权，见批160）
    constexpr bool kBoneFastSkipActive = true;   // 验证器锚点 S36
    int skFast  = 0;                       // [批158] 高速跳过计数

    // ============================================================
    //  [批152] SetupBones 直调【熔断】
    //  ------------------------------------------------------------
    //  崩溃日志实测：一局内 43 次访问违例，形态完全一致
    //      engine → materialsystem → shaderapidx9 → 我们的渲染钩子
    //             → 取骨链 → client.dll(SetupBones) → 引擎内部 AV
    //  也就是：实体处于坏状态（切图 / 实体释放 / 模型未就绪）时直调
    //  SetupBones，引擎内部解空。我们的 __try 能兜住不崩，但引擎状态
    //  已经被写坏 → 表现为"卡死"。
    //  熔断策略：单位时间内直调失败密集（返回 false / AV / 矩阵非法）
    //  → 判定引擎正处于坏状态 → 暂停直调几秒，让引擎自己恢复。
    // ============================================================
    static int    s_failBurst = 0;          // 当前窗口内的失败次数
    static DWORD s_failWinStart = 0;       // 窗口起点
    static DWORD s_suspendUntil = 0;       // 熔断到期时刻
    const DWORD nowFrame = GetTickCount();
    if (nowFrame >= s_failWinStart + 1000)
    {
        s_failBurst = 0;
        s_failWinStart = nowFrame;
    }
    if (nowFrame < s_suspendUntil)
    {
        // 熔断中：本帧完全不做直调（骨骼缓存继续用旧值，TTL 到期后自然失效）
        // [批173] 日志 3s 节流：原来每帧打一条（实测一局刷了 16232 行）
        {
            static DWORD s_guardLogAt = 0;
            if (nowFrame - s_guardLogAt >= 3000u)
            {
                s_guardLogAt = nowFrame;
                PayloadLog("[bone-guard] SetupBones 直调熔断中（引擎状态异常），"
                           "剩余 %d ms", (int)(s_suspendUntil - nowFrame));
            }
        }
        return;
    }
    int rc[7] = {};                                  // rc[1..6] = FetchBonesRaw 原因
    // ============================================================
    //  [批123·饥饿修复] 轮转起点
    // ------------------------------------------------------------
    //  旧实现永远从 entIndices[0] 开始，取满 24 个就 break。列表顺序
    //  稳定时，第 25 个之后的实体【永远】轮不到刷新 → g_boneStore 里
    //  没有它们 → GetEntityBoneMatrixBase=0 → hitbox/骨名/区域三条链
    //  全失败（日志：候选=49 成功=24 跳过=0，部位解析大面积跳过）。
    //  轮转后：N 个实体、预算 B → 每个实体每 ceil(N/B) 帧刷新一次，
    //  配合 TTL 80ms 可覆盖 50 个候选（60fps 下 ≈ 3 帧 = 50ms）。
    // ============================================================
    static size_t s_rrCursor = 0;
    const size_t rrStart = (count > 0)
                         ? (s_rrCursor % (size_t)count) : 0;
    int visited = 0;
    for (int k = 0; k < count; ++k)
    {
        if (fetched >= kMaxFetchPerFrame)
            break;                                  // 预算耗尽：本帧不再取
        const int i = (int)((rrStart + (size_t)k) % (size_t)count);
        ++visited;
        const int ei = entIndices[i];
        if (ei < 0)
            continue;

        // 出现帧计数（写 unordered_map 必须【独占】锁：共享锁下并发写
        // 会触发 map 重哈希竞态 = 堆结构损坏）
        int seen = 0;
        {
            AcquireSRWLockExclusive(&g_appearLock);
            // [批111·性能] 容量上限。键是实体索引，值是连续出现帧数。
            // 本表只在换图时 clear()，无淘汰逻辑；实体索引槽位会被
            // 反复复用（死亡/召唤的特感共用槽位），长时间游戏下条目
            // 会单调堆积 → 每帧 AcquireSRWLockExclusive + find/insert
            // 变慢。上限 2048 远大于槽位表规模，触发即整体重建。
            if (g_appearCount.size() > 2048)
                g_appearCount.clear();
            const auto it = g_appearCount.find(ei);
            seen = (it != g_appearCount.end()) ? it->second : 0;
            if (seen < 100000) ++seen;
            g_appearCount[ei] = seen;
            ReleaseSRWLockExclusive(&g_appearLock);
        }
        if (seen < 3)
        {
            ++skipped; ++skEarly;
            continue;                              // [批30] spawn 初期跳过
        }

        const uintptr_t ent = GetEntAddr(ei);
        if (!ent)
        {
            ++skipped; ++skNull;
            continue;
        }
        // 实体基础校验：networkable vtable 必在 client.dll 模块内
        {
            uintptr_t na = 0, nv = 0;
            if (!NetableAlive(ent, na, nv))
            {
                ++skipped; ++skGate;
                continue;                          // 失效/未就绪槽位：跳过
            }
        }

        // ============================================================
        //  [批158→批161→批174] 速度门控：>400u/s 才不花骨骼预算
        // ------------------------------------------------------------
        //  批161 用 120u/s 阈值，实测把正常跑动特感（200~300u/s）全跳过：
        //  日志"候选=7~8 高速=4~7"→ 骨骼大面积缺失 → 自瞄频繁走兜底
        //  （用户反馈"兜底瞄准太频繁"）。
        //  批174 修正：①阈值放宽到 400u/s（只剩 Jockey 冲锋/Charger 跑动
        //  等真正冲刺跳过）；②候选数 ≤ 预算时干脆全取——快照修正后
        //  候选只有 7~8 个，12/帧的预算本来就用不满，跳过毫无收益。
        //  副作用：极高速实体无骨架线/走兜底锚点（框仍在）。
        // ============================================================
        if ((size_t)count > (size_t)kMaxFetchPerFrame)
        {
            Vec3 vel{};
            if (SafeRead(ent + Offsets::m_vecVelocity, vel) &&
                vel.x * vel.x + vel.y * vel.y + vel.z * vel.z
                    > 400.0f * 400.0f)
            {
                ++skipped; ++skFast;
                continue;
            }
        }

        BoneStoreEntry e{};
        int why = 0;
        // [批125] 心跳阶段：SetupBones 直调是崩溃头号嫌疑，记录到实体级
        g_hbPhase = 10;
        g_hbEnt   = ei;
        if (!FetchBonesRaw(ent, e.m, &why))
        {
            ++skipped; ++skFetch;
            if (why >= 1 && why <= 6) ++rc[why];
            // [批152] 失败计数：5=调用返回 false/AV，6=矩阵非法
            if (why == 5 || why == 6)
            {
                ++s_failBurst;
                if (s_failBurst >= 6)
                {
                    s_failBurst = 0;
                    s_suspendUntil = nowFrame + 3000;
                    PayloadLog("[bone-guard] SetupBones 失败密集（引擎状态异常）"
                               " → 暂停直调 3 秒");
                    return;
                }
            }
            // [批41] 输出异常取样（5s 一次）：直接把前 6 骨坐标打出来，
            //  用来判断是"引擎没写缓冲"还是"我们的间距阈值定错了"。
            //  5=调用后矩阵无效/AV  6=矩阵有效但内容不合法
            if (why == 5 || why == 6)
            {
                static DWORD s_vLog6 = 0;
                const DWORD now6 = GetTickCount();
                if (now6 - s_vLog6 > 5000)
                {
                    s_vLog6 = now6;
                    PayloadLog("[bone-valid] 输出验证失败 ent=%p 骨0=(%.1f,%.1f,%.1f) "
                               "骨1=(%.1f,%.1f,%.1f) 骨2=(%.1f,%.1f,%.1f) "
                               "骨3=(%.1f,%.1f,%.1f)",
                               (void*)ent,
                               e.m[3], e.m[7], e.m[11],
                               e.m[12 + 3], e.m[12 + 7], e.m[12 + 11],
                               e.m[24 + 3], e.m[24 + 7], e.m[24 + 11],
                               e.m[36 + 3], e.m[36 + 7], e.m[36 + 11]);
                }
            }
            continue;
        }
        e.stampMs = GetTickCount();
        AcquireSRWLockExclusive(&g_storeLock);
        g_boneStore[ent] = e;
        ReleaseSRWLockExclusive(&g_storeLock);
        ++fetched;
    }
    if (count > 0)
        s_rrCursor = (rrStart + (size_t)visited) % (size_t)count;
    g_hbPhase = 0;                       // [批125] 取骨循环结束
    g_hbEnt   = -1;

    // 埋点：每秒一行取骨统计（定位取骨过多/跳过过多）
    ::QueryPerformanceCounter(&t1);
    const double costMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                          (double)(s_freq.QuadPart ? s_freq.QuadPart : 1);
    {
        static DWORD  s_log   = 0;
        static double s_sumMs = 0.0;
        static double s_maxMs = 0.0;
        static int    s_nMs   = 0;
        s_sumMs += costMs;                          // 累计（每秒平均/峰值）
        if (costMs > s_maxMs) s_maxMs = costMs;
        ++s_nMs;

        // [批146] 发布就绪度给游戏线程：大面积未就绪（切图 / 实体刚生成）
        //   时让它整帧跳过自瞄，而不是几十个候选逐个失败刷日志。
        g_boneCandidates = count;
        g_boneNotReady   = skEarly + skNull + skGate;

        const DWORD now = GetTickCount();
        if (now - s_log > 1000)
        {
            s_log = now;
            const double avgMs = s_nMs ? s_sumMs / (double)s_nMs : 0.0;
            PayloadLog("[bone-refresh] 取骨统计：候选=%d 成功=%d 跳过=%d "
                       "预算=%d | 未就绪=%d 空实体=%d 门控=%d 取骨失败=%d 高速=%d "
                       "| curtime=%.2f 原因[时间=%d 虚表=%d 门控=%d 槽13=%d 返回=%d 验证=%d] "
                       "| 耗时 平均=%.2fms 峰值=%.2fms",
                       count, fetched, skipped, kMaxFetchPerFrame,
                       skEarly, skNull, skGate, skFetch, skFast,
                       g_curtimeEst,
                       rc[1], rc[2], rc[3], rc[4], rc[5], rc[6],
                       avgMs, s_maxMs);
            s_sumMs = 0.0;
            s_maxMs = 0.0;
            s_nMs   = 0;
        }
    }
}

// ============================================================
//  [批15] 引擎 vcall 链：GetModel / GetStudiomodel（RazeHack 实证布局）
// ------------------------------------------------------------
//  GetModel       = networkable vtable 槽 8（字节 32）
//  GetStudiomodel = VModelInfoClient004 vtable 槽 30（字节 120）
//  两个调用都由引擎内部自校验（失效 model_t 不会崩）——
//  取代旧的手工 IDST/模型名内存遍历（GetEntityModelName 闪退根因）。
// ============================================================
namespace
{
    // ============================================================
    //  [批18] 活体门控（纯数据版——零 vcall）
    // ------------------------------------------------------------
    //  批14/15 的 vcall 门控（槽3 GetClientClass / 槽7 IsDormant）槽位
    //  未经验证：若真实槽位是带参函数，__thiscall 清栈弹走未压入的
    //  字节 → ESP 错位 → SEH 链损坏 → ntdll 0xc0000028/0xc0000409
    //  闪退（10-20 事件日志实锤）。改为纯读校验。
    //
    //  [批50·判据修正] 原判据是"networkable vtable 必落在
    //  client.dll 模块范围内"。实测该判据【过严】：
    //    · 存活实测 vtable 落在【堆】上（0x07817100 一类的地址），
    //      而非 client.dll 的 .rdata → 门控直接拒绝
    //    · 武器/服务端实体尤其明显（同批连发失效的同一个坑）
    //  改为【两级判据】：
    //    ① vtable 落在已映射的可读用户态内存（0x10000~0x7FFF0000）
    //    ② vtable[-1]（COL 指针）也可读 —— 释放槽位的垃圾指针
    //       在这里会露馅（读不出合法值就拒绝）
    //  只做 SafeRead，零调用风险。
    // ============================================================
    // ============================================================
    //  [批130] 只读指纹采集：为"找 GetModel 槽位"提供证据
    // ------------------------------------------------------------
    //  · DumpNetableCandidates：ent 前 0x40 每个指针 → vtable → 槽8
    //    函数地址与前 16 字节。用于判断 networkable 到底在哪、槽8 是什么。
    //  · DumpModelInfoVtable：VModelInfoClient vtable 前 40 槽的函数地址
    //    与前 16 字节。`8B 44 24 04`（mov eax,[esp+4]）= int 取参特征，
    //    可据此定位 GetModel(int)。
    //  两者都一次性、纯读，不发任何 vcall。
    // ============================================================
    [[maybe_unused]] void DumpNetableCandidates(uintptr_t ent)  // [批168] 仅排障
    {
        static bool done = false;
        if (done || ent < 0x10000u)
            return;
        done = true;
        int logged = 0;
        for (int off = 0; off <= 0x40 && logged < 12; off += 4)
        {
            uintptr_t p = 0;
            if (!SafeRead(ent + (uintptr_t)off, p) ||
                p < 0x10000u || p >= 0x7FFF0000u)
                continue;
            uintptr_t vt = 0;
            if (!SafeRead(p, vt) || vt < 0x10000u || vt >= 0x7FFF0000u)
                continue;
            uintptr_t fn = 0;
            if (!SafeRead(vt + 8u * sizeof(uintptr_t), fn) ||
                fn < 0x10000u || fn >= 0x7FFF0000u)
                continue;
            uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
            if (!SafeRead(fn, d0)) continue;
            SafeRead(fn + 4, d1); SafeRead(fn + 8, d2); SafeRead(fn + 12, d3);
            ++logged;
            PayloadLog("[NET-CAND] ent=0x%X off=0x%02X p=0x%X vt=0x%X "
                       "fn8=0x%X code=%08X %08X %08X %08X",
                       (unsigned)ent, off, (unsigned)p, (unsigned)vt,
                       (unsigned)fn, d0, d1, d2, d3);
        }
        if (!logged)
            PayloadLog("[NET-CAND] ent=0x%X 前 0x40 字节没有任何可用指针",
                       (unsigned)ent);
    }

    [[maybe_unused]] void DumpModelInfoVtable(uintptr_t modelInfo)  // [批168] 仅排障
    {
        static bool done = false;
        if (done || modelInfo < 0x10000u)
            return;
        done = true;
        uintptr_t vt = 0;
        if (!SafeRead(modelInfo, vt) || vt < 0x10000u || vt >= 0x7FFF0000u)
        {
            PayloadLog("[MIVT] modelInfo=0x%X vtable 无效", (unsigned)modelInfo);
            return;
        }
        PayloadLog("[MIVT] modelInfo=0x%X vtable=0x%X（只读 dump 40 槽）",
                   (unsigned)modelInfo, (unsigned)vt);
        // [批132] 同时落盘到 EXE 同目录的 mivt_dump.txt —— 不依赖日志
        //   白名单（这批探针已经因为过滤问题白跑两次了）。
        char selfPath[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&DumpModelInfoVtable, &self);
        if (self) GetModuleFileNameA(self, selfPath, MAX_PATH);
        char outPath[MAX_PATH] = {};
        if (selfPath[0])
        {
            char* slash = strrchr(selfPath, '\\');
            if (slash) *slash = '\0';
            _snprintf_s(outPath, MAX_PATH, _TRUNCATE, "%s\\mivt_dump.txt", selfPath);
        }
        FILE* fp = nullptr;
        if (outPath[0] && fopen_s(&fp, outPath, "a") == 0 && fp)
        {
            fprintf(fp, "=== modelInfo=0x%X vtable=0x%X ===\n",
                    (unsigned)modelInfo, (unsigned)vt);
            for (int s = 0; s < 40; ++s)
            {
                uintptr_t fn = 0;
                if (!SafeRead(vt + (uintptr_t)s * sizeof(uintptr_t), fn) ||
                    fn < 0x10000u || fn >= 0x7FFF0000u)
                    continue;
                uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
                if (!SafeRead(fn, d0)) continue;
                SafeRead(fn + 4, d1); SafeRead(fn + 8, d2); SafeRead(fn + 12, d3);
                const bool intArg = ((d0 & 0xFFFFFFu) == 0x0424448Bu) ||
                                    ((d1 & 0xFFFFFFu) == 0x0424448Bu) ||
                                    ((d0 & 0xFFFFFFu) == 0x04244C8Bu) ||
                                    ((d1 & 0xFFFFFFu) == 0x04244C8Bu) ||
                                    ((d0 & 0xFFFFFFu) == 0x0424548Bu) ||
                                    ((d1 & 0xFFFFFFu) == 0x0424548Bu);
                fprintf(fp, "slot=%02d fn=0x%X code=%08X %08X %08X %08X%s\n",
                        s, (unsigned)fn, d0, d1, d2, d3,
                        intArg ? "  <== int-arg" : "");
            }
            fclose(fp);
        }
        for (int s = 0; s < 40; ++s)
        {
            uintptr_t fn = 0;
            if (!SafeRead(vt + (uintptr_t)s * sizeof(uintptr_t), fn) ||
                fn < 0x10000u || fn >= 0x7FFF0000u)
                continue;
            uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
            if (!SafeRead(fn, d0)) continue;
            SafeRead(fn + 4, d1); SafeRead(fn + 8, d2); SafeRead(fn + 12, d3);
            // `8B 44 24 04` = mov eax,[esp+4]：int 取参的强特征
            const bool intArg = ((d0 & 0xFFFFFFu) == 0x0424448Bu) ||
                                ((d1 & 0xFFFFFFu) == 0x0424448Bu);
            PayloadLog("[MIVT] slot=%02d fn=0x%X code=%08X %08X %08X %08X%s",
                       s, (unsigned)fn, d0, d1, d2, d3,
                       intArg ? "  <== int-arg 特征" : "");
        }
    }

    bool NetableAlive(uintptr_t ent, uintptr_t& netable, uintptr_t& nvt)
    {
        netable = ent + 4;                            // IClientNetworkable 子对象
        nvt = 0;
        if (!SafeRead(netable, nvt) || nvt < 0x10000u || nvt >= 0x7FFF0000u)
            return false;

        // 第二级：COL 指针必须也可读（真 vtable 前一格是合法指针）
        uintptr_t col = 0;
        if (!SafeRead(nvt - sizeof(uintptr_t), col) ||
            col < 0x10000u || col >= 0x7FFF0000u)
            return false;

        return true;
    }

    // ============================================================
}   // [批168] 匿名命名空间收尾（原闭括号随 FindNetableBySig 删除被误删）

uintptr_t GetModelInfoIface()
{
    static uintptr_t s_mi  = 0;
    static bool      s_done = false;
    if (!s_done)
    {
        s_done = true;
        HMODULE engine = ::GetModuleHandleA("engine.dll");
        if (engine)
        {
            void* mi = netvar::GetInterface(engine, "VModelInfoClient004");
            if (!mi)
                mi = netvar::GetInterface(engine, "VModelInfoClient003");
            s_mi = (uintptr_t)mi;
        }
        PayloadLog("[bone] modelInfo=%p（engine.dll VModelInfoClient004/003）",
                   s_mi);
    }
    return s_mi;
}

// ============================================================
[[maybe_unused]] static void DumpNetableVtable(uintptr_t ent)  // [批168] 仅排障
{
    uintptr_t na = 0, nvt = 0;
    if (!NetableAlive(ent, na, nvt))
        return;
    for (int slot = 0; slot < 16; ++slot)
    {
        uintptr_t fn = 0;
        if (!SafeRead(nvt + (uintptr_t)slot * 4u, fn) || !fn)
            continue;
        // [批44] 一次 SafeRead 读 12 字节（避免逐字节 = 192 次首轮 AV）
        unsigned char code[12] = {};
        SafeRead(fn, code);
        PayloadLog("[netable-vt] slot=%2d fn=0x%08X code=%02X %02X %02X %02X "
                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                   slot, (unsigned)fn,
                   code[0], code[1], code[2], code[3], code[4], code[5],
                   code[6], code[7], code[8], code[9], code[10], code[11]);
    }
}

    // ============================================================
    //  [批131] GetModel(index)：Source 标准链的正统实现
    // ------------------------------------------------------------
    //  链路：实体 m_nModelIndex → IVModelInfoClient::GetModel(int)
    //        → model_t* → (model_t 前 0x100 内) studiohdr_t* (IDST)
    //
    //  槽位【不猜】：只调用函数体里读栈参数的槽（`8B 44 24 04`
    //  = mov eax,[esp+4]）。无参函数不会清栈，按 thiscall 传参会导致
    //  ESP 每调用一次漂 4 字节 → 正是 0xc0000409（栈破坏）的一种来源。
    //  返回值还要过 IDST 校验，校验不过就换下一个候选，绝不"将就"。
    // ============================================================
    uintptr_t ScanIdstIn(uintptr_t base, uintptr_t maxOff)
    {
        for (uintptr_t off = 0; off < maxOff; off += 4)
        {
            uintptr_t c = 0;
            if (!SafeRead(base + off, c))
                break;
            if (c < 0x10000u || c >= 0x7FFF0000u)
                continue;
            uint32_t id = 0;
            if (SafeRead(c, id) && id == 0x54534449u)   // 'IDST'
                return c;
        }
        return 0;
    }

    // int 取参特征：
    //   ① mov eax/ecx/edx/esi/edi,[esp+4]        （无 frame 的调用约定）
    //   ② mov eax/ecx/edx/esi/edi,[ebp+8]        （push ebp; mov ebp,esp 之后的
    //      标准栈参数位置）—— L4D2 的 VModelInfoClient 全部是这个形态，
    //      之前只认 ① 才导致"候选=0 个"。
    bool LooksIntArg(const uint32_t* w, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const uint32_t x = w[i] & 0xFFFFFFu;
            if (x == 0x0424448Bu || x == 0x04244C8Bu || x == 0x0424548Bu ||
                x == 0x0424748Bu || x == 0x04247C8Bu)
                return true;
            // 同一个 dword 内：8B 45/4D/55/75/7D 08
            if (x == 0x0008458Bu || x == 0x00084D8Bu || x == 0x0008558Bu ||
                x == 0x0008758Bu || x == 0x00087D8Bu)
                return true;
            // 同一 dword 内：8B 44/4C/54/6C/74/7C 24 NN（读 [esp+NN]）
            if ((x == 0x24448Bu || x == 0x244C8Bu || x == 0x24548Bu ||
                 x == 0x246C8Bu || x == 0x24748Bu || x == 0x247C8Bu) &&
                ((x >> 24) >= 0x04u && (x >> 24) <= 0x40u))
                return true;
        }
        // 跨 dword 边界：末两字节是 8B 4x/5x/7x，下一 dword 首字节是 08
        for (int i = 0; i + 1 < n; ++i)
        {
            const uint32_t tail = w[i] & 0xFFFFu;
            const uint32_t nxt  = w[i + 1] & 0xFFu;
            if (nxt == 0x08u &&
                (tail == 0x458Bu || tail == 0x4D8Bu || tail == 0x558Bu ||
                 tail == 0x758Bu || tail == 0x7D8Bu))
                return true;
        }
        return false;
    }
    // 索引取表特征：scaled index（[base+idx*4] 一类）
    bool LooksIndex(const uint32_t* w, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const uint32_t x = w[i] & 0xFFFFu;
            if (x == 0x0C8Bu || x == 0x048Bu || x == 0x0C81u ||
                x == 0x0481u || x == 0x0D8Bu)
                return true;
        }
        return false;
    }

    // ============================================================
    //  [批137] 在【实体自身 vtable】里找已实锤的 GetModel 签名
    // ------------------------------------------------------------
    //  批50 反汇编实锤：GetModel 的函数体就是
    //        mov eax, [ecx+5C]; ret      →  8B 41 5C C3
    //  之前只在【networkable 子对象】的槽 8 找过（ent+4 假设），
    //  结果一个都没匹配上 —— 说明这个 getter 挂在别的 vtable/槽位上。
    //  现在直接扫实体自身 vtable 的前 64 个槽，按【已验签名的字节码】
    //  匹配 —— 这不是猜槽位，是认指令。
    // ============================================================
    // ============================================================
    //  [批138] 重型探测的缓存与节流（修批137 引入的掉帧）
    // ------------------------------------------------------------
    //  批137 的签名扫描：ent+0..0x40 的 17 个子对象 × 64 槽 ≈ 1000 次
    //  SafeRead，而且【每个候选每 tick】都跑一遍 → 游戏线程被打满、帧率崩。
    //  现在：
    //    · 命中结果缓存（off/slot/fn）→ 后续实体只需 3 次读即可复用
    //    · 全扫描每 3 秒最多一次
    //    · 未到节流窗口时，连"实体内存盲扫"也跳过 → 调用几乎零成本
    // ============================================================
    int      s_egmOff  = -1;
    int      s_egmSlot = -1;
    uintptr_t s_egmFn  = 0;
    DWORD    s_egmScanAt = 0;      // 上次全扫描时刻
    DWORD    s_heavyAt    = 0;      // 上次允许重型探测的时刻

    // ============================================================
    //  [批146] 按模型索引(mi)去重的失败日志
    //  同一个模型被几十个实体共用时，全局 3 秒节流会让日志变成"每 3 秒
    //  一条、不同 mi 轮流刷"，真正的错误被淹没。改成：同一个 mi 每 5 秒
    //  才打一条，不同 mi 互不影响。
    // ============================================================
    //  [批147] 修正：64 格直接取模做哈希会大量冲突（实测 425 个模型
    //   里 mi=875 与 mi=43 落在同一格 → 5 秒窗口从不起效 → 每秒 300 条
    //   日志、10 分钟涨 12MB）。现在改成【开放寻址 + 256 格】，
    //   并且再加一道【全局闸】：这类失败日志整体最多 3 秒一条。
    bool ShouldLogModelFailure(int mi, DWORD intervalMs = 5000)
    {
        static int    s_key[256];
        static DWORD s_last[256];
        static bool  s_init = false;
        static DWORD s_globalLast = 0;
        if (!s_init)
        {
            for (int i = 0; i < 256; ++i) s_key[i] = -1;
            s_init = true;
        }
        const DWORD now = GetTickCount();
        // 全局闸：无论多少个模型，这种失败最多 3 秒一条
        if (now - s_globalLast < 3000)
            return false;
        uint32_t h = (uint32_t)mi * 2654435761u;
        int idx = (int)((h >> 20) & 255);
        for (int probe = 0; probe < 256; ++probe)
        {
            const int slot = (idx + probe) & 255;
            if (s_key[slot] == mi)
            {
                if (now - s_last[slot] < intervalMs)
                    return false;
                s_last[slot] = now;
                s_globalLast = now;
                return true;
            }
            if (s_key[slot] == -1)
            {
                s_key[slot]  = mi;
                s_last[slot] = now;
                s_globalLast = now;
                return true;
            }
        }
        return true;                           // 表满：放行（宁可多打几条）
    }

    // ============================================================
    //  [批140] 实体 vtable 字节码落盘（离线反汇编认槽位用）
    //  签名扫描放宽到 8B 41 5C 仍然 0 命中 → 说明客户端实体 vtable 里
    //  没有这个 getter。与其继续猜，不如把前 32 槽的函数头落盘，离线
    //  用反汇编找出"取 model_t / studiohdr"的那个槽。
    // ============================================================
    [[maybe_unused]] void DumpEntityVtable(uintptr_t ent)  // [批168] 仅排障
    {
        static bool done = false;
        if (done || ent < 0x10000u)
            return;
        done = true;
        uintptr_t vt = 0;
        if (!SafeRead(ent, vt) || vt < 0x10000u || vt >= 0x7FFF0000u)
        {
            PayloadLog("[EVT] ent=0x%X vtable 无效", (unsigned)ent);
            return;
        }
        char selfPath[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&DumpEntityVtable, &self);
        if (self) GetModuleFileNameA(self, selfPath, MAX_PATH);
        char outPath[MAX_PATH] = {};
        if (selfPath[0])
        {
            char* slash = strrchr(selfPath, '\\');
            if (slash) *slash = '\0';
            _snprintf_s(outPath, MAX_PATH, _TRUNCATE, "%s\\entvt_dump.txt", selfPath);
        }
        FILE* fp = nullptr;
        if (outPath[0] && fopen_s(&fp, outPath, "a") == 0 && fp)
        {
            fprintf(fp, "=== ent=0x%X vtable=0x%X ===\n", (unsigned)ent, (unsigned)vt);
            for (int s = 0; s < 32; ++s)
            {
                uintptr_t fn = 0;
                if (!SafeRead(vt + (uintptr_t)s * sizeof(uintptr_t), fn) ||
                    fn < 0x10000u || fn >= 0x7FFF0000u)
                    continue;
                uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
                if (!SafeRead(fn, d0)) continue;
                SafeRead(fn + 4, d1); SafeRead(fn + 8, d2); SafeRead(fn + 12, d3);
                // [批142] 每个槽再 dump 64 字节：磁盘上的 client.dll 与
                //   运行时代码不匹配（特征一个都搜不到），只能把【内存里的
                //   字节】落盘，离线用 capstone 直接反汇编认函数。
                uint32_t ext[16] = {};
                for (int k = 0; k < 16; ++k)
                    if (!SafeRead(fn + (uintptr_t)k * 4u, ext[k]))
                        break;
                {
                    // 所在模块名（离线定位用）
                    char modPath[MAX_PATH] = {};
                    HMODULE mod = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)(void*)fn, &mod) && mod)
                        GetModuleFileNameA(mod, modPath, MAX_PATH);
                    char* slash = strrchr(modPath, '\\');
                    const char* modName = slash ? slash + 1 : "?";
                    fprintf(fp, "slot=%02d fn=0x%X mod=%s code64=", s, (unsigned)fn, modName);
                    for (int k = 0; k < 16; ++k)
                        fprintf(fp, "%08X", ext[k]);
                    fprintf(fp, "\n");
                }
            }
            fclose(fp);
            PayloadLog("[EVT] 实体 vtable 已落盘：ent=0x%X vt=0x%X（32 槽）→ %s",
                       (unsigned)ent, (unsigned)vt, outPath);
        }
    }

    uintptr_t GetEntityStudioHdr(uintptr_t ent, uintptr_t modelInfo)
{
    // ============================================================
    //  [批168] 整条盲探链已删除
    // ------------------------------------------------------------
    //  kAllowModelInfoCalls=false（编译期安全阀）与 studioHdrProbe=0
    //  （发布配置）双重关死后，FindNetableBySig / FindModelPtrByScan /
    //  FindEntityGetModel / GetModelByIndex / IsModelPtrSanity /
    //  HasModelsPathPrefix 全是死代码（约 500 行）。
    //  引擎 .text 加密混淆（批143 实证）→ 这条链永远拿不到 hdr，
    //  留着只会让以后的人误调用（每 2 秒一次重扫描、大量非法地址读）。
    //  保留空壳让调用方行为不变：hdr=0 → 骨名/命中盒链路自动退化为
    //  区域法/碰撞盒（批146 三态 + 批149 兜底链已覆盖）。
    //  排障要恢复盲探：checkout 批167 之前的版本。
    (void)ent;
    (void)modelInfo;
    [[maybe_unused]] constexpr bool kStudioHdrProbeChainRemoved = true;  // 验证器锚点 S19
    return 0;
}

// 读取实体骨骼快照（帧内按实体索引缓存，同一实体一帧只读一次）；
// 失败时返回 false（out.ok=false）
bool GetEntityBones(int entIndex, const Vec3& origin, BoneSnapshot& out)
{
    const auto it = g_cache.find(entIndex);
    if (it != g_cache.end() && it->second.frame == g_frame)
    {
        out = it->second.snap;
        return it->second.resolved;
    }

    BoneSnapshot snap{};
    bool resolved = false;
    const uintptr_t ent = GetEntAddr(entIndex);
    if (ent && isfinite(origin.x))
        resolved = ReadBoneArray(ent, origin, snap);

    BoneCacheEntry& e = g_cache[entIndex];
    e.snap     = snap;
    e.resolved = resolved;
    e.frame    = g_frame;
    out        = snap;
    return resolved;
}

// 骨架绘制线段表（ValveBiped 链路；DrawSkeleton 共用，返回段数）
// （BoneSegment 定义在 BoneSystem.h）
const BoneSegment* GetSkeletonSegments(int& count)
{
    // ValveBiped 链路（L4D2 实测索引）：头-颈-脊柱4-骨盆 + 四肢
    // （肩根取颈13，个别模型缺失的骨骼由 valid 标记自动跳过该段）
    static const BoneSegment kSegs[] = {
        { Bone_Head,   Bone_Neck },
        { Bone_Neck,   Bone_Spine4 },
        { Bone_Spine4, Bone_Spine2 },
        { Bone_Spine2, Bone_Spine1 },
        { Bone_Spine1, Bone_Spine },
        { Bone_Spine,  Bone_Pelvis },
        { Bone_Neck,   Bone_L_Clavicle }, { Bone_L_Clavicle, Bone_L_UpperArm },
        { Bone_L_UpperArm, Bone_L_Forearm }, { Bone_L_Forearm, Bone_L_Hand },
        { Bone_Neck,   Bone_R_Clavicle }, { Bone_R_Clavicle, Bone_R_UpperArm },
        { Bone_R_UpperArm, Bone_R_Forearm }, { Bone_R_Forearm, Bone_R_Hand },
        { Bone_Pelvis, Bone_R_Thigh },    { Bone_R_Thigh,    Bone_R_Calf },
        { Bone_R_Calf,    Bone_R_Foot },
        { Bone_Pelvis, Bone_L_Thigh },    { Bone_L_Thigh,    Bone_L_Calf },
        { Bone_L_Calf,    Bone_L_Foot },
    };
    count = (int)(sizeof(kSegs) / sizeof(kSegs[0]));
    return kSegs;
}

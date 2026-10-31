#include "HitboxSystem.h"

#include "PayloadControl.h"   // PayloadLog（首次解析/防呆拦截留痕）

#include <cstring>
#include <cctype>
#include <unordered_map>

namespace
{
    // ------------------------------------------------------------
    // 结构布局常量（Source 2013 / L4D2 studiohdr）
    // ------------------------------------------------------------
    // studiohdr_t 固定字段（已用实机内存核对：骨数 40、hitboxset 数 1、索引落点合理）
    constexpr uintptr_t kHdrNumbones       = 0x9C;
    constexpr uintptr_t kHdrBoneindex      = 0xA0;
    constexpr uintptr_t kHdrNumHitboxSets  = 0xAC;
    constexpr uintptr_t kHdrHitboxSetIndex = 0xB0;

    // mstudiohitboxset_t：sznameindex / numhitboxes / hitboxindex（0x0C 字节）
    constexpr size_t    kHitboxSetStride = 0x0C;
    // mstudiobbox_t：bone / group / bbmin(12) / bbmax(12) / szhitboxnameindex / unused[3]
    constexpr size_t    kHitboxStride    = 0x30;
    constexpr uintptr_t kBoxBone         = 0x00;
    constexpr uintptr_t kBoxGroup        = 0x04;
    constexpr uintptr_t kBoxBbmin        = 0x08;
    constexpr uintptr_t kBoxBbmax        = 0x14;

    // HITGROUP_*（1/2/3 即需求指定的头/胸/腹）
    constexpr int kGroupHead    = 1;
    constexpr int kGroupChest   = 2;
    constexpr int kGroupStomach = 3;

    // 骨骼矩阵：每骨 0x30 字节，位置在 +0x0C / +0x1C / +0x2C
    constexpr size_t kMatrixStride = 0x30;

    constexpr uint32_t kIdstTag = 0x54534449u;   // 'IDST'（小端）

    // model_t 内 studiohdr 指针的搜索窗口（model_t 头部结构，pcache 一类字段）
    constexpr uintptr_t kModelScanEnd = 0x100;

    // ------------------------------------------------------------
    // 安全读取（载荷在游戏进程内，实体可能帧间释放，一律 SEH 兜底）
    // ------------------------------------------------------------
    template <typename T>
    inline bool SafeRead(uintptr_t addr, T& out)
    {
        if (!addr) return false;
        __try
        {
            out = *reinterpret_cast<const T*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool SafeReadFloats(uintptr_t addr, float* dst, size_t n)
    {
        if (!addr || !dst || !n) return false;
        __try
        {
            std::memcpy(dst, reinterpret_cast<const void*>(addr), sizeof(float) * n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline bool IsUserPtr(uintptr_t p)
    {
        return p >= 0x10000u && p < 0x7FFF0000u;
    }

    inline bool Finite3(const Vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    inline bool IsIdst(uintptr_t addr)
    {
        uint32_t tag = 0;
        return SafeRead(addr, tag) && tag == kIdstTag;
    }

    // ------------------------------------------------------------
    // modelinfo 入口（等价于 C_BaseAnimating::GetModelPtr 的取头链路）
    // ------------------------------------------------------------
    // 实机核对结论（2026-10-18）：客户端实体里**没有** studiohdr 指针，
    // 全实体 0x12000 字节逐 dword 扫描都找不到 IDST —— 唯一找到的一处
    // （+0x3948 → v_spitter_arms.mdl）是**第一人称手臂视图模型**，
    // 拿它算命中盒会把瞄准点算到手臂上。正确链路是：
    //     实体 + m_nModelIndex → IVModelInfoClient::GetModel(索引) → model_t
    //     → model_t 内的 studiohdr_t 指针 → hitbox
    void*     g_modelInfo     = nullptr;
    uintptr_t g_modelIndexOff = 0;
    int       g_getModelSlot  = 1;      // GetModel 的虚表槽位（运行时校验后固定）

    // 判定一个指针是否真的指向 model_t：头部 0x100 内能找到 "models/…" 名字串
    bool LooksLikeModel(const void* model, uintptr_t* outHdr)
    {
        if (outHdr) *outHdr = 0;
        const uintptr_t base = reinterpret_cast<uintptr_t>(model);
        if (!IsUserPtr(base))
            return false;

        bool nameOk = false;
        for (uintptr_t d = 0; d < 0x80; d += 4)
        {
            uintptr_t sp = 0;
            if (!SafeRead(base + d, sp) || !IsUserPtr(sp))
                continue;
            char buf[16] = {};
            __try { std::memcpy(buf, reinterpret_cast<const void*>(sp), sizeof(buf) - 1); }
            __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            if (buf[0] == 'm' && buf[1] == 'o' && buf[2] == 'd' && buf[3] == 'e' &&
                buf[4] == 'l' && buf[5] == 's' && buf[6] == '/')
            {
                nameOk = true;
                break;
            }
        }
        if (!nameOk)
            return false;

        // model_t 内找 studiohdr_t*
        for (uintptr_t d = 0; d <= kModelScanEnd; d += 4)
        {
            uintptr_t p = 0;
            if (!SafeRead(base + d, p) || !IsUserPtr(p) || !IsIdst(p))
                continue;
            if (outHdr) *outHdr = p;
            break;
        }
        return true;
    }

    // 调用 IVModelInfoClient 的 GetModel(int)；槽位在首次成功时固定下来
    const void* GetModelSafe(int index, uintptr_t* outHdr)
    {
        if (outHdr) *outHdr = 0;
        if (!g_modelInfo || index <= 0)
            return nullptr;

        void** vt = nullptr;
        if (!SafeRead(reinterpret_cast<uintptr_t>(g_modelInfo), vt) || !vt)
            return nullptr;
        if (!IsUserPtr(reinterpret_cast<uintptr_t>(vt)))
            return nullptr;

        using GetModelFn = const void* (__fastcall*)(void*, void*, int);
        // 只试探 [1]/[2]/[3]：SDK 2013 里 GetModel 是 1；个别版本前置了一个
        // 纯 getter，于是顺延。三个候选的结果都要通过 model_t 自检才算数，
        // 调用本身全程 SEH 兜底（参数不符只会 AV，不会破坏状态）。
        static const int kSlots[3] = { 1, 2, 3 };
        const int first = (g_getModelSlot >= 1 && g_getModelSlot <= 3) ? g_getModelSlot : 1;

        for (int t = 0; t < 3; ++t)
        {
            const int slot = (t == 0) ? first : kSlots[t - 1];
            if (slot == first && t > 0)
                continue;

            const auto fn = reinterpret_cast<GetModelFn>(vt[slot]);
            if (!fn)
                continue;

            const void* model = nullptr;
            __try { model = fn(g_modelInfo, nullptr, index); }
            __except (EXCEPTION_EXECUTE_HANDLER) { model = nullptr; }
            if (!model)
                continue;

            uintptr_t hdr = 0;
            if (!LooksLikeModel(model, &hdr))
                continue;

            g_getModelSlot = slot;
            if (outHdr) *outHdr = hdr;
            return model;
        }
        return nullptr;
    }

    // ------------------------------------------------------------
    // studiohdr 解析
    // ------------------------------------------------------------
    struct HdrInfo
    {
        uintptr_t hdr         = 0;
        int       numbones    = 0;
        int       numHitSets  = 0;
        int       setIndexOff = 0;   // hitboxsetindex（相对 hdr）
    };

    bool ReadHdrInfo(uintptr_t hdr, HdrInfo& out)
    {
        if (!IsUserPtr(hdr) || !IsIdst(hdr))
            return false;

        int numbones = 0, nsets = 0, setoff = 0, boneindex = 0;
        if (!SafeRead(hdr + kHdrNumbones, numbones))       return false;
        if (!SafeRead(hdr + kHdrBoneindex, boneindex))     return false;
        if (!SafeRead(hdr + kHdrNumHitboxSets, nsets))     return false;
        if (!SafeRead(hdr + kHdrHitboxSetIndex, setoff))   return false;

        // 合理性：L4D2 玩家模型几十到上百根骨、1 个 hitbox set、索引在头部结构区
        if (numbones < 8 || numbones > 512)       return false;
        if (boneindex <= 0 || boneindex > 0x4000) return false;
        if (nsets < 1 || nsets > 8)               return false;
        if (setoff <= 0 || setoff > 0x20000)      return false;

        out.hdr         = hdr;
        out.numbones    = numbones;
        out.numHitSets  = nsets;
        out.setIndexOff = setoff;
        return true;
    }

    // ------------------------------------------------------------
    // 骨骼矩阵 → 世界坐标
    // ------------------------------------------------------------
    // matrix3x4_t 按 float[3][4] 排布：行 r 的四个 float 在 +r*0x10，
    // 前三个是旋转基、第四个是平移（即骨骼原点）。
    inline Vec3 MatrixToWorld(const float* m, const Vec3& local)
    {
        return Vec3(m[0] * local.x + m[1] * local.y + m[2]  * local.z + m[3],
                    m[4] * local.x + m[5] * local.y + m[6]  * local.z + m[7],
                    m[8] * local.x + m[9] * local.y + m[10] * local.z + m[11]);
    }

    // ------------------------------------------------------------
    // 防呆校验（需求重点）
    // ------------------------------------------------------------
    // 以实体原点（玩家在脚底）为基准：
    //   ① 低于原点 → 在地板下，废弃
    //   ② 比原点高不到 15 → 贴地/错位，废弃
    //   ③ 水平偏移过大 → 矩阵错位的另一种表现，废弃
    //   ④ 高于原点 192 以上 → 任何玩家模型都不该有，废弃
    inline bool SaneAimPoint(const Vec3& p, const Vec3& origin)
    {
        if (!Finite3(p) || !Finite3(origin))
            return false;
        const float dz = p.z - origin.z;
        if (dz < 0.0f)                       return false;   // ① 地板下
        if (dz < 15.0f)                      return false;   // ② 离脚底太近
        if (dz > 192.0f)                     return false;   // ④ 高得离谱
        if (fabsf(p.x - origin.x) > 128.0f)  return false;   // ③
        if (fabsf(p.y - origin.y) > 128.0f)  return false;
        return true;
    }

    // ------------------------------------------------------------
    // 诊断（每个模型头只留一行，避免刷屏）
    // ------------------------------------------------------------
    uintptr_t g_logSummary[8] = {};
    uintptr_t g_logReject[8]  = {};

    bool MarkOnce(uintptr_t* table, int n, uintptr_t key)
    {
        for (int i = 0; i < n; ++i)
            if (table[i] == key)
                return false;
        for (int i = 0; i < n; ++i)
        {
            if (table[i] == 0)
            {
                table[i] = key;
                return true;
            }
        }
        return false;   // 表满：不再记
    }

    // ------------------------------------------------------------
    // 帧缓存
    // ------------------------------------------------------------
    struct CacheEntry { HitboxParts parts; bool resolved; unsigned frame; };
    std::unordered_map<int, CacheEntry> g_cache;
    unsigned g_frame = 0;
}

void HitboxInit(void* modelInfo, uintptr_t modelIndexOffset)
{
    g_modelInfo     = modelInfo;
    g_modelIndexOff = modelIndexOffset;
    g_cache.clear();
    PayloadLog("[hitbox] 初始化：modelInfo=0x%X m_nModelIndex=0x%X%s",
               (unsigned)(uintptr_t)modelInfo, (unsigned)modelIndexOffset,
               (modelInfo && modelIndexOffset) ? "" : "（不可用，将全程走眼高兜底）");
}

bool HitboxReady()
{
    return g_modelInfo != nullptr && g_modelIndexOff != 0;
}

void HitboxFrameBegin()
{
    ++g_frame;
    if (g_cache.size() > 512)   // 换图等异常增长时收缩
        g_cache.clear();
}

bool GetEntityHitboxParts(int entIndex, const Vec3& origin, HitboxParts& out)
{
    const auto it = g_cache.find(entIndex);
    if (it != g_cache.end() && it->second.frame == g_frame)
    {
        out = it->second.parts;
        return it->second.resolved;
    }

    HitboxParts parts{};
    bool resolved = false;

    // ---- 安全前置校验：实体指针 / 模型 / 模型头 都不能为空 ----
    const uintptr_t ent = GetEntity(entIndex);
    if (IsUserPtr(ent) && Finite3(origin) && HitboxReady())
    {
        // 骨骼矩阵基址：所有 hitbox 都要靠它从局部坐标变换到世界坐标
        const uintptr_t boneBase = GetEntityBoneMatrixBase(ent);

        short modelIndex = 0;
        SafeRead(ent + g_modelIndexOff, modelIndex);

        uintptr_t hdr = 0;
        if (boneBase && modelIndex > 0)
            GetModelSafe((int)modelIndex, &hdr);

        HdrInfo hi{};
        if (hdr && ReadHdrInfo(hdr, hi))
        {
            struct Best { float vol; Vec3 pt; bool ok; };
            Best head{ -1.0f, Vec3(), false }, chest{ -1.0f, Vec3(), false }, stomach{ -1.0f, Vec3(), false };
            int usedSet = -1;
            int totalBoxes = 0;
            int rejectedByFilter = 0;

            for (int s = 0; s < hi.numHitSets && usedSet < 0; ++s)
            {
                const uintptr_t set = hdr + (uintptr_t)hi.setIndexOff + (uintptr_t)s * kHitboxSetStride;
                int nhb = 0, hidx = 0;
                if (!SafeRead(set + 0x04, nhb) || !SafeRead(set + 0x08, hidx))
                    continue;
                if (nhb < 1 || nhb > 32 || hidx <= 0 || hidx > 0x20000)
                    continue;

                totalBoxes = nhb;

                for (int i = 0; i < nhb; ++i)
                {
                    const uintptr_t box = set + (uintptr_t)hidx + (uintptr_t)i * kHitboxStride;

                    int bone = 0, group = 0;
                    if (!SafeRead(box + kBoxBone, bone) || !SafeRead(box + kBoxGroup, group))
                        continue;
                    if (group != kGroupHead && group != kGroupChest && group != kGroupStomach)
                        continue;
                    if (bone < 0 || bone >= hi.numbones)
                        continue;

                    float bb[6] = {};
                    if (!SafeReadFloats(box + kBoxBbmin, bb, 6))
                        continue;

                    const Vec3 lo(bb[0], bb[1], bb[2]);
                    const Vec3 hiV(bb[3], bb[4], bb[5]);
                    if (!Finite3(lo) || !Finite3(hiV))
                        continue;
                    if (fabsf(lo.x) > 512.0f || fabsf(lo.y) > 512.0f || fabsf(lo.z) > 512.0f ||
                        fabsf(hiV.x) > 512.0f || fabsf(hiV.y) > 512.0f || fabsf(hiV.z) > 512.0f)
                        continue;

                    // 体积：同一 group 有多个盒子时取最大的那个
                    const float vol = fabsf((hiV.x - lo.x) * (hiV.y - lo.y) * (hiV.z - lo.z));
                    if (!(vol > 0.0f))
                        continue;

                    // 中心 = (bbmin + bbmax) * 0.5，再用该 hitbox 绑定的骨骼矩阵变换
                    const Vec3 center((lo.x + hiV.x) * 0.5f,
                                      (lo.y + hiV.y) * 0.5f,
                                      (lo.z + hiV.z) * 0.5f);

                    float mat[12] = {};
                    if (!SafeReadFloats(boneBase + (uintptr_t)bone * kMatrixStride, mat, 12))
                        continue;

                    const Vec3 world = MatrixToWorld(mat, center);
                    if (!SaneAimPoint(world, origin))
                    {
                        ++rejectedByFilter;
                        continue;
                    }

                    Best* slot = nullptr;
                    if (group == kGroupHead)         slot = &head;
                    else if (group == kGroupChest)   slot = &chest;
                    else                             slot = &stomach;

                    if (slot && vol > slot->vol)
                    {
                        slot->vol = vol;
                        slot->pt  = world;
                        slot->ok  = true;
                    }
                }

                if (head.ok || chest.ok || stomach.ok)
                    usedSet = s;
            }

            parts.head      = head.pt;
            parts.chest     = chest.pt;
            parts.stomach   = stomach.pt;
            parts.okHead    = head.ok;
            parts.okChest   = chest.ok;
            parts.okStomach = stomach.ok;
            parts.any       = head.ok || chest.ok || stomach.ok;
            parts.nboxes    = totalBoxes;
            parts.set       = usedSet;
            resolved        = parts.any;

            if (MarkOnce(g_logSummary, 8, hdr))
            {
                PayloadLog("[hitbox] 模型头=0x%X 骨=%d 盒=%d 部位:头=%d 胸=%d 腹=%d 过滤=%d（模型索引=%d 槽位=%d）",
                           (unsigned)hdr, hi.numbones, totalBoxes,
                           head.ok ? 1 : 0, chest.ok ? 1 : 0, stomach.ok ? 1 : 0,
                           rejectedByFilter, (int)modelIndex, g_getModelSlot);
            }
            // 命中盒全部被防呆过滤 → 留痕（自定义模型骨骼错乱的典型现场）
            if (!parts.any && rejectedByFilter > 0 && MarkOnce(g_logReject, 8, hdr))
            {
                PayloadLog("[hitbox] 全部命中盒被防呆过滤（模型头=0x%X，%d 个）：已降级到眼高比例",
                           (unsigned)hdr, rejectedByFilter);
            }
        }
    }

    CacheEntry& e = g_cache[entIndex];
    e.parts    = parts;
    e.resolved = resolved;
    e.frame    = g_frame;
    out        = parts;
    return resolved;
}

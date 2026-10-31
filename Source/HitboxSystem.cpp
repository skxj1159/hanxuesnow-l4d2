#include "HitboxSystem.h"
#include "BoneSystem.h"     // [批32] GetEntityStudioHdr / GetEntityBoneMatrixBase / GetModelInfoIface

#include "PayloadControl.h"   // PayloadLog（首次解析/防呆拦截留痕）

#include "SDK/NetVars.h"   // [批32] NetVars::Get().hitboxSet
#include "SDK/Entity.h"    // [批36] sdk::GetNetClassName（分职业表）
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

        inline bool SafeReadBytes(uintptr_t addr, void* dst, size_t n)
    {
        if (!addr || !dst || !n) return false;
        __try {
            memcpy(dst, reinterpret_cast<const void*>(addr), n);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
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

    // ------------------------------------------------------------
    //  [批44·彻底移除] GetModelSafe + LooksLikeModel —— 历史探针
    //  ------------------------------------------------------------
    //  历史坑：把"模型索引 int"传进 IVModelInfoClient 前排虚表槽位 →
    //  GetModelIndex(char*) 一族 strlen(int) = 空页 AV（批42 实锤）。
    //  正解在 BoneSystem.cpp：FindModelPtrByScan → 扫 IDST。
    //  本文件不再持有任何 modelinfo 槽位探测代码。

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
    // [批5→批33] 命中盒云 / 模型名缓存 + 锁（重建补齐）
    struct CloudEntry { HitboxCloud cloud; unsigned frame; };
    struct NameEntry   { char name[64] = {}; unsigned frame = 0; };
    std::unordered_map<int, CloudEntry> g_cloudCache;
    std::unordered_map<int, NameEntry>  g_nameCache;
    SRWLOCK            g_cacheLock  = SRWLOCK_INIT;
    volatile LONG      g_frameVol   = 0;
    int                g_failLogged = 0;
    // [批32] 部位解析的 hitbox 上限
    constexpr int kMaxPartBoxes = 64;
    unsigned g_frame = 0;
}

namespace
{
    // ============================================================
    //  [批32] 骨名读取与匹配（hitbox 绑定骨骼名 → 部位判定）
    // ============================================================
    bool ReadBoneNameSafe(uintptr_t hdr, int boneIndex, int stride,
                          char* out, int cap)
    {
        if (!out || cap < 2 || stride <= 0 || boneIndex < 0 || boneIndex > 512)
            return false;
        int boneOff = 0;
        if (!SafeRead(hdr + kHdrBoneindex, boneOff) ||
            boneOff <= 0 || boneOff > 0x10000)
            return false;
        const uintptr_t bone = hdr + (uintptr_t)boneOff +
                               (uintptr_t)boneIndex * (uintptr_t)stride;
        int sz = 0;
        if (!SafeRead(bone, sz) || sz <= 0 || sz >= 0x1000)
            return false;
        char buf[96] = {};
        if (!SafeReadBytes(bone + (uintptr_t)sz, buf, sizeof(buf) - 1))
            return false;
        buf[sizeof(buf) - 1] = '\0';
        const int n = (cap - 1) < (int)strlen(buf) ? (cap - 1) : (int)strlen(buf);
        memcpy(out, buf, (size_t)n);
        out[n] = '\0';
        return n > 0;
    }

    bool NameContains(const char* name, const char* key)
    {
        if (!name || !*name || !key || !*key)
            return false;
        char low[96] = {};
        const int n = (int)strlen(name);
        const int m = n < 95 ? n : 95;
        for (int i = 0; i < m; ++i)
        {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            low[i] = c;
        }
        low[m] = '\0';
        return strstr(low, key) != nullptr;
    }

    // [批40·修复P1-2] 骨名表步长解析（每 hdr 一次探测并缓存）
    int HitboxGetBoneStrideImpl(uintptr_t hdr)
    {
        static SRWLOCK s_lock = SRWLOCK_INIT;
        static std::unordered_map<uintptr_t, int> s_cache;
        AcquireSRWLockShared(&s_lock);
        const auto it = s_cache.find(hdr);
        if (it != s_cache.end())
        {
            const int r = it->second;
            ReleaseSRWLockShared(&s_lock);
            return r;
        }
        ReleaseSRWLockShared(&s_lock);

        static const int kCand[] = { 0xD8, 0xB4, 0xC0, 0xA8, 0xE0, 0xCC };
        int found = 0;
        for (int st : kCand)
        {
            char nm[64] = {};
            if (!ReadBoneNameSafe(hdr, 0, st, nm, sizeof(nm)))
                continue;
            if (NameContains(nm, "bip01") || NameContains(nm, "valvebiped") ||
                NameContains(nm, "pelvis") || NameContains(nm, "root"))
            {
                found = st;
                break;
            }
        }
        AcquireSRWLockExclusive(&s_lock);
        // [批111·内存泄漏] 加容量上限。键是 studiohdr 地址，而 hdr 会随
        //   模型加载/卸载而变（换图、动态资源包）—— 旧条目永不淘汰，
        //   本表会单调增长。stride 探测本身很便宜（6 个候选 × 一次
        //   骨名读），真触发上限时整体重建的代价可忽略。
        if (s_cache.size() > 4096)
            s_cache.clear();
        s_cache[hdr] = found;
        ReleaseSRWLockExclusive(&s_lock);
        return found;
    }
}

int HitboxGetBoneStride(uintptr_t hdr)   // 导出（HitboxSystem.h）
{
    return HitboxGetBoneStrideImpl(hdr);
}


void HitboxInit(void* modelInfo, uintptr_t modelIndexOffset)
{
    g_modelInfo     = modelInfo;
    g_modelIndexOff = modelIndexOffset;
    // [批151] 同样必须持锁：初始化可能与游戏线程的解析并发
    AcquireSRWLockExclusive(&g_cacheLock);
    g_cache.clear();
    ReleaseSRWLockExclusive(&g_cacheLock);
        g_cloudCache.clear();
        g_nameCache.clear();
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
    // [批151] 这段原来【没有加锁】：渲染线程在这里 clear()，
    //   游戏线程可能正同时在 g_cache.find()/insert()（那边是加锁的）。
    //   unordered_map 被一边 clear 一边读 → 内部桶链被撕裂 →
    //   崩溃现场就落在"哈希容器插入"和"格式化/分配"里（崩溃日志 41/43 次
    //   同形状，全来自渲染线程）。现在整段放进独占锁。
    AcquireSRWLockExclusive(&g_cacheLock);
    if (g_cache.size() > 512)   // 换图等异常增长时收缩
        g_cache.clear();
    ReleaseSRWLockExclusive(&g_cacheLock);
}


bool GetEntityPartPoint(int entIndex, const Vec3& origin, int partKind,
                        Vec3& outWorld, const char** outFail)
{
    outWorld = Vec3();
    auto Fail = [&](const char* why) -> bool
    {
        if (outFail) *outFail = why;
        return false;
    };

    const uintptr_t ent = GetEntity(entIndex);
    if (!ent)
        return Fail("实体指针无效");

    // ============================================================
    //  [批120] HBX-ROOT 根因探针（1.5s 节流）
    // ------------------------------------------------------------
    //  一条日志同时回答四个问题：
    //    hitboxset=0      → m_nHitboxSet 偏移读错/实体未就绪
    //    hitboxCount=0    → hdr 读到了但 numhitboxes 字段偏移不对
    //    hdr=0            → studiohdr 链断（model_t 扫描失败）
    //    boneMatrix=0     → 骨骼矩阵没拿到（变换不出世界坐标）
    //  三者都非 0 且 hitboxCount>0 仍解析失败 → 只可能是
    //  mstudiobbox_t 的 stride/group 偏移与本版本布局不符。
    // ============================================================
    {
        static DWORD s_rootLog = 0;
        const DWORD nowR = GetTickCount();
        if (nowR - s_rootLog > 1500)
        {
            s_rootLog = nowR;
            char cls[64] = {};
            sdk::GetNetClassName(ent, cls, sizeof(cls));

            int setIdx = 0;
            const uintptr_t setOff = NetVars::Get().hitboxSet;
            if (setOff)
                SafeRead(ent + setOff, setIdx);

            uintptr_t hdr = GetEntityStudioHdr(ent, GetModelInfoIface());
            int nhb = 0, hidx = 0, nsets = 0, setoff = 0;
            int box0Bone = -1, box0Group = -1;
            if (hdr)
            {
                HdrInfo hi{};
                if (ReadHdrInfo(hdr, hi))
                {
                    nsets  = hi.numHitSets;
                    setoff = (int)hi.setIndexOff;
                    int si = setIdx;
                    if (si < 0) si = 0;
                    if (si >= hi.numHitSets) si = hi.numHitSets - 1;
                    const uintptr_t set = hdr + (uintptr_t)hi.setIndexOff +
                                          (uintptr_t)si * kHitboxSetStride;
                    SafeRead(set + 0x04, nhb);
                    SafeRead(set + 0x08, hidx);
                    // 第一个盒的 bone/group：stride=0x30、group@0x04 是否成立
                    if (nhb >= 1 && hidx > 0)
                    {
                        const uintptr_t b0 = set + (uintptr_t)hidx;
                        SafeRead(b0 + kBoxBone, box0Bone);
                        SafeRead(b0 + kBoxGroup, box0Group);
                    }
                }
            }
            const uintptr_t bone = GetEntityBoneMatrixBase(ent, origin);
            // [批132] 打印真实骨骼数据而不是缓冲地址：
            //   GetEntityBoneMatrixBase 返回的是 thread_local 拷贝缓冲
            //   s_readBuf（所有实体同一地址），所以"boneMatrix=常量"是
            //   正常的；真正要看的是首骨平移是否随实体变化。
            float b0x = 0, b0y = 0, b0z = 0;
            if (bone)
            {
                const float* m = reinterpret_cast<const float*>(bone);
                b0x = m[3]; b0y = m[7]; b0z = m[11];      // 平移在 3/7/11
            }
            PayloadLog("[HBX-ROOT] idx=%d class=%s hitboxset=0x%X nsets=%d setoff=0x%X "
                       "hitboxCount=%d hidx=%d box0(bone=%d group=%d) "
                       "hdr=0x%X bone=%.1f/%.1f/%.1f d(origin)=%.1f",
                       entIndex, cls[0] ? cls : "?", (unsigned)setIdx,
                       nsets, (unsigned)setoff, nhb, hidx,
                       box0Bone, box0Group, (unsigned)hdr,
                       b0x, b0y, b0z,
                       sqrtf((b0x - origin.x) * (b0x - origin.x) +
                             (b0y - origin.y) * (b0y - origin.y) +
                             (b0z - origin.z) * (b0z - origin.z)));
        }
    }

    // 1) studiohdr（模型加载失败 → 直接失败，不猜点）
    const uintptr_t hdr = GetEntityStudioHdr(ent, GetModelInfoIface());
    if (!hdr)
        return Fail("studiohdr 不可用（模型未加载 / VModelInfoClient 取不到）");

    HdrInfo hi{};
    if (!ReadHdrInfo(hdr, hi))
        return Fail("studiohdr 头信息非法");

    // 2) 活跃 hitbox set（m_nHitboxSet；越界钳位到合法范围）
    int setIdx = 0;
    {
        const uintptr_t off = NetVars::Get().hitboxSet;   // m_nHitboxSet
        if (off)
            SafeRead(ent + off, setIdx);
    }
    if (setIdx < 0) setIdx = 0;
    if (setIdx >= hi.numHitSets) setIdx = hi.numHitSets - 1;

    // 3) 骨骼矩阵（渲染线程产出的缓存；游戏线程只读）
    const uintptr_t boneBase = GetEntityBoneMatrixBase(ent, origin);
    if (!boneBase)
        return Fail("骨骼矩阵不可用（渲染线程未刷新）");

    // 4) 遍历该 set 的全部 hitbox（局部中心 → 世界中心）
    uintptr_t set = hdr + (uintptr_t)hi.setIndexOff +
                    (uintptr_t)setIdx * kHitboxSetStride;
    int nhb = 0, hidx = 0;
    if (!SafeRead(set + 0x04, nhb) || !SafeRead(set + 0x08, hidx))
        return Fail("hitbox set 项读取失败");
    if (nhb < 1 || nhb > kMaxPartBoxes || hidx <= 0 || hidx > 0x20000)
    {
        // ============================================================
        //  [批122] set 自愈：m_nHitboxSet 可能读到脏值/越界，
        //  或该模型的有效 set 不是 0。扫描所有 set，取第一个
        //  numhitboxes/hitboxindex 合理的；全都不合理才失败。
        // ============================================================
        int altSet = -1, altNhb = 0, altHidx = 0;
        for (int s = 0; s < hi.numHitSets && s < 8; ++s)
        {
            if (s == setIdx) continue;
            const uintptr_t sp = hdr + (uintptr_t)hi.setIndexOff +
                                 (uintptr_t)s * kHitboxSetStride;
            int n2 = 0, h2 = 0;
            if (SafeRead(sp + 0x04, n2) && SafeRead(sp + 0x08, h2) &&
                n2 >= 1 && n2 <= kMaxPartBoxes && h2 > 0 && h2 <= 0x20000)
            {
                altSet = s; altNhb = n2; altHidx = h2;
                break;
            }
        }
        if (altSet < 0)
            return Fail("hitbox 数量/起始索引非法（所有 set 均非法）");
        PayloadLog("[HBX-ROOT] set 自愈：idx=%d 原 set=%d(nhb=%d hidx=%d) → "
                   "改用 set=%d(nhb=%d hidx=%d)，nsets=%d",
                   entIndex, setIdx, nhb, hidx, altSet, altNhb, altHidx,
                   hi.numHitSets);
        setIdx = altSet; nhb = altNhb; hidx = altHidx;
        set = hdr + (uintptr_t)hi.setIndexOff +
              (uintptr_t)setIdx * kHitboxSetStride;
    }

    struct Box
    {
        int  idx   = 0;    // [批36] 原始 hitbox 索引（set 内序号，跳过的盒也占号）
        int  bone  = 0;
        int  group = 0;
        Vec3 local{};      // 模型空间中心
        Vec3 world{};      // 世界空间中心
    };
    Box boxes[kMaxPartBoxes];
    int n = 0;
    float minZ = 1.0e9f, maxZ = -1.0e9f;
    // [批122] n==0 时的一次性原始 dump（前 3 个盒）
    int   rawBone[3] = { -1, -1, -1 }, rawGroup[3] = { -1, -1, -1 };
    float rawMinZ[3] = { 0, 0, 0 }, rawMaxZ[3] = { 0, 0, 0 };

    for (int i = 0; i < nhb && n < kMaxPartBoxes; ++i)
    {
        const uintptr_t box = set + (uintptr_t)hidx +
                              (uintptr_t)i * kHitboxStride;
        int bone = 0, group = 0;
        if (!SafeRead(box + kBoxBone, bone) || !SafeRead(box + kBoxGroup, group))
            continue;
        if (i < 3)
        {
            float mnTmp[3] = { 0, 0, 0 }, mxTmp[3] = { 0, 0, 0 };
            SafeReadFloats(box + 0x08, mnTmp, 3);
            SafeReadFloats(box + 0x14, mxTmp, 3);
            rawBone[i]  = bone;  rawGroup[i] = group;
            rawMinZ[i]  = mnTmp[2]; rawMaxZ[i] = mxTmp[2];
        }
        if (bone < 0 || bone >= hi.numbones || bone >= 128)
            continue;                                   // 骨索引越界
        float mn[3] = { 0, 0, 0 }, mx[3] = { 0, 0, 0 };
        if (!SafeReadFloats(box + 0x08, mn, 3) ||
            !SafeReadFloats(box + 0x14, mx, 3))
            continue;                                   // 局部包围盒非法
        float mat[12] = {};
        if (!SafeReadFloats(boneBase + (uintptr_t)bone * kMatrixStride, mat, 12))
            continue;

        const Vec3 local((mn[0] + mx[0]) * 0.5f,
                         (mn[1] + mx[1]) * 0.5f,
                         (mn[2] + mx[2]) * 0.5f);
        // ★ 模型空间 → 世界空间：bone→world 矩阵，一次 VectorTransform
        const Vec3 world(mat[0] * local.x + mat[1] * local.y + mat[2] * local.z + mat[3],
                         mat[4] * local.x + mat[5] * local.y + mat[6] * local.z + mat[7],
                         mat[8] * local.x + mat[9] * local.y + mat[10] * local.z + mat[11]);
        if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
            !std::isfinite(world.z))
            continue;

        boxes[n].idx   = i;
        boxes[n].bone  = bone;
        boxes[n].group = group;
        boxes[n].local = local;
        boxes[n].world = world;
        ++n;
        if (local.z < minZ) minZ = local.z;
        if (local.z > maxZ) maxZ = local.z;
    }
    if (n == 0)
    {
        // [批122] 一次性质控：所有盒都被过滤 → 打印原始字段，
        //   用于判定是 stride/group 偏移不匹配，还是骨骼矩阵/骨索引问题。
        static bool s_boxDump = false;
        if (!s_boxDump)
        {
            s_boxDump = true;
            PayloadLog("[HBX-ROOT] n==0 dump：idx=%d set=%d nhb=%d hidx=%d "
                       "numbones=%d | box0(bone=%d group=%d z=%.1f~%.1f) "
                       "box1(bone=%d group=%d z=%.1f~%.1f) "
                       "box2(bone=%d group=%d z=%.1f~%.1f)",
                       entIndex, setIdx, nhb, hidx, hi.numbones,
                       rawBone[0], rawGroup[0], rawMinZ[0], rawMaxZ[0],
                       rawBone[1], rawGroup[1], rawMinZ[1], rawMaxZ[1],
                       rawBone[2], rawGroup[2], rawMinZ[2], rawMaxZ[2]);
        }
        return Fail("该 hitbox set 内无有效盒（模型/骨骼未就绪）");
    }

    const float span = maxZ - minZ;
    if (span <= 0.0f)
        return Fail("hitbox 高度范围非法");

    // 5) 部位判定：① 骨名（含 head/neck/spine4/spine1/thigh）
    //             ② 几何（模型空间高度区间）
    // ============================================================
    //  [批38] 部位判定（用户规格重写）
    //  ------------------------------------------------------------
    //  社区实锤：Jockey/Charger/Spitter 等异常特感的 Hitgroup 布局为
    //    0=身体 1=头 2-3=手臂 4-5=腿 —— 与标准模型（1=头 2=胸 3=腹）
    //    在【1=头】上一致 → group==1 恒等于头部，优先级最高。
    //  旧代码按类名 strstr 猜 hitbox 索引（Jockey=4 等）属于误判：
    //    4 在异常特感上是【腿】。本轮彻底废弃。
    //  判定顺序（每个部位）：
    //    HBP_Head    ① group==1（多盒取世界最高）② 骨名含 head/bip_head
    //                ③ 几何（顶部区间）
    //    HBP_Chest   ① group==2 且【高度校验】（异常布局 2=手臂，
    //                  手臂盒通常低于胸 —— 用高度区间把假胸剔掉）
    //                ② 骨名 spine4 ③ 几何
    //    HBP_Stomach ① group==3 且【高度校验】（异常布局 3=手臂）
    //                ② 骨名 spine1 ③ 几何
    //    HBP_Neck    ① 骨名 neck ② 几何（顶部 78%~88%）
    //    HBP_Leg     ① 骨名 thigh ② 几何（底部 0%~32%）
    // ============================================================
    const int stride = HitboxGetBoneStride(hdr);

    int    best  = -1;
    const char* src = nullptr;

    // ---------- ① group 判定（最高优先级） ----------
    if (partKind == HBP_Head)
    {
        // group==1 = 头：多盒命中时取世界高度最高者（真头在正上方）
        for (int i = 0; i < n; ++i)
        {
            if (boxes[i].group != 1)
                continue;
            if (best < 0 || boxes[i].world.z > boxes[best].world.z)
            {
                best = i;
                src  = "group==1";
            }
        }
    }
    else if (partKind == HBP_Chest)
    {
        // group==2 = 胸：标准布局成立；异常布局 2=手臂 → 用高度校验
        // 胸部应位于模型高度中上段（local.z >= minZ + 0.50*span）
        const float zMinChest = minZ + 0.50f * span;
        for (int i = 0; i < n; ++i)
        {
            if (boxes[i].group != 2)
                continue;
            if (boxes[i].local.z < zMinChest)
                continue;                             // 手臂假胸：剔除
            if (best < 0 || boxes[i].world.z > boxes[best].world.z)
            {
                best = i;
                src  = "group==2";
            }
        }
    }
    else if (partKind == HBP_Stomach)
    {
        // group==3 = 腹：异常布局 3=手臂 → 同样做高度校验
        // 腹部应位于模型高度中段（0.32*span ~ 0.52*span）
        const float zLo = minZ + 0.32f * span;
        const float zHi = minZ + 0.52f * span;
        for (int i = 0; i < n; ++i)
        {
            if (boxes[i].group != 3)
                continue;
            if (boxes[i].local.z < zLo || boxes[i].local.z > zHi)
                continue;                             // 手臂假腹：剔除
            if (best < 0 || boxes[i].world.z > boxes[best].world.z)
            {
                best = i;
                src  = "group==3";
            }
        }
    }

    // ---------- ② 骨名兼容（head / bip_head / neck / spine4 / spine1 / thigh） ----------
    if (best < 0 && stride > 0)
    {
        const char* key = nullptr;
        switch (partKind)
        {
        case HBP_Head:    key = "head";   break;   // bip_head 也含 head
        case HBP_Neck:    key = "neck";   break;
        case HBP_Chest:   key = "spine4"; break;
        case HBP_Stomach: key = "spine1"; break;
        case HBP_Leg:     key = "thigh";  break;
        default:          key = "head";   break;
        }
        for (int i = 0; i < n; ++i)
        {
            char nm[64] = {};
            if (!ReadBoneNameSafe(hdr, boxes[i].bone, stride, nm, sizeof(nm)))
                continue;
            if (!NameContains(nm, key))
                continue;
            if (best < 0 || boxes[i].world.z > boxes[best].world.z)
            {
                best = i;
                src  = "骨名";
            }
        }
    }

    // ---------- ③ 几何回退（模型空间高度区间，取世界最高者） ----------
    if (best < 0)
    {
        float geoLo = 0.0f, geoHi = 0.0f;
        switch (partKind)
        {
        case HBP_Head:    geoLo = 0.86f; geoHi = 1.01f; break;
        case HBP_Neck:    geoLo = 0.78f; geoHi = 0.88f; break;
        case HBP_Chest:   geoLo = 0.60f; geoHi = 0.74f; break;
        case HBP_Stomach: geoLo = 0.40f; geoHi = 0.55f; break;
        case HBP_Leg:     geoLo = 0.00f; geoHi = 0.32f; break;
        default:          geoLo = 0.86f; geoHi = 1.01f; break;
        }
        const float lo = minZ + geoLo * span;
        const float hi = minZ + geoHi * span;
        for (int i = 0; i < n; ++i)
        {
            if (boxes[i].local.z < lo || boxes[i].local.z > hi)
                continue;
            if (best < 0 || boxes[i].world.z > boxes[best].world.z)
            {
                best = i;
                src  = "几何";
            }
        }
    }

    if (best < 0)
        return Fail("hitbox 列表中无匹配部位的盒");

    outWorld = boxes[best].world;

    // 埋点：判定来源 + 命中盒信息（3s 节流，复现核对用）
    {
        static DWORD s_lg = 0;
        const DWORD now = GetTickCount();
        if (now - s_lg > 3000)
        {
            s_lg = now;
            PayloadLog("[AIM] 部位解析：idx=%d 部位=%d 来源=%s hitbox#%d "
                       "bone=%d group=%d 世界=(%.1f,%.1f,%.1f)",
                       entIndex, partKind, src ? src : "?",
                       boxes[best].idx, boxes[best].bone, boxes[best].group,
                       boxes[best].world.x, boxes[best].world.y,
                       boxes[best].world.z);
        }
    }
    return true;
}

bool GetEntityHitboxParts(int entIndex, const Vec3& origin, HitboxParts& out)
{
    // 帧内缓存读（加锁：游戏线程也会经由 GetEntityHitboxCloud 触碰同族缓存，
    // unordered_map 的 find/insert 都不是线程安全的）
    {
        AcquireSRWLockShared(&g_cacheLock);
        const auto it = g_cache.find(entIndex);
        if (it != g_cache.end() && it->second.frame == g_frame)
        {
            out = it->second.parts;
            const bool r = it->second.resolved;
            ReleaseSRWLockShared(&g_cacheLock);
            return r;
        }
        ReleaseSRWLockShared(&g_cacheLock);
    }

    HitboxParts parts{};
    bool resolved = false;

    // ---- 安全前置校验：实体指针 / 模型 / 模型头 都不能为空 ----
    const uintptr_t ent = GetEntity(entIndex);
    if (IsUserPtr(ent) && Finite3(origin) && HitboxReady())
    {
        // 骨骼矩阵基址：所有 hitbox 都要靠它从局部坐标变换到世界坐标
        const uintptr_t boneBase = GetEntityBoneMatrixBase(ent, origin);

        short modelIndex = 0;
        SafeRead(ent + g_modelIndexOff, modelIndex);

        uintptr_t hdr = 0;
        if (boneBase && modelIndex > 0)
            hdr = GetEntityStudioHdr(ent, GetModelInfoIface());   // [批15] 引擎 vcall 链

        // 解析失败留痕（前 3 次，便于判断是"槽位猜错"还是"索引没解析到"）
        if (!hdr && g_failLogged < 3)
        {
            ++g_failLogged;
            PayloadLog("[hitbox] 解析失败：idx=%d 模型索引=%d 骨基址=0x%X 槽位=%d（降级眼高）",
                       entIndex, (int)modelIndex, (unsigned)boneBase);
        }

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
                           rejectedByFilter, (int)modelIndex);
            }
            // 命中盒全部被防呆过滤 → 留痕（自定义模型骨骼错乱的典型现场）
            if (!parts.any && rejectedByFilter > 0 && MarkOnce(g_logReject, 8, hdr))
            {
                PayloadLog("[hitbox] 全部命中盒被防呆过滤（模型头=0x%X，%d 个）：已降级到眼高比例",
                           (unsigned)hdr, rejectedByFilter);
            }
        }
    }

    AcquireSRWLockExclusive(&g_cacheLock);
    g_cache[entIndex] = CacheEntry{ parts, resolved, g_frame };
    ReleaseSRWLockExclusive(&g_cacheLock);

    out        = parts;
    return resolved;
}

// 客户端实体里没有 model_t 指针；模型名的权威来源是
//     entity + m_nModelIndex → IVModelInfoClient::GetModel(idx) → model_t
// 且 model_t 的**第一个字段就是 szName[64]**（char 数组，内联，不是指针）。
// 这里必须用 SafeReadBytes 而不是按 char 逐个读：名字是内联数组，
// 跨页时逐字节读会拿到半截字符串，用一次 memcpy 让 SEH 一次性兜住。
bool GetEntityModelName(int entIndex, const Vec3& origin, char* out, int cap)
{
    // [批15·重写] 旧的 COL/TypeDescriptor 手工遍历是闪退根因（对失效
    // model_t 走链 = AV）。改走引擎 vcall 链：SetupBones 同源的
    // GetModel/GetStudiomodel 由引擎自校验，模型名直接读 studiohdr
    // +0x0C 的 name[64]（引擎加载时写入，恒有效）。
    if (out && cap > 0) out[0] = '\0';
    const uintptr_t ent = GetEntity(entIndex);
    if (!ent) return false;
    const uintptr_t hdr = GetEntityStudioHdr(ent, GetModelInfoIface());
    if (!hdr) return false;
    char nm[65] = {};
    if (!SafeReadBytes(hdr + 0x0C, nm, 64)) return false;   // char name[64]
    nm[64] = '\0';
    if (cap > 0)
    {
        const int n = cap - 1 < 64 ? cap - 1 : 64;
        std::memcpy(out, nm, (size_t)n);
        out[n] = '\0';
    }
    return true;
}

// ============================================================
//  Storm 式瞄准点枚举（CreateMove 命中验证用）
// ------------------------------------------------------------
//  排序口径：先按 HITGROUP 分层（头 → 胸 → 腹 → 其余），同层内按体积
//  降序。理由：头部盒在多数模型里体积最小、体积降序会把它排到很后面，
//  而"头 → 胸 → 腹"才是玩家对瞄准部位的期望优先级。
//  体积用盒的边长和衡量（比体积更抗异常值），只用于同层内的次序。
// ============================================================
int GetEntityAimPoints(int entIndex, const Vec3& origin, Vec3* out, int maxOut)
{
    if (!out || maxOut <= 0)
        return 0;

    HitboxCloud cloud;
    if (!GetEntityHitboxCloud(entIndex, origin, cloud) || !cloud.any)
        return 0;

    struct Item
    {
        Vec3 pt;
        float rank;   // 层号 * 1e6 + 边长和（越小越优先）
    };
    Item tmp[HitboxCloud::kMaxBoxes];
    int n = 0;

    for (int b = 0; b < cloud.count && n < HitboxCloud::kMaxBoxes; ++b)
    {
        const HitboxOBB& b0 = cloud.box[b];
        if (!b0.ok)
            continue;

        // 边长和：用 8 角的世界坐标算包围盒（与模型朝向无关）
        Vec3 mn(b0.corner[0]), mx(b0.corner[0]);
        for (int c = 1; c < 8; ++c)
        {
            const Vec3& p = b0.corner[c];
            mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mn.z = fminf(mn.z, p.z);
            mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); mx.z = fmaxf(mx.z, p.z);
        }
        const float extent = (mx.x - mn.x) + (mx.y - mn.y) + (mx.z - mn.z);
        if (!(extent > 0.0f) || extent > 512.0f)
            continue;

        // 层号：头=0 胸=1 腹=2 其余=3（无 group 的盒放最后一层）
        int layer = 3;
        if (b0.group == 1)      layer = 0;
        else if (b0.group == 2) layer = 1;
        else if (b0.group == 3) layer = 2;

        tmp[n].pt   = b0.center;
        tmp[n].rank = (float)layer * 1.0e6f + extent;
        ++n;
    }

    if (n <= 0)
        return 0;

    // 小规模插入排序即可（n <= 32，且只有头/胸/腹等少数几个会被取用）
    for (int i = 1; i < n; ++i)
    {
        const Item key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j].rank > key.rank)
        {
            tmp[j + 1] = tmp[j];
            --j;
        }
        tmp[j + 1] = key;
    }

    const int cnt = (n < maxOut) ? n : maxOut;
    for (int i = 0; i < cnt; ++i)
        out[i] = tmp[i].pt;
    return cnt;
}

// ============================================================
//  命中盒云（ESP 用）：全盒 OBB 角点 → 世界坐标
// ------------------------------------------------------------
//  与 GetEntityHitboxParts 的区别：
//    · 不按 HITGROUP 过滤，**所有**盒都产出（ESP 要的是整体轮廓）
//    · 每个盒变换 8 个角点而不是只取中心（角点才是可见边界）
//  解析链完全复用（ReadHdrInfo / SafeRead*），
//  但走独立的帧缓存条目，两者互不触发对方的重复解析。
// ============================================================
bool GetEntityHitboxCloud(int entIndex, const Vec3& origin, HitboxCloud& out)
{
    out = HitboxCloud{};

    // ---- 帧内缓存（游戏线程也会走到这里，故必须加锁）----
    {
        AcquireSRWLockShared(&g_cacheLock);
        const auto cit = g_cloudCache.find(entIndex);
        const bool hit = (cit != g_cloudCache.end() && cit->second.frame == g_frame);
        const HitboxCloud cached = hit ? cit->second.cloud : HitboxCloud{};
        ReleaseSRWLockShared(&g_cacheLock);
        if (hit)
        {
            out = cached;
            return out.any;
        }
    }

    HitboxCloud cloud{};
    cloud.done = true;

    const uintptr_t ent = GetEntity(entIndex);
    if (!IsUserPtr(ent) || !Finite3(origin) || !HitboxReady())
    {
        // 链路不可用：不写缓存也不留痕（每帧都会重来，留痕等于刷屏）
        out = cloud;
        return false;
    }

    const uintptr_t boneBase = GetEntityBoneMatrixBase(ent, origin);
    short modelIndex = 0;
    SafeRead(ent + g_modelIndexOff, modelIndex);

    uintptr_t hdr = 0;
    if (!boneBase || modelIndex <= 0)
    {
        out = cloud;
        return false;
    }
    hdr = GetEntityStudioHdr(ent, GetModelInfoIface());       // [批15] 引擎 vcall 链

    HdrInfo hi{};
    if (!hdr || !ReadHdrInfo(hdr, hi))
    {
        out = cloud;
        return false;
    }

    // ---- 遍历 hitbox set，收集全部合法盒 ----
    // [批5 诊断] 拒绝原因计数（3s 节流日志）——云空时不再静默
    int rejOther = 0, rejFinite = 0, rejZ = 0, rejHoriz = 0;
    int totalBoxes = 0;
    for (int s = 0; s < hi.numHitSets; ++s)
    {
        const uintptr_t set = hdr + (uintptr_t)hi.setIndexOff + (uintptr_t)s * kHitboxSetStride;
        int nhb = 0, hidx = 0;
        if (!SafeRead(set + 0x04, nhb) || !SafeRead(set + 0x08, hidx))
            continue;
        if (nhb < 1 || nhb > 32 || hidx <= 0 || hidx > 0x20000)
            continue;
        totalBoxes = nhb;

        for (int i = 0; i < nhb && cloud.count < HitboxCloud::kMaxBoxes; ++i)
        {
            const uintptr_t box = set + (uintptr_t)hidx + (uintptr_t)i * kHitboxStride;

            int bone = 0, group = 0;
            if (!SafeRead(box + kBoxBone, bone) || !SafeRead(box + kBoxGroup, group))
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

            // 量纲：局部坐标一般在 ±64 内，超出即脏数据（自瞄路径用 ±512
            // 是因为中心点会叠加骨偏移；这里角点同样会变换，放宽到 ±256
            // 兼顾 Tank 这类大模型）
            if (fabsf(lo.x) > 256.0f || fabsf(lo.y) > 256.0f || fabsf(lo.z) > 256.0f ||
                fabsf(hiV.x) > 256.0f || fabsf(hiV.y) > 256.0f || fabsf(hiV.z) > 256.0f)
                continue;

            // 退化盒（零体积）通常是无效占位，画出来是屏幕上一个点
            const float vol = fabsf((hiV.x - lo.x) * (hiV.y - lo.y) * (hiV.z - lo.z));
            if (!(vol > 0.0f))
                continue;

            float mat[12] = {};
            if (!SafeReadFloats(boneBase + (uintptr_t)bone * kMatrixStride, mat, 12))
                continue;

            // 8 角 → 世界
            HitboxOBB& obb = cloud.box[cloud.count];
            int bad = 0;
            for (int c = 0; c < 8; ++c)
            {
                const Vec3 local((c & 1) ? hiV.x : lo.x,
                                 (c & 2) ? hiV.y : lo.y,
                                 (c & 4) ? hiV.z : lo.z);
                const Vec3 w = MatrixToWorld(mat, local);
                if (!Finite3(w))
                {
                    bad = 1;
                    break;
                }
                obb.corner[c] = w;
            }
            if (bad)
                continue;

            // [批5 放宽] 防呆改为宽松窗（云服务自瞄，需兼容骑乘/爬地姿态）：
            //  SaneAimPoint 的 dz∈[15,192] 硬窗会把 Jockey 骑乘（盒心低于
            //  origin，dz<0）等特殊姿态全部拒掉 → 云恒空 → 永远固定点降级。
            //  宽松窗 dz∈[-48,192]、水平 ≤96：仍拦错位矩阵（那些给出成百
            //  上千的偏移），放过特殊姿态。拒绝计数进诊断日志。
            const Vec3 center = MatrixToWorld(mat, Vec3((lo.x + hiV.x) * 0.5f,
                                                        (lo.y + hiV.y) * 0.5f,
                                                        (lo.z + hiV.z) * 0.5f));
            if (!Finite3(center))
                { ++rejFinite; continue; }
            const float cdz = center.z - origin.z;
            const float chx = center.x - origin.x;
            const float chy = center.y - origin.y;
            if (cdz < -48.0f || cdz > 192.0f)
                { ++rejZ; continue; }
            if (chx * chx + chy * chy > 96.0f * 96.0f)
                { ++rejHoriz; continue; }

            obb.center = center;
            obb.group  = group;
            obb.ok     = true;
            ++cloud.count;
        }

        // 第一个能产出盒子的 set 就够了（多 set 是不同模型的备用绑定）
        if (cloud.count > 0)
            break;
    }

    // [批5 诊断] 云空原因（3s 节流）：拒绝计数 + 总盒数，直接定位断点
    if (!cloud.any)
    {
        static DWORD s_cloudDiag = 0;
        const DWORD t = GetTickCount();
        if (t - s_cloudDiag > 3000)
        {
            s_cloudDiag = t;
            PayloadLog("[hitbox] 云空诊断：totalBoxes=%d rejOther=%d rejFinite=%d "
                       "rejZ=%d rejHoriz=%d 骨基址=0x%X",
                       totalBoxes, rejOther, rejFinite, rejZ, rejHoriz,
                       (unsigned)boneBase);
        }
    }

    cloud.any = (cloud.count > 0);

    {
        AcquireSRWLockExclusive(&g_cacheLock);
        g_cloudCache[entIndex] = CloudEntry{ cloud, g_frame };
        ReleaseSRWLockExclusive(&g_cacheLock);
    }

    out = cloud;
    return cloud.any;
}

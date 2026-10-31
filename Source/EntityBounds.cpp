#include "Include.h"
#include "EntityBounds.h"
#include "Netvar.h"
#include "BoneSystem.h"
#include "HitboxSystem.h"
#include "Utils.h"
#include "PayloadControl.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include "Offsets.h"        // [批158] dwLocalPlayer / m_vecOrigin

using netvar::RecvTableX;
using netvar::RecvPropX;
using netvar::ClientClassX;
using netvar::FindPropR;
using netvar::GetAllClasses;

namespace
{
    // ========================================================
    // 碰撞盒 netvar 偏移（按名递归解析，懒加载一次）
    // ========================================================
    uintptr_t g_offMins = 0;
    uintptr_t g_offMaxs = 0;
    bool      g_collisionResolved = false;
    bool      g_collisionOk = false;

    // 在 DT_BaseEntity 的 m_Collision 子表里取 m_vecMins / m_vecMaxs 相对偏移
    bool ResolveCollisionProp(const RecvTableX* table, uintptr_t& outMins, uintptr_t& outMaxs)
    {
        if (!table) return false;
        for (int i = 0; i < table->m_nProps; ++i)
        {
            const RecvPropX* prop = &table->m_pProps[i];
            if (!prop->m_pVarName)
                continue;

            if (_stricmp(prop->m_pVarName, "m_Collision") == 0 && prop->m_pDataTable)
            {
                const uintptr_t mins = FindPropR(prop->m_pDataTable, "m_vecMins", 0);
                const uintptr_t maxs = FindPropR(prop->m_pDataTable, "m_vecMaxs", 0);
                if (mins && maxs)
                {
                    outMins = static_cast<uintptr_t>(prop->m_Offset) + mins;
                    outMaxs = static_cast<uintptr_t>(prop->m_Offset) + maxs;
                    return true;
                }
            }

            // m_Collision 也可能挂在基类表里：一并递归
            if (prop->m_pDataTable &&
                ResolveCollisionProp(prop->m_pDataTable, outMins, outMaxs))
                return true;
        }
        return false;
    }

    void EnsureCollisionOffsets()
    {
        if (g_collisionResolved)
            return;
        g_collisionResolved = true;

        ClientClassX* head = GetAllClasses();
        for (ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            if (ResolveCollisionProp(cc->m_pRecvTable, g_offMins, g_offMaxs))
            {
                g_collisionOk = (g_offMins != 0 && g_offMaxs != 0);
                break;
            }
        }
    }

    // 进程内安全读（实体帧间释放 / 未提交页）
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
    // ============================================================
    //  [批158] 本地玩家实时原点（速度/距离门控用）
    //  全程 SEH：dwLocalPlayer / 实体地址在切图瞬间随时可能失效
    // ============================================================
    bool TryGetLocalOrigin(Vec3& out)
    {
        // [批163·性能] 帧级缓存：GetStormBounds 每实体每帧调用一次，
        //   原来每次做 2 次 SEH 读（dwLocalPlayer + m_vecOrigin），
        //   N 个实体 = 2N 次读/帧。本地玩家原点一帧内不变 →
        //   按 GetTickCount 16ms 粒度惰性刷新。
        //   调用方只有渲染线程（GetStormBounds），单线程访问静态缓存安全。
        static Vec3  s_cache{};
        static bool  s_ok = false;
        static DWORD s_at = 0;
        const DWORD now = GetTickCount();
        if (now - s_at >= 16u)                     // ≈ 一帧（60fps）
        {
            s_at = now;
            s_ok = false;
            out = Vec3();
            const uintptr_t client =
                reinterpret_cast<uintptr_t>(::GetModuleHandleA("client.dll"));
            if (client)
            {
                uintptr_t lp = 0;
                if (SafeRead(client + Offsets::dwLocalPlayer, lp) &&
                    lp >= 0x10000u)
                {
                    Vec3 o{};
                    if (SafeRead(lp + Offsets::m_vecOrigin, o) &&
                        std::isfinite(o.x) && std::isfinite(o.y) &&
                        std::isfinite(o.z))
                    {
                        s_cache = o;
                        s_ok = true;
                    }
                }
            }
        }
        if (!s_ok)
            return false;
        out = s_cache;
        return true;
    }


    // ========================================================
    // 碰撞盒：读 m_vecMins/m_vecMaxs 并做量纲校验
    // ========================================================
    bool ReadCollisionBounds(uintptr_t ent, Vec3& outMin, Vec3& outMax)
    {
        if (!ent)
            return false;
        EnsureCollisionOffsets();
        if (!g_collisionOk)
            return false;

        Vec3 mins{}, maxs{};
        if (!SafeRead(ent + g_offMins, mins) || !SafeRead(ent + g_offMaxs, maxs))
            return false;

        // 有限性 + 量纲：人形约 ±16/±16/0~72，Jockey 更矮，Tank 更高更宽。
        // 越界即视为未同步 / 读到了脏数据，丢弃而不是画错框。
        if (!isfinite(mins.x) || !isfinite(mins.y) || !isfinite(mins.z) ||
            !isfinite(maxs.x) || !isfinite(maxs.y) || !isfinite(maxs.z))
            return false;

        const float width  = maxs.x - mins.x;
        const float depth  = maxs.y - mins.y;
        const float height = maxs.z - mins.z;
        if (width < 6.0f || width > 96.0f)  return false;
        if (depth < 6.0f || depth > 96.0f)  return false;
        if (height < 20.0f || height > 210.0f) return false;
        if (fabsf(mins.x) > 64.0f || fabsf(mins.y) > 64.0f || fabsf(mins.z) > 64.0f) return false;
        if (fabsf(maxs.x) > 64.0f || fabsf(maxs.y) > 64.0f) return false;

        outMin = mins;
        outMax = maxs;
        return true;
    }

    // ========================================================
    // 骨骼 AABB：本帧骨骼矩阵求世界盒 + 数量/量纲校验
    // ========================================================
    bool ReadBoneBounds(int entIndex, const Vec3& origin, Vec3& outMin, Vec3& outMax,
                        float& outHeight)
    {
        BoneSnapshot bones;
        if (!GetEntityBones(entIndex, origin, bones))
        {
            // 分档诊断：骨骼链全挂的直接证据（当前 0x6C0 口径是否失效）
            static bool s_logged = false;
            if (!s_logged)
            {
                s_logged = true;
                PayloadLog("[bone-diag] GetEntityBones 失败 idx=%d origin=%.0f,%.0f,%.0f"
                           "（骨骼矩阵基址解析挂了，命中盒云会跟着挂）",
                           entIndex, origin.x, origin.y, origin.z);
            }
            return false;
        }

        Vec3 mn(FLT_MAX, FLT_MAX, FLT_MAX);
        Vec3 mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        int n = 0;
        for (int b = 0; b < Bone_Count_; ++b)
        {
            if (!bones.valid[b])
                continue;
            const Vec3& p = bones.pos[b];
            mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mn.z = fminf(mn.z, p.z);
            mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); mx.z = fmaxf(mx.z, p.z);
            ++n;
        }

        // 骨骼数不足时 AABB 会明显偏小（只剩局部骨骼）→ 交给碰撞盒
        if (n < 8)
            return false;

        const float w = mx.x - mn.x, d = mx.y - mn.y, h = mx.z - mn.z;
        if (!(w > 1.0f && d > 1.0f && h > 8.0f) || w > 220.0f || d > 220.0f || h > 260.0f)
            return false;

        outMin = mn;
        outMax = mx;
        outHeight = h;
        return true;
    }

    // 世界 AABB → 屏幕矩形（8 角投影取包围）
    bool ProjectBounds(const Vec3& wmin, const Vec3& wmax, ScreenRect& out)
    {
        if (!IsFrameViewValid())
            return false;
        const CView& view = GetFrameView();

        float loX = FLT_MAX, loY = FLT_MAX, hiX = -FLT_MAX, hiY = -FLT_MAX;
        int proj = 0;
        for (int c = 0; c < 8; ++c)
        {
            const Vec3 corner((c & 1) ? wmax.x : wmin.x,
                              (c & 2) ? wmax.y : wmin.y,
                              (c & 4) ? wmax.z : wmin.z);
            Vec2 s;
            if (!view.WorldToScreen(corner, s))
                continue;
            loX = fminf(loX, s.x); loY = fminf(loY, s.y);
            hiX = fmaxf(hiX, s.x); hiY = fmaxf(hiY, s.y);
            ++proj;
        }
        if (proj == 0)
            return false;

        // 角点全在相机后 / 投影退化
        if (!(hiX > loX && hiY > loY))
            return false;
        // 屏幕尺寸的合理上限：目标贴脸或解析出错时防止画出横跨全屏的巨框
        const float maxW = Gui.Window.Size.x * 4.0f;
        const float maxH = Gui.Window.Size.y * 4.0f;
        if ((hiX - loX) > maxW || (hiY - loY) > maxH)
            return false;

        out.left = loX; out.top = loY; out.right = hiX; out.bottom = hiY;
        return true;
    }
// ========================================================
    // 命中盒云 → 世界 AABB（最高优先级几何来源）
    // --------------------------------------------------------
    // 为什么排在骨骼 AABB 之前：骨骼点是"关节原点"，只有点没有面；
    // 命中盒是模型自带的 OBB，其角点就是可见轮廓的边界近似。用盒角点
    // 求 AABB 比用骨骼点求更紧、更贴姿态（尤其横躺/扑击/骑乘时）。
    // ========================================================
    bool ReadHitboxCloudBounds(const EntityInfo& info, Vec3& outMin, Vec3& outMax,
                               int& outBoxes)
    {
        outBoxes = 0;
        HitboxCloud cloud;
        if (!GetEntityHitboxCloud(info.index, info.position, cloud) || !cloud.any)
            return false;

        Vec3 mn(FLT_MAX, FLT_MAX, FLT_MAX);
        Vec3 mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        int n = 0;
        for (int b = 0; b < cloud.count; ++b)
        {
            if (!cloud.box[b].ok)
                continue;
            for (int c = 0; c < 8; ++c)
            {
                const Vec3& p = cloud.box[b].corner[c];
                mn.x = fminf(mn.x, p.x); mn.y = fminf(mn.y, p.y); mn.z = fminf(mn.z, p.z);
                mx.x = fmaxf(mx.x, p.x); mx.y = fmaxf(mx.y, p.y); mx.z = fmaxf(mx.z, p.z);
            }
            ++n;
        }
        if (n < 1)
            return false;

        // 量纲校验：与骨骼/碰撞盒同级（人形 ~72 高，Tank ~200+）
        const float w = mx.x - mn.x, d = mx.y - mn.y, h = mx.z - mn.z;
        if (!(w > 1.0f && d > 1.0f && h > 8.0f) || w > 240.0f || d > 240.0f || h > 300.0f)
            return false;

        outMin  = mn;
        outMax  = mx;
        outBoxes = n;
        return true;
    }
}

// levelOut：几何来源（诊断用）0=命中盒云 1=互证-骨骼 2=互证-碰撞
//           3=仅骨骼 4=仅碰撞 5=固定盒兜底 6=全失败
static bool GetEntityWorldBoundsInner(uintptr_t ent, const EntityInfo& info,
                                      Vec3& outMin, Vec3& outMax, int& levelOut,
                                      bool allowBoneSources)
{
    levelOut = 6;

    // [批158] allowBoneSources=false（高速/远距门控）时跳过骨骼来源的
    //   0/1 级，直接用实时碰撞盒（本帧直读，零滞后）。
    const float velMag = sqrtf(info.velocity.x * info.velocity.x +
                               info.velocity.y * info.velocity.y +
                               info.velocity.z * info.velocity.z);
    const bool fastMover = !allowBoneSources || (velMag > 120.0f);

    // ---- 第 0 级：命中盒云（最贴合轮廓）----
    int hbCount = 0;
    if (!fastMover && ReadHitboxCloudBounds(info, outMin, outMax, hbCount))
    {
        levelOut = 0;
        return true;
    }

    Vec3 boneMin{}, boneMax{};
    float boneHeight = 0.0f;
    const bool boneOk = fastMover ? false
                                  : ReadBoneBounds(info.index, info.position,
                                                   boneMin, boneMax, boneHeight);

    Vec3 colMin{}, colMax{};
    const bool colOk = ReadCollisionBounds(ent, colMin, colMax);
    if (colOk)
    {
        // ⚠ m_vecMins/m_vecMaxs 是【相对实体原点的局部碰撞盒】（约 ±16/72），
        //   必须加 origin 才是世界坐标。之前直接当世界盒用 → 框全部画在
        //   地图原点附近（"地图外的框 / Spitter 位置相差甚远"的元凶）。
        //   （局部盒不含旋转：AABB 近似，姿态贴合交给命中盒云那一级）
        colMin = Vec3(colMin.x + info.position.x,
                      colMin.y + info.position.y,
                      colMin.z + info.position.z);
        colMax = Vec3(colMax.x + info.position.x,
                      colMax.y + info.position.y,
                      colMax.z + info.position.z);
    }

    if (boneOk && colOk)
    {
        // 骨骼盒与碰撞盒互相印证：骨骼盒高度若明显小于碰撞盒（骨骼残缺,
        // 只剩部分肢体），采信碰撞盒；否则用姿态更贴合的骨骼盒。
        const float colHeight = colMax.z - colMin.z;
        if (boneHeight >= colHeight * 0.55f && boneHeight <= colHeight * 1.75f)
        {
            outMin = boneMin; outMax = boneMax;
            levelOut = 1;
            return true;
        }
        outMin = colMin; outMax = colMax;
        levelOut = 2;
        return true;
    }
    if (boneOk) { outMin = boneMin; outMax = boneMax; levelOut = 3; return true; }
    if (colOk)  { outMin = colMin;  outMax = colMax;  levelOut = 4; return true; }

    // ---- 兜底：骨骼盒与碰撞盒都拿不到时，用 origin + 标准人形盒 ----
    // 不再返回 false：某些特感（幽灵态 / 换模型瞬间 / 刚出生未完成碰撞盒同步）
    // 两条路径同时失效，会让整只特感从 ESP 里静默消失。这里退化为固定盒，
    // 宁可框略粗也不漏画；投影端仍有 maxW/maxH 上限兜底，不会画出全屏巨框。
    if (isfinite(info.position.x) && isfinite(info.position.y) && isfinite(info.position.z))
    {
        outMin = Vec3(info.position.x - 16.0f, info.position.y - 16.0f, info.position.z);
        outMax = Vec3(info.position.x + 16.0f, info.position.y + 16.0f, info.position.z + 72.0f);
        levelOut = 5;
        return true;
    }
    return false;
}

bool GetEntityWorldBounds(uintptr_t ent, const EntityInfo& info,
                          Vec3& outMin, Vec3& outMax, bool allowBoneSources)
{
    int level = 6;
    const bool ok = GetEntityWorldBoundsInner(ent, info, outMin, outMax, level,
                                              allowBoneSources);

    // ---- 几何来源诊断（约 3 秒一次）：定位"框不准/乱跳/漏画"卡在哪级 ----
    // level 含义见 GetEntityWorldBoundsInner 注释；第 0 级常年为 0 是
    // 本机 modelInfo 断链的已知常态，看 1~5 的分布即可。
    static unsigned s_tick = 0;
    static int s_cnt[7] = {};
    if (++s_tick >= 180u)
    {
        PayloadLog("[esp-geom] 180帧几何来源 hitbox=%d 互证骨骼=%d 互证碰撞=%d "
                   "仅骨骼=%d 仅碰撞=%d 固定盒=%d 失败=%d",
                   s_cnt[0], s_cnt[1], s_cnt[2], s_cnt[3], s_cnt[4], s_cnt[5], s_cnt[6]);
        std::memset(s_cnt, 0, sizeof(s_cnt));
        s_tick = 0;
    }
    ++s_cnt[level <= 6 ? level : 6];
    return ok;
}

bool GetEntityScreenRect(const EntityInfo& info, const EntityState& state, ScreenRect& out)
{
    (void)state;
    const uintptr_t ent = GetEntity(info.index);
    if (!ent)
        return false;

    Vec3 wmin{}, wmax{};
    if (!GetEntityWorldBounds(ent, info, wmin, wmax))
        return false;

    // 外扩 3 单位：包住蒙皮边缘（骨骼点与碰撞盒都略小于可见模型）
    constexpr float kPad = 3.0f;
    wmin = Vec3(wmin.x - kPad, wmin.y - kPad, wmin.z - kPad);
    wmax = Vec3(wmax.x + kPad, wmax.y + kPad, wmax.z + kPad);

    return ProjectBounds(wmin, wmax, out);
}

// [批111·死代码] 删除了 EntityBoundsCollisionReady()（原 363-367 行）
//   —— 全项目仅"头文件声明 + 此处定义"两处，零调用点。
//   它只是 EnsureCollisionOffsets() + 返回 g_collisionOk 的透传，
//   而真正的使用者都直接调 GetStormBounds/GetEntityHitboxCloud，
//   由它们内部各自调 EnsureCollisionOffsets()。

// ============================================================
//  命中盒 → 屏幕线框
// ------------------------------------------------------------
//  投影策略（与 Storm Paint.hpp 对齐）：
//    · 逐盒 8 角全部投影，成功的记入 pt
//    · 角点全部在相机后时跳过该盒（整盒丢弃，而不是画半个框）
//    · 角点数 ≥ 4 才算可画 —— 少于 4 点连不成闭环
//  与 GetEntityScreenRect 的关系：这是**可选的精细画法**，由 boxStyle
//  决定；线框不可用时调用方自动回退到整体 ScreenRect 方框，保证
//  "任何情况下都有框"，不会出现"有实体却什么都没画"。
// ============================================================
int GetEntityHitboxQuads(const EntityInfo& info, const EntityState& state,
                         HitboxQuad* out, int maxOut)
{
    (void)state;
    if (!out || maxOut <= 0)
        return 0;
    if (!IsFrameViewValid())
        return 0;

    HitboxCloud cloud;
    if (!GetEntityHitboxCloud(info.index, info.position, cloud) || !cloud.any)
        return 0;

    const CView& view = GetFrameView();

    int n = 0;
    for (int b = 0; b < cloud.count && n < maxOut; ++b)
    {
        const HitboxOBB& src = cloud.box[b];
        if (!src.ok)
            continue;

        HitboxQuad q;
        int valid = 0;
        float loX = FLT_MAX, loY = FLT_MAX, hiX = -FLT_MAX, hiY = -FLT_MAX;
        for (int c = 0; c < 8; ++c)
        {
            Vec2 s;
            if (!view.WorldToScreen(src.corner[c], s))
                continue;
            if (!isfinite(s.x) || !isfinite(s.y))
                continue;
            q.pt[c] = s;
            q.okMask |= (uint8_t)(1u << c);
            ++valid;
            loX = fminf(loX, s.x); loY = fminf(loY, s.y);
            hiX = fmaxf(hiX, s.x); hiY = fmaxf(hiY, s.y);
        }

        // 角点不足 4 个 → 连不成闭环；整体退化 → 跳过
        if (valid < 4 || !(hiX > loX && hiY > loY))
            continue;

        // 与方框同级的上限：贴脸时个别盒可能算出横跨全屏的巨框
        const float maxW = Gui.Window.Size.x * 4.0f;
        const float maxH = Gui.Window.Size.y * 4.0f;
        if ((hiX - loX) > maxW || (hiY - loY) > maxH)
            continue;

        q.group   = src.group;        q.visible = (valid == 8) ? 2 : 1;   // 2=完整 1=部分（部分画虚线感/淡色）
        out[n++] = q;
    }

    return n;
}

// ============================================================
//  Storm Paint.hpp 原生几何：全部命中盒 8 角逐点投影 → 屏幕并集框
// ------------------------------------------------------------
//  Storm 的 Get_Bounds（Paint.hpp:82-192）就是逐角点WorldToScreen 后
//  直接累加屏幕 min/max，**不先在���界空间求 AABB**。我们照搬这个数学。
//
//  两处刻意偏离 Storm（都是修它的缺陷，不是改它的行为）：
//   ① 投影失败时Storm 直接 return 0 放弃整个实体 —— 贴边/转身瞬间会整框
//      闪烁。我们改成"该角点跳过、其余继续"，只要还有 4 个角点投影成功就取包围。
//   ② Storm 不用上限校验，我们保留 4×屏 的巨框保护（读脏数据时兜底）。
// ============================================================
bool GetStormBounds(const EntityInfo& info, float pad, StormBounds& out)
{
    out = StormBounds{};
    if (!IsFrameViewValid())
        return false;

    const CView& view = GetFrameView();
    float loX = FLT_MAX, loY = FLT_MAX, hiX = -FLT_MAX, hiY = -FLT_MAX;
    int valid = 0, boxes = 0;

    // ============================================================
    //  [批158] 动态门控（Dynamic Bounds Gating）
    // ------------------------------------------------------------
    //  分支 A 高速（>120u/s）：骨骼缓存滞后最多 80ms（400u/s ≈ 32u
    //          ≈ 半个身位）→ 框贴不住 → 绝不走骨骼/命中盒并集，
    //          直接用实时碰撞盒（m_vecMins/Maxs 本帧直读，零滞后）。
    //  分支 B 远距（>1500u ≈ 38m）：骨骼细节肉眼不可见 → 走实时碰撞盒
    //          省掉整条骨骼解析链的开销。
    //  分支 C 其余（慢速近战）：原路径（命中盒并集），贴合度最高。
    // ============================================================
    const float velMag = sqrtf(info.velocity.x * info.velocity.x +
                               info.velocity.y * info.velocity.y +
                               info.velocity.z * info.velocity.z);
    float dist2Local = 0.0f;
    {
        Vec3 localOrigin{};
        if (TryGetLocalOrigin(localOrigin))
        {
            const float dx = info.position.x - localOrigin.x;
            const float dy = info.position.y - localOrigin.y;
            const float dz = info.position.z - localOrigin.z;
            dist2Local = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        // 本地原点读不到时不启用"远距"分支（宁可用精确路径）
    }
    const bool useBones = (velMag <= 120.0f) &&
                          (dist2Local <= 1500.0f || dist2Local == 0.0f);

    // ---- ① 首选：Storm 原生几何（全部命中盒 8 角逐点投影取并集）----
    HitboxCloud cloud;
    if (useBones &&
        GetEntityHitboxCloud(info.index, info.position, cloud) && cloud.any)
    {
        for (int b = 0; b < cloud.count; ++b)
        {
            const HitboxOBB& src = cloud.box[b];
            if (!src.ok)
                continue;

            int boxPts = 0;
            for (int c = 0; c < 8; ++c)
            {
                Vec2 s;
                if (!view.WorldToScreen(src.corner[c], s))
                    continue;                   // 角点在相机后：跳过，不整框放弃
                if (!isfinite(s.x) || !isfinite(s.y))
                    continue;
                loX = fminf(loX, s.x); loY = fminf(loY, s.y);
                hiX = fmaxf(hiX, s.x); hiY = fmaxf(hiY, s.y);
                ++boxPts;
            }
            if (boxPts >= 4)       // 该盒至少有 4 角可见，才计入
            {
                ++boxes;
                valid += boxPts;
            }
        }
    }

    // ---- hitbox 并集框 sanity（骨骼矩阵"半成功"时 corner 会错乱出怪框）----
    // 框中心应贴近目标原点的屏幕投影；偏离超半屏 → corner 数据不可信，
    // 弃用 hitbox 框，走下面的世界 AABB 降级。
    if (boxes >= 1)
    {
        Vec2 orgScr;
        if (view.WorldToScreen(info.position, orgScr))
        {
            const float cx = (loX + hiX) * 0.5f;
            const float cy = (loY + hiY) * 0.5f;
            const float maxDev = Gui.Window.Size.x * 0.5f;
            if (fabsf(cx - orgScr.x) > maxDev || fabsf(cy - orgScr.y) > maxDev)
            {
                boxes = 0;
                valid = 0;
                loX = loY = FLT_MAX;
                hiX = hiY = -FLT_MAX;
            }
        }
    }

    // ---- hitbox 巨框 sanity ②（尺寸参照）：骨骼矩阵"半成功"时 corner
    //  会错乱出巨框（实测 20m 处 448x1670）。世界 AABB（碰撞盒+origin）
    //  投影高度是物理正确的参照：hitbox 框高超过其 2.5 倍 → corner 不可信，
    //  弃用 hitbox 框走降级。----
    if (boxes >= 1)
    {
        Vec3 wMin{}, wMax{};
        if (GetEntityWorldBounds(GetEntity(info.index), info, wMin, wMax))
        {
            float rLoY = FLT_MAX, rHiY = -FLT_MAX;
            int rPts = 0;
            static const int kC[8][3] = {
                {0,0,0},{1,0,0},{0,1,0},{1,1,0},
                {0,0,1},{1,0,1},{0,1,1},{1,1,1}};
            const Vec3 src[2] = { wMin, wMax };
            for (int c = 0; c < 8; ++c)
            {
                Vec2 s;
                const Vec3 wp(src[kC[c][0]].x, src[kC[c][1]].y, src[kC[c][2]].z);
                if (!view.WorldToScreen(wp, s) || !isfinite(s.x) || !isfinite(s.y))
                    continue;
                rLoY = fminf(rLoY, s.y); rHiY = fmaxf(rHiY, s.y);
                ++rPts;
            }
            const float refH = rHiY - rLoY;
            const float hitH = hiY - loY;
            if (rPts >= 4 && refH > 5.0f && hitH > refH * 2.5f)
            {
                boxes = 0; valid = 0;
                loX = loY = FLT_MAX; hiX = hiY = -FLT_MAX;
            }
        }
    }

    // ---- ② 降级：命中盒链路不可用时走世界 AABB 三级策略 ----
    //  2026-10-18 实机回归修复：modelInfo 拿不到（GetInterface 失败）时
    //  命中盒云恒为空，原实现直接 return false → **整个透视一个框都不画**。
    //  而旧透视有三级降级（命中盒云 AABB → 骨骼 AABB → 碰撞盒）所以不受影响。
    //  这里把降级链补回 Storm 几何层：形状不如逐盒贴合，但保证有框。
    if (boxes < 1 || valid < 4)
    {
        Vec3 wMin{}, wMax{};
        if (!GetEntityWorldBounds(GetEntity(info.index), info,
                                  wMin, wMax, useBones))
            return false;

        static const int kCorner[8][3] = {
            {0,0,0},{1,0,0},{0,1,0},{1,1,0},
            {0,0,1},{1,0,1},{0,1,1},{1,1,1}
        };
        const Vec3 src[2] = { wMin, wMax };
        int boxPts = 0;
        for (int c = 0; c < 8; ++c)
        {
            const Vec3 wp(src[kCorner[c][0]].x, src[kCorner[c][1]].y, src[kCorner[c][2]].z);
            Vec2 s;
            if (!view.WorldToScreen(wp, s) || !isfinite(s.x) || !isfinite(s.y))
                continue;
            loX = fminf(loX, s.x); loY = fminf(loY, s.y);
            hiX = fmaxf(hiX, s.x); hiY = fmaxf(hiY, s.y);
            ++boxPts;
        }
        if (boxPts < 4)
            return false;
        boxes = 1;
    }

    if (!(hiX > loX && hiY > loY))
        return false;

    // 巨框保护（Storm 无此校验）
    const float maxW = Gui.Window.Size.x * 4.0f;
    const float maxH = Gui.Window.Size.y * 4.0f;
    if ((hiX - loX) > maxW || (hiY - loY) > maxH)
        return false;

    // 每边外扩（Storm 用 2.0f）
    out.left   = loX - pad;
    out.right  = hiX + pad;
    out.top    = loY - pad;
    out.bottom = hiY + pad;
    out.boxes  = boxes;
    out.ok     = true;
    return true;
}
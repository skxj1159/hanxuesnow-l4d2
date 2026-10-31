// ============================================================
//  Visibility.cpp —— 掩体两色判定实现（结构同源自删掉的 Storm 自瞄）
// ============================================================

#include "Visibility.h"
#include "PayloadControl.h"
#include "Globals.h"

#include <cstring>
#include <cctype>

// ------------------------------------------------------------
//  引擎 trace 结构（与 engine 的 Ray_t / trace_t 同构，32 位。
//  槽位与布局在自瞄体系上实机验证过，备份 _removed_aim_20261018）
// ------------------------------------------------------------
namespace
{
    //  [批148] 去掉 DEBRIS 位（0x2000000）：碎片/血块/可破坏杂物本来
    //   就打不穿，却会让可见性射线停在上面 → "明明露出来却不瞄"。
    //   HITBOX 位（0x4000000）必须保留：去掉它射线会穿过所有怪物的身体，
    //   变成"隔着小僵尸锁后面的特感"，是另一个方向的错。
    constexpr unsigned int MASK_SHOT = 0x4600400B & ~0x2000000u;

    struct V3A
    {
        float x, y, z, w;
        V3A() : x(0), y(0), z(0), w(0) {}
        V3A(float X, float Y, float Z) : x(X), y(Y), z(Z), w(0) {}
    };

    struct Ray_t
    {
        V3A         m_Start;                  // 0x00
        V3A         m_Delta;                  // 0x10
        V3A         m_StartOffset;            // 0x20
        V3A         m_Extents;                // 0x30
        const void* m_pWorldAxisTransform;    // 0x40
        bool        m_IsRay;                  // 0x44
        bool        m_IsSwept;                // 0x45

        Ray_t(const Vec3& start, const Vec3& end)
        {
            m_Start = V3A(start.x, start.y, start.z);
            m_Delta = V3A(end.x - start.x, end.y - start.y, end.z - start.z);
            m_pWorldAxisTransform = nullptr;
            m_IsRay   = true;
            m_IsSwept = (m_Delta.x != 0.0f || m_Delta.y != 0.0f || m_Delta.z != 0.0f);
        }
    };

    struct cplane_t
    {
        Vec3          normal;
        float         dist;
        unsigned char type;
        unsigned char signbits;
        unsigned char _pad[2];
    };

    struct csurface_t
    {
        const char*    name;          // 如 "WORLD"、"GLASS"、"WOOD_PANEL"
        short          surfaceProps;
        unsigned short flags;
    };

    struct CBaseTrace
    {
        Vec3     startpos;
        Vec3     endpos;
        cplane_t plane;
        float    fraction;
        int      contents;
        unsigned short dispFlags;
        bool     allsolid;
        bool     startsolid;
    };

    struct CGameTrace : public CBaseTrace
    {
        float      fractionleftsolid;
        csurface_t surface;
        int        hitgroup;
        short      physicsbone;
        unsigned short worldSurfaceIndex;
        void*      m_pEnt;
        int        hitbox;

        bool DidHit() const { return fraction < 1.0f || allsolid || startsolid; }
    };

    class ITraceFilter
    {
    public:
        virtual bool ShouldHitEntity(void* pEntity, int contentsMask) = 0;
        virtual int  GetTraceType() const = 0;
    };

    // 命中一切（除本地玩家自己——视线起点在体内，避开自命中）
    class CTraceFilterLocal : public ITraceFilter
    {
    public:
        explicit CTraceFilterLocal(void* pIgnore) : m_pIgnore(pIgnore) {}
        bool ShouldHitEntity(void* pEntity, int) override { return pEntity != m_pIgnore; }
        int  GetTraceType() const override { return 0; }
    private:
        void* m_pIgnore;
    };

    class IEngineTrace
    {
    public:
        virtual int  GetPointContents(const Vec3&, int, void**) = 0;                            // 0
        virtual int  GetPointContents_WorldOnly(const Vec3&, int) = 0;                          // 1
        virtual int  GetPointContents_Collideable(void*, const Vec3&) = 0;                      // 2
        virtual void ClipRayToEntity(const Ray_t&, unsigned int, void*, CGameTrace*) = 0;       // 3
        virtual void ClipRayToCollideable(const Ray_t&, unsigned int, void*, CGameTrace*) = 0;  // 4
        virtual void TraceRay(const Ray_t&, unsigned int, ITraceFilter*, CGameTrace*) = 0;      // 5
    };

    // ---- 接口与缓存 ----
    void*  g_trace = nullptr;
    bool   g_tried = false;

    // 每实体限频缓存（2048 槽，tick==0 表示从未测过）
    constexpr int kVisSlots = 2048;
    DWORD s_tick[kVisSlots] = {};
    bool  s_vis[kVisSlots]  = {};
    constexpr DWORD kVisIntervalMs = 100;

    // ---- 材质分类 ----
    // 可穿透白名单（用户语义"能看到也能穿透 = 可见"）
    const char* kPenetrable[] =
    {
        "wood", "plank", "plaster", "drywall", "sheetrock", "grate",
        "fence", "foliage", "bush", "cardboard", "paper", "canvas",
        "dirt", "grass", "sand", "snow", "mud", "ash", "foam", "rubble",
    };
    // 明确不可见：玻璃（看得见但打不穿，用户特别指定）
    constexpr const char* kGlass = "glass";

    bool NameHas(const char* name, const char* key)
    {
        if (!name) return false;
        // 大小写不敏感子串
        for (const char* p = name; *p; ++p)
        {
            const char* a = p;
            const char* b = key;
            while (*b && std::tolower((unsigned char)*a) == std::tolower((unsigned char)*b))
            { ++a; ++b; }
            if (!*b) return true;
        }
        return false;
    }
} // anonymous namespace

bool VisInit()
{
    if (g_tried) return g_trace != nullptr;
    g_tried = true;
    HMODULE engine = GetModuleHandleA("engine.dll");
    if (!engine) return false;
    using Fn = void* (*)(const char*, int*);
    const auto create = reinterpret_cast<Fn>(GetProcAddress(engine, "CreateInterface"));
    if (!create) return false;
    g_trace = create("EngineTraceClient003", nullptr);
    PayloadLog("[vis] EngineTraceClient003=%s", g_trace ? "ok" : "FAIL");
    return g_trace != nullptr;
}

bool VisReady()
{
    return g_trace != nullptr;
}

bool VisIsVisible(int entIndex, const Vec3& eye, const Vec3& target)
{
    if (!enableVisibility || !g_trace)
        return true;   // 功能关闭 / trace 不可用：全按可见画

    // ---- 限频缓存 ----
    if (entIndex < 0 || entIndex >= kVisSlots)
        return true;
    const DWORD now = GetTickCount();
    if (s_tick[entIndex] != 0 && now - s_tick[entIndex] < kVisIntervalMs)
        return s_vis[entIndex];
    s_tick[entIndex] = now;

    // 本地玩家指针作为 filter 排除项（视线起点在自己模型内）
    extern uintptr_t localPlayer;
    CTraceFilterLocal filter(reinterpret_cast<void*>(localPlayer));
    CGameTrace tr{};
    Ray_t ray(eye, target);
    reinterpret_cast<IEngineTrace*>(g_trace)->TraceRay(ray, MASK_SHOT, &filter, &tr);

    bool vis = true;
    if (tr.allsolid || tr.startsolid)
    {
        vis = false;                       // 起点就在实体内部：按不可见
    }
    else if (tr.fraction >= 0.999f || !tr.DidHit())
    {
        vis = true;                        // 中途无任何遮挡
    }
    else
    {
        // 命中了东西 → 看表面材质
        if (NameHas(tr.surface.name, kGlass))
            vis = false;                   // 玻璃：看得见但打不穿 → 不可见
        else
        {
            bool pen = false;
            for (const char* k : kPenetrable)
                if (NameHas(tr.surface.name, k)) { pen = true; break; }
            if (pen)
                vis = true;                // 可穿透物：能看见也能穿
            else if (tr.m_pEnt)
                vis = true;                // 命中其他实体（僵尸/队友）：软遮挡
            else
                vis = false;               // 世界静态几何：混凝土/金属 → 不可见
        }
    }

    s_vis[entIndex] = vis;
    return vis;
}

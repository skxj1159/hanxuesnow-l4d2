// ============================================================
//  Features/Aimbot.cpp —— 自瞄（**功能层**，Legit + PSilent）
// ------------------------------------------------------------
//  【架构】Legit（平滑视觉）单模式（微静默功能已整体移除，批30）
//  「选靶 / 可见性 / 瞄准点」求解器；本文件为批1骨架：
//    · 选靶链路（GetBestTarget）已完整落地：实体遍历 →
//      死亡/休眠/队友过滤 → 头盒(降级 origin+64) → TraceRay
//      可见性 → FOV 圆锥内取视角夹角最近者
//    · 角度计算 / 平滑 / 拟人化 / PSilent 应用 = // TODO（批2/3/4）
//
//  【冻结遵守】本文件只【只读消费】GetFrameSnapshot、Globals、
//  sdk::EntityList；不 include / 不修改任何透视模块文件。
//  TraceRay 的 Ray_t/CGameTrace 结构为本模块自持副本
//  （布局与 Visibility.cpp 同源，在自瞄体系上实机验证过）。
// ============================================================

#include "Aimbot.h"
#include "../Globals.h"
#include "../PayloadControl.h"
#include "../LoopEntity.h"        // GetFrameSnapshot：选靶吃 ESP 同源快照（分类同源）
#include "../BoneSystem.h"        // GetEntityBoneMatrixBase（降级原因归类）/ GetEntityBones（预热）
#include "../SDK/NetVars.h"       // [批5·L3] 碰撞盒/姿态 netvar 偏移
#include "PunchProbe.h"           // [批100] punch 字段范围校验
#include <mutex>                  // [批16] 快照跨线程复制锁

#include <cerrno>
#include <cmath>
#include <cstring>

namespace
{
    // [批36] cmd 角度写入标志（Hook 返回 false 的依据；每 tick 由
    // CreateMove 头部清零、在写角点置位）
    bool g_wroteCmdAngles = false;

    // ============================================================
    //  [批55] 静默模式的"服务端可见角度"状态机
    // ------------------------------------------------------------
    //  为什么要它 —— Lilac 的 AIMBOT_FLAG_REPEAT 判据（lilac_aimbot.sp:276-285）：
    //      tdelta = delta(ang[shot-1], ang[shot+1]);      // 前后两 tick 跨度
    //      if (tdelta < 10.0 && delta(ang[shot-1], ang[shot]) > 0.5
    //          && delta(ang[shot-1], ang[shot]) > tdelta * 5.0)
    //          detected |= AIMBOT_FLAG_REPEAT;
    //
    //  旧实现只在 IN_ATTACK 那一 tick 写目标角、其余 tick 保持玩家真实角，
    //  服务端看到的就是  A → B → A  —— 前后跨度为 0，中间那 tick 有偏移，
    //  三个条件【必然同时成立】。实测日志 14 次"静默生效" = 14 次候选，
    //  而 AIMBOT_BAN_MIN = 5，够封两轮。这就是封号的直接原因。
    //
    //  解法不是把角度藏起来（藏不住，服务端只认 cmd），而是让 cmd 角
    //  变成一条【连续曲线】：每 tick 只挪一点点，开火那一 tick 相对
    //  上一 tick 几乎没有变化。此时
    //      delta(shot-1, shot) ≈ step，delta(shot-1, shot+1) ≈ 2*step
    //  恒有  step < 5 * 2*step  → REPEAT 的第三个条件永不成立。
    //
    //  同一个限速还顺手封印了 SNAP / SNAP2：
    //      SNAP  需 tdelta > 10.0
    //      SNAP2 需 tdelta > 5.0
    //  只要每 tick 位移 < 5°，两者在数值上就不可能触发。
    // ============================================================
    sdk::QAngle g_srvAng{};          // 当前"服务端看到的角度"
    bool        g_srvAngValid = false;
    // [批63] 上一 tick 玩家真实视角 —— 用来算"玩家自己转了多少"，
    //   这部分增量要 1:1 应用到曲线上（限速只约束瞄准修正）。
    sdk::QAngle g_prevReal{};

    // ============================================================
    //  引擎 Trace 结构副本（与 Visibility.cpp 同源，实机验证过）
    // ============================================================
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
        V3A         m_Start;
        V3A         m_Delta;
        V3A         m_StartOffset;
        V3A         m_Extents;
        const void* m_pWorldAxisTransform;
        bool        m_IsRay;
        bool        m_IsSwept;

        Ray_t(const sdk::Vector& start, const sdk::Vector& end)
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
        sdk::Vector   normal;
        float         dist;
        unsigned char type;
        unsigned char signbits;
        unsigned char _pad[2];
    };

    struct csurface_t
    {
        const char*    name;
        short          surfaceProps;
        unsigned short flags;
    };

    struct CBaseTrace
    {
        sdk::Vector startpos;
        sdk::Vector endpos;
        cplane_t    plane;
        float       fraction;
        int         contents;
        unsigned short dispFlags;
        bool        allsolid;
        bool        startsolid;
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

    // 忽略两个实体（本地玩家 + 当前目标本体——射线终点在目标盒内）
    class CTraceFilterIgnore : public ITraceFilter
    {
    public:
        CTraceFilterIgnore(void* a, void* b) : m_a(a), m_b(b) {}
        bool ShouldHitEntity(void* pEntity, int) override
        { return pEntity != m_a && pEntity != m_b; }
        int  GetTraceType() const override { return 0; }
    private:
        void* m_a;
        void* m_b;
    };

    // ============================================================
    //  [批146] CTraceFilterAimbot —— 把普通感染者（小僵尸）当作透明
    // ------------------------------------------------------------
    //  背景：小僵尸挤在特感前面时，射线打中它 → 判"被挡" → 特感明明
    //  露在外面却不瞄。现在【可见性射线】直接穿透小僵尸。
    //  识别方式：渲染线程发布它们的实体 vtable（g_commonInfectedVtable），
    //  比对 vtable 即可，O(1)，绝不在引擎回调里解析类名。
    //  ⚠ 只用于可见性判定（TraceVisible）。宽限期的墙检测
    //    （TraceBlockedByWorld）必须保持【不穿透】—— 否则"被小僵尸挡住"
    //    会变成"射线穿透后打到背后的墙" → 误判为墙 → 软锁被秒破。
    // ============================================================
    class CTraceFilterAimbot : public ITraceFilter
    {
    public:
        CTraceFilterAimbot(void* a, void* b) : m_a(a), m_b(b) {}
        bool ShouldHitEntity(void* pEntity, int) override
        {
            if (pEntity == nullptr || pEntity == m_a || pEntity == m_b)
                return false;
            const void* commonVt = g_commonInfectedVtable;
            if (commonVt != nullptr)
            {
                void* entVt = nullptr;
                __try { entVt = *(void**)pEntity; }
                __except (EXCEPTION_EXECUTE_HANDLER) { entVt = nullptr; }
                if (entVt == commonVt)
                    return false;               // 小僵尸：当作透明
            }
            return true;
        }
        int GetTraceType() const override { return 0; }
    private:
        void* m_a;
        void* m_b;
    };

    class IEngineTrace
    {
    public:
        virtual int  GetPointContents(const sdk::Vector&, int, void**) = 0;                     // 0
        virtual int  GetPointContents_WorldOnly(const sdk::Vector&, int) = 0;                   // 1
        virtual int  GetPointContents_Collideable(void*, const sdk::Vector&) = 0;               // 2
        virtual void ClipRayToEntity(const Ray_t&, unsigned int, void*, CGameTrace*) = 0;       // 3
        virtual void ClipRayToCollideable(const Ray_t&, unsigned int, void*, CGameTrace*) = 0;  // 4
        virtual void TraceRay(const Ray_t&, unsigned int, ITraceFilter*, CGameTrace*) = 0;      // 5
    };

    // ---- EngineTraceClient003（懒获取并缓存）----
    //  [批153] 引擎接口懒获取必须串行！
    //  ------------------------------------------------------------
    //  旧实现是 `static bool s_tried` 裸标志：渲染线程（ESP 可见性）与
    //  游戏线程（自瞄可见性）可能【同时】看到 s_tried==false 并双双调用
    //  CreateInterface —— 而它在引擎内部是遍历接口链表 + 字符串比较，
    //  不可重入 → 崩在 VCRUNTIME 的 strcmp/memcpy（access=null）。
    //  崩溃日志里"渲染帧内 CRT 空指针"就是这条路径。
    std::mutex& InterfaceMutex()
    {
        static std::mutex s_mx;
        return s_mx;
    }

    void* EnsureEngineTrace()
    {
        static void* s_trace = nullptr;
        static bool  s_tried = false;
        if (!s_tried)
        {
            std::lock_guard<std::mutex> lk(InterfaceMutex());
            if (!s_tried)
            {
                s_tried = true;
                // [批153] 统一走加锁 helper（跨线程/跨 TU 同一把锁）
                s_trace = GetModuleInterfaceLocked("engine.dll",
                                                   "EngineTraceClient003");
            }
        }
        return s_trace;
    }

    // ============================================================
    //  角度数学（批2 角度计算的前置件）
    // ============================================================
    sdk::Vector DirectionTo(const sdk::Vector& from, const sdk::Vector& to)
    {
        sdk::Vector d = to - from;
        const float len = d.Length();
        return (len > 1e-6f) ? sdk::Vector(d.x / len, d.y / len, d.z / len) : sdk::Vector();
    }

    // 两方向夹角（度）
    float AngleBetweenDeg(const sdk::Vector& a, const sdk::Vector& b)
    {
        const float dot = a.Dot(b);
        return acosf(fminf(fmaxf(dot, -1.0f), 1.0f)) * (180.0f / sdk::kPi);
    }

    // ============================================================
    //  [批2] 角度计算：世界两点 → Source 视角（pitch/yaw）
    //  Source 约定：pitch 正=低头，yaw 绕 z 逆时针
    //  全程 isfinite 防线，奇点（hyp≈0）返回 false
    // ============================================================
    bool CalcAngle(const sdk::Vector& from, const sdk::Vector& to, sdk::QAngle& out)
    {
        const float dx = to.x - from.x;
        const float dy = to.y - from.y;
        const float dz = to.z - from.z;
        const float hyp = sqrtf(dx * dx + dy * dy);
        if (!(hyp > 1e-4f) && !(fabsf(dz) > 1e-4f))
            return false;                        // 两点重合：无方向

        out.pitch = -atan2f(dz, (hyp > 1e-4f) ? hyp : 1e-4f) * (180.0f / sdk::kPi);
        out.yaw   = atan2f(dy, dx) * (180.0f / sdk::kPi);
        out.roll  = 0.0f;

        // 归一化：pitch 钳制、yaw 环绕
        out.pitch = fminf(fmaxf(out.pitch, -89.0f), 89.0f);
        out.yaw   = sdk::NormalizeYaw(out.yaw);
        return isfinite(out.pitch) && isfinite(out.yaw);
    }

    // yaw 差值归一化到 [-180,180]（取最短转向路径）
    float AngleDiff(float to, float from)
    {
        float d = fmodf(to - from + 540.0f, 360.0f) - 180.0f;
        return d;
    }

    // ============================================================
    //  [批2] 视角引擎：GetViewAngles / SetViewAngles 槽位自探测
    // ------------------------------------------------------------
    //  VEngineClient013 的 8/9/12 槽已实证与 CSGO 同代布局一致，
    //  GetViewAngles/SetViewAngles 按同代惯例探测 18~21 号槽。
    //  · Get 探测：连续 4 次读取全部通过 sanity（有限值 + pitch/yaw 合理区间）
    //  · Set 验证：首次写入后读回比对（差 > 0.5° = 槽位错，永久停用写入）
    //  —— 全程失败安全：探测/验证不通过 = aim 不可用，绝不误写陌生槽位。
    // ============================================================
    struct ViewAngles { float pitch = 0.f, yaw = 0.f, roll = 0.f; };

    bool ViewSanity(const ViewAngles& v)
    {
        return isfinite(v.pitch) && isfinite(v.yaw) &&
               v.pitch >= -89.0f && v.pitch <= 89.0f &&
               v.yaw   >= -180.0f && v.yaw <= 180.0f;
    }

    class ViewEngine
    {
    public:
        bool Ready()
        {
            Resolve();
            return m_client && m_getSlot > 0;
        }

        bool Read(ViewAngles& out)
        {
            Resolve();
            if (!Ready())
                return false;
            // [批153] vtable 读取必须在 __try 内：m_client 可能是【陈旧指针】
            //   （切图/重连时客户端对象已被释放），解引用当场 AV。
            //   崩溃日志里 engine.dll+0x5D11C access=null 就是这条路径。
            float a[3] = { 0.f, 0.f, 0.f };
            bool ok = false;
            __try
            {
                void** vt = *reinterpret_cast<void***>(m_client);
                if (reinterpret_cast<uintptr_t>(vt) < 0x10000u ||
                    reinterpret_cast<uintptr_t>(vt) >= 0x7FFF0000u)
                    return false;
                reinterpret_cast<void(__thiscall*)(void*, float[3])>(
                    vt[m_getSlot])(m_client, a);
                ok = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            if (!ok) return false;
            out = ViewAngles{ a[0], a[1], a[2] };
            return ViewSanity(out);
        }

        bool Write(const ViewAngles& v)
        {
            Resolve();
            // [批96] 冷却到期后自动解除停用，重新尝试验证
            if (m_writeBroken && GetTickCount() >= m_writeBrokenUntil)
            {
                m_writeBroken = false;
            }
            if (!Ready() || m_writeBroken || !ViewSanity(v))
                return false;
            float a[3] = { v.pitch, v.yaw, v.roll };
            __try
            {
                // [批153] 同 Read：vtable 读取必须在 __try 内 + 指针校验
                void** vt = *reinterpret_cast<void***>(m_client);
                if (reinterpret_cast<uintptr_t>(vt) < 0x10000u ||
                    reinterpret_cast<uintptr_t>(vt) >= 0x7FFF0000u)
                    return false;
                reinterpret_cast<void(__thiscall*)(void*, const float[3])>(
                    vt[m_setSlot])(m_client, a);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }

            // [安全] 首次写入读回验证：差 > 0.5° = 槽位错 → 临时停用
            // [批96] 原"永久停用"是换图后平滑失效的根因：
            //   引擎在地图加载期可能短暂返回临时/异常角度 → 读回验证
            //   失败 → m_writeBroken=true 永不复位 → 之后所有 Write 直接
            //   return false → 平滑视角永远不生效。静默不调 Write 所以
            //   不受影响 —— 这正是"换图后自瞄失效但静默还有效"的原因。
            //   改为冷却 3 秒后允许重试，引擎恢复后自动重新验证。
            if (!m_writeVerified)
            {
                ViewAngles back{};
                if (Read(back) &&
                    fabsf(back.pitch - v.pitch) < 0.5f &&
                    fabsf(back.yaw   - v.yaw)   < 0.5f)
                {
                    m_writeVerified = true;
                    PayloadLog("[aim] SetViewAngles 槽位 %d 读回验证通过", m_setSlot);
                }
                else
                {
                    m_writeBroken = true;
                    m_writeBrokenUntil = GetTickCount() + 3000;  // 冷却 3 秒
                    PayloadLog("[aim] WARN: SetViewAngles 槽位 %d 读回验证失败，"
                               "冷却 3 秒后重试", m_setSlot);
                    return false;
                }
            }
            return true;
        }

    private:
        // [批153] 取接口单独一个函数：lock_guard 不能和 __try 同处一个函数
        //   （MSVC C2712：要求对象展开的函数里不能用 __try）。锁只包这一小段。
        void* FetchClientLocked()
        {
            std::lock_guard<std::mutex> lk(InterfaceMutex());
            return GetModuleInterfaceLocked("engine.dll", "VEngineClient013");
        }

        void Resolve()
        {
            if (m_probed)
                return;
            m_probed = true;
            m_client = FetchClientLocked();
            if (!m_client)
                return;

            // [批153] vtable 读取同样要在 __try 内（切图后 m_client 可能已陈旧）
            void** vt = nullptr;
            __try { vt = *reinterpret_cast<void***>(m_client); }
            __except (EXCEPTION_EXECUTE_HANDLER) { vt = nullptr; }
            if (!vt)
                return;
            for (int slot = 18; slot <= 21; ++slot)
            {
                bool allSane = true;
                for (int k = 0; k < 4 && allSane; ++k)
                {
                    float a[3] = { 0.f, 0.f, 0.f };
                    bool called = false;
                    __try
                    {
                        reinterpret_cast<void(__thiscall*)(void*, float[3])>(
                            vt[slot])(m_client, a);
                        called = true;
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        allSane = false;
                    }
                    if (!called) break;
                    const ViewAngles v{ a[0], a[1], a[2] };
                    if (!ViewSanity(v)) allSane = false;
                }
                if (allSane)
                {
                    m_getSlot = slot;
                    m_setSlot = slot + 1;
                    PayloadLog("[aim] GetViewAngles 槽位=%d（SetViewAngles=%d）",
                               slot, m_setSlot);
                    return;
                }
            }
            PayloadLog("[aim] WARN: GetViewAngles 槽位探测失败（18~21），aim 不可用");
        }

        void* m_client   = nullptr;
        int   m_getSlot  = -1;
        int   m_setSlot  = -1;
        bool  m_probed       = false;
        bool  m_writeVerified = false;
        bool  m_writeBroken   = false;
        DWORD m_writeBrokenUntil = 0;    // [批96] 冷却到期时间
    };

    ViewEngine& ViewEng()
    {
        static ViewEngine s_ve;
        return s_ve;
    }

    // [批4] 迟滞防抖：上一帧胜出目标（实体索引，-1=无）。仅游戏线程访问。
    int s_prevTargetForHysteresis = -1;
    // [批134] 最小锁定窗口：换靶后 300ms 内不换
    DWORD s_minLockUntil = 0;
    // [批123] 可见性宽限（丢失宽限）状态表
    //   键 = 实体索引 & 255；值 = 最近一次"真正可见"的时刻/瞄准点。
    //   用途：可见性瞬时失败时，同一目标在 180ms 内仍允许保留，
    //   避免"丢失→重新锁定→reactionMs 重置"造成的一顿一顿。
    constexpr int kVisTrack = 256;
    DWORD       s_lastVisMs[kVisTrack] = {};
    sdk::Vector s_lastVisPos[kVisTrack] = {};
    unsigned char s_visMiss[kVisTrack] = {};   // [批123] 连续不可见帧数

    // [批5·L4] 漂移门控记忆（上帧瞄准点）。仅游戏线程访问。
    int         s_driftIdxLast = -1;
    sdk::Vector s_driftPosLast{};
    sdk::Vector s_driftOriginLast{};     // [批147] 上一帧的实体原点（身体位移基准）

    // [批5·L3/L4] 进程内 SEH 读（碰撞盒 netvar 字段用；实体地址刚验过有效性）
    bool ReadVec3At(uintptr_t addr, sdk::Vector& out)
    {
        __try
        {
            out = *reinterpret_cast<const sdk::Vector*>(addr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        return isfinite(out.x) && isfinite(out.y) && isfinite(out.z);
    }

    // ============================================================
    //  [批9] GetAimPosition —— 全文件唯一的瞄准点决策点（纯净版）
    // ------------------------------------------------------------
    //  链路（全部动态获取，零硬编码偏移）：
    //    实体 m_nModelIndex → VModelInfoClient004::GetModel
    //    → model_t 内 studiohdr_t* → hitbox set 遍历（HitboxSystem 实现）
    //    → group == HITGROUP_HEAD(1) 过滤（引擎枚举，跨模型稳定；
    //      Spitter/Jockey/Charger 错位只影响躯干/四肢，头部恒为 1）
    //    → 盒中心（模型局部）× 骨骼 matrix3x4 → 世界坐标（VectorTransform）
    //  铁律（用户规格，2026-10-19）：
    //    · 单盒数据异常 → HitboxSystem 填充期逐盒 continue 跳过，
    //      绝不 break 去 Origin 兜底
    //    · 全链失败 → 返回 (0,0,0) + 节流错误日志，上层弃权该目标
    //    · 本文件不存在任何 "+64 / ×0.90" 式数字兜底——
    //      瞄准点只有这一个出生地
    // ============================================================
    // ============================================================
    //  [批23] 部位骨骼按【骨名】解析（studiohdr 骨名表，唯一权威）
    // ------------------------------------------------------------
    //  之前按骨骼编号（14/13/12/10）猜部位——Charger/Jockey 骨骼顺序
    //  不同 → 头部位指到腹部/脚（用户实测）。骨名表在任何模型上都
    //  指向正确的关节：
    //    头  = 含 "head"   （ValveBiped.Bip01_Head1）
    //    颈  = 含 "neck"
    //    胸  = 含 "spine4"（上胸；回退 spine3）
    //    腹  = 含 "spine1"（回退 spine）
    //    腿  = 含 "thigh"
    //  studiohdr: numbones@0x9C、boneindex@0xA0（相对 hdr 偏移）；
    //  mstudiobone_t.sznameindex@+0（相对骨结构自身）；stride 每会话
    //  一次探测（bone0 名字须含 Bip01/ValveBiped）。
    // ============================================================
    struct PartBones
    {
        int head = -1, neck = -1, chest = -1, stomach = -1, leg = -1;
    };
    static std::unordered_map<int, PartBones> g_partBoneCache;

    bool ReadBytesAt(uintptr_t addr, void* dst, size_t n)
    {
        __try
        {
            std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool ReadInt32At(uintptr_t addr, int& out)
    {
        __try
        {
            out = *reinterpret_cast<const int*>(addr);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // 读某骨结构处的名字（sznameindex 相对骨结构起点）
    // [批40·修复P1-2] stride 由调用方传入（HitboxSystem 共享探测），
    //  不再使用各处自持的全局 stride
    bool ReadBoneName(uintptr_t hdr, int boneIdx, int stride, char* out, int cap)
    {
        if (stride <= 0 || boneIdx < 0)
            return false;
        int bio = 0;
        if (!ReadInt32At(hdr + 0xA0, bio) || bio <= 0 || bio >= 0x10000)
            return false;
        const uintptr_t bone = hdr + (uintptr_t)bio +
                               (uintptr_t)boneIdx * (uintptr_t)stride;
        int sz = 0;
        if (!ReadInt32At(bone, sz) || sz <= 0 || sz >= 0x2000)
            return false;
        char buf[96] = {};
        const int n = cap < 96 ? cap : 96;
        if (!ReadBytesAt(bone + (uintptr_t)sz, buf, (size_t)(n - 1)))
            return false;
        if (!buf[0])
            return false;
        std::memcpy(out, buf, (size_t)n);
        out[cap - 1] = '\0';
        return true;
    }

    void LowerInPlace(char* s)
    {
        for (; *s; ++s)
            if (*s >= 'A' && *s <= 'Z')
                *s = (char)(*s - 'A' + 'a');
    }

    bool ResolvePartBones(const sdk::CBaseEntity& e, PartBones& out)
    {
        int mi = 0;
        const uintptr_t mo = NetVars::ModelIndex();
        if (!mo || !ReadInt32At(e.Address() + mo, mi) || mi <= 0)
            return false;

        const auto it = g_partBoneCache.find(mi);
        if (it != g_partBoneCache.end())
        {
            out = it->second;
            return out.head >= 0;
        }

        PartBones pb{};
        int stride = 0;                               // [批40] 函数级作用域（日志用）
        const uintptr_t hdr = GetEntityStudioHdr(e.Address(), GetModelInfoIface());
        if (hdr)
        {
            // [批40·修复P1-2] stride 用 HitboxSystem 的共享探测（每 hdr 一次）
            stride = HitboxGetBoneStride(hdr);
            int nb = 0;
            ReadInt32At(hdr + 0x9C, nb);              // numbones
            if (stride > 0 && nb > 0 && nb <= 256)
            {
                {
                    for (int b = 0; b < nb; ++b)
                    {
                        char nm[64] = {};
                        if (!ReadBoneName(hdr, b, stride, nm, sizeof(nm)))
                            continue;
                        LowerInPlace(nm);
                        // [批38] 头骨名兼容："head" 与 "bip_head"
                        // （ValveBiped 命名两种写法都覆盖）
                        if (pb.head < 0 &&
                            (strstr(nm, "head") || strstr(nm, "bip_head")))
                            pb.head = b;
                        else if (pb.neck < 0 && strstr(nm, "neck"))
                            pb.neck = b;
                        else if (pb.chest < 0 && (strstr(nm, "spine4") ||
                                                  strstr(nm, "spine3")))
                            pb.chest = b;
                        else if (pb.stomach < 0 && strstr(nm, "spine1"))
                            pb.stomach = b;
                        else if (pb.leg < 0 && strstr(nm, "thigh"))
                            pb.leg = b;
                    }
                }
            }
        }

// [批40·修复P1-2] 只有解析出头骨才缓存；失败不缓存（下一帧重试，
    //  引擎侧已有 BoneSystem 负缓存兜底，不会刷爆引擎调用）
    //
    //  [批111·性能] 加容量上限。键是模型索引 mi，值 PartBones，
    //  正常一局内模型种类只有几十种；但换图/自定义资源包会不断引入
    //  新 mi，且本缓存【只在解析成功时写入、没有任何淘汰逻辑】
    //  —— 长时间游戏（多图轮换）会单调增长，属于典型的"越跑越慢"。
    //  上限取 512（与 BoneSystem/HitboxSystem 的 g_cache 同量级）：
    //  远大于任何一局的模型种类数，触发即整体重建，代价可忽略。
    if (pb.head >= 0)
    {
        if (g_partBoneCache.size() > 512)
            g_partBoneCache.clear();
        g_partBoneCache[mi] = pb;
    }
        PayloadLog("[AIM] 骨名解析：idx=%d mi=%d hdr=0x%X stride=0x%X → "
                   "头=%d 颈=%d 胸=%d 腹=%d 腿=%d%s",
                   e.Index(), mi, (unsigned)hdr, stride,
                   pb.head, pb.neck, pb.chest, pb.stomach, pb.leg,
                   pb.head >= 0 ? "" : "（未缓存，将重试）");
        out = pb;
        return pb.head >= 0;
    }

    // ============================================================
    //  [批27] 骨骼区域法 —— 零假设部位定位（用户思路落地）
    // ------------------------------------------------------------
    //  1) 先把【全部骨骼】读出来（0..127，SetupBones 128 骨缓冲）
    //  2) 按高度三分：底段=腿/下段 中段=腹胸 顶段=头颈
    //  3) 取【区域】内的骨位置平均 = 部位点（单骨摆动被平均吸收）
    //  不依赖骨名/骨骼编号/hitbox——任何模型的骨架都适用。
    //    头 = 顶部 10%   颈 = 82%~92%
    //    胸 = 62%~75%   腹 = 42%~55%
    //  （相对骨高度范围；蹲/爬等姿态比例不变）
    // ============================================================
    // ============================================================
    //  [批38·重写] 骨骼区域法 —— 截尾统计 + 废骨过滤（用户规格）
    //  ------------------------------------------------------------
    //  旧版直接用全部骨骼的 maxZ/minZ：Jockey 等模型的异形拉伸骨
    //  （骑乘链/舌头/武器骨）会把 maxZ 顶到天上 → 头部区域完全偏离。
    //  新版三道防线：
    //    ① 废骨过滤：与实体 Origin 的【水平距离】> 40u 的骨剔除
    //       （舌头/武器/手指等横向拉伸骨）；垂直范围 sanity（-20~220u）
    //    ② 截尾统计：Z 值排序后去掉最高 5% 与最低 5%，
    //       用截尾后的 min/max 作为身体高度范围（异形骨不再污染）
    //    ③ 区域平均：区域内的骨位置取平均（单骨摆动被吸收）
    //  部位区域（相对截尾后高度）：头=顶10% 颈=82~92% 胸=62~75%
    //                             腹=42~55% 腿=2~30%
    // ============================================================
    bool PartByRegion(const uintptr_t base, const sdk::Vector& o, int part,
                      sdk::Vector& out, int* outCount, float* outMinZ,
                      float* outMaxZ)
    {
        sdk::Vector pos[128];
        bool  ok[128] = {};
        float zs[128] = {};
        int   zsN = 0;
        int   n = 0;

        for (int b = 0; b < 128; ++b)
        {
            const uintptr_t ma = base + (uintptr_t)b * 0x30u;
            float bx = 0, by = 0, bz = 0;
            __try
            {
                bx = *reinterpret_cast<const float*>(ma + 0x0C);
                by = *reinterpret_cast<const float*>(ma + 0x1C);
                bz = *reinterpret_cast<const float*>(ma + 0x2C);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            if (!isfinite(bx) || !isfinite(by) || !isfinite(bz))
                continue;

            // ① 废骨过滤：水平距离 > 40u = 横向拉伸骨（舌头/武器/手指）
            const float hx = bx - o.x;
            const float hy = by - o.y;
            if (hx * hx + hy * hy > 40.0f * 40.0f)
                continue;
            // 垂直 sanity：骨骼不会低于脚底 20u、不会高于 220u
            if (bz - o.z < -20.0f || bz - o.z > 220.0f)
                continue;

            pos[b] = sdk::Vector(bx, by, bz);
            ok[b]  = true;
            ++n;
            if (zsN < 128)
                zs[zsN++] = bz;
        }
        if (outCount) *outCount = n;
        if (n < 8)
            return false;

        // ② 截尾统计：排序 → 去最高 5% / 最低 5% → 截尾后的高度范围
        {
            for (int i = 1; i < zsN; ++i)               // 插入排序（≤128，够快）
            {
                const float v = zs[i];
                int j = i - 1;
                while (j >= 0 && zs[j] > v)
                {
                    zs[j + 1] = zs[j];
                    --j;
                }
                zs[j + 1] = v;
            }
        }
        const int trim = (zsN >= 20) ? (int)(zsN * 0.05f) : (zsN >= 10 ? 1 : 0);
        const int loI  = trim;
        const int hiI  = zsN - 1 - trim;
        if (hiI <= loI)
            return false;
        const float loZ = zs[loI];
        const float hiZ = zs[hiI];
        if (outMinZ) *outMinZ = loZ;
        if (outMaxZ) *outMaxZ = hiZ;

        const float span = hiZ - loZ;
        if (span < 10.0f)
            return false;                             // 高度异常（半初始化等）

        // ③ 区域 → 区间内骨位置平均
        float lo = 0.0f, hi = 0.0f;
        switch (part)
        {
        case 0: lo = hiZ - 0.10f * span; hi = hiZ + 0.01f;       break; // 头
        case 1: lo = loZ + 0.82f * span; hi = loZ + 0.92f * span; break; // 颈
        case 2: lo = loZ + 0.62f * span; hi = loZ + 0.75f * span; break; // 胸
        case 3: lo = loZ + 0.42f * span; hi = loZ + 0.55f * span; break; // 腹
        case 4: lo = loZ + 0.02f * span; hi = loZ + 0.30f * span; break; // 腿
        default: return false;
        }

        sdk::Vector acc(0.0f, 0.0f, 0.0f);
        int c = 0;
        for (int b = 0; b < 128; ++b)
        {
            if (!ok[b] || pos[b].z < lo || pos[b].z > hi)
                continue;
            acc.x += pos[b].x;
            acc.y += pos[b].y;
            acc.z += pos[b].z;
            ++c;
        }
        if (c <= 0)
            return false;
        out = sdk::Vector(acc.x / (float)c, acc.y / (float)c,
                          acc.z / (float)c);
        return true;
    }

    // 骨表一次性 dump（每模型一次）：idx + 相对原点 Z——按高度直接看出
    // 该模型各骨的分布（Charger/Jockey 异常结构一目了然）
    void DumpBoneTable(const uintptr_t base, const sdk::Vector& o, int mi)
    {
        static int s_dumpedMi[16] = {};
        static int s_dumpedN = 0;
        for (int i = 0; i < s_dumpedN; ++i)
            if (s_dumpedMi[i] == mi)
                return;
        if (s_dumpedN < 16)
            s_dumpedMi[s_dumpedN++] = mi;

        // [批33·安全] 安全追加：snprintf 返回本应写入长度，截断时会大于
        // 实际写入量 —— 直接累加会让 cc 超过 buf 尺寸，下一轮
        // sizeof(buf)-cc 下溢成巨大 size_t → 越界写栈 → GS cookie 破坏
        // → __report_gsfailure（0xc0000409）。这里按实际写入量钳位。
        char buf[1024] = {};
        int cc = 0;
        int truncated = 0;
        for (int b = 0; b < 40; ++b)
        {
            if (cc > (int)sizeof(buf) - 24)
            {
                truncated = 1;
                break;
            }
            const uintptr_t ma = base + (uintptr_t)b * 0x30u;
            float bz = 0;
            __try
            {
                bz = *reinterpret_cast<const float*>(ma + 0x2C);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                continue;
            }
            const int dz = (int)(bz - o.z);
            const int n = snprintf(buf + cc, sizeof(buf) - (size_t)cc,
                                   "%d:%d ", b, dz);
            if (n < 0)
                break;
            cc += (n < (int)sizeof(buf) - cc) ? n : (int)sizeof(buf) - cc - 1;
        }
        if (truncated)
            PayloadLog("[bone-warn] 骨表 dump 截断（缓冲区已满，骨骼数可能 >40）");
        PayloadLog("[AIM] 骨表 mi=%d（idx:相对Z）%s", mi, buf);
    }

    // ============================================================
    //  [批146] 瞄准点来源三态
    // ------------------------------------------------------------
    //  把"几何兜底"与"彻底失败"在【类型层面】分开：外层只认 Fail 才
    //  continue。以前二态（usedFallback）让"几何点"和"解析失败"共用一个
    //  标志，外层一旦按老习惯 continue，几何点就被丢掉 —— 这是软锁长期
    //  为 0 的根源之一。
    //    Visible       当前可见的部位
    //    Geometric     全不可见 → 屏幕最近的几何点（可能是肢体）
    //    ReuseLastPart 全不可见 → 沿用上一帧部位（连续不可见 ≤3 帧）
    //    Fail          骨骼都读不到 —— 唯一该 continue 的情况
    // ============================================================
    //  [批149] 兜底层：碰撞盒顶 / 实体眼高。
    //    这两类点【不是真部位】——它们只是"打不到总比打空气强"的猜测，
    //    必须在评分上重罚、并且必须过可见性射线才允许写视角。
    enum class AimPointKind
    {
        Visible,            // 当前可见部位（真点）
        Geometric,          // 全不可见 → 屏幕最近的几何点
        ReuseLastPart,      // 全不可见 → 沿用上一帧部位
        FallbackBoxTop,     // 兜底层2：碰撞盒顶（Origin + maxs.z - 4）
        FallbackViewOffset, // 兜底层3：实体眼高（Origin + m_vecViewOffset）
        Fail                // 连兜底都读不到（唯一该 continue 的）
    };

    struct AimPoint
    {
        sdk::Vector pos{};
        AimPointKind kind = AimPointKind::Fail;
        int partIndex = -1;                       // 0头 1胸 2腹 3腿（肢体判定用）
        bool ok() const { return kind != AimPointKind::Fail; }
    };

    // 可见性采样点总数（头/胸/双肩/髋/双膝）—— 命中计数降权用
    constexpr int kVisSampleCount = 7;

    // [批164] tick 序号：GetBestTarget 每次调用自增（= 一个 CreateMove tick），
    //   SolveTargetAngle 用它判断"胜者的瞄准点是不是本 tick 刚算的"。
    int g_aimTickSeq = 0;

    // ============================================================
    //  [批149] 兜底链 层2/层3（绝不返回零向量，但绝不冒充"可见部位"）
    //  ------------------------------------------------------------
    //  层2 碰撞盒顶：Origin + (0,0, maxs.z - 4)
    //       引擎的物理碰撞盒永远是正确的，而且随姿态同步：
    //       Jockey（骑乘）顶高只有 ~40，Tank ~100 —— 天然适配各种体型，
    //       不像固定 +64 那样对所有特感一刀切。
    //  层3 实体眼高：Origin + m_vecViewOffset（引擎自己的眼高）
    //  两层的点都标成 Fallback* 态：外层评分重罚 + 可见性射线必须通过，
    //  这正是当年 Origin+64 "打空气/打地板" 的区别 —— 当年那个是被当成
    //  【可见部位】直接拿去平滑了。
    // ============================================================
    // ============================================================
    //  [批161] 按 sid 的兜底锚点比例（占碰撞盒净高的百分比）
    //  ------------------------------------------------------------
    //  盒中点(0.50)对多数模型=躯干，但各特感身形差异大：
    //    Jockey 0.55 —— 弓身小模型，盒中点已在其头颈，略抬打头
    //    Charger 0.45 —— 驼背，盒中点偏低是背，略降
    //    Tank/Witch 0.60 —— 直立大体型，偏高是胸
    //  未列出的用 0.50。想微调手感改这一张表即可。
    // ============================================================
    float FallbackAnchorRatio(int sidAbs)
    {
        switch (sidAbs)
        {
        case sdk::sid::kJockey:  return 0.55f;
        case sdk::sid::kCharger: return 0.45f;
        case sdk::sid::kTank:    return 0.60f;
        case sdk::sid::kWitch:   return 0.60f;
        default:                 return 0.50f;
        }
    }

    bool TryFallbackAimPoint(const sdk::CBaseEntity& e, const sdk::Vector& origin,
                             sdk::Vector& outPos, AimPointKind& outKind,
                             int sidAbs)
    {
        const auto& off = NetVars::Get();
        // ---- 层2：碰撞盒【垂直中点】----
        //  [批160] 旧锚点 maxs.z-4 对弓身/骑乘模型严重过冲：
        //    Jockey 碰撞盒 maxs.z≈68 → 瞄准点 ≈ Origin+64（与当年
        //    Origin+64 一模一样），而它弓身姿态的可见模型只有 ~35 高
        //    → 比"头"高出 ~30 单位 → 打空气（用户实测确认：
        //    准星跟得住 Jockey，但瞄的是一个固定的点位而不是实体）。
        //  L4D2 的碰撞盒不随姿态收缩是常态，锚点改取【盒高中点】：
        //    Jockey(0..68) → 34 ≈ 头颈部 ✓  Tank(0..100) → 50 = 躯干 ✓
        //    站立感染者(0..72) → 36 = 胸口 ✓
        //  枚举名 FallbackBoxTop 保留（判据/日志引用），语义已变为盒中点。
        if (off.collisionMins && off.collisionMaxs)
        {
            sdk::Vector mn{}, mx{};
            if (ReadVec3At(e.Address() + off.collisionMins, mn) &&
                ReadVec3At(e.Address() + off.collisionMaxs, mx) &&
                mx.z > 8.0f && mx.z < 200.0f && mx.z > mn.z)
            {
                const float ratio = FallbackAnchorRatio(sidAbs);
                outPos = sdk::Vector(origin.x, origin.y,
                                     origin.z + mn.z + (mx.z - mn.z) * ratio);
                outKind = AimPointKind::FallbackBoxTop;   // 名沿旧，实为盒内锚点
                return true;
            }
        }
        // ---- 层3：实体眼高 ----
        const uintptr_t vo = NetVars::ViewOffset();
        if (vo)
        {
            sdk::Vector v{};
            if (ReadVec3At(e.Address() + vo, v) &&
                fabsf(v.z) > 8.0f && fabsf(v.z) < 200.0f)
            {
                outPos = sdk::Vector(origin.x + v.x, origin.y + v.y, origin.z + v.z);
                outKind = AimPointKind::FallbackViewOffset;
                return true;
            }
        }
        // [批159] 兜底链整体失败时记录一次现场：偏移缺失还是数值校验拒绝，
        //   一眼定位。没有这条，兜底链死了日志上完全无声（实测 107 次
        //   AIM-ERR 配 0 次兜底点，就是这个形态）。
        {
            static bool s_diagOnce = false;
            if (!s_diagOnce)
            {
                s_diagOnce = true;
                sdk::Vector mxRead{}, vRead{};
                const bool mxReadOk = off.collisionMaxs
                    ? ReadVec3At(e.Address() + off.collisionMaxs, mxRead) : false;
                const bool vReadOk = vo ? ReadVec3At(e.Address() + vo, vRead) : false;
                PayloadLog("[AIM] 兜底链失败现场：collisionMaxs=0x%X viewOffset=0x%X "
                           "mxRead=%d mx.z=%.1f vRead=%d v.z=%.1f（origin z=%.1f）",
                           (unsigned)off.collisionMaxs, (unsigned)vo,
                           mxReadOk ? 1 : 0, mxRead.z,
                           vReadOk ? 1 : 0, vRead.z, origin.z);
            }
        }
        return false;
    }

    AimPoint GetAimPosition(const sdk::CBaseEntity& e, int partGroup,
                            int sidAbs = -1)
    {
        static DWORD s_failLog = 0;

        if (partGroup < 0 || partGroup > 3)
            return AimPoint{};                       // [批146] Fail

        const uintptr_t ent = e.Address();
        if (!ent)
            return AimPoint{};                       // [批146] Fail

        const sdk::Vector o = e.GetOrigin();
        // [批109] 必须用与取骨端同源的地址去查骨骼缓存。
        //   e.Address() 与 GetEntAddr(idx) 不一致时，g_boneStore 永远
        //   查不到 → 返回 0 → "骨骼矩阵不可用" → 自瞄退到兜底点 = 空气。
        //   日志里"先瞄头后瞄空气"的根因就在这里（取骨健康，读取失配）。
        const uintptr_t ent2 = BoneSysEntityAddr(e.Index());
        if (ent2 && ent2 != ent)
        {
            // 地址确实不一致 —— 记一次（3s 节流），修完后这条应彻底消失
            static DWORD s_addrLog = 0;
            const DWORD nowA = GetTickCount();
            if (nowA - s_addrLog > 3000)
            {
                s_addrLog = nowA;
                PayloadLog("[AIM] 实体地址失配：idx=%d SDK=0x%X 取骨端=0x%X"
                           " —— 已改用取骨端地址",
                           e.Index(), (unsigned)ent, (unsigned)ent2);
            }
        }
        const uintptr_t lookup = ent2 ? ent2 : ent;
        const uintptr_t base = GetEntityBoneMatrixBase(lookup, Vec3(o.x, o.y, o.z));
        if (!base)
        {
            if (GetTickCount() - s_failLog > 3000)
            {
                s_failLog = GetTickCount();
                PayloadLog("[AIM-ERR] 骨骼矩阵不可用：idx=%d（SetupBones/curtime）",
                           e.Index());
            }
            return AimPoint{};                       // [批146] Fail
        }

        auto ReadBone = [&](int b, sdk::Vector& out) -> bool
        {
            if (b < 0 || b > 127)
                return false;
            const uintptr_t ma = base + (uintptr_t)b * 0x30u;
            float bx = 0, by = 0, bz = 0;
            __try
            {
                bx = *reinterpret_cast<const float*>(ma + 0x0C);
                by = *reinterpret_cast<const float*>(ma + 0x1C);
                bz = *reinterpret_cast<const float*>(ma + 0x2C);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
            if (!isfinite(bx) || !isfinite(by) || !isfinite(bz) ||
                fabsf(bx - o.x) > 320.0f || fabsf(by - o.y) > 320.0f ||
                bz - o.z < -320.0f || bz - o.z > 320.0f)
                return false;
            out = sdk::Vector(bx, by, bz);
            return true;
        };

        // ============================================================
        //  [批32] 主路径：通用 hitbox 部位解析（HitboxSystem）
        //  · 动态遍历 CStudioHdr 的 hitbox 列表（不硬编码 group==1）
        //  · 头部判定 = 绑定骨骼【骨名】含 head，回退 = 几何高度区间
        //  · 局部中心 → VectorTransform(bone→world) → 世界坐标（一次变换）
        // ============================================================
        {
            static const int kKind[4] = { HBP_Head, HBP_Neck, HBP_Chest, HBP_Stomach };
            const char* fail = nullptr;
            Vec3 wp{};
            if (GetEntityPartPoint(e.Index(), Vec3(o.x, o.y, o.z),
                                   kKind[partGroup], wp, &fail))
                return AimPoint{ sdk::Vector(wp.x, wp.y, wp.z),
                                 AimPointKind::Visible, partGroup };
            if (GetTickCount() - s_failLog > 3000)
            {
                s_failLog = GetTickCount();
                PayloadLog("[AIM-ERR] hitbox 部位解析失败：idx=%d 部位=%d 原因=%s",
                           e.Index(), partGroup, fail ? fail : "未知");
            }
        }

        // ============================================================
        //  [批27] 回退：骨骼区域法（hitbox 链不可用时）
        //  先把全部骨骼读出来（PartByRegion 内部 0..127），按高度三分
        //  取区域平均。骨表每模型 dump 一次供核对。
        // ============================================================
        int mi = 0;
        {
            const uintptr_t mo = NetVars::ModelIndex();
            if (mo)
            {
                __try
                {
                    mi = *reinterpret_cast<const int*>(e.Address() + mo);
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    mi = 0;
                }
            }
        }
        DumpBoneTable(base, o, mi);

        // ============================================================
        //  [批29] 骨名解析【优先】——唯一精确的方法。
        //  骨表实证：不同模型骨骼布局差异极大
        //    mi=167/168/169：标准 ValveBiped（13颈/14头）
        //    mi=170：骨7/8=71、10=72 高于头（65）→ 取最高会瞄到头上方
        //    mi=171：骨1-4 递增到 42（骑乘链），头只有 19 → 瞄到天上
        //    mi=172：骨14 = 4（脚！）→ 固定编号打脚
        //  只有骨名表能跨模型精确定位；区域法仅作最后兜底。
        // ============================================================
        PartBones pb{};
        if (ResolvePartBones(e, pb))
        {
            int idx = -1;
            switch (partGroup)
            {
            case 0: idx = pb.head;    break;
            case 1: idx = pb.neck;    break;
            case 2: idx = pb.chest;   break;
            case 3: idx = pb.stomach; break;
            }
            sdk::Vector out{};
            if (idx >= 0 && ReadBone(idx, out))
                return AimPoint{ out, AimPointKind::Visible, partGroup };
        }

        // 兜底：骨骼区域法（骨名链不可用时）
        sdk::Vector regionPt{};
        if (PartByRegion(base, o, partGroup, regionPt, nullptr, nullptr, nullptr))
            return AimPoint{ regionPt, AimPointKind::Visible, partGroup };

        // ============================================================
        //  [批38→批112] 全部解析链失败 → 返回零向量，由上层跳过。
        //  旧 Origin+64 兜底已删除：拿不到真实骨骼点就不瞄，
        //  绝不用裸 m_vecOrigin 猜坐标去开枪（"锁空气"源头之一）。
        //  旧 kFallback[4]={14,13,12,10} 盲猜索引更早已删（mi=172
        //  的骨14在脚上，畸形模型直接报废）。
        // ============================================================
        {
            static DWORD s_allFailLog = 0;
            const DWORD now = GetTickCount();
            if (now - s_allFailLog > 3000)
            {
                s_allFailLog = now;
                PayloadLog("[AIM-ERR] 全部部位解析链失败：idx=%d 部位=%d"
                           " → 返回零向量，本帧不瞄（无 Origin 兜底）",
                           e.Index(), partGroup);
            }
            // [批149] 兜底链：hitbox / 骨名 / 区域 全失败 → 层2 碰撞盒顶
            //   → 层3 眼高。绝不返回零向量，但标成 Fallback* 态（重罚 +
            //   可见性校验），不会像当年的 Origin+64 那样当"可见部位"用。
            {
                sdk::Vector fb{};
                AimPointKind kind = AimPointKind::Fail;
                if (TryFallbackAimPoint(e, o, fb, kind, sidAbs))
                {
                    static DWORD s_fbLog = 0;
                    const DWORD nowF = GetTickCount();
                    if (nowF - s_fbLog > 3000)
                    {
                        s_fbLog = nowF;
                        PayloadLog("[AIM] 兜底点：idx=%d 层=%s 部位=%d",
                                   e.Index(),
                                   kind == AimPointKind::FallbackBoxTop
                                       ? "碰撞盒中点" : "眼高", partGroup);
                    }
                    return AimPoint{ fb, kind, -1 };
                }
            }
            return AimPoint{};                       // [批146] Fail（唯一该 continue 的）
        }
    }

    // ============================================================
    //  [批19] GetAutoAimPosition —— 自动模式（骨骼版）
    // ------------------------------------------------------------
    //  ① 部位候选集（骨骼直瞄，全部特感骨架都有）：头14/胸12/腹10/腿2
    //  ② 每部位：WorldToScreen 投影 → 与屏幕中心的 2D 像素距
    //  ③ 可见性过滤在比距【之前】：TraceRay 不通的部位直接剔除
    //     （绝不锁墙后的脚）
    //  ④ 最优解 = 屏幕距离最小的可见部位
    //  ⑤ [批146] 全不可见 → 返回三态里的 Geometric / ReuseLastPart，
    //     由上层交给宽限期判定（不再用二态标志把几何点误当"解析失败"）
    // ============================================================
    AimPoint GetAutoAimPosition(const sdk::CBaseEntity& e, int sidAbs = -1)
    {
        static DWORD s_autoFailLog = 0;
        AimPoint out;
        const char* fail = nullptr;

        const uintptr_t ent = e.Address();
        const sdk::Vector o = e.IsValid() ? e.GetOrigin()
                                          : sdk::Vector(0, 0, 0);
        // [批109] 同 GetAimPosition：查缓存必须用与取骨端同源的地址
        const uintptr_t ent2 = BoneSysEntityAddr(e.Index());
        const uintptr_t lookup = ent2 ? ent2 : ent;
        const uintptr_t base = lookup ? GetEntityBoneMatrixBase(lookup, Vec3(o.x, o.y, o.z))
                                    : 0;
        if (!base)
            fail = "骨骼矩阵不可用（SetupBones/curtime）";
        else
        {
            // [批29] 骨名优先（头/胸/腹/腿），区域法兜底
            const int kPart[4] = { 0, 2, 3, 4 };
            PartBones pb{};
            const bool named = ResolvePartBones(e, pb);

            const sdk::CBaseEntity local = sdk::EntityList::LocalPlayer();
            const sdk::Vector eye = local.IsValid()
                                  ? local.GetEyePos() : sdk::Vector(0, 0, 0);

            sdk::Vector partPos[4] = {};
            bool partFound[4] = {};
            for (int p = 0; p < 4; ++p)
            {
                sdk::Vector pt{};
                bool got = false;
                if (named)
                {
                    int idx = -1;
                    switch (p)
                    {
                    case 0: idx = pb.head;    break;
                    case 1: idx = pb.chest;   break;
                    case 2: idx = pb.stomach; break;
                    case 3: idx = pb.leg;     break;
                    }
                    if (idx >= 0)
                    {
                        const uintptr_t ma = base + (uintptr_t)idx * 0x30u;
                        float bx = 0, by = 0, bz = 0;
                        __try
                        {
                            bx = *reinterpret_cast<const float*>(ma + 0x0C);
                            by = *reinterpret_cast<const float*>(ma + 0x1C);
                            bz = *reinterpret_cast<const float*>(ma + 0x2C);
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER)
                        {
                            bx = by = bz = 0.0f;
                        }
                        if (isfinite(bx) && isfinite(by) && isfinite(bz) &&
                            fabsf(bx - o.x) <= 320.0f &&
                            fabsf(by - o.y) <= 320.0f &&
                            bz - o.z >= -320.0f && bz - o.z <= 320.0f)
                        {
                            pt = sdk::Vector(bx, by, bz);
                            got = true;
                        }
                    }
                }
                if (!got)
                    got = PartByRegion(base, o, kPart[p], pt, nullptr, nullptr, nullptr);
                if (got)
                {
                    partPos[p]   = pt;
                    partFound[p] = true;
                }
            }

            if (IsFrameViewValid())
            {
                const CView& view = GetFrameView();
                const float cx = Gui.Window.Size.x * 0.5f;
                const float cy = Gui.Window.Size.y * 0.5f;

                float bestD2 = -1.0f, bestAnyD2 = -1.0f;
                int bestP = -1, bestAnyP = -1;
                float d2arr[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
                static int s_lastAutoIdx  = -1;
                static int s_lastAutoPart = -1;
                static unsigned char s_pmiss[4] = { 0, 0, 0, 0 };  // [批141] 连续不可见帧
                bool visPart[4] = { false, false, false, false };    // [批141] 本帧可见性
                for (int p = 0; p < 4; ++p)
                {
                    if (!partFound[p])
                        continue;
                    Vec2 sp{};
                    if (!view.WorldToScreen(Vec3(partPos[p].x, partPos[p].y,
                                                 partPos[p].z), sp))
                        continue;                     // 相机后
                    const float dx = sp.x - cx;
                    const float dy = sp.y - cy;
                    const float d2 = dx * dx + dy * dy;
                    d2arr[p] = d2;
                    // [批124] 先记录"屏幕最近部位"（不看可见性）：
                    //   全不可见时把它交给上层，让 GetBestTarget 的
                    //   3 帧宽限决定是否软锁定。否则这里返回零向量，
                    //   候选在外层可见性判断之前就被跳过，宽限永远
                    //   不会生效（日志实测 软锁=0 的根因）。
                    if (bestAnyD2 < 0.0f || d2 < bestAnyD2)
                    {
                        bestAnyD2 = d2;
                        bestAnyP  = p;
                    }
                    // ③ 可见性过滤（优先选真正可见的部位）
                    if (!Aimbot::TraceVisible(eye, partPos[p],
                                              (void*)localPlayer,
                                              (void*)ent))
                        continue;
                    visPart[p] = true;                       // [批141] 记录可见性
                    if (bestD2 < 0.0f || d2 < bestD2)
                    {
                        bestD2 = d2;
                        bestP  = p;
                    }
                }

                // [批143] 部位选择整体重写（见下方"固定优先级"段）：
                //   固定头 > 胸 > 腹 > 腿，不再用"离准星最近"去猜。

                // ============================================================
                //  [批127] 部位粘滞（Part Hysteresis）
                // ------------------------------------------------------------
                //  中距离（300~800u）头/胸的屏幕像素距非常接近，逐帧取
                //  "最近"会在头和胸之间来回切 → 准星一顿一顿。
                //  规则：同一实体的上一部位只要不比最优部位差 20px（400px²），
                //  就继续锁它；换实体时清零。
                // ============================================================
                //  [批141] 稳定优先（补上"可见性翻转"这个洞）
                //  ------------------------------------------------------------
                //  20px 迟滞救不了这种情况：头被小僵尸/掩体挡了 1~2 帧 →
                //  这一帧没有"可见的头"→ 落到"最近可见部位"（胸）→ 下一帧
                //  头又可见 → 再切回头 → 头/胸互切（用户实测"时不时两个部位
                //  切换"）。现在：记住的部位只要"刚刚还可见"（连续不可见
                //  ≤3 帧），就继续用它，不再因为可见性抖动改瞄别的。
                // ============================================================
                if (s_lastAutoIdx == e.Index() &&
                    s_lastAutoPart >= 0 && s_lastAutoPart < 4 &&
                    partFound[s_lastAutoPart])
                {
                    if (visPart[s_lastAutoPart])
                        s_pmiss[s_lastAutoPart] = 0;
                    else if (s_pmiss[s_lastAutoPart] < 255)
                        ++s_pmiss[s_lastAutoPart];
                }
                else
                {
                    s_pmiss[0] = s_pmiss[1] = s_pmiss[2] = s_pmiss[3] = 0;
                }

                // ============================================================
                //  [批143] 固定优先级：头(0) > 胸(1) > 腹(2) > 腿(3)
                //  ------------------------------------------------------------
                //  彻底放弃"离准星最近的部位"：它既会头/胸互切，又让瞄准点
                //  在多个部位之间甩（用户实测：直接选"头"反而最准）。
                //  规则：按优先级取第一个【可见】部位；都不可见时沿用上一
                //  部位（连续不可见 ≤3 帧），再不行才用"屏幕最近几何点"。
                // ============================================================
                // [批146] 三态分开标：Visible / ReuseLastPart / Geometric
                int prio = -1;
                for (int p = 0; p < 4; ++p)
                {
                    if (visPart[p]) { prio = p; break; }
                }
                AimPointKind kind = AimPointKind::Geometric;
                if (prio >= 0)
                {
                    bestP = prio;
                    kind = AimPointKind::Visible;
                }
                else if (s_lastAutoIdx == e.Index() &&
                         s_lastAutoPart >= 0 && s_lastAutoPart < 4 &&
                         partFound[s_lastAutoPart] &&
                         s_pmiss[s_lastAutoPart] <= 3)
                {
                    bestP = s_lastAutoPart;
                    kind = AimPointKind::ReuseLastPart;
                }
                else
                {
                    bestP = bestAnyP;
                }
                if (bestP >= 0)
                {
                    out.pos       = partPos[bestP];
                    out.kind      = kind;
                    out.partIndex = bestP;
                    s_lastAutoIdx  = e.Index();
                    s_lastAutoPart = bestP;
                }
                else
                {
                    s_lastAutoIdx  = -1;
                    s_lastAutoPart = -1;
                    fail = "自动模式：无任何可用部位（骨骼区域解析失败）";
                }
            }
            else
                fail = "相机矩阵不可用（本帧未取到视图）";
        }

        if (fail && GetTickCount() - s_autoFailLog > 3000)
        {
            s_autoFailLog = GetTickCount();
            PayloadLog("[AIM-ERR] GetAutoAimPosition：idx=%d 原因=%s",
                       e.Index(), fail);
        }

        // [批146] 旧 Origin+64 兜底早已删除：全部链失败 → kind 保持 Fail，
        //  上层（GetBestTarget / SolveTargetAngle）见到 Fail 即跳过 ——
        //  不准开枪、不准平滑、不准 Origin 降级。
        //  Geometric / ReuseLastPart 不是失败：交给外层宽限期判定。
        // [批149] 但仍可走【兜底链】：碰撞盒顶 → 眼高（同样标 Fallback*）。
        if (out.kind == AimPointKind::Fail)
        {
            sdk::Vector fb{};
            AimPointKind kind = AimPointKind::Fail;
            if (TryFallbackAimPoint(e, o, fb, kind, sidAbs))
            {
                out.pos       = fb;
                out.kind      = kind;
                out.partIndex = -1;
                static DWORD s_fbLog = 0;
                const DWORD nowF = GetTickCount();
                if (nowF - s_fbLog > 3000)
                {
                    s_fbLog = nowF;
                    PayloadLog("[AIM] 兜底点：idx=%d 层=%s（自动模式）",
                               e.Index(),
                               kind == AimPointKind::FallbackBoxTop
                                   ? "碰撞盒中点" : "眼高");
                }
            }
        }
        return out;
    }

    // [批111·死代码] 删除了 `LastAimFailReason()` —— 全项目零调用。
    //  它原本是给菜单显示"上次选靶失败原因"用的，但界面从未接上；
    //  实际排障靠的是日志里的 [AIM-ERR] / [aim] 选靶统计两条输出。

    // ============================================================
    //  [批2→批3] LegitApply —— 平滑视觉应用体（原批2 逻辑整编）
    //  cmd->viewangles 向 target 指数缓出，并 SetViewAngles 同步本地。
    // ============================================================
    void LegitApply(sdk::CUserCmd* pCmd, const sdk::QAngle& target, float dt,
                    float distUnits)
    {
        // [批5 修正] 平滑语义翻转（用户实测：旧公式越大越快，与说明相反）。
        // 新语义：S 越大越慢 —— 66tick 基准下每 tick 走剩余距离的 (1-S)：
        //   S=0.30 → 70%/tick（快）；S=0.75 → 25%/tick（≈ Osiris smooth 4）；
        //   S=0.95 → 5%/tick（很慢）。frac = 1 - S^(66*dt)，帧率无关。
        float S = fminf(fmaxf(g_Config.humanize.smoothFactor, 0.05f), 0.95f);

        // ============================================================
        //  [批123/批134/批144] 距离自适应平滑（废除固定 smoothFactor）
        // ------------------------------------------------------------
        //  单位是 Source unit：300u ≈ 7.6m，800u ≈ 20m。
        // [批144/批145] 距离自适应平滑（收窄后再次微调）
        //  近战(<300u) k=0.85 → S=0.82×0.85=0.70 → 每 tick 约 30%（比批144
        //    的 43% 更柔，恢复"平滑自瞄"的手感；用户反馈"近战还能再慢点"）。
        //  远距(>800u) k=1.10 → 每 tick 约 10%。
        //  【铁律】只影响平滑层，不进入 GetBestTarget 的选靶评分。
        if (distUnits > 0.0f)
        {
            float k;                                   // S 的乘数（越小越快）
            if (distUnits <= 300.0f)
                k = 0.85f;
            else if (distUnits >= 800.0f)
                k = 1.10f;
            else
                k = 0.85f + (distUnits - 300.0f) / 500.0f * (1.10f - 0.85f);
            S = fminf(fmaxf(S * k, 0.20f), 0.95f);
        }
        const float frac = 1.0f - powf(S, 66.0f * dt);

        // 差值：yaw 走最短转向路径，pitch 直接差
        const float entryYaw = pCmd->viewangles.yaw;      // [批10] 写入前视角（movement fix 基准）
        const float dPitch = target.pitch - pCmd->viewangles.pitch;
        const float dYaw   = AngleDiff(target.yaw, pCmd->viewangles.yaw);

        // ---- [批6·修正3] 死区 + 退出滞后（防临界点反复开关 = 卡顿）----
        //  进入（双轴 < deadzone）→ 停瞄；重新介入需任一轴超过
        //  deadzone × 2（乘法滞回，用户方案）——临界点不再反复横跳。
        const float deadzone = g_Config.humanize.deadzone;
        if (deadzone > 0.0f)
        {
            static bool s_inDead = false;
            const float dP = fabsf(dPitch);
            const float dY = fabsf(dYaw);
            if (s_inDead)
            {
                if (dP > deadzone * 2.0f || dY > deadzone * 2.0f)
                    s_inDead = false;                 // 超出退出阈：重新介入
                else
                    return;                           // 死区内：保持停瞄
            }
            else if (dP < deadzone && dY < deadzone)
            {
                s_inDead = true;                      // 进入死区：停瞄
                return;
            }
        }

        // 写入 cmd（服务器吃到的角度）
        //
        // [批61] 单 tick 步长硬上限 —— 与静默曲线共用同一个反检测边界。
        //   为什么需要：frac 随 dt 变化，dt 上限 0.1s 时 66*dt = 6.6，
        //   frac = 1-0.82^6.6 ≈ 0.72；此时若偏差 8°（换目标 / 掉帧尖峰），
        //   单 tick 位移就是 5.8° —— 已越过 Lilac SNAP2 的 5.0 门槛。
        //   平滑系数本身保证不了上限（它只削比例，不封顶），必须显式夹。
        constexpr float kMaxStepPerTick = 4.5f;

        float stepP = dPitch * frac;
        float stepY = dYaw   * frac;
        const float stepMag = sqrtf(stepP * stepP + stepY * stepY);
        if (stepMag > kMaxStepPerTick && stepMag > 1e-6f)
        {
            const float k = kMaxStepPerTick / stepMag;
            stepP *= k;
            stepY *= k;
        }

        sdk::QAngle next;
        next.pitch = fminf(fmaxf(pCmd->viewangles.pitch + stepP, -89.0f), 89.0f);
        next.yaw   = sdk::NormalizeYaw(pCmd->viewangles.yaw + stepY);
        next.roll  = 0.0f;
        if (!isfinite(next.pitch) || !isfinite(next.yaw))
            return;                                          // 数学安全防线

    pCmd->viewangles.pitch = next.pitch;
    pCmd->viewangles.yaw   = next.yaw;
    pCmd->viewangles.roll  = 0.0f;
    g_wroteCmdAngles = true;               // [批36] Hook 据此返回 false

    // ============================================================
    //  [批10] Movement fix（参考 RazeHack §12）：视角改变后把原移动
    //  向量按视角变化量重新旋转——否则瞄准时按住 W 会往侧面滑。
    //  先写视角、后修移动（用户指定的写入顺序）。
    // ============================================================
    {
        const float fm = pCmd->forwardmove;
        const float sm = pCmd->sidemove;
        if (fm != 0.0f || sm != 0.0f)
        {
            const float mag     = sqrtf(fm * fm + sm * sm);
            const float baseAng = atan2f(sm, fm);
            const float dYawRad = (next.yaw - entryYaw) * (sdk::kPi / 180.0f);
            const float na      = baseAng + dYawRad;
            pCmd->forwardmove = cosf(na) * mag;
            pCmd->sidemove    = sinf(na) * mag;
        }
    }

    // 写入本地视角（Legit：屏幕准星平滑滑动）
    // 内含首次读回验证，槽位错自动停用（失败安全）
    ViewEng().Write(ViewAngles{ next.pitch, next.yaw, 0.0f });
}
} // namespace

// ============================================================
//  TraceRay 单射线可见性 [批112 语义重写]
//  · 过滤器只忽略本地玩家（ignoreA）；目标实体【参与】射线。
//  · 判据 = "子弹可达"：射线未命中任何东西（畅通），
//    或命中的就是目标本体（无论头/手/脚/任意 hitgroup）→ 可见。
//    旧语义把目标本体也忽略、要求 !DidHit()：回答的是"路径是否
//    畅通"而非"能否打到目标"，其它僵尸挥臂/擦边会误杀可见性。
//  · ignoreB 语义变更：不再是"被忽略实体"，而是"目标实体"
//    （用于 m_pEnt 比较）。传 nullptr = 纯畅通检测。
// ============================================================
bool Aimbot::TraceVisible(const sdk::Vector& from, const sdk::Vector& to,
                          void* ignoreEnt)
{
    return TraceVisible(from, to, ignoreEnt, nullptr);
}

bool Aimbot::TraceVisible(const sdk::Vector& from, const sdk::Vector& to,
                          void* ignoreA, void* targetEnt)
{
    void* trace = EnsureEngineTrace();
    if (!trace)
        return false;                        // 接口不可用：保守判不可见

    Ray_t        ray(from, to);
    CGameTrace   tr{};
    // [批146] 可见性射线穿透小僵尸（它们挤在特感前面会把特感挡没）。
    //   宽限期的墙检测（TraceBlockedByWorld）仍用不穿透的过滤器。
    CTraceFilterAimbot filter(ignoreA, nullptr);

    reinterpret_cast<IEngineTrace*>(trace)->TraceRay(ray, MASK_SHOT, &filter, &tr);
    if (!tr.DidHit())
        return true;                         // 路径畅通：可见
    // 打到的就是目标本体（任意部位）→ 可见；打到别的 → 被挡
    return targetEnt != nullptr && tr.m_pEnt == targetEnt;
}

    // ============================================================
    //  [批134] TraceBlockedByWorld —— 宽限期的"防穿墙"判定
    // ------------------------------------------------------------
    //  宽限期如果无条件保留，目标躲到墙后 180ms 里我们还在对着墙拉枪。
    //  判据：向"上一次有效瞄准点"打一条射线：
    //    · 命中且 m_pEnt == nullptr → 命中的是【世界几何】= 墙 → 放弃宽限
    //    · 命中的是别的实体（小僵尸/栏杆）→ 仍保留（这正是宽限要救的场景）
    //    · 完全没命中 → 通路，当然保留
    //  注意：这里只看"是不是墙"，不改变可见性本身的判定。
    // ============================================================
    bool Aimbot::TraceBlockedByWorld(const sdk::Vector& from, const sdk::Vector& to,
                                     void* ignoreA)
    {
        void* trace = EnsureEngineTrace();
        if (!trace)
            return true;                        // 接口不可用：保守放弃宽限
        Ray_t        ray(from, to);
        CGameTrace   tr{};
        CTraceFilterIgnore filter(ignoreA, nullptr);
        reinterpret_cast<IEngineTrace*>(trace)->TraceRay(ray, MASK_SHOT, &filter, &tr);
        if (!tr.DidHit())
            return false;                       // 没被挡
        return tr.m_pEnt == nullptr;            // 命中世界几何 = 墙
    }

    // ============================================================
    //  [批4·批112] 多点可见性：头 / 胸 / 左右肩 四射线
// ------------------------------------------------------------
//  四点中任意一条"可达目标"（畅通或命中的就是目标本体）即判可见；
//  全部被别的实体挡住才弃权。
//  肩点 = 胸口 ± 视线水平垂直方向 14 单位（无需实体角度）。
//  返回可见点的引用写回 aimPos（头部优先：射线顺序即优先级）。
// ============================================================
int Aimbot::MultiPointVisible(const sdk::Vector& eyePos,
                              const sdk::Vector& origin,
                              sdk::Vector& aimPos,
                              void* ignoreA, void* ignoreB,
                              int maxHits)
{
    // 头点 = 当前瞄准点（refine 后的头盒中心 / origin+64）
    const sdk::Vector chest(origin.x, origin.y, origin.z + 40.0f);

    // 水平"右向"（垂直于 眼→胸 方向），用于左右肩偏移
    const float ddx = chest.x - eyePos.x;
    const float ddy = chest.y - eyePos.y;
    const float dl  = sqrtf(ddx * ddx + ddy * ddy);
    const float rx = (dl > 1e-4f) ? (-ddy / dl) : 1.0f;
    const float ry = (dl > 1e-4f) ? ( ddx / dl) : 0.0f;
    const sdk::Vector shoulderL(chest.x + rx * 14.0f, chest.y + ry * 14.0f, chest.z);
    const sdk::Vector shoulderR(chest.x - rx * 14.0f, chest.y - ry * 14.0f, chest.z);

    struct RayPt { const char* name; sdk::Vector to; };
    const RayPt pts[4] = {
        { "head",     aimPos    },
        { "chest",    chest     },
        { "shoulderL", shoulderL },
        { "shoulderR", shoulderR },
    };
    // [批145] 追加下半身采样点：只露一条腿/半个身子时，4 个上半身点
    //   全被挡 → 判不可见 → "僵尸明明在掩体外却不瞄准"。这三点只在
    //   前 4 点全失败时才打（早退），成本可控。
    const sdk::Vector hip (origin.x, origin.y, origin.z + 22.0f);
    const sdk::Vector kneeL(origin.x + rx * 5.0f, origin.y + ry * 5.0f, origin.z + 8.0f);
    const sdk::Vector kneeR(origin.x - rx * 5.0f, origin.y - ry * 5.0f, origin.z + 8.0f);
    const RayPt pts2[3] = {
        { "hip",   hip   },
        { "kneeL", kneeL },
        { "kneeR", kneeR },
    };

    // [批146] 命中计数（原来只有 bool）：返回打通的采样点数量，外层按
    //   "露出的多少"给评分降权（露得少 → 排后）。命中 ≥4 就收工：
    //   大半身体可见时没必要再打剩下三条射线。
    if (maxHits < 1) maxHits = 1;
    int hits = 0;
    for (const RayPt& pt : pts)
    {
        if (!isfinite(pt.to.x) || !isfinite(pt.to.y) || !isfinite(pt.to.z))
            continue;
        if (TraceVisible(eyePos, pt.to, ignoreA, ignoreB))
            if (++hits >= maxHits) return hits;
    }
    for (const RayPt& pt : pts2)
    {
        if (!isfinite(pt.to.x) || !isfinite(pt.to.y) || !isfinite(pt.to.z))
            continue;
        if (TraceVisible(eyePos, pt.to, ignoreA, ignoreB))
            if (++hits >= maxHits) return hits;
    }
    return hits;                             // 0 = 全被挡
}

// ============================================================
//  当前视角前向（视角引擎槽位自探测就绪后可用）
// ============================================================
bool Aimbot::GetViewForward(sdk::Vector& fwd)
{
    ViewAngles v;
    if (!ViewEng().Read(v))
        return false;

    // Source 约定：pitch 正=低头 → 前向 z 分量取 -sin(pitch)
    const float p = v.pitch * (sdk::kPi / 180.0f);
    const float y = v.yaw   * (sdk::kPi / 180.0f);
    const float cp = cosf(p), sp = sinf(p);
    fwd = sdk::Vector(cp * cosf(y), cp * sinf(y), -sp);
    return isfinite(fwd.x) && isfinite(fwd.y) && isfinite(fwd.z);
}

// ============================================================
//  选靶：FOV 粗筛 → 评分排序 → Top-K 多点可见性 → 迟滞防抖
// ------------------------------------------------------------
//  [批4 排查修正]
//    · TeamNum==0（新生/未同步）的实体跳过，避免误判阵营
//    · 评分 = 角度距 + distWeight×(3D 距离/30m)，距离不再被小角度碾压
//    · 迟滞：上一帧目标享 0.75° 等效优势，两目标分数接近时不跳变
//    · 候选上限 16，可见性射线只花在评分前 4 名（性能上限）
//    · 可见性 = 头/胸/左右肩四射线，任一畅通即可见（薄壁/栅栏盲区）
//    · 头盒精修：hitbox 0（头）优先于"Z 最高"猜测，且距 origin<100u 才采信
// ============================================================
bool Aimbot::GetBestTarget(const sdk::Vector& eyePos, float fovDeg, Target& out)
{
    ++g_aimTickSeq;                       // [批164] 每次选靶 = 一个 tick
    out = Target{};

    // 眼位有效性防线（本地抖动/异常状态导致 NaN 的输入直接拒绝）
    if (!isfinite(eyePos.x) || !isfinite(eyePos.y) || !isfinite(eyePos.z))
        return false;

    sdk::Vector viewFwd;
    if (!GetViewForward(viewFwd))
        return false;

    const float fovLimit  = fmaxf(fovDeg, 1.0f);
    const int   localTeam = sdk::EntityList::LocalPlayer().GetTeam();
    const float distW     = fmaxf(g_Config.aim.distWeight, 0.0f);

    // [批14] 本地队伍读不到（localTeam==0）= 无法判定友敌 → 直接不选靶。
    //  这是"幽灵实体/队友误锁"的总闸：LocalPlayer 失效时队伍过滤失效，
    //  队友甚至失效槽位都会通过白名单，后续 SetupBones 直调 = 闪退源头。
    if (localTeam == 0)
        return false;

    // [诊断·L5] 过滤链各段计数（1s 一次摘要，失败定位）
    int cand = 0;
    int diagSid = -1; float diagDist = 0.0f;
    char diagName[32] = {};
    int boneSkipCnt = 0, gateFail = 0, fovSkip = 0, softLockCnt = 0, distSkip = 0;
    const char* diagFail = nullptr;

    // ---- 候选池（白名单 + 基础过滤；瞄准点由精修期 GetAimPosition
    //      唯一决定——此处【不生成】任何瞄准点：无 +64、无比例、无兜底）----
    struct Cand
    {
        int index = -1;
        sdk::Vector origin{};
        float velSpeed = 0.0f;  // [批5·L4] 漂移门控用（u/s）
        int health = 0;
        int sid = -1;
    };
    constexpr int kMaxCand = 16;
    Cand cands[kMaxCand];
    int nCand = 0;

    // ============================================================
    //  [批16·闪退根因] 快照跨线程安全
    // ------------------------------------------------------------
    //  g_entities 由渲染线程每帧 clear+swap 重建（g_entitiesMutex 保护），
    //  游戏线程持引用直接迭代 = 与重建竞态（UB → 堆损坏 → bad_alloc/
    //  闪退）——批4 起 aim 每 tick 读快照，批11 起 SetupBones+命中盒遍历
    //  把单 tick 处理时间拉长，竞态窗口大到必然命中。
    //  修复：持锁【复制】快照后迭代副本（锁只包复制瞬间，微秒级）。
    //
    //  [批111·性能] 拷贝改用 thread_local 复用缓冲，消除每 tick 堆分配。
    //  GetFrameSnapshot() 返回的是【引用】，所以 `snapCopy = ...` 是一次
    //  完整的拷贝构造：CreateMove 每 tick（66~100 次/秒）都要
    //  malloc + 拷贝全部实体（EntityInfo+EntityState 各数十字节，
    //  场上通常 30~60 个）→ 每秒几千次堆分配，且随实体数线性增长。
    //  thread_local 缓冲只在首次调用分配，之后靠 clear() 复用容量。
    //  为什么安全：CreateMove 只在游戏线程调用，无跨线程复用；
    //  递归重入也不会发生（GetBestTarget 内不自调用）。
    // ============================================================
    static thread_local std::vector<std::pair<EntityInfo, EntityState>> snapCopy;
    {
        std::lock_guard<std::mutex> lk(g_entitiesMutex);
        // operator= 语义等价于"新建 vector 拷贝"：元素被整体替换，
        // 不存在上一帧残值。复用 capacity 即可，无需额外 clear()。
        snapCopy = GetFrameSnapshot();
    }

    for (const auto& [info, state] : snapCopy)
    {
        // ============================================================
        //  [批134] 最小锁定时间（300ms）
        //  一旦选中目标，300ms 内【只认这个目标】：旁边有更近的僵尸也不
        //  换靶（除非目标死亡/休眠/出 FOV —— 那种情况下本轮没有候选，
        //  下面的"无目标"分支会解除最小锁定）。
        // ============================================================
        if (GetTickCount() < s_minLockUntil && info.index != s_prevTargetForHysteresis)
            continue;

        // ---- 分类白名单（与 ESP 收录同源）----
        const int sidAbs = (info.stormId < 0) ? -info.stormId : info.stormId;
        if (info.stormId == -1)
            continue;                                 // 未分类：宁缺勿错
        if (sidAbs == sdk::sid::kInfected || sidAbs == sdk::sid::kRock)
            continue;                                 // 小僵尸/石头不锁
        switch (sidAbs)
        {
        case sdk::sid::kTank: case sdk::sid::kWitch:
        case sdk::sid::kSmoker: case sdk::sid::kBoomer: case sdk::sid::kHunter:
        case sdk::sid::kSpitter: case sdk::sid::kJockey: case sdk::sid::kCharger:
        case sdk::sid::kSurvivor:                           // 对抗局敌对幸存者
            break;
        default:
            continue;                                 // 道具等：不锁
        }

        // ---- 基础过滤（快照字段，零 netvar 读）----
        if (state.lifeState != 0)                     // 只有 LIFE_ALIVE(0) 通过
            continue;                                 //   （1=濒死动画 2=尸体 全拦截）
        if (state.isGhost)                            // 幽灵态特感（未出生，不可见）
            continue;
        if (info.team == 0)
            continue;                                 // [批4] TeamNum 未同步：跳过
        if (!g_Config.aim.friendlyFire &&
            localTeam != 0 && info.team == localTeam)
            continue;                                 // 队友（可配置）
        if (info.position.x == 0.0f && info.position.y == 0.0f &&
            info.position.z == 0.0f)
            continue;                                 // 未同步（地图原点）
        ++cand;

        // ---- [批9] 此处不生成瞄准点（无 +64/比例/兜底）——
        //      只入池，瞄准点在精修期由 GetAimPosition 唯一决定 ----
        if (nCand >= kMaxCand)
            continue;                                 // 池满：特感数量自然少

        Cand& c = cands[nCand++];
        c.index    = info.index;
        c.origin   = sdk::Vector(info.position.x, info.position.y,
                                 info.position.z);
        c.velSpeed = sqrtf(info.velocity.x * info.velocity.x +
                           info.velocity.y * info.velocity.y +
                           info.velocity.z * info.velocity.z);
        c.health  = info.health;
        c.sid     = info.stormId;
    }

    if (nCand == 0)
        return false;

    // ============================================================
    //  [批9→批112] 精修单遍：GetAimPosition（唯一瞄准点决策点）→
    //  FOV（真实头点）→ 评分（角度+距离+迟滞）→ 碰撞盒校验 →
    //  漂移门控 → 多点可见性 → 评分最优者胜出。
    //  任何一环失败 → 弃权该候选（【无兜底】：解析不出骨骼点
    //  就跳过，绝不用 Origin/碰撞盒猜一个坐标硬瞄）。
    // ============================================================
    float bestScore = 0.0f;
    bool  winnerGrace = false;                       // [批145] 胜出的是"软锁"目标？
    for (int k = 0; k < nCand; ++k)
    {
        Cand& c = cands[k];
        bool cWasGrace = false;                      // [批145] 本候选是否只靠软锁存活

        const sdk::CBaseEntity e = sdk::EntityList::Get(c.index);
        if (!e.IsValid())
            continue;
        // [批5] 休眠 = 离开 PVS/数据冻结：位置是旧数据，不参与瞄准点计算
        // （休眠 ≠ 不可见——实体回到 PVS 后数据会刷新，届时自然解锁）
        if (e.IsDormant())
            continue;
        if (e.Address() == localPlayer)
            continue;                                 // 自己（双保险）

        // ---- [批12] FOV 锥外粗筛（origin 基准，宽容 +25°）----
        //  把 SetupBones 直调限制在"值得瞄"的可见锥内实体上——
        //  视野外实体的动画状态不保证有效，对全部候选盲调引擎函数
        //  是卡死/闪退的诱因（RazeHack 检测特征 #6 同款教训）
        {
            const float angCoarse = AngleBetweenDeg(viewFwd,
                                              DirectionTo(eyePos, c.origin));
            if (angCoarse > fovLimit + 25.0f)
                continue;
        }

        // ---- [批9] 头盒优先：GetAimPosition（唯一精准来源）----
        // [批13] aimPart==4 = 自动模式（屏幕最近可见部位）
        // [批112] 解析失败（零向量 / usedFallback）→ 直接跳过该候选。
        //  旧的碰撞盒比例兜底与 Origin+64 兜底已全部删除：
        //  拿不到真实骨骼点绝不硬瞄（"锁空气/瞄地板"的源头）。
        // [批146] 三态：只有 Fail 才跳过；Geometric（几何兜底）与
        //   ReuseLastPart（沿用上一部位）都要进评分池，交给宽限期判定。
        const AimPoint ap = (g_Config.aim.aimPart == 4)
            ? GetAutoAimPosition(e, c.sid)
            : GetAimPosition(e, g_Config.aim.aimPart, c.sid);
        const sdk::Vector headP = ap.pos;
        if (!ap.ok())
        {
            ++boneSkipCnt;
            static DWORD s_boneSkipLog = 0;
            const DWORD nowBs = GetTickCount();
            if (nowBs - s_boneSkipLog > 3000)
            {
                s_boneSkipLog = nowBs;
                PayloadLog("[aim] 候选跳过：部位解析失败 idx=%d 部位=%d"
                           " —— 无兜底，不瞄",
                           c.index, g_Config.aim.aimPart);
            }
            continue;
        }

        // ---- [批112] 碰撞盒只做【校验器】，不做兜底坐标 ----
        //  m_vecMins/m_vecMaxs 随姿态网络同步（蹲/爬实时反映）。
        //  读取结果供下方 Z 范围/水平半径/漂移门控使用；
        //  读取失败只影响门控宽度（回退固定假设），不影响瞄准点。
        float colMinsZ = 0.0f, colMaxsZ = 0.0f, colMaxH = 0.0f;
        bool  colOk = false;
        {
            const auto& off = NetVars::Get();
            if (off.collisionMins && off.collisionMaxs)
            {
                sdk::Vector mn{}, mx{};
                if (ReadVec3At(e.Address() + off.collisionMins, mn) &&
                    ReadVec3At(e.Address() + off.collisionMaxs, mx) &&
                    mx.z > mn.z)
                {
                    colMinsZ = mn.z; colMaxsZ = mx.z;
                    colMaxH  = fmaxf(fabsf(mn.x),
                                fmaxf(fabsf(mx.x),
                                fmaxf(fabsf(mn.y), fabsf(mx.y))));
                    colOk = true;
                }
            }
        }

        // ---- FOV 圆锥（真实头点，纯数学）----
        const float ang = AngleBetweenDeg(viewFwd,
                                          DirectionTo(eyePos, headP));
        // [批147] 上一目标放宽锥角：平滑有滞后，高速移动的特感（冲锋的
        //   Charger、扑咬的 Hunter）会短暂偏出 10° 锥 → 被拒 → 丢靶。
        //   给它 3 倍余量：锁着的时候不因为"准星还没追上"而丢掉目标。
        const float fovGate = (c.index == s_prevTargetForHysteresis)
                            ? fovLimit * 3.0f : fovLimit;
        if (ang > fovGate)
        {
            ++fovSkip;
            continue;
        }

        // ---- 评分：角度距 + 3D 距离加权 + 迟滞 ----
        const float dx = c.origin.x - eyePos.x;
        const float dy = c.origin.y - eyePos.y;
        const float dz = c.origin.z - eyePos.z;
        const float distM = sqrtf(dx * dx + dy * dy + dz * dz) * 0.0254f;
        // [批124] 菜单里的"最大距离(米)"此前【从未被读取】—— 选 150m 等于
        //   没设。现在按米硬门控；单位换算：1 Source unit = 0.0254m。
        //   同时用"眼位→瞄准点"的距离打日志（旧日志用脚底 origin，
        //   贴脸时 dz≈64u 会把 3m 记成 3.4m，看起来"距离不符"）。
        const float adx = headP.x - eyePos.x;
        const float ady = headP.y - eyePos.y;
        const float adz = headP.z - eyePos.z;
        const float aimDistM = sqrtf(adx * adx + ady * ady + adz * adz) * 0.0254f;
        const float maxDistM = (g_Config.aim.maxDistance > 0.0f)
                             ? g_Config.aim.maxDistance : 0.0f;
        if (maxDistM > 0.0f && distM > maxDistM)
        {
            ++distSkip;
            continue;
        }
        float score = ang + distW * (distM / 30.0f);
        // [批146] 三态降权：几何兜底点 / 沿用上一部位的点在评分上排后，
        //   可见的真目标永远优先（评分越小越优先 → 这里是【加】分）。
        if (ap.kind == AimPointKind::Geometric)
            score += 1.5f;
        else if (ap.kind == AimPointKind::ReuseLastPart)
            score += 0.5f;
        else if (ap.kind == AimPointKind::FallbackBoxTop)
            score += 2.5f;           // [批149] 猜测点：重罚，真目标优先
        else if (ap.kind == AimPointKind::FallbackViewOffset)
            score += 4.0f;           // [批149] 最后一层：罚得最重
        // 肢体（腿）当兜底点时额外重罚：黏在腿上会"锁着但打不中"
        if (ap.partIndex == 3 && ap.kind != AimPointKind::Visible)
            score += 2.0f;
        // [批145] 迟滞改为【按可见性分级】：
        //   上一目标【真的可见】→ 极强粘性（-15°），别乱换靶；
        //   上一目标只是【软锁撑着】（被小僵尸挡住）→ 弱粘性（-3°），
        //   让"掩体外清清楚楚的新目标"能立刻抢过来（用户反馈：
        //   僵尸明明在掩体外却不瞄准）。
        //   原来在评分阶段无条件 -15，等于让"被挡住的旧目标"霸占锁定。

        // ============================================================
        //  [批5·L4] 最终门控（可见性前）：范围 / 半径 / 漂移三重校验
        //  校验对象 = GetAimPosition 的头点；碰撞盒只做校验器。
        //  任一失败 → 弃权（绝不 fallback 到固定点继续锁）
        // ============================================================
        {
            const float zMin = c.origin.z + (colOk ? colMinsZ : 0.0f) - 2.0f;
            const float zMax = c.origin.z + (colOk ? colMaxsZ : 96.0f) + 2.0f;
            const float hMax = (colOk ? colMaxH : 48.0f) + 6.0f;
            const float hx = headP.x - c.origin.x;
            const float hy = headP.y - c.origin.y;

            bool ok = headP.z >= zMin && headP.z <= zMax;            // Z 范围
            ok = ok && (hx * hx + hy * hy) <= hMax * hMax;           // 水平半径

            // 漂移门控：与上帧瞄准点的差 ≤ 目标速度×0.05s + 16u（防骨漂）
            //  [批147] 死代码修复：这里原来用的是【另一组从未被赋值的
            //   局部 static】（s_driftIdx/s_driftPos），真正被写的却是外层
            //   s_driftIdxLast/s_driftPosLast → 条件永远不成立 → 漂移门控
            //   从来没生效过。现在统一用外层那组，门控才真正在工作。
            if (ok && c.index == s_driftIdxLast)
            {
                const float mx = headP.x - s_driftPosLast.x;
                const float my = headP.y - s_driftPosLast.y;
                const float mz = headP.z - s_driftPosLast.z;
                // [批147] 判据改成【相对身体的位移】：
                //   旧公式 limit = 速度×0.05 + 16（绝对值）—— 高速移动的
                //   特感每帧位移本来就大（冲锋 Charger 头骨能甩 20~30u/tick），
                //   加上骨骼缓存最多 80ms 的滞后 → 直接被判"漂移" → 丢靶。
                //   现在允许"瞄准点比身体多走 48u"（动画摆臂/俯仰的余量），
                //   身体走多少就允许多少。
                const float ox = c.origin.x - s_driftOriginLast.x;
                const float oy = c.origin.y - s_driftOriginLast.y;
                const float oz = c.origin.z - s_driftOriginLast.z;
                const float bodyMove = sqrtf(ox * ox + oy * oy + oz * oz);
                // 余量 72u：动画摆臂(≈20u) + 骨骼缓存最多 80ms 的滞后
                //   （400u/s 的冲锋 ≈ 32u）都留得下；而"读到别的实体的骨骼"
                //   这种脏数据动辄上百单位，仍会被拦下。
                const float limit = bodyMove + 72.0f;
                if (mx * mx + my * my + mz * mz > limit * limit)
                    ok = false;
            }
            if (!ok) { ++gateFail; continue; }
        }

        // ---- 多点可见性：头/胸/左右肩，任一畅通即可见 ----
        //  [批9] 可见性只决定【是否交战】，绝不改写瞄准点
        //  [批123] 丢失宽限（Grace Period）：
        //    · 可见 → 记录 (时刻, 瞄准点)
        //    · 不可见 → 若就是上一帧目标、180ms 内可见过、
        //      且瞄准点没有瞬移（<96 单位）→ 仍保留（软锁定），
        //      分数 +1.0° 惩罚，让真正可见的目标优先
        //    效果：小僵尸擦边挡一下不再丢目标，reactionMs 不重置，
        //    消除"锁定→丢失→再锁定"的一顿一顿。
        {
            const int vi = c.index & (kVisTrack - 1);
            sdk::Vector visPt = headP;
            // [批146] 返回命中点数（0 = 全被挡），用于按"露出多少"降权
            // [批165] 两档射线：只有"可能胜出"的候选才补满命中计数
            //   （≤ 最优+0.9°，或就是上一目标）——差 >0.9° 的候选加满
            //   最大惩罚也翻不了盘，1 条射线判可见即可。判据等价、省射线。
            const bool mightWin = (bestScore >= 1e9f) ||
                                  (score <= bestScore + 0.9f) ||
                                  (c.index == s_prevTargetForHysteresis);
            const int visHits = Aimbot::MultiPointVisible(
                eyePos, c.origin, visPt,
                (void*)localPlayer, (void*)e.Address(),
                mightWin ? 4 : 1);
            const bool vis = (visHits > 0);
            if (vis)
            {
                s_lastVisMs[vi]  = GetTickCount();
                s_lastVisPos[vi] = headP;
                s_visMiss[vi]    = 0;
                if (c.index == s_prevTargetForHysteresis)
                    score -= 15.0f;                   // [批145] 可见的上一目标：极强粘性
                // [批146] 露得越少，排得越后（命中 4 点以上视为"基本全露"）
                score += (4 - visHits) * 0.30f;
            }
            else
            {
                // [批123] 连续不可见帧计数：超过 3 帧（≈45ms@66tick）
                //   立即释放 —— 对着墙/目标已消失时不再黏着。
                if (s_visMiss[vi] < 255)
                    ++s_visMiss[vi];

                const DWORD nowV = GetTickCount();
                bool grace = false;
                // [批136] 宽限的释放条件 = 【打到墙】，不再是"超过 3 帧"。
                //   旧规则（3 帧上限）有个致命副作用：目标被小僵尸挡了 4 帧
                //   之后，s_visMiss 再也不会归零 → 该目标【永久失去】宽限
                //   资格 → 立刻丢靶 → 换到远处新目标 → 重走 140~240ms
                //   反应延迟 → 视觉上一顿一顿（日志实测：软锁=0 次）。
                //   现在：被【别的实体】挡住 → 继续宽限（时间窗 180ms 内）；
                //         被【世界几何】挡住 → 立刻放弃（绝不穿墙锁）。
                if (c.index == s_prevTargetForHysteresis &&
                    s_lastVisMs[vi] != 0 &&
                    nowV - s_lastVisMs[vi] <= 180)
                {
                    const float dx = headP.x - s_lastVisPos[vi].x;
                    const float dy = headP.y - s_lastVisPos[vi].y;
                    const float dz = headP.z - s_lastVisPos[vi].z;
                    if (dx * dx + dy * dy + dz * dz <= 96.0f * 96.0f)
                    {
                        const bool wall = Aimbot::TraceBlockedByWorld(
                            eyePos, s_lastVisPos[vi], (void*)localPlayer);
                        grace = !wall;             // 墙 → 放弃；实体遮挡 → 保留
                    }
                }
                if (!grace)
                    continue;
                cWasGrace = true;                    // [批145]
                score += 1.0f;                 // 软锁定惩罚
                if (c.index == s_prevTargetForHysteresis)
                    score -= 3.0f;             // [批145] 软锁的上一目标：弱粘性
                ++softLockCnt;
            }
        }

        // ---- 胜出（评分最优者）----
        if (out.index == -1 || score < bestScore)
        {
            bestScore = score;
            winnerGrace = cWasGrace;                // [批145]
            out.index   = c.index;
            out.sid     = c.sid;          // [批161] 供兜底锚点按 sid 选比例
            out.tickSeq = g_aimTickSeq;   // [批164] 供 SolveTargetAngle 复用
            out.entity  = e.Address();
            out.aimPos  = headP;                          // [批9] 唯一出生地
            out.origin  = c.origin;
            out.fovDeg  = ang;
            out.health  = c.health;
            out.visible = true;

            // 漂移门控记忆 + 迟滞记忆
            s_driftIdxLast = c.index;
            s_driftPosLast = headP;
            s_driftOriginLast = c.origin;      // [批147] 漂移门控要用身体位移
            // [批134] 换靶 → 开启 300ms 最小锁定窗口
            if (s_prevTargetForHysteresis != c.index)
                s_minLockUntil = GetTickCount() + 300;
            s_prevTargetForHysteresis = c.index;

            // [诊断] 选中详情（1s 节流在调用方）
            diagSid = c.sid;
            diagDist = aimDistM;   // [批124] 日志改为"眼位→瞄准点"距离
            snprintf(diagName, sizeof(diagName), "%s", e.ClassName());
        }
    }

    static DWORD s_prevLostMs = 0;   // [批123] 必须在 if 外声明（作用域陷阱）
    if (out.index == -1)
    {
        // [批123] 无目标的帧不能立刻清迟滞：否则"丢失宽限"只能生效一帧。
        //   保留上一目标 180ms（与宽限窗口同一量级），超时才真正清空。
        if (s_prevTargetForHysteresis >= 0)
        {
            if (s_prevLostMs == 0)
                s_prevLostMs = GetTickCount();
            else if (GetTickCount() - s_prevLostMs > 180)
            {
                s_prevTargetForHysteresis = -1;
                s_prevLostMs = 0;
            }
        }
        else
            s_prevLostMs = 0;
        // [批134] 本轮没有任何候选（目标死亡/出 FOV）→ 解除最小锁定，
        //   让下一帧可以自由改选。
        // [批145] 胜出目标只是"软锁撑着"（被小僵尸挡住）→ 同样解除：
        //   软锁不算稳定锁定，掩体外的新目标下一帧就能抢。
        if (out.index == -1 || winnerGrace)
            s_minLockUntil = 0;
        s_driftIdxLast = -1;
    }
    else
    {
        s_prevLostMs = 0;
    }

    // [诊断·L5] 1s 一次：过滤链 + 降级比例（头盒成功率长期 0 = 根因未修）
    static DWORD s_lastDiag = 0;
    {
        const DWORD t = GetTickCount();
        if (t - s_lastDiag >= 1000)
        {
            s_lastDiag = t;
            PayloadLog("[aim] 选靶：候选=%d FOV内=%d 部位解析跳过=%d 门控拒=%d "
                       "选中=%d 软锁=%d 超距=%d",
                       cand, fovSkip, boneSkipCnt, gateFail,
                       out.index, softLockCnt, distSkip);
            if (out.index != -1)
            {
                PayloadLog("[AIM] 目标: %s | 原点Z: %.2f | 瞄准点Z: %.2f",
                           diagName, out.origin.z, out.aimPos.z);
                PayloadLog("[aim] 锁定：sid=%d 实体=%d 距离=%.0fm 血量=%d",
                           diagSid, out.index, diagDist, out.health);
                if (diagFail)
                    PayloadLog("[aim] 最近弃权原因：%s", diagFail);
            }
        }
    }

    return out.index != -1;
}

// ============================================================
//  [批30] 渲染线程：刷新骨骼缓存（引擎骨骼取数只在此处发生）
//  —— 召唤瞬间崩溃的根治：不在游戏线程碰未就绪的实体骨骼。
// ============================================================
bool Aimbot::WroteCmdAngles()
{
    return g_wroteCmdAngles;
}

void Aimbot::RefreshBonesForFrame()
{
    // [批34] 关卡加载门控：本地玩家为空 = 正在加载/切换地图 →
    // 不做任何骨骼取数（此时实体处于半初始化状态，最易触发引擎崩）
    // [批95] 自主读 dwLocalPlayer —— 不依赖透视模块设的 localPlayer 全局。
    //   透视延迟启动或某些路径没走时 localPlayer 永远 0 → 骨骼刷新被
    //   门控挡住 → [批112 后] 自瞄全部"部位解析跳过" → 不瞄不打
    //   （旧实现会全走兜底 Origin+64 → 打空气，现已无兜底）。
    if (!localPlayer)
    {
        const uintptr_t lp = BoneSysLocalPlayerPtr();
        if (!lp)
            return;
        localPlayer = lp;
    }

    static thread_local std::vector<int> idxs;
    idxs.clear();
    for (const auto& [info, state] : GetFrameSnapshot())
    {
        if (info.index >= 0)
            idxs.push_back(info.index);
    }
    if (idxs.empty())
    {
        // [批98] 快照里没有实体 = 自瞄什么都锁不到，必须留痕
        static DWORD s_emptyLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_emptyLog > 3000)
        {
            s_emptyLog = t;
            PayloadLog("[bone-gate] 快照无实体（localPlayer=%p 快照=%d）"
                       "—— 自瞄无目标可锁", (void*)localPlayer,
                       (int)GetFrameSnapshot().size());
        }
        return;
    }
    BoneSysRefreshEntities(idxs.data(), (int)idxs.size());
}

// ============================================================
//  [批21] 自瞄配置持久化（独立文件 aim_config.ini）
// ------------------------------------------------------------
//  EspConfig 的条目表只覆盖 ESP 字段（固定容量快照），自瞄配置
//  从未被保存。这里用独立的 key=value 文件，与 ESP 配置解耦，
//  不触碰冻结的 EspConfig 结构。
// ============================================================
namespace
{
    std::string AimCfgPath()
    {
        static const std::string p = []() {
            char buf[MAX_PATH] = {};
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&Aimbot::CreateMove, &self);
            GetModuleFileNameA(self, buf, MAX_PATH);
            std::string path = buf;
            //  [批49] 原来这里拼的是单反斜杠开头的文件名：C++ 字符串里
            //  "\a"（反斜杠 + a）是【响铃符 BEL = 0x07】，不是反斜杠。
            //  于是目标文件名变成了 "<目录>\x07im_config.ini" 这种
            //  根本不存在的路径 → fopen_s 静默失败 → 自瞄配置
            //  【从来没有被保存过】。这正是"菜单保存无效"的另一半
            //  根因（另一半是 ConfigMgr 从未被菜单调用）。
            //  C4129 警告报的就是这一处。
            //  顺带修掉 find_last_of 里的无效转义：GetModuleFileNameA
            //  返回的是反斜杠，所以必须找反斜杠，不能用斜杠。
            const size_t pos = path.find_last_of('\\');
            std::string dir = (pos == std::string::npos ? std::string(".")
                                                         : path.substr(0, pos));

            //  [批53·单 EXE 模式] DLL 在 %TEMP% 下时（由单 EXE 启动器
            //  释放后注入），配置必须落到 EXE 所在目录 —— 否则写在临时
            //  目录里，重启即丢，用户在 EXE 旁边也找不到配置文件。
            //  启动器注入前会设置 HANXUESNOW_HOME 指向 EXE 目录。
            char home[MAX_PATH] = {};
            if (GetEnvironmentVariableA("HANXUESNOW_HOME", home, MAX_PATH)
                && home[0])
            {
                dir = home;
            }

            return dir + "\\aim_config.ini";
        }();
        return p;
    }
}

bool Aimbot::AimConfigSave()
{
    FILE* f = nullptr;
    if (fopen_s(&f, AimCfgPath().c_str(), "w") != 0 || !f)
    {
        // [批49] 原来这里直接 return —— 打不开文件时【静默失败】，
        // 菜单照样打印"已保存"，用户完全无从察觉。配置写不进去是
        // 必须暴露的问题（路径错/目录只读/文件被占用），所以补日志。
        // [批52] 改为返回 false，让菜单能显示真实的失败提示。
        PayloadLog("[aim] ERR: 配置写入失败：%s（errno=%d）",
                   AimCfgPath().c_str(), errno);
        return false;
    }
    const auto& a = g_Config.aim;
    const auto& h = g_Config.humanize;
    fprintf(f, "; aim config (auto)\n");
    fprintf(f, "enabled=%d\n", a.enabled ? 1 : 0);
    fprintf(f, "key=%d\n", a.key);
    fprintf(f, "fov=%.3f\n", a.fov);
    fprintf(f, "showFov=%d\n", a.showFov ? 1 : 0);
    fprintf(f, "silent=%d\n", a.silent ? 1 : 0);
    fprintf(f, "silentFov=%.3f\n", a.silentFov);
    fprintf(f, "distWeight=%.3f\n", a.distWeight);
    fprintf(f, "aimPart=%d\n", a.aimPart);
    fprintf(f, "friendlyFire=%d\n", a.friendlyFire ? 1 : 0);
    fprintf(f, "recoilComp=%d\n", a.recoilComp ? 1 : 0);
    fprintf(f, "smoothFactor=%.3f\n", h.smoothFactor);
    fprintf(f, "reactionMs=%d\n", h.reactionMs);
    fprintf(f, "reactionMsMax=%d\n", h.reactionMsMax);
    fprintf(f, "jitter=%.3f\n", h.jitterAngle);
    fprintf(f, "deadzone=%.3f\n", h.deadzone);
    fprintf(f, "; [批150] 0=完整日志 1=精简日志（发布版）\n");
    fprintf(f, "logLevel=%d\n", a.logLevel);
    fprintf(f, "; studiohdr 探测（排障用；发布版建议 0）\n");
    fprintf(f, "studioHdrProbe=%d\n", a.studioHdrProbe);
    fclose(f);
    PayloadLog("[aim] 配置已保存：%s", AimCfgPath().c_str());
    return true;      // [批52] 供菜单给出真实的成功提示
}

void Aimbot::AimConfigLoad()
{
    FILE* f = nullptr;
    if (fopen_s(&f, AimCfgPath().c_str(), "r") != 0 || !f)
        return;
    auto& a = g_Config.aim;
    auto& h = g_Config.humanize;
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == ';' || line[0] == '#' || line[0] == '\n')
            continue;
        char k[64] = {};
        float v = 0.0f;
        if (sscanf_s(line, "%63[^=]=%f", k, (unsigned)sizeof(k), &v) != 2)
            continue;
        ++n;
        if (!strcmp(k, "enabled"))              a.enabled = v != 0.0f;
        else if (!strcmp(k, "key"))             a.key = (int)v;
        else if (!strcmp(k, "fov"))             a.fov = v;
        else if (!strcmp(k, "showFov"))         a.showFov = v != 0.0f;
        else if (!strcmp(k, "silent"))          a.silent = v != 0.0f;
        else if (!strcmp(k, "silentFov"))       a.silentFov = v;
        else if (!strcmp(k, "distWeight"))      a.distWeight = v;
        else if (!strcmp(k, "aimPart"))         a.aimPart = (int)v;
        else if (!strcmp(k, "friendlyFire"))    a.friendlyFire = v != 0.0f;
        else if (!strcmp(k, "recoilComp"))      a.recoilComp = v != 0.0f;
        else if (!strcmp(k, "smoothFactor"))    h.smoothFactor = v;
        else if (!strcmp(k, "reactionMs"))      h.reactionMs = (int)v;
        else if (!strcmp(k, "reactionMsMax"))   h.reactionMsMax = (int)v;
        else if (!strcmp(k, "jitter"))          h.jitterAngle = v;
        else if (!strcmp(k, "deadzone"))        h.deadzone = v;
        else if (!strcmp(k, "logLevel"))        a.logLevel = (int)v;
        else if (!strcmp(k, "studioHdrProbe"))  a.studioHdrProbe = (int)v;
    }
    fclose(f);
    PayloadLog("[aim] 配置已加载：%d 项（%s）", n, AimCfgPath().c_str());
}

// ============================================================
//  FOV 范围圈（渲染线程）：屏中心圆 = FOV 圆锥的屏幕投影
// ------------------------------------------------------------
//  半径换算：radius = (屏高/2) × tan(aim.fov) / tan(vFov/2)
//  L4D2 默认 fov cvar = 90（横向），16:9 下垂直 FOV ≈ 58.7°
//  （批4 可接 fov cvar 实值消除估算误差）。
//  只读 g_Config + DisplaySize，无任何游戏内存访问——渲染线程安全。
// ============================================================
// ============================================================
//  [批94] 引擎视角角读取 —— 给 OverrideView 的去后坐力当参照
// ------------------------------------------------------------
//  为什么不用 cmd 角度：静默模式下 cmd 装的是【目标角】，
//  拿它当参照会把"输入角与目标角的差"当成后坐力 → 静默失效。
//  引擎视角角两种模式下都是"画面应该在哪"，是唯一的正确参照。
// ============================================================
bool Aimbot::ReadViewAngles(float out[3])
{
    if (!out) return false;
    ViewAngles v{};
    if (!ViewEng().Read(v))
        return false;
    out[0] = v.pitch; out[1] = v.yaw; out[2] = v.roll;
    return true;
}

void Aimbot::RenderFovRing()
{
    if ((!g_Config.aim.enabled && !g_Config.aim.silent) || !g_Config.aim.showFov)
        return;
    if (!ImGui::GetCurrentContext() || !localPlayer)
        return;                                       // 菜单外/未进对局不画

    // [批19] 骨基址预热已移除：SetupBones 直调不再依赖窗口扫描，
    // 预热（每帧 GetEntityBones 驱动扫描）失去意义，且减少引擎调用。

    // ============================================================
    //  [批11] 范围圈（用户指定：静态正圆，不随视角变形）
    //  白色实线圈 = 平滑自瞄获取范围（aim.fov）
    //  [批91] 实现已改为【解析圆】：圆心恒在屏幕中心，半径由
    //  h/2 · tan(cone) / tan(vFov/2) 直接算出，不依赖任何矩阵快照
    //  （旧实现用 GetFrameView 的矩阵投影，转视角时圈会滞后于画面）。
    //  [批111·死代码] 删除了 kVFovDeg / halfH 两个局部变量：批91 改造
    //  后它们已无人引用（W4 的 C4189 报出），上面"半径换算：halfH ×
    //  tan(角)/tan(vFov/2)"那句注释描述的也是旧公式，一并删掉 ——
    //  注释描述死公式比没有注释更糟，会让人以为圈是这么算的。
    // ============================================================
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (!(disp.x > 0.0f && disp.y > 0.0f))
        return;

    // ============================================================
    //  [批29] 范围圈 = FOV 圆锥在屏幕上的【真实投影】（方向空间）
    //  · 用投影矩阵的【方向分量】求 NDC（不含相机平移项）→ 跳跃/开枪/
    //    后坐/走路 bob 都不改变圈的大小与形状（不再变形）
    //  · 圈上每一点的 3D 角度恰为 coneDeg → 与选靶判定（3D 球面角）
    //    完全一致：圈上=角度边界，圈内=必然触发
    // ============================================================
    // ============================================================
    //  [批91] 范围圈改为【解析圆】：圆心恒在屏幕中心，不依赖任何矩阵
    // ------------------------------------------------------------
    //  实测问题：大幅度转视角时，圈明显偏离准心。
    //  原因：旧实现用 GetFrameView() 的矩阵投影圆锥，而那个矩阵是
    //  tick 级快照（66Hz），画面却按渲染帧率插值 —— 转得越快，
    //  圈越滞后。这和"相机被降频"是同一类错误。
    //
    //  现在直接算半径：
    //      vFov = 2·atan( tan(90°/2) · h / w )       // 由 90° 水平 FOV 推
    //      r_px = (h/2) · tan(cone) / tan(vFov/2)
    //  正圆、永远居中、零延迟；对小角度（≤15°）与真实投影的误差 < 1px。
    // ============================================================
    auto DrawConeRing = [&](float coneDeg, ImU32 col, float thickness)
    {
        const float cx = disp.x * 0.5f;
        const float cy = disp.y * 0.5f;

        // ============================================================
        //  [批116] 垂直 FOV 从视图矩阵求，不再用 g_viewFovDeg
        // ------------------------------------------------------------
        //  视图矩阵 = P·V（WorldToScreen 就是这么用的）。投影的 y 缩放
        //  sy = 1/tan(vFov/2) 乘在 V 的单位行上 → 组合矩阵第 2 行
        //  (m4,m5,m6) 的模恒等于 sy，与视角朝向无关。
        //  r_px = (屏高/2) · tan(cone) · sy。
        //  旧实现读 OverrideView 自定位的 fov：偏移会采到随视角/跳跃
        //  变化的字段 → 圈一大一小、跳跃缩小。
        // ============================================================
        float sy = 0.0f;
        if (IsFrameViewValid())
        {
            const CView& fv = GetFrameView();
            sy = sqrtf(fv.Matrix[1][0] * fv.Matrix[1][0] +
                       fv.Matrix[1][1] * fv.Matrix[1][1] +
                       fv.Matrix[1][2] * fv.Matrix[1][2]);
        }
        if (!(sy > 0.3f && sy < 6.0f))                // 矩阵不可用/异常：估算兜底
        {
            constexpr float kHFovDeg = 90.0f;         // L4D2 默认水平 FOV
            const float hFov = kHFovDeg * (sdk::kPi / 180.0f);
            const float vFov = 2.0f * atanf(tanf(hFov * 0.5f) * (disp.y / disp.x));
            sy = 1.0f / tanf(vFov * 0.5f);
        }
        const float rad = coneDeg * (sdk::kPi / 180.0f);
        const float rpx = (disp.y * 0.5f) * tanf(rad) * sy;

        constexpr int N = 64;
        ImVec2 pts[N];
        for (int i = 0; i < N; ++i)
        {
            const float phi = (float)i / (float)N * 2.0f * sdk::kPi;
            pts[i] = ImVec2(cx + cosf(phi) * rpx, cy + sinf(phi) * rpx);
        }
        ImGui::GetBackgroundDrawList()->AddPolyline(
            pts, N, col, ImDrawFlags_Closed, thickness);
    };

    // [批31] 范围圈：静默模式只画静默圈（半透明白），平滑模式只画平滑圈
    if (g_Config.aim.silent)
        DrawConeRing(fminf(fmaxf(g_Config.aim.silentFov, 1.0f), 15.0f),
                     IM_COL32(255, 255, 255, 60), 1.0f);
    else if (g_Config.aim.enabled)
        DrawConeRing(g_Config.aim.fov, IM_COL32(255, 255, 255, 220), 1.2f);
}

namespace
{
    // ============================================================
    //  [批41·根因修复] 引擎时间注入 —— 必须【无条件、每 tick】执行
    // ------------------------------------------------------------
    //  旧实现把 BoneSysSetCurtime() 放在"已选中目标"之后（原 1877 行），
    //  构成死锁：
    //      选靶 需要骨骼 → 骨骼 需要 curtime → curtime 需要选靶
    //  → g_curtimeEst 恒为 0 → FetchBonesRaw() 第一道门就 return false
    //  → 整场 0 根骨（日志实锤：589 次统计，成功=0 跳过=全部）
    //  → 部位解析全失败 → 固定兜底 Origin+(0,0,64)。
    //  这就是"头/胸/腹怎么改都打脚/打空气"的真正原因。
    //
    //  现在：tick 时钟在 CreateMove 入口推进（与自瞄开关、触发键、是否有
    //  目标、是否静默完全无关），dt 用文件级 g_cmdDt 传给自瞄平滑段。
    // ============================================================
    float g_cmdDt = 0.0f;                 // 本 tick 的 dt（秒），平滑用

    float UpdateTickClock(int tick)
    {
        static int    s_lastTick = 0;
        static bool   s_init     = false;
        static double s_tickInt  = 1.0 / 30.0;      // interval_per_tick 估计
        static DWORD  s_calStart = 0;
        static int    s_calTick  = 0;
        // [批117] curtime 必须【单调累加】，不能每帧重算 tick×interval。
        //   旧写法下 interval 由 EMA 自适应校准，tick 跑到几万时，
        //   interval 改 0.0005 就会让 curtime 回跳几秒 —— BoneSystem 的
        //   "curtime 倒退 = 换图中"门控因此每 3 秒被重新触发一次，
        //   切图冷却无限续期，骨骼缓存整场不刷新（实战 24 次选靶全 -1）。
        static double s_curtime  = 0.0;

        float dt = 0.0f;
        if (!s_init)
        {
            s_init     = true;
            s_lastTick = tick;
            s_calStart = GetTickCount();
            s_calTick  = tick;
            s_curtime  = (double)tick * s_tickInt;
            dt = 0.0f;                              // 首 tick 不推进
        }
        else
        {
            const int dTick = tick - s_lastTick;
            if (dTick > 0)
            {
                // interval 自校准：窗口 ≥8 tick 且 ≥120ms 才更新（EMA 30%）
                const int calDT = tick - s_calTick;
                if (calDT >= 8)
                {
                    const DWORD calMS = GetTickCount() - s_calStart;
                    if (calMS >= 120)
                    {
                        const double est = (double)calMS / 1000.0 / (double)calDT;
                        if (est > 0.004 && est < 0.1)
                            s_tickInt = s_tickInt * 0.7 + est * 0.3;
                        s_calStart = GetTickCount();
                        s_calTick  = tick;
                    }
                }
                const double dT = (double)dTick * s_tickInt;
                s_curtime += dT;                    // 只累加增量：天然单调
                dt = (float)dT;
                s_lastTick = tick;
            }
            else if (dTick < 0)
            {
                // [批117] tick 回溯（换图/重连）：必须重建基线。
                //   旧代码只在 dTick>0 分支更新 s_lastTick，回溯后它永远
                //   停在旧值 → dTick 恒负 → dt 恒 0 → legit 平滑完全不动。
                s_lastTick = tick;
                s_calStart = GetTickCount();
                s_calTick  = tick;
                s_curtime  = (double)tick * s_tickInt;
                dt = 0.0f;
                PayloadLog("[aim] tick 时钟回溯：重建基线 tick=%d curtime=%.2f",
                           tick, s_curtime);
            }
            // dTick == 0（同 tick 重复调用）：dt 保持 0 → 不二次推进
        }
        dt = fminf(dt, 0.100f);                     // 只钳上限；dt=0 = 合法空推进

        // SetupBones 的时间参数（引擎动画钟估计）。旧 GetTickCount 秒与
        // 动画钟差数万秒 → SetupBones 插值卡死，故用 tick×interval。
        BoneSysSetCurtime((float)s_curtime);
        return dt;
    }
}

// ============================================================
//  CreateMove 主入口（Hooks.cpp 每 tick 调用一次）
// ============================================================
void Aimbot::CreateMove(sdk::CUserCmd* pCmd)
{
    if (!pCmd)
        return;

    // 拟人化随机源（一次性播种）
    static bool s_seeded = false;
    if (!s_seeded) { s_seeded = true; srand((unsigned)GetTickCount()); }


    // ============================================================
    //  [批36] 静默架构（对齐开源参考实现 Panguins L4D2_Internal）：
    //  Hook 在本 tick 写角度后【返回 false】→ 引擎不再把 cmd 角应用到
    //  视角；因此这里先手动同步引擎视角 = cmd 的输入角（游戏本该做的
    //  动作）。平滑由 LegitApply 再写平滑角；静默只写 cmd → 画面不动。
    // ============================================================
    g_wroteCmdAngles = false;
    {
        ViewAngles in{};
        in.pitch = pCmd->viewangles.pitch;
        in.yaw   = pCmd->viewangles.yaw;
        in.roll  = pCmd->viewangles.roll;
        ViewEng().Write(in);                       // 视角 = 输入角（基线）
    }

    // [批21] 首次 tick 载入自瞄配置（独立 aim_config.ini）
    //  AimConfigLoad 返回 void，无法区分成功失败——这里只能"一次性",
    //  依赖自身幂等；若以后改成 bool ok，则把置位挪到 Load 之后。
    static bool s_aimCfgLoaded = false;
    if (!s_aimCfgLoaded)
    {
        s_aimCfgLoaded = true;
        AimConfigLoad();
    }

    // ---- 哑调用过滤（CInput::ExtraMouseSample 的空 cmd）----
    if (!pCmd->command_number || !pCmd->tick_count)
        return;

    // [批41] 引擎时间注入：必须在所有"是否开自瞄/是否按触发键"判断之前。
    //  骨骼取数的 curtime 参数依赖它；放在后面就是死锁（见函数头注释）。
    g_cmdDt = UpdateTickClock(pCmd->tick_count);

    // ---- 总开关 / 触发键 ----
    if (!g_Config.aim.enabled && !g_Config.aim.silent)
        return;

    // [批74] 触发键判定。
    //  如果触发键就是左键、而自动连点的宏正在替玩家按左键，
    //  那么"左键"在语义上就是按下的 —— 只查 GetAsyncKeyState 会漏掉
    //  这种情况，表现为"宏在连射，自瞄完全不接管"。
    bool keyDown = (g_Config.aim.key == 0) ||
                   ((GetAsyncKeyState(g_Config.aim.key) & 0x8000) != 0);
    if (!keyDown && g_Config.aim.key == VK_LBUTTON && g_autoFirePressing)
        keyDown = true;                        // 宏代按左键

    if (!keyDown)
    {
        // [诊断] 5s 一次：总开关已开但触发键未按住（最常见"无效果"原因）
        static DWORD s_keyLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_keyLog > 5000)
        {
            s_keyLog = t;
            PayloadLog("[aim] 等待触发键：按住侧键/已绑键后生效（key=0x%X）",
                       g_Config.aim.key);
        }
        return;
    }
    if (!localPlayer)
    {
        // [诊断] 5s 一次：localPlayer 空 = 未进对局（主菜单 dwLocalPlayer=0）
        static DWORD s_lpLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_lpLog > 5000)
        {
            s_lpLog = t;
            PayloadLog("[aim] localPlayer 空（未进对局？）——主菜单内 aim 不工作");
        }
        return;
    }

    // ---- 本地眼位 ----
    const sdk::CBaseEntity local = sdk::EntityList::LocalPlayer();
    if (!local.IsValid())
    {
        // [诊断] 5s 一次：触发键按住了但本地实体无效
        static DWORD s_lvLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_lvLog > 5000)
        {
            s_lvLog = t;
            PayloadLog("[aim] WARN: 本地实体无效（EntityList 解析失败？）");
        }
        return;
    }
    const sdk::Vector eye = local.GetEyePos();

    // ============================================================
    //  [批4·批119 修正] 本地玩家状态门控
    //    · 速度：**不做门控**。EyePos 每 tick 现读实体内存，高速
    //      （连跳/被撞飞）不会让瞄准数学失效；旧注释宣称">380 暂停"
    //      与实现不符，已删。
    //    · 开镜（IN_ZOOM）：有效 FOV 收窄 40%（放大后视野变小，
    //      同样角度覆盖的屏幕范围更大）
    //    · [TODO 批4] ONGROUND/眼位抖动门控：需 m_fFlags 偏移实测
    // ============================================================
    const float localSpeed = local.GetVelocity().Length();
    const bool  zoomed     = (pCmd->buttons & sdk::Button::IN_ZOOM) != 0;
    // [批31] 静默模式：选靶锥放大到 max(FOV, 静默FOV)——否则静默圈内的
    // 目标根本进不了候选（采集锥比静默范围还小 = 静默永远不触发）
    const float silentCap0 = fminf(fmaxf(g_Config.aim.silentFov, 1.0f), 15.0f);
    const float fovBase    = g_Config.aim.silent
                           ? fmaxf(g_Config.aim.fov, silentCap0)
                           : g_Config.aim.fov;
    const float fovEff     = fovBase * (zoomed ? 0.6f : 1.0f);

    // ---- 选靶 ----
    Target tgt;
    // [批146] 大面积未就绪（切图 / 实体刚生成）→ 整帧跳过自瞄。
    //   否则几十个候选逐个失败：既刷日志又白跑一整轮解析。
    if (g_boneCandidates >= 8 && g_boneNotReady * 10 >= g_boneCandidates * 6)
        return;                                   // 未就绪占比 ≥60%

    if (!GetBestTarget(eye, fovEff, tgt))
        return;

    // ============================================================
    //  [批127] 目标切换检测（独立于拟人化开关）
    //  用途①：起手 3 tick 关闭微抖动（jitOff=0），避免起手被随机量
    //        带偏；用途②：距离自适应平滑的"距离值"换目标时重置。
    // ============================================================
    static int s_prevAimIdx = -1;
    const bool freshTarget = (tgt.index != s_prevAimIdx);
    s_prevAimIdx = tgt.index;
    static int s_lockTicks = 0;
    if (freshTarget)
        s_lockTicks = 0;
    else if (s_lockTicks < 1000)
        ++s_lockTicks;

    // ============================================================
    //  [批3] 共用段：帧间隔 / 反应延迟 / 目标完美角（含微抖动）
    // ============================================================

    // ============================================================
    //  [批41] 平滑时间基准：沿用入口 UpdateTickClock() 的 dt
    // ------------------------------------------------------------
    //  旧实现在此处自建 tick 时钟（并在末尾注入 curtime）→ 未选中目标
    //  时永不注入 → 骨骼源恒不可用（死锁，批41 已修）。
    //  dt 语义不变：Δengine_tick × interval_per_tick，同 tick 重复调用
    //  dt=0 不二次推进，仅钳上限 0.1s。
    // ============================================================
    const float dt = g_cmdDt;

    // ---- 拟人化①：反应延迟（目标首次锁定/切换时启动随机延迟）----
    //  延迟中辅助不工作：全透传
    //
    //  [批61] humanize.enabled 之前【没有任何消费点】—— 菜单里关掉
    //  拟人化仍然会延迟/抖动，属于"开关是假的"。这里补上总闸：
    //  关闭时反应延迟与微抖动都按 0 处理，等于纯平滑。
    const bool humanOn = g_Config.humanize.enabled;

    const DWORD nowMs = GetTickCount();
    static int  s_lastTarget      = -1;
    static DWORD s_reactionUntil  = 0;
    if (!humanOn)
    {
        s_reactionUntil = 0;
        s_lastTarget    = -1;
    }
    else if (tgt.index != s_lastTarget)
    {
        s_lastTarget     = tgt.index;
        const int lo = g_Config.humanize.reactionMs;
        int hi = g_Config.humanize.reactionMsMax;
        if (hi < lo) hi = lo;
        const int span = hi - lo;
        s_reactionUntil = nowMs + (DWORD)(lo + (span > 0 ? rand() % span : 0));
    }
    if (nowMs < s_reactionUntil)
        return;                                       // 反应延迟中：本 tick 不出手

    // ============================================================
    //  [批6·修正2] 目标点 EMA（移动时直读，静止时滤波）——已并入求解器
    // ============================================================
    const bool moving = localSpeed > 10.0f;           // [批6] 玩家移动开关

    // ============================================================
    //  [批6·修正2] 微抖动（移动时完全关闭，静止时慢变噪声）
    //  静态状态提升到函数作用域——SolveTargetAngle 求解器也要用。
    // ============================================================
    static sdk::QAngle jitOff{}, jitTgt{};
    static DWORD       jitNext = 0;
    {
        const float jit = humanOn ? g_Config.humanize.jitterAngle : 0.0f;
        // [批127] 起手前 3 tick 强制关抖动：目标刚锁定/切换时准星还在
        //   追赶，叠加随机偏移会把"起手方向"带偏。
        if (moving || jit <= 0.0f || s_lockTicks < 3)
        {
            jitOff = {};                              // [批6] 移动中：抖动完全关闭
            jitTgt = {};
        }
        else
        {
            if (nowMs >= jitNext)
            {
                jitNext     = nowMs + 150;
                jitTgt.pitch = ((rand() % 201) - 100) / 100.0f * jit;
                jitTgt.yaw   = ((rand() % 201) - 100) / 100.0f * jit;
            }
            jitOff.pitch += (jitTgt.pitch - jitOff.pitch) * 0.25f;
            jitOff.yaw   += (jitTgt.yaw   - jitOff.yaw)   * 0.25f;
        }
    }

    // ============================================================
    //  [批31] 微静默执行器（全新实现，独立功能）
    // ------------------------------------------------------------
    //  原理：cmd->viewangles 决定【服务器收到的射击角】；L4D2 相机
    //  跟随鼠标角（cl.viewangles），cmd 不影响画面 → 只写 cmd 即天然
    //  静默（RazeHack §10 实证：mode 0 直接写 cmd->viewangles）。
    //  本函数【不写本地视角、不做平滑】：
    //    · 不写视角 = 画面完全不动（真静默）
    //    · 不做平滑 = 静默命中点就是头/胸的精确骨骼点（更准）
    //  触发条件：开火 + 目标在静默 FOV 内 + 增量在人类可达范围
    //  （counts 门槛防服务器判角度突变）。
    // ============================================================
    auto RunSilentAim = [&](sdk::QAngle& outAngle) -> bool
    {
        ViewAngles local{};
        if (!ViewEng().Read(local))
            return false;                            // 视角读不到：放弃

        // 玩家自己的真实视角 = 曲线在没有目标时回落的终点
        const sdk::QAngle realAng{ local.pitch, local.yaw, 0.0f };

        // 首次进入以真实视角为起点。若从 (0,0) 起步，第一帧就会从
        // "抬头看天"瞬间折到目标 —— 那一下本身就够一个 SNAP。
        if (!g_srvAngValid)
        {
            g_srvAng      = realAng;
            g_srvAngValid = true;
            g_prevReal    = realAng;      // [批63] 玩家视角基准同步
        }

        // ============================================================
        //  [批63] 关键修复：玩家的手 1:1 跟随，只有"瞄准修正"限速
        // ------------------------------------------------------------
        //  旧实现把【玩家自己转视角】和【瞄准修正】塞进同一个 4.5°/tick
        //  限速里，于是玩家一甩鼠标，曲线就被甩在后面慢慢爬。
        //  实测日志：
        //      服务端角=(-10.9,-165.4) 真实角=(-0.5,-4.5) 偏移=161.2°
        //      服务端角=(  2.6,  44.5) 真实角=(-11.9,167.3) 偏移=123.7°
        //  161° 要爬 161/4.5 ≈ 36 tick ≈ 0.55 秒 —— 这半秒里写的 cmd 角
        //  既不是玩家瞄的方向、也不是目标，子弹全飞；表现为"静默时好时坏"。
        //
        //  正确语义：玩家转动视角是他自己的操作，服务端【本来就该】看到
        //  这个变化（不这么写反而会让画面和服务器脱节）。所以我们把
        //  realAng 的增量 1:1 加进曲线，只让"朝目标的修正量"受限速约束。
        //
        //  这样：
        //    · 无目标时曲线 == 真实视角，偏移恒≈0 → 立刻交还引擎，
        //      不会留下一个"陈旧的远角"等着下次爬
        //    · 目标出现时曲线已在真实视角上，2 个 tick 内咬住（≤8°）
        //    · 转视角时服务端看到的就是玩家自己的甩枪 + ≤4.5°/tick 修正，
        //      总位移里没有任何"非人类"成分（玩家自己那部分不加不减）
        // ============================================================
        {
            const float viewDP = realAng.pitch - g_prevReal.pitch;
            const float viewDY = AngleDiff(realAng.yaw, g_prevReal.yaw);
            const float flickMag = sqrtf(viewDP * viewDP + viewDY * viewDY);
            g_prevReal = realAng;

            if (flickMag > 45.0f)
            {
                // 一 tick 转过 45° 以上：这是瞬移/复活/回合重开级别的跳变，
                // 逐步跟随没有意义（会拖出 0.5 秒的错位），直接对齐。
                g_srvAng.pitch = realAng.pitch;
                g_srvAng.yaw   = realAng.yaw;
            }
            else if (isfinite(viewDP) && isfinite(viewDY))
            {
                g_srvAng.pitch = fminf(fmaxf(g_srvAng.pitch + viewDP, -89.0f), 89.0f);
                g_srvAng.yaw   = sdk::NormalizeYaw(g_srvAng.yaw + viewDY);
            }
        }

        // ── 求目标角。任何一步校验失败都当"无目标"，让曲线回落，
        //   绝不把垃圾值写进 cmd（写 NaN/0 才是真正的特征）──
        bool haveTarget = false;
        sdk::QAngle want{};
        {
            const sdk::CBaseEntity te = sdk::EntityList::Get(tgt.index);
            if (te.IsValid() && !te.IsDormant())
            {
                const AimPoint ap = (g_Config.aim.aimPart == 4)
                    ? GetAutoAimPosition(te, tgt.sid)
                    : GetAimPosition(te, g_Config.aim.aimPart, tgt.sid);
                const sdk::Vector aim = ap.pos;

                // [批146] 只有彻底失败（Fail）才放弃；几何兜底点仍可用。
                if (ap.ok())
                {
                    sdk::QAngle ang{};
                    if (CalcAngle(eye, aim, ang))
                    {
                        // FOV 判据用【真实视角】与目标的差：
                        //   玩家自己得大致朝着敌人，自瞄才接管。
                        // 用曲线角去判会出现"玩家已经转走、曲线还黏在
                        // 目标上"的黏滞，视觉上很假。
                        const float dPitch = ang.pitch - realAng.pitch;
                        const float dYaw   = AngleDiff(realAng.yaw, ang.yaw);
                        if (sqrtf(dPitch * dPitch + dYaw * dYaw) <= silentCap0)
                        {
                            want       = ang;
                            haveTarget = true;
                        }
                    }
                }
            }
        }

        // ============================================================
        //  [批55] 限速步进 —— 本改动的核心
        // ------------------------------------------------------------
        //  kStepAim = 4.5°/tick。这个数字不是随手取的，是照着 Lilac
        //  的门槛往内收的：
        //    · SNAP2 要求 tdelta > 5.0  → 4.5 永远够不着
        //    · SNAP  要求 tdelta > 10.0 → 更够不着
        //    · REPEAT 要求 d1 > 5 * tdelta，连续步进下 d1≈step、
        //      tdelta≈2*step，恒有 step < 10*step → 永不成立
        //  66tick 下 4.5°/tick ≈ 297°/s，比人甩枪还快，手感无损失。
        // ============================================================
        constexpr float kStepAim  = 4.5f;   // 追目标
        //  回落也必须是 4.5，不能更快。回落方向是玩家真实视角，
        //  万一它恰好落在某个敌人身上，回落末步就是一次"7° 骤降到 0"，
        //  正好满足 SNAP2 的 aimdist < laimdist*0.1 && tdelta > 5.0。
        //  两个方向共用一个上限，就不存在这种角落。
        constexpr float kStepIdle = 4.5f;   // 回落真实视角

        const sdk::QAngle goal = haveTarget ? want : realAng;
        const float step = haveTarget ? kStepAim : kStepIdle;

        {
            // [批59] ⚠ 参数顺序就是方向：AngleDiff(to, from) 返回 from→to
            //   的带符号最短差。这里要"从当前角走向 goal"，所以 to=goal、
            //   from=g_srvAng。写成 AngleDiff(g_srvAng, goal) 方向整个反掉：
            //   曲线【远离】目标一路爬到 ±180° 对跖点附近震荡，
            //   实测日志：偏移 173.5°→179.0°→179.7°，yaw 在
            //   -177.8/+177.7 之间穿缝往返 —— 自瞄"永远不生效"。
            //   pitch 行写的正是 goal - g_srvAng（正确），两行方向曾不一致，
            //   这就是暴露问题的对照。
            const float dp   = goal.pitch - g_srvAng.pitch;
            const float dy   = AngleDiff(goal.yaw, g_srvAng.yaw);
            const float dist = sqrtf(dp * dp + dy * dy);
            if (dist > 1e-4f)
            {
                const float k  = (dist <= step) ? 1.0f : (step / dist);
                const float np = g_srvAng.pitch + dp * k;
                const float ny = sdk::NormalizeYaw(g_srvAng.yaw + dy * k);
                if (isfinite(np) && isfinite(ny))    // 数学安全防线
                {
                    g_srvAng.pitch = fminf(fmaxf(np, -89.0f), 89.0f);
                    g_srvAng.yaw   = ny;
                    g_srvAng.roll  = 0.0f;
                }
            }
        }

        // ── 曲线已贴合真实视角且无目标：本 tick 交还引擎 ──
        //  不写 cmd = 完全走原逻辑。否则每 tick 都吞掉 cmd，移动预测
        //  会持续对不上，玩家会觉得"一直被拉"。
        const float offP    = g_srvAng.pitch - realAng.pitch;
        const float offY    = AngleDiff(realAng.yaw, g_srvAng.yaw);
        const float offReal = sqrtf(offP * offP + offY * offY);
        if (!haveTarget && offReal < 0.05f)
        {
            g_srvAng = realAng;                      // 彻底对齐，防长期漂移
            return false;
        }

        // ── 写 cmd：每 tick 都写（这正是"连续曲线"的落地方式）──
        //  旧实现只在 IN_ATTACK 帧写，其余帧不写 → 服务端看到 A→B→A。
        //  现在每帧都在同一条曲线上，开火帧只是曲线上的普通一点。
        const float entryYaw = pCmd->viewangles.yaw;

        pCmd->viewangles.pitch = g_srvAng.pitch;
        pCmd->viewangles.yaw   = g_srvAng.yaw;
        pCmd->viewangles.roll  = 0.0f;
        g_wroteCmdAngles = true;           // [批36] Hook 据此返回 false

        // Movement fix：视角改了，把移动向量按视角差重新旋转。
        // 缺这段时，服务端按 cmd 角重建 wishdir、和我们本地预测的对不上，
        // 表现为"瞄准时按住 W 往侧面滑 / 被拉回"。
        {
            const float fm = pCmd->forwardmove;
            const float sm = pCmd->sidemove;
            if (fm != 0.0f || sm != 0.0f)
            {
                const float mag     = sqrtf(fm * fm + sm * sm);
                const float baseAng = atan2f(sm, fm);
                const float dYawRad = (g_srvAng.yaw - entryYaw) * (sdk::kPi / 180.0f);
                const float na      = baseAng + dYawRad;
                pCmd->forwardmove = cosf(na) * mag;
                pCmd->sidemove    = sinf(na) * mag;
            }
        }

        outAngle = g_srvAng;

        // [诊断] 2s 节流。判读用的关键量是"曲线与真实视角的偏移"：
        //  长期偏移很小 = 曲线在跟着玩家走（正常）
        //  长期偏移很大 = 曲线黏在目标上没回落（会被看出来）
        static DWORD s_slLog = 0;
        const DWORD now = GetTickCount();
        if (now - s_slLog > 2000)
        {
            s_slLog = now;
            PayloadLog("[aim] 静默曲线：%s 服务端角=(%.1f,%.1f) "
                       "真实角=(%.1f,%.1f) 偏移=%.1f° 步长上限=%.1f°/tick",
                       haveTarget ? "追目标" : "回落",
                       g_srvAng.pitch, g_srvAng.yaw,
                       realAng.pitch, realAng.yaw, offReal, step);
        }
        return true;
    };

    // ============================================================
    //  [批6·修正3 + 批9] 目标角实时求解器：LegitApply 前一瞬才求角
    // ------------------------------------------------------------
    //  瞄准点 = GetAimPosition（唯一决策点，实时重取——骨骼矩阵每帧
    //  由引擎更新，头盒中心天然跟随姿态）。EMA 在此应用（移动直读/
    //  静止滤波）。dPitch/dYaw 在 LegitApply 内部基于【当时】的
    //  pCmd->viewangles 与最新目标位置计算——移动视差为零。
    // ============================================================
    auto SolveTargetAngle = [&](sdk::QAngle& out) -> bool
    {
        const sdk::CBaseEntity te = sdk::EntityList::Get(tgt.index);
        if (!te.IsValid() || te.IsDormant())
            return false;

        // [批164·性能] 复用胜者瞄准点：GetBestTarget 本 tick 刚解析过
        //   同一实体（骨骼缓存由渲染线程每帧刷新，同 tick 内不变），
        //   不必二次解析（省一次部位解析：骨骼读 ×4 + 射线 ×4 + 碰撞盒读）。
        //   仅当 tick 序号匹配（= 同一 CreateMove tick）才复用；
        //   否则退回实时解析（例如静默诊断块跨 tick 调用）。
        sdk::Vector headP{};
        bool reused = false;
        if (tgt.tickSeq == g_aimTickSeq &&
            !(tgt.aimPos.x == 0.0f && tgt.aimPos.y == 0.0f &&
              tgt.aimPos.z == 0.0f))
        {
            headP  = tgt.aimPos;
            reused = true;
        }
        if (!reused)
        {
            // [批9] 头盒优先：GetAimPosition（实时重取）
            // [批13] aimPart==4 = 自动模式
            // [批146] 三态：Fail 才放弃（不写视角、保持上一帧角度）；
            //   Geometric / ReuseLastPart 照常参与平滑。
            const AimPoint ap = (g_Config.aim.aimPart == 4)
                ? GetAutoAimPosition(te, tgt.sid)
                : GetAimPosition(te, g_Config.aim.aimPart, tgt.sid);
            headP = ap.pos;
            if (!ap.ok())
            {
                // [批10→批112] 旧"头盒不可用 → 碰撞盒比例兜底"已删除：
                //  拿不到真实骨骼点就不平滑，绝不拿裸 m_vecOrigin 猜坐标。
                //  调用方（LegitApply 前一瞬）见 false 直接跳过本帧。
                static DWORD s_solveSkipLog = 0;
                const DWORD nowS = GetTickCount();
                if (nowS - s_solveSkipLog > 3000)
                {
                    s_solveSkipLog = nowS;
                    PayloadLog("[aim] 目标角求解失败：部位解析不可用 idx=%d"
                               " —— 本帧不平滑（无兜底）", tgt.index);
                }
                return false;
            }
        }

        // [批6] 目标点 EMA（移动直读 / 静止滤波）
        static int         s_emaIdx = -1;
        static sdk::Vector s_emaPos{};
        if (moving)
        {
            // [批20] 移动中改轻 EMA（alpha 0.65）：骨骼关节带行走 bob，
            // 直读会上下抖；0.65 快速收敛但吸收动画抖动。
            // [批127·起手侧拉修复] 换目标必须先重建基线！
            //   旧代码只更新 s_emaIdx、不重置 s_emaPos → 首帧从
            //   【上一个目标的位置】向新目标插值 → 准星先往侧边拉一下
            //   （"起手侧拉"的根因，与 Yaw 最短路径无关）。
            if (tgt.index != s_emaIdx)
            {
                s_emaIdx = tgt.index;
                s_emaPos = headP;
            }
            else if (s_emaPos.x == 0.0f && s_emaPos.y == 0.0f && s_emaPos.z == 0.0f)
                s_emaPos = headP;
            else
            {
                const float a = 0.65f;
                s_emaPos = sdk::Vector(
                    s_emaPos.x + (headP.x - s_emaPos.x) * a,
                    s_emaPos.y + (headP.y - s_emaPos.y) * a,
                    s_emaPos.z + (headP.z - s_emaPos.z) * a);
            }
        }
        else if (tgt.index != s_emaIdx)
        {
            s_emaIdx = tgt.index;
            s_emaPos = headP;
        }
        else
        {
            const float a = 0.45f;
            s_emaPos = sdk::Vector(
                s_emaPos.x + (headP.x - s_emaPos.x) * a,
                s_emaPos.y + (headP.y - s_emaPos.y) * a,
                s_emaPos.z + (headP.z - s_emaPos.z) * a);
        }

        const sdk::Vector eyeNow = local.GetEyePos(); // 当前 tick 眼位
        if (!CalcAngle(eyeNow, s_emaPos, out))
            return false;
        out.pitch = fminf(fmaxf(out.pitch + jitOff.pitch, -89.0f), 89.0f);
        out.yaw   = sdk::NormalizeYaw(out.yaw + jitOff.yaw);
        out.roll  = 0.0f;
        return isfinite(out.pitch) && isfinite(out.yaw);
    };

    // ---- [批6 诊断] 锁定开始连打 5 tick（判读工具，保留）----
    {
        sdk::QAngle dTarget{};
        static int s_diagN    = 0;
        static int s_diagLast = -1;
        if (tgt.index != s_diagLast)
        {
            s_diagLast = tgt.index;
            s_diagN    = 5;
        }
        if (s_diagN > 0)
        {
            --s_diagN;
            if (SolveTargetAngle(dTarget))
                PayloadLog("[aim-diag] tgt=(%.2f,%.2f) cur=(%.2f,%.2f) "
                           "ema=(%.1f,%.1f,%.1f) eye=(%.1f,%.1f,%.1f)",
                           dTarget.pitch, dTarget.yaw,
                           pCmd->viewangles.pitch, pCmd->viewangles.yaw,
                           tgt.aimPos.x, tgt.aimPos.y, tgt.aimPos.z,
                           local.GetOrigin().x, local.GetOrigin().y,
                           local.GetOrigin().z);
        }
    }

    // ============================================================
    //  [批31] 微静默独立模式：开启后【平滑自瞄不执行】（互斥）
    // ============================================================
    if (g_Config.aim.silent)
    {
        sdk::QAngle silentAngle{};
        g_hbPhase = 3;                             // [批125] 静默写角阶段
        RunSilentAim(silentAngle);
        g_hbPhase = 0;
        return;                                    // 静默模式：到此为止
    }

    // [批55] 非静默：让静默曲线失效。否则下次开启静默时会从上次
    //  的陈旧角度起步 —— 那一帧就是一次真实跳变（正是要规避的东西）。
    g_srvAngValid = false;

    // ============================================================
    //  [批30] 平滑视觉（Legit）
    //  （目标角全部走实时求解器，LegitApply 前一瞬才求角）
    //
    //  [批111·死代码] 删除了这里的
    //      ViewAngles localView{}; const bool haveLocalView = ViewEng().Read(localView);
    //  两者都无人引用（W4 的 C4189 报出）。ViewEng().Read() 是一次
    //  引擎虚调用，每 tick 白跑一次；留着只会让人以为局部视角在这里
    //  参与了什么判断。真正需要视角的地方在 LegitApply / RunSilentAim
    //  内部各自现读。
    // ============================================================
    const bool attacking     = (pCmd->buttons & sdk::Button::IN_ATTACK) != 0;
    // [批119] 删除 `unstable = localSpeed > 380` —— 它从未参与任何门控，
    //   只被塞进下面那条日志，害得日志读起来像"高速时自瞄罢工"。
    //   实测结论：高速度不影响瞄准正确性 —— EyePos 每 tick 直接读实体
    //   内存（local.GetEyePos()），不是客户端预测缓存；数学上也只用
    //   当前帧的世界坐标，速度不会引入 NaN/乱飞。
    //   高速真正的问题是"目标在动、我们没有提前量"，与本地速度无关。


    // [诊断] 开火帧走平滑（2s 节流）
    if (attacking)
    {
        static DWORD s_npLog = 0;
        const DWORD t = GetTickCount();
        if (t - s_npLog > 2000)
        {
            s_npLog = t;
            PayloadLog("[aim] 开火帧走平滑（本地速度=%.0f u/s）", localSpeed);
        }
    }

    // ── 纯 Legit 模式：前一瞬重求角 → 每帧平滑 + 同步本地视角 ──
    {
        sdk::QAngle tNow{};
        if (SolveTargetAngle(tNow))
        {
            // [批123] 距离自适应平滑：眼位→瞄准点的 3D 距离（Source 单位）
            const float dxU = tgt.aimPos.x - eye.x;
            const float dyU = tgt.aimPos.y - eye.y;
            const float dzU = tgt.aimPos.z - eye.z;
            const float dU  = sqrtf(dxU * dxU + dyU * dyU + dzU * dzU);
            // [批127] 距离也做轻 EMA：目标前后微动时 dU 每 tick 抖动，
            //   会让距离自适应的 S 在 300/800u 边界来回跳 → 中距离的
            //   "一顿一顿"。平滑后速度连续，不再跳变。
            static float s_smDist = 0.0f;
            if (freshTarget || s_smDist <= 0.0f)
                s_smDist = dU;
            else
                s_smDist += (dU - s_smDist) * 0.25f;
            g_hbPhase = 3;                     // [批125] 平滑写角阶段
            LegitApply(pCmd, tNow, dt, s_smDist);
            g_hbPhase = 0;
        }
    }
}

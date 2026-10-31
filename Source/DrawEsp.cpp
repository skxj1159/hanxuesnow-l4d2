// ============================================================
//  DrawEsp.cpp —— Storm Paint.hpp 风格透视绘制层
// ------------------------------------------------------------
//  2026-10-18 移植自 Storm（qwertyuiop3/Storm）Paint.hpp。
//  2026-10-18 21:38 性能重构：文本全部在收录期一次性解析并缓存
//  （UTF-8 归一 + 字体度量 + 距离/血量格式化），绘制期零堆分配、
//  零 sprintf、零重复 sqrt —— 见各处 // [优化点] 注释。
//
//  采纳 Storm 的部分：
//  · 收录口径：Get_Identifier 语义 + 敌人无条件收录 / 道具按距离截断
//  · 方框几何：全部命中盒 8 角逐点投影 → 屏幕并集（GetStormBounds）
//  · 方框画法：半透明底色 + 黑外框±1 + 黑内框∓1 + 主色框
//  · 名称标签：包围盒正上方居中（2026-10-18 用户参考实现方案）
//  · 幽灵态灰化：底 (64,64,64) / 线 (128,128,128) / 字 (128,128,128)
//  · 绘制顺序：距离**从远到近**，近的后画压住远的
//  · 幸存者角色名表：Nick/Rochelle/…/Francis
//
//  刻意保留 ours 的部分（用户 2026-10-18 明确要求）：
//  · 血条（贴左双层：黑槽 + 内填充，按真实满血值归一）
//  · 血量数字 / 距离文本
//  备份见 _removed_esp/、_removed_esp_name_20261018/。
// ============================================================

#include "Include.h"
#include "EntityBounds.h"
#include "IdentifierSystem.h"
#include "MenuFonts.h"        // [优化点] 显式引用 g_fontEsp：文本度量不再
                              //          依赖 DrawCallBack 的 PushFont 隐式状态
#include "PayloadControl.h"
#include "Visibility.h"

#include <algorithm>
#include <cfloat>
#include <vector>
#include <cstring>
#include <string>

// 1 Source 单位 ≈ 2.54cm（人物高 72 单位 ≈ 1.83m）
static constexpr float kGameUnitToMeter = 0.0254f;

// Storm 的方框外扩（Paint.hpp:182-188）与内缩（:271）
static constexpr float kBoxPad   = 2.0f;
static constexpr float kBoxInset = 2.0f;

// 保留 ours 的血条布局参数（血条贴左）
static constexpr float kHealthGap     = 2.0f;
static constexpr float kHealthInset   = 1.0f;
static constexpr int   kHealthOutline = 150;

// ============================================================
//  Storm 配色表（Paint.hpp:205-238 的语义，逐项对齐）
//    橙 (255,128,0) = 特感 / 红 (255,0,0) = 小僵尸
//    蓝 (0,128,255) = 坦克·石头 / 白 = 道具 / 绿 (0,255,0) = 幸存者
//  女巫沿用 ours 的紫（Storm 未为女巫单列颜色）。
//  [优化点·死代码] 删除零引用的 GhostFill（DrawBox 的幽灵填充用
//  内联 ImColor(64,64,64,espFillAlpha)，随用户可调 alpha 变化）。
// ============================================================
namespace StormPaintColor
{
    inline const ImColor Special   (255, 128, 0);
    inline const ImColor Tank      (0, 128, 255);
    inline const ImColor Witch     (200, 0, 255);
    inline const ImColor GhostLine (128, 128, 128, 255);
    inline const ImColor GhostText (128, 128, 128, 255);

    // 底色 = 主色 / 2，alpha 取用户可调的 espFillAlpha（0 = 不画填充）
    inline ImColor HalfFill(const ImColor& c)
    {
        const ImVec4 v = c;
        return ImColor(v.x * 0.5f, v.y * 0.5f, v.z * 0.5f, (float)espFillAlpha);
    }
}

// Storm 的名字标签（Paint.hpp:205-247）
namespace StormLabel
{
    const char* ForId(int id)
    {
        switch (id)
        {
        case sid::Boomer:        return "Boomer";
        case sid::Rock:          return "Rock";
        case sid::Aid:           return "Aid";
        case sid::Charger:       return "Charger";
        case sid::Adrenaline:    return "Adrenaline";
        case sid::Defibrillator: return "Defibrillator";
        case sid::Pain:          return "Pain";
        case sid::Ammo:          return "Ammo";
        case sid::Hunter:        return "Hunter";
        case sid::Infected:      return "Infected";
        case sid::Jockey:        return "Jockey";
        case sid::Smoker:        return "Smoker";
        case sid::Spitter:       return "Spitter";
        case sid::Tank:          return "Tank";
        case sid::Witch:         return "Witch";
        case sid::Survivor:      return "Survivor";
        default:                 return "";
        }
    }

    // Storm 的幸存者角色名表（Paint.hpp:244）
    static const char* const kSurvivors[8] = {
        "Nick", "Rochelle", "Coach", "Ellis",
        "Bill", "Zoey", "Louis", "Francis"
    };

    const char* SurvivorById(int id)
    {
        if (id < 0 || id > 7) return "Survivor";
        return kSurvivors[id];
    }
}

// 主色：按 Storm Identifier 决定（颜色取自 Globals 可调全局，菜单可改）
static ImColor StormColorFor(int id)
{
    switch (id)
    {
    case sid::Survivor: return g_colSurvivor;
    case sid::Infected: return g_colInfected;
    case sid::Tank:     return g_colTank;
    case sid::Witch:    return g_colWitch;
    case sid::Rock:     return g_colTank;
    case sid::Aid: case sid::Adrenaline: case sid::Defibrillator:
    case sid::Pain: case sid::Ammo:
        return g_colSpecial;
    default:
        // 其余全是特感（Smoker / Boomer / Hunter / Spitter / Jockey / Charger）
        return g_colSpecial;
    }
}

// ============================================================
//  名称显示辅助（参考实现方案：实体索引直读 GetPlayerInfo）
// ------------------------------------------------------------
//  参考来源：用户提供的 ESP.cpp —— 对 CTerrorPlayer/SurvivorBot
//  直接 GetPlayerInfo(实体索引) 取 pi.name。L4D2 玩家类实体的
//  实体索引 == 玩家槽位（真人实测成立），无映射链、无槽位换算。
//
//  ⚠ 两条铁律（19:38 卡死 / 21:15 闪退的教训）：
//    · 输出缓冲必须【静态】且宽裕（0x400）—— 引擎对非法槽位也会
//      memset/写整个真实 player_info_t，栈上传必写穿；
//    · 名字字节编码不保证（UTF-8 / 本机 GBK 都可能）→ 归一化后渲染。
// ============================================================
namespace
{
    struct PiRaw { unsigned char b[0x400]; };

    // GetLocalPlayer（虚表槽 12，返回本地玩家【真实实体索引】）—— SEH 包裹
    int SafeGetLocalPlayerIdx(void* client)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(client);
            using Fn = int (__thiscall*)(void*);
            return reinterpret_cast<Fn>(vt[12])(client);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // 引擎直读（SEH 包裹虚调用；函数内不得出现需要对象展开的 C++ 对象）
    bool GetPlayerInfoName(int entIdx, char* out, size_t cap, unsigned int& xuid)
    {
        out[0] = '\0';
        xuid   = 0;

        static void* s_client = nullptr;
        static bool  s_tried  = false;
        if (!s_tried)
        {
            s_tried = true;
            // [批153] 加锁获取（与自瞄/视角共用同一把接口锁）
            s_client = GetModuleInterfaceLocked("engine.dll",
                                                "VEngineClient013");
        }
        if (!s_client)
            return false;

        // ── [修正错位] 实体表槽位 → 玩家槽位的换算（自校准，不写死）──
        // LiveEsp 的 info.index 是 dwEntityList 指针表的【槽位号】；
        // 实测列表头占一个 stride（幸存者出现在表槽 0），即表槽位比
        // 真实实体索引/玩家槽位偏 1。偏移用本地玩家双索引差值动态标定：
        //   offset = GetLocalPlayer()（真实索引）- localPlayerIndex（表槽位）
        // 两者都 sane 且偏移在合理区间才采信；未标定前拒绝取名（宁缺勿错）。
        static int s_slotOffset = INT_MIN;          // INT_MIN = 尚未标定
        if (s_slotOffset == INT_MIN)
        {
            const int lp = localPlayerIndex;        // Globals：本地玩家表槽位
            if (lp >= 0 && lp < 34)
            {
                const int trueIdx = SafeGetLocalPlayerIdx(s_client);
                if (trueIdx > 0 && trueIdx < 34 &&
                    trueIdx - lp >= -1 && trueIdx - lp <= 2)
                {
                    s_slotOffset = trueIdx - lp;
                    PayloadLog("[name-map] 槽位偏移自校准：表槽位 %d → 真实索引 %d（offset=%+d）",
                               lp, trueIdx, s_slotOffset);
                }
            }
        }
        if (s_slotOffset == INT_MIN)
            return false;                           // 未标定：本轮不出名

        const int slot = entIdx + s_slotOffset;     // [修正错位] 表槽位→玩家槽位
        if (slot < 1 || slot > 33)
            return false;

        static PiRaw raw;                            // 静态！单渲染线程访问
        memset(raw.b, 0, sizeof(raw.b));
        bool ok = false;
        __try
        {
            void** vt = *reinterpret_cast<void***>(s_client);
            using Fn = int (__thiscall*)(void*, int, void*);
            ok = reinterpret_cast<Fn>(vt[8])(s_client, slot, raw.b) != 0;  // 槽 8
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        if (!ok)
            return false;

        // name @ +0x08 原样拷贝（编码归一化由调用方统一做）
        const char* nm = reinterpret_cast<const char*>(raw.b + 0x08);
        size_t n = 0;
        while (n + 1 < cap && nm[n]) { out[n] = nm[n]; ++n; }
        out[n] = '\0';
        xuid = *reinterpret_cast<const unsigned int*>(raw.b);

        // 控制字符 = 布局错位碎片 → 拒绝（宁缺勿错）
        for (const char* q = out; *q; ++q)
            if ((unsigned char)*q < 0x20) { out[0] = '\0'; return false; }
        return out[0] != '\0';
    }

    // ============================================================
    //  [优化点] 玩家名 500ms TTL 缓存
    //  名字几乎不变 —— 引擎调用（含 1KB memset + SEH）从每帧每玩家
    //  降到每玩家 2 次/秒。
    // ============================================================
    struct NameCacheSlot { char name[40]; DWORD stamp; bool valid; };
    NameCacheSlot s_nameCache[34] = {};
    constexpr DWORD kNameCacheTtl = 500;

    bool GetPlayerInfoNameCached(int entIdx, char* out, size_t cap)
    {
        out[0] = '\0';
        if (entIdx < 1 || entIdx > 33)
            return false;

        NameCacheSlot& c = s_nameCache[entIdx];
        const DWORD now = GetTickCount();
        if (!c.valid || (now - c.stamp) >= kNameCacheTtl)
        {
            unsigned int xuid = 0;
            c.valid = GetPlayerInfoName(entIdx, c.name, sizeof(c.name), xuid);
            c.stamp = now;
        }
        if (!c.valid)
            return false;
        strncpy_s(out, cap, c.name, _TRUNCATE);
        return true;
    }

    // ============================================================
    //  [优化点] 编码归一化改为纯缓冲版本（零堆分配）
    //  合法 UTF-8 = 纯内存拷贝；GBK 路径的宽字符中转用栈上定长数组
    //  （名字 ≤35 字节 → ≤35 个 wchar），不再产生 std::string/wstring。
    // ============================================================
    bool IsValidUtf8Name(const char* s, size_t len)
    {
        size_t i = 0;
        while (i < len)
        {
            const unsigned char c = (unsigned char)s[i];
            if (c < 0x80) { ++i; continue; }
            int need = 0;
            unsigned int cp = 0;
            if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
            else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
            else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
            else return false;
            if (i + need >= len)
                return false;
            for (int k = 1; k <= need; ++k)
            {
                const unsigned char cc = (unsigned char)s[i + k];
                if ((cc & 0xC0) != 0x80) return false;
                cp = (cp << 6) | (cc & 0x3F);
            }
            if ((need == 1 && cp < 0x80) || (need == 2 && cp < 0x800) ||
                (need == 3 && cp < 0x10000) || cp > 0x10FFFF)
                return false;
            i += (size_t)need + 1;
        }
        return true;
    }

    // 返回写入的 UTF-8 字节数（不含 NUL）；cap 含 NUL 位
    size_t NormalizeToUtf8(const char* src, char* dst, size_t cap)
    {
        size_t len = strlen(src);
        if (len + 1 > cap)
            len = cap - 1;
        if (IsValidUtf8Name(src, len))
        {
            memcpy(dst, src, len);
            dst[len] = '\0';
            return len;
        }

        // 非法 UTF-8 → 按本机 ANSI（中文 Windows = GBK）转宽字符再转 UTF-8
        wchar_t wide[64];
        const int wlen = MultiByteToWideChar(CP_ACP, 0, src, (int)len,
                                             wide, (int)(sizeof(wide) / sizeof(wide[0]) - 1));
        if (wlen <= 0) { dst[0] = '\0'; return 0; }
        wide[wlen] = L'\0';

        const int ulen = WideCharToMultiByte(CP_UTF8, 0, wide, wlen,
                                             dst, (int)cap - 1, nullptr, nullptr);
        if (ulen <= 0) { dst[0] = '\0'; return 0; }
        dst[ulen] = '\0';
        return (size_t)ulen;
    }
} // namespace

// ============================================================
//  可见 ESP 列表（收录期一次性解析，绘制期零计算）
// ------------------------------------------------------------
//  [优化点] EspItem 瘦身：
//    · 删除 isEquipment（道具在收录期全部 continue，恒 false）
//    · 删除 info/state 两个结构体拷贝（全文件无消费者，每实体 ~80B）
//    · label[48] → nameUtf8[64]：收录期完成「取名 + 编码归一 + 字体
//      度量」，绘制期直接 AddText，不再有 std::string/CalcTextSize
//    · distText/hpText：收录期格式化完毕（快照即本帧数据，一帧内不变）
// ============================================================
namespace
{
    struct EspItem
    {
        StormBounds bounds{};
        Vec3   position{};
        int    index       = 0;
        int    sidId       = -1;     // Storm Identifier（恒正值；道具不入列）
        int    health      = 0;
        int    maxHealth   = 100;
        bool   ghost       = false;
        bool   visible     = true;   // 可见性判定（掩体/玻璃后 = false → g_colHidden）
        float  distanceSq  = 0.0f;
        float  textScale   = 1.0f;   // 文字/血条随距离缩放（近大远小，0.45~1.0）
        char   nameUtf8[64] = {};    // [优化点] 最终显示串（已归一 UTF-8）
        float  nameW       = 0.0f;   // [优化点] 缓存的文本度量（ESP 字体）
        float  nameH       = 0.0f;
        char   distText[16] = {};    // [优化点] "12.3 m" 收录期格式化
        char   hpText[8]    = {};    // [优化点] 血量数字收录期格式化

        int absId() const { return (sidId < 0) ? -sidId : sidId; }
    };

    std::vector<EspItem> g_espItems;
    bool g_espReady = false;
    Vec3 g_localPos{};
    bool g_localPosOk = false;
    double g_lastBuildMs = 0.0;      // [优化点] 收录期耗时自检（[esp-perf] 用）

    // 各类型满血值（血条归一用；沿用 ours 实测表）
    int MaxHealthForSid(int sidAbs, int zombieClass, int team)
    {
        if (team == 2) return 100;                      // 幸存者
        switch (sidAbs)
        {
        case sid::Witch:    return 1000;
        case sid::Smoker:   return 250;
        case sid::Boomer:   return 50;
        case sid::Hunter:   return 250;
        case sid::Spitter:  return 100;
        case sid::Jockey:   return 325;
        case sid::Charger:  return 600;
        case sid::Tank:     return 4000;
        case sid::Infected: return 50;
        default:            return (zombieClass >= 1 && zombieClass <= 8) ? 50 : 100;
        }
    }

    void EspFrameBeginInternal()
    {
        g_espItems.clear();          // capacity 保留：稳态零扩容
        g_espReady = false;
        g_localPosOk = false;

        // 几何降级诊断（限流）：命中盒链路是否可用、失败时是否靠世界 AABB 兜住
        static int s_logNoLocal = 0, s_logNoBounds = 0, s_logFallback = 0;

        if (!localPlayer || !IsFrameViewValid())
            return;

        g_localPos = ReadMemory<Vec3>(localPlayer + Offsets::m_vecOrigin);
        g_localPosOk = isfinite(g_localPos.x) && isfinite(g_localPos.y) && isfinite(g_localPos.z);
        if (!g_localPosOk)
        {
            if (s_logNoLocal < 3)
            {
                ++s_logNoLocal;
                PayloadLog("[esp] 诊断：本地玩家位置无效（localPlayer=%p）或视图未就绪，本帧不画",
                           (void*)localPlayer);
            }
            return;
        }

        g_espItems.reserve(64);

        // 绘制侧过滤链计数（每 ~3 秒 dump：快照里有、框却没画的实体死在哪一步）
        int fClass = 0, fZero = 0, fEquip = 0, fGeom = 0, fCommon = 0;
        static bool s_visInit = VisInit();

        // [优化点] 文本度量用显式 ESP 字体（18px 常规雅黑）——
        // 收录期在 PushFont 作用域之外，必须传字体指针而非吃 GetFont() 隐式状态
        ImFont* espFont = Neverlus::g_fontEsp ? Neverlus::g_fontEsp
                                              : ImGui::GetFont();

        for (const auto& [info, state] : GetFrameSnapshot())
        {
            // ---- 过滤链：便宜在前，昂贵在后（短路求值）----
            // 分类 → 全零位置 → 队友开关 → 道具 → 小僵尸，
            // 全过才进几何投影（GetStormBounds）与可见性射线（VisIsVisible）
            const int id = info.stormId;
            if (id == -1) { ++fClass; continue; }

            // 位置全零 = 实体尚未同步（幽灵态未出生/刚生成/骑乘交接瞬间），
            // 投影后画在地图原点（可玩区外）—— 这就是"地图外出现特感框"
            if (info.position.x == 0.0f && info.position.y == 0.0f &&
                info.position.z == 0.0f) { ++fZero; continue; }

            // Storm 无条件收录幸存者；ours 保留「显示队友」开关（菜单可见）
            if (info.team == 2 && !showAllies)
                continue;

            // 道具透视已整体拆除（2026-10-18）：负值 Identifier 不进绘制列表
            if (id < 0) { ++fEquip; continue; }
            const int sidAbs = id;

            // 过滤普通僵尸（画面降噪；Tank/女巫/特感不受影响）
            if (filterCommonInfected && id == sid::Infected)
                { ++fCommon; continue; }

            // ---- 几何（每实体仅此一次投影，结果传给全部绘制段）----
            StormBounds bounds;
            if (!GetStormBounds(info, kBoxPad, bounds))
            {
                if (s_logNoBounds < 5)
                {
                    ++s_logNoBounds;
                    PayloadLog("[esp] 诊断：idx=%d sid=%d 几何不可用（命中盒与世界 AABB 都失败）",
                               info.index, id);
                }
                ++fGeom;
                continue;   // 几何完全不可用：跳过（与 Storm 一致）
            }
            if (bounds.boxes == 1 && s_logFallback < 3)
            {
                ++s_logFallback;
                PayloadLog("[esp] 诊断：命中盒链路不可用，正在用世界 AABB 兜底画框（形状不如逐盒贴合）");
            }

            // ---- 可见性判定（掩体两色）：本地眼位 → 目标胸口 ----
            const Vec3 eye    (g_localPos.x, g_localPos.y, g_localPos.z + 60.0f);
            const Vec3 target (info.position.x, info.position.y, info.position.z + 40.0f);
            const bool visible = VisIsVisible(info.index, eye, target);

            // ---- 文字/血条近大远小：10m 内原尺寸，随距离线性衰减到 0.45 ----
            float distanceSq = 0.0f;
            {
                const float dx = g_localPos.x - info.position.x;
                const float dy = g_localPos.y - info.position.y;
                const float dz = g_localPos.z - info.position.z;
                distanceSq = dx * dx + dy * dy + dz * dz;
            }
            const float distM    = sqrtf(distanceSq) * kGameUnitToMeter;
            const float textScale = fminf(fmaxf(1.15f - distM / 40.0f, 0.45f), 1.0f);

            EspItem it;
            it.index      = info.index;
            it.sidId      = id;
            it.position   = info.position;
            it.health     = info.health;
            it.ghost      = state.isGhost;
            it.distanceSq = distanceSq;
            it.textScale  = textScale;
            it.bounds     = bounds;
            it.visible    = visible;
            it.maxHealth  = MaxHealthForSid(sidAbs, info.zombieClass, info.team);

            // ---- [优化点] 名称：收录期一次解析（编码归一 + 字体度量）----
            if (enableESPName)
            {
                // 文本度量用显式 ESP 字体（18px 常规雅黑）——
                // 收录期在 PushFont 作用域之外，必须传字体指针而非吃 GetFont() 隐式状态
                ImFont* espFont = Neverlus::g_fontEsp ? Neverlus::g_fontEsp
                                                      : ImGui::GetFont();
                char label[40];
                if (sidAbs == sid::Survivor)
                {
                    // 玩家类：GetPlayerInfo(实体索引) 直读（500ms TTL 缓存）
                    if (!GetPlayerInfoNameCached(it.index, label, sizeof(label)))
                        strncpy_s(label, StormLabel::ForId(sid::Survivor), _TRUNCATE);
                }
                else
                {
                    strncpy_s(label, StormLabel::ForId(sidAbs), _TRUNCATE);
                }

                it.nameW = it.nameH = 0.0f;
                if (label[0] && espFont)
                {
                    const size_t n = NormalizeToUtf8(label, it.nameUtf8,
                                                     sizeof(it.nameUtf8));
                    if (n > 0)
                    {
                        const float nameSize = nameTextSize * it.textScale;
                        const ImVec2 ts = espFont->CalcTextSizeA(
                            nameSize, FLT_MAX, 0.0f, it.nameUtf8);
                        it.nameW = ts.x;
                        it.nameH = ts.y;
                    }
                }
            }
            else
            {
                it.nameUtf8[0] = '\0';
            }

            // ---- [优化点] 距离文本：收录期格式化（复用上面已算的 distM，
            //      不再重做 Vec3 减法 + sqrt + 每帧 sprintf）----
            if (enableDistance)
                sprintf_s(it.distText, "%.1f m", distM);
            else
                it.distText[0] = '\0';

            // ---- [优化点] 血量数字：收录期格式化（快照即本帧血量）----
            if (enableHealthESP)
                sprintf_s(it.hpText, "%d", it.health);
            else
                it.hpText[0] = '\0';

            g_espItems.push_back(it);
        }

        // Storm 的排序：距离升序（Paint.hpp:65-70），绘制时倒序 → 从远到近
        std::sort(g_espItems.begin(), g_espItems.end(),
                  [](const EspItem& a, const EspItem& b)
                  { return a.distanceSq < b.distanceSq; });

        g_espReady = true;

        // 汇总诊断：每 ~3 秒一次，便于一眼确认"快照有但画不出"卡在哪一环
        static unsigned s_tick = 0;
        if ((s_tick++ % 180u) == 0)
        {
            int nHitbox = 0;
            for (const EspItem& it : g_espItems)
                if (it.bounds.boxes > 1) ++nHitbox;   // >1 = 走了逐盒并集（真命中盒）
            PayloadLog("[esp] 可见 %d ｜ 逐盒几何 %d ｜ 降级AABB %d ｜ 开关 box=%d name=%d hp=%d hpnum=%d dist=%d allies=%d",
                       (int)g_espItems.size(), nHitbox,
                       (int)g_espItems.size() - nHitbox,
                       enableBox2D ? 1 : 0, enableESPName ? 1 : 0,
                       enableHealthBar ? 1 : 0, enableHealthESP ? 1 : 0,
                       enableDistance ? 1 : 0,
                       showAllies ? 1 : 0);
            PayloadLog("[esp-draw] 快照->绘制过滤：分类-1=%d 全零pos=%d 道具=%d 几何失败=%d 小僵尸=%d 最终=%d",
                       fClass, fZero, fEquip, fGeom, fCommon, (int)g_espItems.size());
            for (const EspItem& it : g_espItems)
            {
                PayloadLog("[esp-draw] idx=%-4d sid=%-4d vis=%d ghost=%d hp=%-5d "
                           "box=%.0fx%.0f dist=%.0fm pos=%.0f,%.0f,%.0f",
                           it.index, it.sidId, it.visible ? 1 : 0,
                           it.ghost ? 1 : 0, it.health,
                           it.bounds.right - it.bounds.left,
                           it.bounds.bottom - it.bounds.top,
                           sqrtf(it.distanceSq) * kGameUnitToMeter,
                           it.position.x, it.position.y, it.position.z);
            }
        }
    }

    const std::vector<EspItem>& EspItems()
    {
        if (!g_espReady)
            EspFrameBeginInternal();
        return g_espItems;
    }
}

void EspFrameBegin()
{
    // [优化点] 收录期耗时自检（每 180 帧随 [esp-perf] 一起打印）
    LARGE_INTEGER f{}, t0{}, t1{};
    static bool s_fInit = false;
    if (!s_fInit) { s_fInit = true; QueryPerformanceFrequency(&f); }
    QueryPerformanceCounter(&t0);

    EspFrameBeginInternal();

    QueryPerformanceCounter(&t1);
    if (f.QuadPart > 0)
        g_lastBuildMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
}

// ============================================================
//  Storm Draw_Box（Paint.hpp:256-291）
//  四层：① 半透明底色（主色/2, alpha128, 内缩 2px）
//        ② 纯黑外框（±1px）③ 纯黑内框（∓1px）④ 主色框
//  幽灵态三处颜色全换灰。
// ============================================================
static void DrawBox(const StormBounds& b, const ImColor& base, bool ghost)
{
    const float l = b.left,  t = b.top;
    const float w = b.right - l, h = b.bottom - t;
    if (!(w > 0.5f && h > 0.5f))
        return;

    // ① 半透明底色（内缩 2px；espFillAlpha=0 时不画填充，只留描边）
    if (espFillAlpha > 0)
        Gui.RectangleFilled(Vec2(l + kBoxInset, t + kBoxInset),
                            Vec2(w - kBoxInset * 2.0f, h - kBoxInset * 2.0f),
                            ghost ? ImColor(64, 64, 64, espFillAlpha)
                                  : StormPaintColor::HalfFill(base));

    const ImColor line = ghost ? ImColor(StormPaintColor::GhostLine) : base;
    const ImColor black(0, 0, 0, 255);

    // ② 黑外框（外扩 1px）
    Gui.Rectangle(Vec2(l - 1.0f, t - 1.0f), Vec2(w + 2.0f, h + 2.0f), black, 1.0f);
    // ③ 黑内框（内缩 1px）
    Gui.Rectangle(Vec2(l + 1.0f, t + 1.0f), Vec2(w - 2.0f, h - 2.0f), black, 1.0f);
    // ④ 主色框
    Gui.Rectangle(Vec2(l, t), Vec2(w, h), line, 1.0f);
}

// ============================================================
//  叠加层（血条 / 血量数字 / 名称）：单次遍历，画在全部方框之后
// ------------------------------------------------------------
//  [优化点·逻辑扁平化] 原来三个函数各自全量遍历 + 各自判开关；
//  合并为一个 pass，倒序与方框一致（近的压住远的）。
//  [优化点] 全部使用收录期缓存的文本/度量 —— 无 sprintf、无
//  CalcTextSize、无 Length() 重算、无 std::string。
// ============================================================
static void DrawOverlays(const std::vector<EspItem>& items)
{
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (!bg)
        return;

    ImFont* font = Neverlus::g_fontEsp ? Neverlus::g_fontEsp : ImGui::GetFont();
    if (!font)
        return;

    for (size_t n = items.size(); n-- > 0; )
    {
        const EspItem& it = items[n];
        const ImColor base = it.visible ? StormColorFor(it.absId()) : g_colHidden;
        const ImColor txt  = it.ghost ? ImColor(StormPaintColor::GhostText) : base;

        // ---- 血条（贴左双层：黑槽 + 内填充）----
        if (enableHealthBar)
        {
            const float barW   = ImMax(healthBarWidth * it.textScale, 2.0f);
            const float bottom = it.bounds.bottom + kHealthInset;
            const float top    = it.bounds.top    - kHealthInset;
            const float total  = (bottom - top) * healthBarHeightRatio;
            const float slotTop = bottom - total;
            const float x0      = it.bounds.left - kHealthGap - barW;

            Gui.RectangleFilled(Vec2(x0, slotTop), Vec2(barW, total),
                                ImColor(0, 0, 0, kHealthOutline));

            const float ratio = (it.maxHealth > 0)
                ? fminf(fmaxf(it.health / (float)it.maxHealth, 0.0f), 1.0f) : 0.0f;
            if (ratio > 0.0f)
            {
                const float fillW = ImMax(barW - 2.0f, 1.0f);
                const float fillH = total * ratio;
                Gui.RectangleFilled(Vec2(x0 + 1.0f, bottom - fillH), Vec2(fillW, fillH),
                                    it.ghost ? ImColor(StormPaintColor::GhostLine)
                                             : StormColorFor(it.absId()));
            }
        }

        // ---- 血量数字（血条左侧，收录期已格式化）----
        if (enableHealthESP && it.hpText[0])
        {
            const float barW = ImMax(healthBarWidth * it.textScale, 2.0f);
            const float size = healthTextSize * it.textScale;
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, it.hpText);
            const float h = (it.bounds.bottom - it.bounds.top + kHealthInset * 2.0f)
                            * healthBarHeightRatio;
            const float x = it.bounds.left - kHealthGap - barW - 4.0f - ts.x;
            const float y = it.bounds.bottom - h * 0.5f - ts.y * 0.5f;
            bg->AddText(font, size, ImVec2(x, y), txt, it.hpText);
        }

        // ---- 距离（盒下方居中，收录期已格式化）----
        if (enableDistance && g_localPosOk && it.distText[0])
        {
            const float size = distanceTextSize * it.textScale;
            const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, it.distText);
            const float x = (it.bounds.left + it.bounds.right) * 0.5f - ts.x * 0.5f;
            bg->AddText(font, size, ImVec2(x, it.bounds.bottom + 3.0f), txt, it.distText);
        }

        // ---- 名称（盒正上方居中；收录期已归一 + 度量，黑影 + 主色两遍）----
        if (enableESPName && it.nameUtf8[0] && it.nameW > 0.0f)
        {
            const float nameSize = nameTextSize * it.textScale;
            const float cx = (it.bounds.left + it.bounds.right) * 0.5f;
            const float tx = cx - it.nameW * 0.5f;
            const float ty = it.bounds.top - it.nameH - 1.0f;
            bg->AddText(font, nameSize, ImVec2(tx + 1.0f, ty + 1.0f),
                        IM_COL32(0, 0, 0, 200), it.nameUtf8);
            bg->AddText(font, nameSize, ImVec2(tx, ty), txt, it.nameUtf8);
        }
    }
}

// ============================================================
//  主绘制：从远到近遍历（Storm Paint.hpp:72 的倒序）
// ============================================================
void DrawStormEsp()
{
    if (!enableBox2D && !enableESPName && !enableHealthBar &&
        !enableHealthESP && !enableDistance)
        return;

    // [优化点] 帧耗时自检：每 180 帧打印一次（构建 + 绘制两段）
    LARGE_INTEGER t0{}, t1{};
    static LARGE_INTEGER s_freq{};
    static bool s_freqInit = false;
    if (!s_freqInit)
    {
        s_freqInit = true;
        QueryPerformanceFrequency(&s_freq);
    }
    QueryPerformanceCounter(&t0);

    const auto& items = EspItems();

    // Storm：size()-1 起倒序 = 远的先画、近的后画（近框压住远框）
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (bg && !items.empty())
    {
        for (size_t n = items.size(); n-- > 0; )
        {
            const EspItem& it = items[n];
            // 可见性两色：掩体/玻璃后 = g_colHidden；幽灵态灰化在 DrawBox 内
            const ImColor base = it.visible ? StormColorFor(it.absId())
                                            : g_colHidden;

            if (enableBox2D)
                DrawBox(it.bounds, base, it.ghost);
        }
    }

    // ours 保留的叠加层（在所有方框之后画，压在最上层）
    DrawOverlays(items);

    QueryPerformanceCounter(&t1);
    static unsigned s_perfTick = 0;
    if (s_freq.QuadPart > 0 && (s_perfTick++ % 180u) == 0)
        PayloadLog("[esp-perf] draw=%.3fms build=%.3fms entities=%d",
                   (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)s_freq.QuadPart,
                   g_lastBuildMs, (int)items.size());
}
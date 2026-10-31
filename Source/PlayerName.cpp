#include "Include.h"
#include "PlayerName.h"

#include "PayloadControl.h"   // PayloadLog

#include <cstring>

// ============================================================
// PlayerName —— ESP 头顶名字缓存
// ------------------------------------------------------------
// 原实现寄生在 SpectatorList.cpp 里，观战者列表移除后独立成模块。
// 取名字走 IVEngineClient013 槽位 8（GetPlayerInfo），固定槽位
// 1s 节流缓存，渲染帧零分配。仅渲染线程访问，无跨线程共享。
// ============================================================

namespace
{
    // ---- VEngineClient013：调用槽位 8 GetPlayerInfo / 9 GetPlayerForUserID
    //      （两个槽位均为社区标准，L4D2 internal base 同款；其余槽位仅对齐）----
    class IVEngineClientX
    {
    public:
        virtual void* _vf0() = 0;   // 0  GetIntersectingSurfaces
        virtual void  _vf1() = 0;   // 1  GetLightForPoint
        virtual void* _vf2() = 0;   // 2  TraceLineMaterialAndLighting
        virtual void* _vf3() = 0;   // 3  ParseFile
        virtual bool  _vf4() = 0;   // 4  CopyLocalFile
        virtual void  _vf5() = 0;   // 5  GetScreenSize
        virtual void  _vf6() = 0;   // 6  ServerCmd
        virtual void  _vf7() = 0;   // 7  ClientCmd
        virtual bool  GetPlayerInfo(int entNum, void* pinfo) = 0;   // 8
        virtual int   GetPlayerForUserID(int userID) = 0;           // 9
        virtual void* _vf10() = 0;  // 10 TextMessageGet
        virtual bool  _vf11() = 0;  // 11 Con_IsVisible
        virtual int   GetLocalPlayerX() = 0;                        // 12
    };

    // ---- L4D2 player_info_t 布局（19:01 [name-raw] hex 终审实锤）----
    //   联机真人 slot=4 buffer：+0x00 = xuid_low（0x50C9D159，SteamID64 低
    //   32 位）/+0x04 标志/+0x08 = name[48]（"Yamada Anna" ASCII）/+0x2C
    //   steam_id 字符串/+0x50 xuid 重复。**没有 user_id 小整数字段**——
    //   GetPlayerForUserID 的键就是 xuid_low。开源 SDK 的布局（name@+0x10
    //   [256]/user_id@+0x110）是错的，已废弃。
    struct PlayerInfoX
    {
        int  xuid_low;          // 0x00 真人=SteamID64 低 32 位；bot=0
        char _pad04[4];         // 0x04 标志位
        char player_name[48];   // 0x08 ← 名字在这（实测 "Yamada Anna"）
        char _tail[512];        // 防御性尾垫
    };

    // SEH 包裹：函数内不得出现需要对象展开的 C++ 对象（C2712）
    bool SafeGetPlayerInfo(void* client, int idx, void* buf)
    {
        __try
        {
            return reinterpret_cast<IVEngineClientX*>(client)->GetPlayerInfo(idx, buf);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int SafeGetPlayerForUserID(void* client, int userID)
    {
        __try
        {
            return reinterpret_cast<IVEngineClientX*>(client)->GetPlayerForUserID(userID);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // ---- 槽位自校准 ----
    // GetPlayerForUserID 的社区标准槽位是 9，但 L4D2（2009 引擎分支）未逐一
    // 验证。用已知锚校准：GetLocalPlayer（槽 12，全版本一致）返回本地玩家
    // 实体索引；host 的 userID 从槽 1 拿。「哪个槽把 hostUserID 映射回
    // localEntIdx，哪个就是正确槽位」。候选 9~11（避开 SetViewAngles 危险区）。
    int SafeCallVtable0(void* client, int vslot, int arg)
    {
        __try
        {
            void** vt = *reinterpret_cast<void***>(client);
            using Fn = int (__thiscall*)(void*, int);
            return reinterpret_cast<Fn>(vt[vslot])(client, arg);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    int SafeGetLocalPlayer(void* client)
    {
        __try
        {
            return reinterpret_cast<IVEngineClientX*>(client)->GetLocalPlayerX();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    void* GetInterface(HMODULE module, const char* name)
    {
        if (!module)
            return nullptr;
        using CreateInterfaceFn = void* (*)(const char*, int*);
        const auto fn = reinterpret_cast<CreateInterfaceFn>(
            GetProcAddress(module, "CreateInterface"));
        return fn ? fn(name, nullptr) : nullptr;
    }

    // ---- VEngineClient013（懒获取并缓存）----
    void* EnsureEngineClient()
    {
        static void* s_client = nullptr;
        static bool  s_tried  = false;
        if (!s_tried)
        {
            s_tried = true;
            s_client = GetInterface(GetModuleHandleA("engine.dll"), "VEngineClient013");
            if (s_client)
                PayloadLog("[name] VEngineClient013 已获取：玩家名可用");
            else
                PayloadLog("[name] WARN: VEngineClient013 获取失败：玩家名显示为编号");
        }
        return s_client;
    }

    // ---- ESP 头顶名字缓存（仅渲染线程访问）----
    // 设计：固定槽位数组 + 单槽 1s 节流——渲染帧取名字零分配、零
    // GetPlayerInfo 调用；读取瞬时失败/空名字不覆盖旧值（显示更稳）。
    constexpr int   kNameCacheSlots = 34;     // 覆盖 player index 0..33
    constexpr DWORD kNameCacheTtlMs = 1000;   // 单槽刷新周期（ms）

    char  g_nameCache[kNameCacheSlots][32] = {};
    DWORD g_nameStamp[kNameCacheSlots] = {};
    DWORD g_mapStamp = 0;                 // 整表刷新节流（1s 一轮槽扫描）

    constexpr int kMaxPlayers = 16;

    void RefreshNameMap()
    {
        void* client = EnsureEngineClient();
        if (!client)
            return;

        // ---- GetPlayerForUserID 槽位自校准（一次性）----
        // 锚：GetLocalPlayer（槽 12）= 本地玩家实体索引；host(userID 来自槽 1)
        // 应被正确槽位映射回同一索引。候选 9~11。
        static int  s_pfuidSlot = 9;
        static bool s_calibrated = false;
        if (!s_calibrated)
        {
            s_calibrated = true;
            const int localIdx = SafeGetLocalPlayer(client);
            PlayerInfoX self{};
            // ⚠ 主菜单/大厅未进图时 GetLocalPlayer 返回垃圾大数（实测
            //   9426784）。必须限定在合理槽位区间内才能拿去当槽位用，
            //   否则下面的 GetPlayerInfo(垃圾槽位) + dump 会拖死线程。
            const bool localSane = (localIdx > 0 && localIdx < kNameCacheSlots);
            PayloadLog("[name-map] GetLocalPlayer=%d（%s），GetPlayerForUserID 槽位=%d",
                       localIdx, localSane ? "正常" : "异常值（未进对局？），跳过自校准",
                       s_pfuidSlot);

            if (localSane && SafeGetPlayerInfo(client, localIdx, &self) && self.xuid_low != 0)
            {
                for (int s = 9; s <= 11; ++s)
                {
                    if (SafeCallVtable0(client, s, self.xuid_low) == localIdx)
                    {
                        s_pfuidSlot = s;
                        break;
                    }
                }
            }

            // ---- 原始 buffer dump（一次性）：布局争议的终审证据 ----
            // GetLocalPlayer=本地实体索引 → 对该索引（及其相邻槽）调用
            // GetPlayerInfo，把写出的前 0x120 字节按 hex 打进日志。
            // 人工在其中找真名（"袋鼠"UTF-8 = E8 A2 8B E9 BC A0）与
            // user_id 的真实偏移 —— 不再依赖任何二手资料。
            //
            // ⚠ 2026-10-18 19:38 事故复盘：原实现 raw 只有 0x120 字节且在栈上，
            //   非法槽位时引擎照样 memset/写整个真实 player_info_t（大于
            //   0x120）→ 栈溢出把 localIdx 的溢出槽清零 → 循环条件
            //   slotTry >= localIdx-1 退化成 slotTry >= -1 → 从垃圾槽位
            //   （9426784）一路扫到 1，940 万次 GetPlayerInfo+日志把游戏
            //   卡死（"无法进入对局"）。三道防线：localSane 门 / raw 静态
            //   缓冲 0x240 / tries 硬上限 2。
            if (localSane)
            {
                static unsigned char raw[0x240] = {};   // 静态：引擎写穿也砸不到栈
                //       0x240 ≥ 真实 player_info_t（PlayerInfoX 同款 568B 规模）
                int tries = 0;
                for (int slotTry = localIdx;
                     slotTry >= 1 && slotTry >= localIdx - 1; --slotTry)
                {
                    if (++tries > 2)                     // 双保险：最多 2 个槽
                        break;
                    memset(raw, 0, sizeof(raw));
                    if (!SafeGetPlayerInfo(client, slotTry, raw))
                    {
                        PayloadLog("[name-raw] slot=%d GetPlayerInfo=false", slotTry);
                        continue;
                    }
                    for (int row = 0; row < 0x120; row += 48)
                    {
                        char hex[128]; int off = 0;
                        for (int k = 0; k < 48 && row + k < 0x120; ++k)
                            off += snprintf(hex + off, sizeof(hex) - off, "%02X",
                                            (unsigned)raw[row + k]);
                        PayloadLog("[name-raw] slot=%d +%03X: %s", slotTry, row, hex);
                    }
                    break;   // 只 dump 一个有效槽（相邻槽通常同布局）
                }
            }
        }

        // 全链诊断（每 30s 一次）：slot → ret/name/userID/entIdx，
        // 定位"真人名字为什么没映射上"断在哪一环
        static DWORD s_diagAt = 0;
        const bool diag = (s_diagAt == 0 || GetTickCount() - s_diagAt > 30000);
        if (diag) s_diagAt = GetTickCount();

        for (int slot = 1; slot <= kMaxPlayers; ++slot)
        {
            PlayerInfoX pi{};
            const bool ok = SafeGetPlayerInfo(client, slot, &pi);
            if (diag)
                PayloadLog("[name-map] slot=%d ret=%d name=\"%.24s\" xuid=%08X",
                           slot, ok ? 1 : 0, ok ? pi.player_name : "", pi.xuid_low);
            if (!ok)
                continue;                       // 空槽位：正常跳过

            pi.player_name[sizeof(pi.player_name) - 1] = '\0';
            if (pi.player_name[0] == '\0')
                continue;

            // 名字合法性过滤（布局错位时读到的是碎片，含控制字符）
            bool dirty = false;
            for (const char* q = pi.player_name; *q; ++q)
            {
                if ((unsigned char)*q < 0x20) { dirty = true; break; }
            }
            if (dirty)
                continue;

            // xuid_low==0 = bot（无 Steam ID）：直接用槽位号当实体索引试探
            int entIdx = -1;
            if (pi.xuid_low != 0)
                entIdx = SafeCallVtable0(client, s_pfuidSlot, pi.xuid_low);
            else
                entIdx = slot;                  // bot：槽位≈实体索引（L4D2 常规）
            if (entIdx <= 0 || entIdx >= kNameCacheSlots)
                entIdx = (slot <= 8) ? slot : -1;   // 反查失败再退一步：slot 即索引
            if (entIdx <= 0 || entIdx >= kNameCacheSlots)
                continue;

            strncpy_s(g_nameCache[entIdx], pi.player_name, _TRUNCATE);
            g_nameStamp[entIdx] = g_mapStamp;   // 标记本轮已刷新
            if (diag)
                PayloadLog("[name-map]   → 实体 %d = \"%s\"",
                           entIdx, g_nameCache[entIdx]);
        }
    }
} // namespace

const char* GetCachedPlayerName(int entityIndex)
{
    if (entityIndex < 0 || entityIndex >= kNameCacheSlots)
        return "";

    const DWORD now = GetTickCount();
    if (g_mapStamp == 0 || now - g_mapStamp >= kNameCacheTtlMs)
    {
        g_mapStamp = now;
        RefreshNameMap();
    }
    return g_nameCache[entityIndex];   // 无映射 = 空串（调用方走角色名兜底）
}

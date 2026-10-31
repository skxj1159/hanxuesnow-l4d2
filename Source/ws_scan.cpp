// ============================================================
// ws_scan.cpp -- 外部武器散布字段扫描器（不改游戏、不注入）
// ------------------------------------------------------------
// 目的：无扩散补偿依赖 weapon+kSpreadOff(0xD0C) 读 m_fSpread。
//      游戏内实测读到 0.00000 → 门控 spread<=0.0005 直接 return，
//      表现为"无扩散完全无效"。
//      本工具从外部读同一份内存，把武器对象里所有"像散布"的
//      浮点字段（0.0005 ~ 0.5）连续采样若干秒，找出真正随开火
//      变化的那个偏移，再用它替换 kSpreadOff。
//
// 用法：进游戏、用手里的枪开火（连发/霰弹最好），双击运行本 exe。
//      默认采样 10 秒。输出同时打印到屏幕与 ws_scan.log。
//
// 只读：OpenProcess(PROCESS_VM_READ|QUERY_INFORMATION)，不写一个字节。
// ============================================================

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

#include "Offsets.h"

// ---- 与 Aim.cpp 完全一致的取值口径（改这里必须同步） ----
static constexpr uintptr_t kActiveWeaponOff = 0x1084;   // m_hActiveWeapon（netvar 实测）
static constexpr uintptr_t kSpreadOff       = 0xD0C;    // 待验证的偏移
static constexpr uintptr_t kScanBegin       = 0x0000;
static constexpr uintptr_t kScanEnd         = 0x2800;
static constexpr int       kSleepMs         = 100;       // 采样间隔

static FILE* g_log = nullptr;
static void Out(const char* fmt, ...);

// ------------------------------------------------------------
// 模式 2：模块级字符串扫描
// 目的：确定 "CTerrorGun::FireBullet HorizSpread" 这个流名到底存在于
//      哪个模块 —— 字符串所在的模块就是真正执行 FireBullet 的模块。
//      client.dll 只有客户端预测副本；单机/本地主机的权威弹道在 server.dll。
//      （L4D2 联机时 server.dll 不加载，本地主机时才在进程内。）
// ------------------------------------------------------------
static bool ModuleStringScan(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    const char* kNeedles[] = {
        "CTerrorGun::FireBullet HorizSpread",
        "CTerrorGun::FireBullet VertSpread",
    };

    HANDLE p = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { CloseHandle(snap); Out("OpenProcess 失败\n"); return false; }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::vector<unsigned char> buf;
    std::vector<std::string> mods;

    if (Module32FirstW(snap, &me))
    {
        do
        {
            char nameA[MAX_MODULE_NAME32 + 1] = {};
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, nameA, MAX_MODULE_NAME32, nullptr, nullptr);
            mods.push_back(std::string(nameA) + "  0x" + std::to_string((unsigned long long)(uintptr_t)me.modBaseAddr)
                           + " size=0x" + std::to_string((unsigned long long)me.modBaseSize));
        } while (Module32NextW(snap, &me));
    }
    Out("已加载模块 %d 个：\n", (int)mods.size());
    for (const auto& m : mods) Out("  %s\n", m.c_str());

    Out("\n=== 字符串定位 ===\n");
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            char nameA[MAX_MODULE_NAME32 + 1] = {};
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, nameA, MAX_MODULE_NAME32, nullptr, nullptr);

            const size_t sz = me.modBaseSize;
            if (sz == 0 || sz > (size_t)256 << 20) continue;
            buf.resize(sz);
            SIZE_T got = 0;
            if (!ReadProcessMemory(p, me.modBaseAddr, buf.data(), sz, &got) || got != sz)
                continue;

            for (const char* needle : kNeedles)
            {
                const size_t nlen = strlen(needle);
                int hits = 0;
                for (size_t i = 0; i + nlen <= sz; ++i)
                {
                    if (buf[i] != (unsigned char)needle[0]) continue;
                    if (memcmp(&buf[i], needle, nlen) != 0) continue;
                    if (hits < 4)
                        Out("  %-16s +0x%06X  \"%s\"\n", nameA, (unsigned)(uintptr_t)me.modBaseAddr + (unsigned)i, needle);
                    ++hits;
                }
                if (hits > 4) Out("  %-16s ... 共 %d 处\n", nameA, hits);
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(p);
    CloseHandle(snap);
    return true;
}

static void Out(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

struct ModuleInfo { uintptr_t base = 0; size_t size = 0; };

static bool FindProcess(const wchar_t* exeName, DWORD* pid, ModuleInfo* client)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD found = 0;
    if (Process32FirstW(snap, &pe))
    {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) return false;
    *pid = found;

    // client.dll 基址
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, found);
    if (ms == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(ms, &me))
    {
        do {
            if (_wcsicmp(me.szModule, L"client.dll") == 0)
            {
                client->base = (uintptr_t)me.modBaseAddr;
                client->size = me.modBaseSize;
                break;
            }
        } while (Module32NextW(ms, &me));
    }
    CloseHandle(ms);
    return client->base != 0;
}

static bool Rpm(HANDLE h, uintptr_t addr, void* buf, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, buf, n, &got) && got == n;
}

template <typename T>
static T Rd(HANDLE h, uintptr_t addr, T def = T{})
{
    T v{};
    return Rpm(h, addr, &v, sizeof(T)) ? v : def;
}

int main(int argc, char** argv)
{
    int seconds = (argc > 1) ? atoi(argv[1]) : 30;
    if (seconds < 3) seconds = 3;

    g_log = fopen("ws_scan.log", "w");

    DWORD pid = 0;
    ModuleInfo cli;
    if (!FindProcess(L"left4dead2.exe", &pid, &cli))
    {
        Out("ERROR: 没找到 left4dead2.exe 进程，请先进入游戏。\n");
        if (g_log) fclose(g_log);
        return 2;
    }

    if (argc > 1 && _stricmp(argv[1], "mods") == 0)
    {
        Out("pid=%lu  client.dll base=0x%X\n", pid, (unsigned)cli.base);
        ModuleStringScan(pid);
        if (g_log) fclose(g_log);
        return 0;
    }

    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h)
    {
        Out("ERROR: OpenProcess 失败 err=%lu（请以管理员运行）\n", GetLastError());
        if (g_log) fclose(g_log);
        return 3;
    }

    Out("pid=%lu  client.dll base=0x%X size=0x%X\n", pid,
        (unsigned)cli.base, (unsigned)cli.size);

    const uintptr_t local = Rd<uintptr_t>(h, cli.base + Offsets::dwLocalPlayer);
    Out("localPlayer=0x%X\n", (unsigned)local);
    if (!local || local < 0x10000)
    {
        Out("ERROR: 还没进地图（本地玩家为空）。进图后再跑。\n");
        CloseHandle(h); if (g_log) fclose(g_log); return 4;
    }

    const uint32_t handle = Rd<uint32_t>(h, local + kActiveWeaponOff);
    const uint32_t idx    = handle & 0x1FFF;
    Out("m_hActiveWeapon=0x%X  idx=%u\n", handle, idx);
    if (!idx)
    {
        Out("ERROR: 手里没有武器（空手/爪）。拿枪后再跑。\n");
        CloseHandle(h); if (g_log) fclose(g_log); return 5;
    }

    const uintptr_t table  = cli.base + Offsets::dwEntityList;
    const uintptr_t weapon = Rd<uintptr_t>(h, table + (uintptr_t)idx * 0x10);
    Out("entityTable=0x%X  weapon=0x%X\n", (unsigned)table, (unsigned)weapon);
    if (!weapon || weapon < 0x10000)
    {
        Out("ERROR: 武器实体指针无效。\n");
        CloseHandle(h); if (g_log) fclose(g_log); return 6;
    }

    const uintptr_t vptr = Rd<uintptr_t>(h, weapon);
    const bool vptrOk = (vptr >= cli.base && vptr < cli.base + cli.size);
    Out("weapon vptr=0x%X  在 client.dll 内=%s\n", (unsigned)vptr, vptrOk ? "是" : "否（指针可疑）");
    Out("当前 0xD0C 读值 = %.6f\n\n", Rd<float>(h, weapon + kSpreadOff));

    // ---- 采样：边采边判，一旦抓到"会变的散布候选"提前结束 ----
    struct Rec { int cnt = 0; float mn = 1e9f, mx = -1e9f; int chg = 0; float last = 0; bool had = false; };
    const size_t span = kScanEnd - kScanBegin;
    std::vector<Rec> rec(span / 4);
    std::vector<unsigned char> blk(span, 0);

    Out("开始采样（最长 %d 秒）—— 请现在连续开火，本工具抓到数据会自动结束。\n", seconds);

    int okSamples = 0;
    const int maxSamples = seconds * 1000 / kSleepMs;
    Rec probeD0C;   // 单独跟踪 0xD0C 的变化情况

    for (int s = 0; s < maxSamples; ++s)
    {
        if (!Rpm(h, weapon + kScanBegin, blk.data(), blk.size())) break;
        ++okSamples;
        const float* f = reinterpret_cast<const float*>(blk.data());
        for (size_t i = 0; i < span / 4; ++i)
        {
            const float v = f[i];
            if (!std::isfinite(v)) { continue; }
            if (v <= 0.0005f || v >= 0.5f) { continue; }
            Rec& r = rec[i];
            if (!r.had) { r.had = true; r.last = v; r.mn = v; r.mx = v; r.cnt = 1; continue; }
            if (v != r.last) { ++r.chg; r.last = v; }
            ++r.cnt;
            if (v < r.mn) r.mn = v;
            if (v > r.mx) r.mx = v;
        }
        {
            const float v = Rd<float>(h, weapon + kSpreadOff);
            if (!probeD0C.had) { probeD0C.had = true; probeD0C.last = v; probeD0C.mn = v; probeD0C.mx = v; }
            else
            {
                if (v != probeD0C.last) { ++probeD0C.chg; probeD0C.last = v; }
                if (v < probeD0C.mn) probeD0C.mn = v;
                if (v > probeD0C.mx) probeD0C.mx = v;
            }
        }

        // 提前结束条件：至少 4 个字段发生 >=5 次变化（开火中的典型抖动）
        if ((s % 10) == 0 && s >= 20)
        {
            int hot = 0;
            for (const Rec& r : rec) if (r.had && r.chg >= 5 && r.cnt >= okSamples * 3 / 4) ++hot;
            Out("  ...%d/%d 采样，已抓到 %d 个变化字段\n", s + 1, maxSamples, hot);
            if (hot >= 4) break;
        }
        Sleep(kSleepMs);
    }

    Out("\n有效采样 %d 次\n", okSamples);
    Out("0xD0C（当前代码用的偏移）: min=%.6f max=%.6f 变化=%d\n",
        probeD0C.mn, probeD0C.mx, probeD0C.chg);

    // ---- 候选：出现率高且变化过 ----
    struct Cand { uintptr_t off; int cnt, chg; float mn, mx, last; };
    std::vector<Cand> cand;
    for (size_t i = 0; i < rec.size(); ++i)
    {
        const Rec& r = rec[i];
        if (!r.had) continue;
        if (r.cnt < okSamples * 3 / 4) continue;      // 至少 75% 的帧都在区间内
        cand.push_back({ kScanBegin + (uintptr_t)i * 4, r.cnt, r.chg, r.mn, r.mx, r.last });
    }
    std::sort(cand.begin(), cand.end(),
              [](const Cand& a, const Cand& b) { return a.chg > b.chg; });

    Out("\n=== 候选字段（区间 0.0005~0.5、命中率>=75%%，按变化次数降序）===\n");
    Out("%-10s %6s %6s %12s %12s %12s\n", "offset", "命中", "变化", "min", "max", "last");
    for (size_t i = 0; i < cand.size() && i < 40; ++i)
    {
        const Cand& c = cand[i];
        Out("+0x%04X  %6d %6d %12.6f %12.6f %12.6f%s\n",
            (unsigned)c.off, c.cnt, c.chg, c.mn, c.mx, c.last,
            (c.off == kSpreadOff) ? "   <== 当前代码用的偏移" : "");
    }
    if (cand.empty())
        Out("（无候选：开火期间没有 0.0005~0.5 的浮点在变化，可能本工具读错了实体或该枪无散布）\n");

    // ---- 附加：把 0xD0C 附近 32 个 float 全打出来，看它到底落在什么数据上 ----
    Out("\n=== 0xD0C 附近原始浮点（-32..+32 float）===\n");
    for (int d = -32; d < 32; d += 4)
    {
        const uintptr_t off = kSpreadOff + (uintptr_t)(d * 4);
        const float v = Rd<float>(h, weapon + off);
        Out("+0x%04X  %12.6f   (raw 0x%08X)%s\n", (unsigned)off, v,
            *reinterpret_cast<const uint32_t*>(&v), (d == 0) ? "  <== kSpreadOff" : "");
    }

    // ---- 附加：武器对象头 0x40 字节（vptr / 实体句柄自检） ----
    Out("\n=== 武器对象头（0x00 ~ 0x40）===\n");
    for (uintptr_t off = 0; off < 0x40; off += 0x10)
    {
        uint32_t a = Rd<uint32_t>(h, weapon + off);
        uint32_t b = Rd<uint32_t>(h, weapon + off + 4);
        uint32_t c = Rd<uint32_t>(h, weapon + off + 8);
        uint32_t e = Rd<uint32_t>(h, weapon + off + 12);
        Out("+0x%02X  %08X %08X %08X %08X\n", (unsigned)off, a, b, c, e);
    }

    CloseHandle(h);
    if (g_log) { fclose(g_log); }
    Out("\n完成。结果已写入 ws_scan.log\n");
    Out("按回车退出...");
    (void)getchar();
    return 0;
}

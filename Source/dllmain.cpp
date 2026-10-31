// ============================================================
// dllmain.cpp —— 注入载荷入口（仅内部/注入模式编译）
// ------------------------------------------------------------
// 注入 left4dead2.exe 后的启动流程：
//   1) DllMain 创建初始化线程（DllMain 内不能做重活）
//   2) 等 client.dll / engine.dll 就绪，设置全局量（进程内直取）
//   3) 启动实体扫描线程
//   4) Gui.Start：MinHook 挂钩游戏 DX9 EndScene → 游戏内渲染菜单/ESP
//   5) 解析游戏窗口后代挂 WndProc（菜单鼠标键盘交互）
// 卸载：菜单"卸载注入"按钮 → PayloadRequestUnload：停线程、恢复窗口过程、
//       保存 ESP 配置，再置 EndFlag 让库在下一渲染帧移除 hook 并关闭 ImGui；
//       DLL 保持常驻（避免在自身代码路径中 FreeLibrary 的自卸载竞态），
//       进程退出时由系统回收。
// ============================================================

#include "Include.h"
#include "PayloadControl.h"
#include "CrashLogger.h"    // [批34] VEH 崩溃现场记录
#include "Osimgui/Imgui/imgui_impl_win32.h"
#include "Hooks/Hooks.h"      // 批1 收尾：CreateMove 装配（Aimbot 入口）
#include "Features/Aimbot.h"  // [批47] 卸载前保存自瞄 + 拟人化配置
#include "Config/Config.h"    // [批47] 卸载前保存其余字段（combat/anti/crosshair）
#include "Config/CfgFile.h"   // [批65] 启动时读取可分享的 cfg
// [批56] 已移除 Protect.h —— 见下方注释

#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <mutex>

// OS-ImGui_Internal.cpp 保存的原始窗口过程（库停用时用它恢复窗口）
extern WNDPROC oWndProc;

namespace
{
    HMODULE             g_selfModule = nullptr;
    std::atomic<bool>   g_running{ true };
    std::atomic<bool>   g_unloading{ false };
    std::thread         g_entityThread;
    HWND                g_gameWnd = nullptr;
    WNDPROC             g_origWndProc = nullptr;
    std::mutex          g_logMutex;
    std::string         g_logPath;   // 懒初始化；按次打开追加，游戏运行中外部也可读
} // namespace

// ---- 公共日志（PayloadControl.h）：写入 payload.log，多线程安全 ----
// 用 _fsopen(_SH_DENYNO) 按次打开：不长期持有句柄，避免外部读日志时报
// "正由另一进程使用"；追加模式防覆盖，flockfile 保证多线程原子。
//
// [批67] 编码：本 DLL 按 /utf-8 编译，字符串字面量是 UTF-8 字节；
//   而中文 Windows 的记事本/多数查看器默认按 GBK(936) 解码 ——
//   直接写 UTF-8 就是满屏乱码（用户实测："log 全是乱码，我也看不懂"）。
//   现在写盘前统一转成系统 ANSI，中文编辑器可直接读。
//   转向失败（不是合法 UTF-8，比如本来就从 ANSI 来的路径串）→ 原样写，
//   至少不会比原来更糟。
static void Utf8ToAcp(const char* src, std::string& out)
{
    out.assign(src);
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         src, -1, nullptr, 0);
    if (wlen <= 0) return;                       // 非法 UTF-8：保留原字节

    std::wstring w((size_t)wlen, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, &w[0], wlen) <= 0)
        return;

    const int alen = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1,
                                         nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return;

    std::string a((size_t)alen, '\0');
    if (WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &a[0], alen,
                            nullptr, nullptr) <= 0)
        return;

    a.resize((size_t)alen - 1);                  // 去掉结尾 '\0'
    out.swap(a);
}

// ---- [批69] 发布版日志过滤 ----
// v1.0 要求：日志里只留【报错与关键事件】，诊断噪音全部丢掉。
//
// 为什么在写入端过滤，而不是删掉 124 处调用点：
//   ① 删调用点要动 17 个文件，正是"批量改写把文件截断"的高发场景；
//   ② 诊断信息对排查仍然有价值，删掉就再也拿不回来了；
//   ③ 集中一处判断，规则可读、可回归测试。
//
// 保留规则（任一命中即写）：
//   · 关键前缀：载荷生命周期 / netvar 自检 / 崩溃 / 配置保存
//   · 含错误语义：ERR / WARN / 失败 / 无法 / 异常 / 错误 / 拒绝 / 超时
// 其余（骨名解析、选靶流水、esp-perf、连发门控、CreateMove 耗时…）全部丢弃。
//
// 需要完整诊断时：在配置目录放一个空文件 `verbose.txt`（或设环境变量
//   HANXUESNOW_VERBOSE=1），本次运行就恢复全量日志。发布出去默认没有这个文件。
static bool LooksCritical(const char* s)
{
    static const char* kKeepPrefix[] = {
        "[payload]", "[sdk-netvar]", "[crash]", "[menu] 配置", "[config]",
        // 功能是否真的生效，属于用户最关心的状态，不该被精简掉。
        // 只打一次（首帧），不构成噪音。
        "[move] 去后坐力", "[move] 自动连点",
        // [批109] Movement 模块的诊断也必须可见。
        //   之前只有"去后坐力""自动连点"两条进了白名单，导致
        //   `[move] bhop=... 累计 jump=N` 这条【连跳唯一的状态出口】
        //   被静默丢弃 —— 连跳失效时日志里一个字的证据都没有，
        //   只能靠猜。这是第二次栽在同一个坑（批102 已修过一次，
        //   这次是新加的日志忘了进白名单）。
        "[move] 连发门控", "[move] 连跳", "[move] bhop",
        //  开关状态直接关系到"功能是否真的跑起来"，属于最关心的状态
        "[move] 功能开关",
        "[aim]",           // tick 链路 OK / 选靶统计 / 锁定信息
        "[bone-gate]",     // 取骨被哪道门挡住
        "[bone-refresh]",  // 取骨统计（成功/失败/curtime）
        "[bone-reset]",    // 地图切换重置
        "[move] punch",    // punch 字段已锁定 / 符号判定
        "[hooks]",         // hook 状态 / 异常拦截
        "[hb]",            // [批125] 崩溃心跳（每秒一条，最后一条 = 崩溃现场）
        "[HBX-ROOT]",      // [批126] hitbox 根因探针（曾被白名单吞掉）
        "[MIVT]",          // [批130] VModelInfoClient vtable 只读 dump
        "[NET-CAND]",      // [批130] entity→networkable 候选只读 dump
        "[crash]",         // 崩溃记录器
    };
    for (const char* p : kKeepPrefix)
    {
        const size_t n = strlen(p);
        if (strncmp(s, p, n) == 0) return true;
    }

    // 错误语义（大小写不敏感；中文直接子串匹配）
    //
    //  ⚠ [批72] 先剔除"计数字段"再判断：
    //    诊断行里常有 `取骨失败=0` / `几何失败=0` 这种【计数器】，
    //    它们含"失败"二字，会被当成报错漏进精简日志 —— 实测就是这样
    //    把 [bone-refresh]、[esp-draw] 刷进了 v1.0 的日志。
    //    把 `失败=0`/`错误=0` 这种"数值为 0"的片段抹掉再扫关键字，
    //    真正的报错（失败：xxx / 失败(原因)）仍然命中。
    char low[512] = {};
    size_t i = 0;
    for (; s[i] && i < sizeof(low) - 1; ++i)
        low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;

    static const char* kZeroCounters[] = {
        "失败=0", "错误=0", "fail=0", "error=0", "err=0",
    };
    for (const char* z : kZeroCounters)
    {
        char* p = nullptr;
        while ((p = strstr(low, z)) != nullptr)
            memset(p, ' ', strlen(z));      // 抹平长度，不改变后续偏移
    }

    static const char* kErrTokens[] = {
        "err", "warn", "fail", "[!]",
        "错误", "失败", "无法", "异常", "拒绝", "超时", "不可用",
    };
    for (const char* t : kErrTokens)
    {
        if (strstr(low, t)) return true;
    }
    return false;
}

// -1 = 未判定 / 0 = 精简（发布） / 1 = 全量
static int g_verboseLog = -1;

static bool VerboseEnabled()
{
    if (g_verboseLog >= 0) return g_verboseLog == 1;

    const bool env = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("HANXUESNOW_VERBOSE", v, sizeof(v)) > 0 &&
               v[0] == '1';
    }();

    bool flagFile = false;
    if (!g_logPath.empty())
    {
        const size_t sp = g_logPath.find_last_of("\\/");
        if (sp != std::string::npos)
            flagFile = GetFileAttributesA(
                (g_logPath.substr(0, sp) + "\\verbose.txt").c_str())
                != INVALID_FILE_ATTRIBUTES;
    }

    g_verboseLog = (env || flagFile) ? 1 : 0;
    return g_verboseLog == 1;
}

// ---- [批111·性能] 常驻日志句柄 ----
//  原来每条日志都要 _fsopen + fclose（成百上千次 open/close，且都在
//  游戏线程上）。现在惰性打开一次并常驻，路径变化或卸载时才关闭。
//  句柄状态提到文件作用域，好让 DllMain 的 DETACH 分支能关掉它 ——
//  放函数内 static 的话卸载钩子根本够不着。
namespace
{
    FILE*       g_logFile  = nullptr;
    std::string g_logFor;                    // g_logFile 对应的路径
}

void PayloadLogClose()
{
    // 由 DllMain(DLL_PROCESS_DETACH) 调用：loader 锁内不得加锁/分配，
    // 所以这里不做互斥保护 —— 进程正在卸载，其它线程不会再写日志。
    if (g_logFile)
    {
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logFor.clear();
}

void PayloadLog(const char* fmt, ...)
{
    // [批111·性能] 保持行为不变，但把每条日志的【打开/关闭文件】换成
    // 一个进程级常驻句柄。
    //   原来：每条日志 _fsopen + _lock_file + fwrite + _unlock + fclose，
    //   即一次系统调用级别的 open/close。而调用点遍布各模块（[aim]
    //   选靶统计 1s 一条、[bone-gate] / punch 探测 3s 节流…），
    //   一局游戏下来是成百上千次 open/close，且都在游戏线程上。
    //   现在：句柄惰性打开并常驻，只在路径变化或写失败时重建。
    //   行为等价 —— 仍是追加写、每条独占（g_logMutex 保护）、
    //   同样写进同一个文件。
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logPath.empty()) return;

    // [批162·性能] 过滤【前移】到 vsnprintf 之前：被丢弃的日志不再付
    //   格式化成本。所有调用点的 fmt 都以 "[tag]" 开头（前缀是字面量），
    //   LooksCritical 的前缀白名单/错误关键字直接作用在 fmt 上即可。
    //   已知微小行为差：错误关键字若只出现在【参数】里（格式串本身无），
    //   会被精简掉 —— 实际调用点的错误语义都在格式串里
    //   （[AIM-ERR] / 失败 / 不可用 …），不影响排障。
    if (!VerboseEnabled() && !LooksCritical(fmt))
        return;

    // [批150·发布版] 精简日志（logLevel=1）：只保留 启动 / 崩溃记录 / 心跳。
    //   发布版不需要排障用的逐帧统计（选靶/取骨/studiohdr 失败…），
    //   那些在长局里会刷到几十万行、既是噪音也是 I/O 负担。
    if (g_Config.aim.logLevel >= 1)
    {
        // [批167] 一次性/低频诊断也保留：它们一帧最多打一条，
        //   不构成噪音，但排障时是命脉（渲染钩子失败现场、兜底链死因）。
        static const char* kReleaseKeep[] = {
            "[payload]", "[crash]", "[hb]",
            "[AIM] 兜底链失败现场",   // [批159] 一次性
            "[imgui]",                // [批156] DX9 钩子初始化/失败（低频）
        };
        bool keep = false;
        for (const char* k : kReleaseKeep)
        {
            const size_t n = strlen(k);
            if (strncmp(fmt, k, n) == 0) { keep = true; break; }
        }
        if (!keep)
            return;
    }

    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    // [批111] 转换用的缓冲也复用：Utf8ToAcp 内部会构造 2 个 string +
    // 2 个 wstring，每条日志 4 次堆分配。改为 static 复用容量。
    static std::string line;
    Utf8ToAcp(buf, line);

    // 句柄惰性打开并常驻；路径变化时丢弃旧句柄（换日志目录）。
    if (g_logFile != nullptr && g_logFor != g_logPath)
    {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    if (g_logFile == nullptr)
    {
        g_logFile = _fsopen(g_logPath.c_str(), "a", _SH_DENYNO);
        if (!g_logFile) return;
        g_logFor = g_logPath;
    }
    FILE* f = g_logFile;
    _lock_file(f);
    fwrite(line.data(), 1, line.size(), f);
    fputc('\n', f);
    fflush(f);
    _unlock_file(f);
}

namespace
{

    std::string GetSelfDir()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(g_selfModule, buf, MAX_PATH);
        std::string path = buf;
        size_t pos = path.find_last_of("\\/");
        return (pos == std::string::npos) ? std::string(".") : path.substr(0, pos);
    }

    // 菜单打开时吞掉已被 ImGui 捕获的输入消息，避免操作菜单时游戏同时响应；
    // 其余情况一律原样转发（游戏 SDL 逻辑不受影响）。
    LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (menuOpen && ImGui::GetCurrentContext())
        {
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp);   // 始终喂 ImGui（更新光标/按键）

            ImGuiIO& io = ImGui::GetIO();
            const bool isMouseMsg =
                (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL;
            const bool isKeyMsg =
                msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_CHAR ||
                msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR;

            if (isMouseMsg && io.WantCaptureMouse)  return 1;
            if (isKeyMsg  && io.WantCaptureKeyboard) return 1;
        }
        return CallWindowProcA(g_origWndProc, hWnd, msg, wp, lp);
    }

    void EntityThreadProc()
    {
        // 降级职责：仅维护 localPlayer 指针 + 实体指针表（30ms 一轮）。
        // ESP/自瞄的实体数据已由 LiveEsp::LiveFrameBegin 在渲染帧内直读
        // （零 RPM、零快照年龄），本线程不再构建快照
        while (g_running)
        {
            LocalPlayerTableTick();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    // 本进程面积最大的可见顶层窗口（游戏主窗口；可避开库创建的小尺寸临时窗口）
    struct GameWndCtx { LONG bestArea; HWND bestWnd; };

    BOOL CALLBACK FindGameWndEnumProc(HWND hWnd, LPARAM lp)
    {
        GameWndCtx* ctx = (GameWndCtx*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hWnd))
            return TRUE;
        RECT rc{};
        if (!GetWindowRect(hWnd, &rc))
            return TRUE;
        LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
        if (area > ctx->bestArea)
        {
            ctx->bestArea = area;
            ctx->bestWnd = hWnd;
        }
        return TRUE;
    }

    HWND FindGameWindow()
    {
        GameWndCtx ctx{ 0, nullptr };
        EnumWindows(FindGameWndEnumProc, (LPARAM)&ctx);
        return ctx.bestWnd;
    }

    // ============================================================
    //  日志/配置落盘目录 —— 单 EXE 模式下必须落在【EXE 所在目录】
    // ------------------------------------------------------------
    //  单 EXE（v1.0）把 DLL 作为资源内嵌，运行时释放到 %TEMP%，再注入。
    //  若日志跟着 DLL 走，用户在 EXE 旁边找不到任何东西，日志全跑到
    //  临时目录里 —— 出问题时无法排查。所以这里的优先级是：
    //      ① 与 hanxuesnow.exe 同目录   ← 单 EXE 模式
    //      ② 与 DLL 同目录             ← 开发/传统 DLL 模式
    //      ③ %LOCALAPPDATA%\l4d2_payload ← 兜底（目录都不可写）
    //  判据：环境变量 HANXUESNOW_HOME 由单 EXE 启动器在注入前设置。
    // ============================================================
    std::string ResolveDataDir()
    {
        char env[MAX_PATH] = {};
        if (GetEnvironmentVariableA("HANXUESNOW_HOME", env, MAX_PATH) && env[0])
            return std::string(env);

        std::string selfDir = GetSelfDir();
        // DLL 在 %TEMP% 下 → 说明是单 EXE 释放出来的，改用 EXE 目录
        const std::string tempDir = [&] {
            char t[MAX_PATH] = {};
            GetTempPathA(MAX_PATH, t);
            return std::string(t);
        }();
        if (selfDir.size() >= tempDir.size() &&
            _strnicmp(selfDir.c_str(), tempDir.c_str(), tempDir.size()) == 0)
        {
            // 宿主 EXE 路径由启动器写入
            char host[MAX_PATH] = {};
            if (GetEnvironmentVariableA("HANXUESNOW_EXE", host, MAX_PATH) && host[0])
            {
                std::string hp = host;
                const size_t sp = hp.find_last_of("\\/");
                if (sp != std::string::npos)
                    return hp.substr(0, sp);
            }
        }
        return selfDir;
    }

    // [批125] 崩溃心跳线程（1s/条）。phase 由各危险阶段写入：
    //   10 = 正在取骨循环（SetupBones 直调），ent = 最后处理的实体。
    //   __fastfail 绕过 UEF，但日志每行 fflush —— 最后一条即现场。
    DWORD WINAPI HeartbeatThread(LPVOID)
    {
        for (;;)
        {
            // [批150] 发布版心跳降频到 5 秒：定位崩溃够用，又不刷屏。
            Sleep(g_Config.aim.logLevel >= 1 ? 5000 : 1000);
            PayloadLog("[hb] phase=%d ent=%d tick=%lu",
                       (int)g_hbPhase, (int)g_hbEnt,
                       (unsigned long)GetTickCount());
        }
        // [批169] 原 `return 0;` 在 for(;;) 之后不可达（/W4 C4702）——删除。
    }

    DWORD WINAPI InitThread(LPVOID)
    {
        // ============================================================
        //  [批151] 单例保护：进程里只能有一份本 DLL
        //  ------------------------------------------------------------
        //  为什么需要：单 EXE 模式会把载荷释放到 %TEMP%\hanxuesnow\ 再注入；
        //  如果同一目录里还放着 l4d2_payload.dll，又被别的加载器注入一次，
        //  进程里就会有两份载荷 → 两套 DX9 EndScene / CreateMove 钩子、
        //  两份骨骼缓存、两个配置写入 → 互相踩，表现为【卡死 / 画面异常】。
        //  用命名互斥体做单例：第二份直接放弃初始化。
        // ============================================================
        {
            HANDLE single = CreateMutexA(nullptr, TRUE,
                                          "Global\\HanXueSnow_Payload_Singleton");
            if (single == nullptr)
                return 0;                              // 创建失败：宁可不启动
            if (GetLastError() == ERROR_ALREADY_EXISTS)
            {
                PayloadLog("[payload] 进程内已存在另一份载荷 —— 本次放弃初始化"
                           "（避免双注入互相踩）");
                CloseHandle(single);
                return 0;
            }
            // 句柄故意不关：进程存活期间保持"已存在"状态
        }

        const std::string dataDir = ResolveDataDir();
        {
            std::lock_guard<std::mutex> lk(g_logMutex);
            g_logPath = dataDir + "\\payload.log";
        }

        // [批69] 日志轮转：单文件超过 1.5MB 就先备份成 payload.old.log。
        //   发布版日志已经精简，正常一局只有几十行；留这个上限是防
        //   "开了 verbose.txt 忘了关"或长期挂机把盘写满。
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (GetFileAttributesExA(g_logPath.c_str(), GetFileExInfoStandard, &fad))
            {
                const ULONGLONG size =
                    ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
                constexpr ULONGLONG kMax = 1500ULL * 1024ULL;
                if (size > kMax)
                {
                    const std::string old =
                        dataDir + "\\payload.old.log";
                    DeleteFileA(old.c_str());
                    MoveFileA(g_logPath.c_str(), old.c_str());
                }
            }
        }
        InstallCrashLogger();   // [批34] 崩溃现场记录器（独立头文件）
        PayloadLog("[payload] attached, pid=%lu（崩溃记录器已装）",
                   GetCurrentProcessId());

        // [批80] 构建时间戳：判断"游戏里跑的到底是哪一版"最直接的办法。
        //   实测吃过亏：连续出了三版，用户游戏是十分钟前启动的，
        //   加载的仍是旧 DLL，日志里看不到新特性却以为"修复无效"。
        //   有这行就能立刻确认是否需要重启游戏。
        PayloadLog("[payload] 寒山雪 v1.1.0（2026-10-24）");
        PayloadLog("[payload] 构建时间：%s %s", __DATE__, __TIME__);

        // 等游戏模块就绪（主菜单阶段 client.dll / engine.dll 均已加载）
        while (g_running)
        {
            if (GetModuleHandleA("client.dll") && GetModuleHandleA("engine.dll")) break;
            Sleep(500);
        }
        if (!g_running) return 0;

        // 进程内直接访问：句柄为自身，模块基址直接取
        hProcess = GetCurrentProcess();
        clientBase = (uintptr_t)GetModuleHandleA("client.dll");
        engineBase = (uintptr_t)GetModuleHandleA("engine.dll");
        PayloadLog("[payload] client.dll=0x%llX engine.dll=0x%llX",
            (unsigned long long)clientBase, (unsigned long long)engineBase);

        // ============================================================
        //  [批65] 启动加载配置 —— 之前【只加载了 ESP 的 json】
        // ------------------------------------------------------------
        //  实测问题：用户保存 cfg、甚至手改 cfg 之后重启游戏，菜单里
        //  还是旧值。因为 CfgFile::Load 只挂在菜单"导入选中"按钮上，
        //  启动路径从来没读过它 —— 等于 cfg 只是个导出文件。
        //
        //  现在的加载顺序（后者覆盖前者）：
        //    ① 机器格式：aim_config.ini（自瞄+拟人化）→ config.json（其余）
        //    ② 人格式  ：hanxuesnow.cfg
        //  cfg 放最后 = 它是"人看得见的那份"，以它为准；
        //  保存时两套一起写，所以不会出现两边打架。
        //
        //  ESP 的 esp_config.json 由 EspConfigTick 首次调用自行加载，
        //  不在这里重复（它的字段不在 cfg 范围内）。
        // ============================================================
        {
            Aimbot::AimConfigLoad();          // ① 自瞄 + 拟人化（ini）
            const bool okJson = ConfigMgr::Load();  // ① 其余字段（json）
            g_Config.combat.Clamp();          // ini 不走 Clamp，兜一次

            int applied = 0, ignored = 0;
            std::string err;
            const bool okCfg = CfgFile::Load(CfgFile::DefaultPath(),
                                             &applied, &ignored, &err);
            PayloadLog("[payload] 启动配置：json=%s cfg=%s（应用 %d 项，忽略 %d 项）%s%s",
                       okJson ? "OK" : "无文件",
                       okCfg ? "OK" : "无文件",
                       applied, ignored,
                       okCfg ? "" : " ",
                       okCfg ? "" : err.c_str());

            // [批77] 关键开关在启动时就落一行。之前"功能没开"和"功能无效"
            //   在日志上完全一样（都是零输出），排查只能靠猜。
            PayloadLog("[payload] 功能开关：自瞄=%d 静默=%d 连点=%d(键=0x%X) "
                       "去后坐力=%d",
                       g_Config.aim.enabled ? 1 : 0,
                       g_Config.aim.silent ? 1 : 0,
                       g_Config.combat.autoFire ? 1 : 0,
                       g_Config.combat.autoFireKey,
                       g_Config.combat.noRecoilVis ? 1 : 0);
        }

        // 实体扫描线程（10ms 刷新 localPlayer / 实体快照）
        g_entityThread = std::thread(EntityThreadProc);

        // 内部渲染：钩挂 DX9 EndScene，在游戏内绘制菜单/ESP
        Gui.Start(g_selfModule, DrawCallBack, OSImGui::DirectXType::DX9);
        PayloadLog("[payload] OS-ImGui internal started (DX9 EndScene hook)");

        // 自瞄批1收尾：ClientMode::CreateMove 装配（vtable[27]，签名法）
        // → Hooked_CreateMove 单行透传 Aimbot::CreateMove（Features 层）
        if (Hooks::Init())
            PayloadLog("[payload] aim hook chain ready");
        else
            PayloadLog("[payload] WARN: aim hook init failed (aim disabled)");

        // 等游戏窗口就绪：要求本地解析（面积最大可见窗口）与库解析结果一致，
        // 避免命中库在初始化阶段创建的临时小窗口
        for (int i = 0; i < 600 && g_running; ++i)
        {
            HWND wnd = FindGameWindow();
            if (wnd && wnd == Gui.Window.hWnd) { g_gameWnd = wnd; break; }
            Sleep(100);
        }

        if (g_gameWnd)
        {
            g_origWndProc = (WNDPROC)SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            oWndProc = g_origWndProc;   // 供库在停用时恢复窗口过程
            PayloadLog("[payload] game window 0x%llX hooked", (unsigned long long)g_gameWnd);
        }
        else
        {
            PayloadLog("[payload] WARN: game window not resolved, menu input may be unavailable");
        }

        // ============================================================
        //  [批56] 这里原本调用 Protect::Apply()（PEB 摘链 + 擦 PE 头），
        //  已【永久移除】。原因是实测代价远大于收益：
        //
        //    Windows Defender 判定为
        //      Behavior:Win32/DefenseEvasion.A!ml   （严重性 5）
        //    PEB 模块摘链 + 擦除 PE 头是教科书级的 rootkit 规避手法，
        //    Defender 的启发式模型一看就报，EXE 直接被拦、根本跑不起来。
        //
        //  而它对真正要防的东西（Lilac / SMAC）【完全无效】—— SourceMod
        //  没有能力枚举客户端模块，它们只分析我们发出去的 usercmd。
        //  也就是说：付出了"被杀毒拦"的代价，换来的保护对象根本不吃这套。
        //
        //  结论：要想降低暴露，正确方向是【少留特征】（日志脱敏、文件名
        //  中立），而不是做 evasive 行为。代码留在 Source/_disabled/
        //  备查，不要再接回来。
        // ============================================================

        PayloadLog("[payload] ready. Press INSERT to toggle menu.");
        // [批125] 崩溃心跳线程：__fastfail 不给 UEF 机会，但日志每行 fflush，
        //   最后一条 [hb] 就是崩溃现场（phase 表明阶段，ent 是最后实体）。
        HANDLE hb = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr);
        if (hb) CloseHandle(hb);
        return 0;
    }
} // namespace

// ---- 载荷公共接口（PayloadControl.h）----
void PayloadRequestUnload()
{
    if (g_unloading.exchange(true)) return;
    PayloadLog("[payload] unload requested");

    g_running = false;
    if (g_entityThread.joinable())
        g_entityThread.join();

    Hooks::Shutdown();   // 批1：拆 CreateMove 钩子（先于库自身的 MH 清理）

    if (g_gameWnd && g_origWndProc)
        SetWindowLongPtrA(g_gameWnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);

    EspConfigSaveNow();   // 卸载前同步落盘，避免丢失尚未写入的 ESP 设置
    Aimbot::AimConfigSave();                  // [批47] 自瞄 + 拟人化
    ConfigMgr::SaveNow();                     // [批47] 其余全部字段（combat/anti/…）
                                            // 旧版只存了 ESP 一路，其余全部丢失

    // 触发库停用流程：下一渲染帧移除 DX9 hook、关闭 ImGui、恢复窗口过程
    Gui.EndFlag = true;
    PayloadLog("[payload] unload sequence fired");
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinst);
        g_selfModule = hinst;
        HANDLE h = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // [批111] 关闭 PayloadLog 的常驻日志句柄。
        //   改成常驻句柄后如果不关，游戏退出时缓冲区可能来不及落盘
        //   （尾部日志丢失），DLL 被卸载后句柄也悬空。
        //   注意：此时 loader 锁可能已被持有，所以这里只做一次
        //   fclose（不分配内存、不加锁），不做任何复杂操作 ——
        //   这正是 DllMain 里唯一该做的事。
        PayloadLogClose();
    }
    return TRUE;
}

// ============================================================
//  packer/launcher.cpp —— 寒山雪 单 EXE 启动器
// ------------------------------------------------------------
//  用户手上只有【一个 hanxuesnow.exe】，不需要额外 DLL。
//
//  【工作流程】
//    ① 从 EXE 资源段取 RT_RCDATA / #101 里的载荷 DLL
//    ② 写到 %TEMP%\hanxuesnow\payload*.dll
//    ③ 在目标进程内设置 HANXUESNOW_HOME（决定配置/日志落盘位置）
//    ④ 找 left4dead2.exe → CreateRemoteThread(LoadLibraryA) 注入
//    ⑤ 注入成功后删除临时载荷
//
//  【为什么注入完就能删】
//    LoadLibraryA 之后 OS 已把 DLL 映射进目标进程，往后读写走内存映射
//    的文件视图，不依赖原始路径。删除文件不影响已加载的模块。
//
//  【构建】见 pack_exe.sh：编 DLL → rc.exe 出 .res → cvtres.exe 转 .obj
//    → 链接。cvtres 那一步不能省，否则链接成功但 EXE 里零资源。
// ============================================================

// 注意：不要定义 WIN32_LEAN_AND_MEAN —— 它会把 TlHelp32 的
// PROCESSENTRY32/Process32First 从 windows.h 里摘掉，而 TlHelp32.h 在
// 默认 _WIN32_WINNT 下不会重新引入，结果全部报"未声明"。
#include <Windows.h>
#include <TlHelp32.h>
#include <string>
#include <cstdio>
#include <cstring>
#include <vector>

// 载荷资源 ID（与 packer/payload.rc 里的 "101 10" 对应）
constexpr int kPayloadResId = 101;

namespace
{
    // ============================================================
    //  小工具
    // ============================================================

    // 本 EXE 所在目录。返回 ANSI(本机代码页) 字节，可直接给 A 版 API。
    std::string ExeDir()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        const size_t pos = p.find_last_of("\\/");
        return (pos == std::string::npos) ? std::string(".") : p.substr(0, pos);
    }

    // ANSI → UTF-8，仅用于【显示】。
    //
    // 为什么需要：路径都来自 GetModuleFileNameA，是 GBK 字节；而本文件按
    // -utf-8 编译，其余字面量是 UTF-8。控制台已被切到 CP_UTF8，直接打印
    // GBK 路径就是一串 ◆。环境变量必须保持 GBK（ANSI 环境块），
    // 所以只转换显示副本。
    std::string NarrowToUtf8(const std::string& ansi)
    {
        if (ansi.empty()) return ansi;
        const int wlen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, nullptr, 0);
        if (wlen <= 0) return ansi;
        std::wstring w(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, &w[0], wlen);

        const int u8len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) return ansi;
        std::string u8(static_cast<size_t>(u8len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &u8[0], u8len, nullptr, nullptr);
        if (!u8.empty() && u8.back() == '\0') u8.pop_back();
        return u8;
    }

    // 失败退出：打印原因 + 可选的清理 + 停住窗口。
    //
    // 停窗口是必须的：双击运行时窗口会随 main 返回立刻关闭，用户只看到
    // "闪一下就没了"，完全不知道发生了什么。
    int FailAndPause(const std::string& reason, const std::string& cleanupPath = {})
    {
        printf("\n[!] %s\n", reason.c_str());
        if (!cleanupPath.empty())
            DeleteFileA(cleanupPath.c_str());
        printf("[i] 按任意键关闭...\n");
        system("pause >nul");
        return 1;
    }

    // ============================================================
    //  载荷释放
    // ============================================================

    // 删掉目录里上一次留下的旧副本（超过 1 小时的），只保留本次要用的那个。
    // 加时间门槛是怕误删游戏正在映射的那份。
    void CleanupStalePayloads(const std::string& dir, const std::string& keepPath)
    {
        WIN32_FIND_DATAA fd{};
        const std::string pattern = dir + "\\payload*.dll";
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) return;

        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        ULARGE_INTEGER now{};
        now.LowPart  = nowFt.dwLowDateTime;
        now.HighPart = nowFt.dwHighDateTime;

        constexpr unsigned long long kOneHour = 3600ULL * 10000000ULL;   // 100ns 单位
        int removed = 0;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string full = dir + "\\" + fd.cFileName;
            if (_stricmp(full.c_str(), keepPath.c_str()) == 0) continue;

            ULARGE_INTEGER mt{};
            mt.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
            mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;

            if (now.QuadPart > mt.QuadPart + kOneHour && DeleteFileA(full.c_str()))
                ++removed;
        } while (FindNextFileA(hFind, &fd));

        FindClose(hFind);
        if (removed > 0)
            printf("[*] 清理了 %d 个过期载荷副本\n", removed);
    }

    // 取出载荷并写盘。成功时 outPath 是实际写入的文件。
    bool ExtractPayload(std::string& outPath)
    {
        HMODULE self = GetModuleHandleA(nullptr);
        // ⚠ FindResourceA 参数顺序是【name, type】，不是 (type, name)。
        HRSRC hRes = FindResourceA(self, MAKEINTRESOURCE(kPayloadResId), RT_RCDATA);
        if (!hRes)
        {
            printf("[!] 资源里找不到载荷 DLL —— 打包不完整\n");
            printf("    (期望 type=RT_RCDATA(10) / name=#%d)\n", kPayloadResId);
            return false;
        }
        const HGLOBAL hGlob = LoadResource(self, hRes);
        if (!hGlob) { printf("[!] LoadResource 失败\n"); return false; }

        const DWORD sz = SizeofResource(self, hRes);
        const void* p  = LockResource(hGlob);
        if (!p || sz == 0) { printf("[!] 载荷资源为空\n"); return false; }

        // 释放目录：%TEMP%\hanxuesnow
        //  ⚠ 注释里的路径绝不能以反斜杠收尾 —— 那会被当成【行继续符】，
        //   把下一行整行吞进注释，然后满屏"未声明的标识符"。
        char tbuf[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tbuf);
        const std::string dir = std::string(tbuf) + "hanxuesnow";
        CreateDirectoryA(dir.c_str(), nullptr);

        // 先试固定名；被占用就退回带进程序号的名字。
        //
        // 为什么不能只用固定名：旧载荷可能正被游戏映射着（用户换了新 EXE，
        // 但上次注入的 DLL 还在游戏进程里）—— 此时 fopen 写入必然失败，
        // 注入直接中止。换名是唯一不依赖"文件能否覆盖"的出路。
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            char nameBuf[64] = {};
            if (attempt == 0)
                strcpy_s(nameBuf, "payload.dll");
            else
                sprintf_s(nameBuf, "payload_%lu_%d.dll",
                          (unsigned long)GetCurrentProcessId(), attempt);

            const std::string path = dir + "\\" + nameBuf;

            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
                continue;                       // 被占用 → 换名重试

            const size_t written = fwrite(p, 1, sz, f);
            fclose(f);

            if (written == sz)
            {
                outPath = path;
                printf("[*] 载荷已释放：%s（%u 字节）\n",
                       NarrowToUtf8(path).c_str(), sz);
                CleanupStalePayloads(dir, outPath);
                return true;
            }
            DeleteFileA(path.c_str());          // 写了一半：别留下坏文件
        }

        printf("[!] 无法写入临时载荷（换名重试 8 次仍失败）\n");
        printf("    目录：%s\n", NarrowToUtf8(dir).c_str());
        return false;
    }

    // ============================================================
    //  目标进程
    // ============================================================
    DWORD FindGamePid()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        // ★ 必须用【无后缀】的 PROCESSENTRY32 / Process32First。
        //   这个 SDK 里没有 PROCESSENTRY32A / Process32FirstA —— 只有
        //   无后缀版(ANSI) 与 _W 版，UNICODE 下由宏重定向。写 A 版必报未声明。
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(PROCESSENTRY32);

        DWORD pid = 0;
        if (Process32First(snap, &pe))
        {
            do {
                if (_stricmp(pe.szExeFile, "left4dead2.exe") == 0)
                {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    // 目标进程句柄。析构自动关闭，任何提前 return 都不会漏句柄
    // —— 之前 5 个失败分支各写一遍 CloseHandle，漏一个就是泄漏。
    struct Target
    {
        HANDLE proc = nullptr;

        Target() = default;
        Target(const Target&) = delete;
        Target& operator=(const Target&) = delete;

        ~Target()
        {
            if (proc) CloseHandle(proc);
        }

        bool Open(DWORD pid)
        {
            proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                   PROCESS_VM_READ,
                               FALSE, pid);
            return proc != nullptr;
        }
    };

    // 建远程线程：先走常规 API，失败再走 NtCreateThreadEx 直连。
    // 定义在下方"注入"段，这里先声明 —— 设环境变量也要用到它，
    // 否则 CreateRemoteThread 被拦时主注入走了直连、环境变量却送不到。
    HANDLE CreateRemoteThreadAny(HANDLE proc, LPVOID start, LPVOID arg);

    // ============================================================
    //  [批58 修复] 跨进程地址解析
    // ------------------------------------------------------------
    //  ⚠⚠⚠ 这是【批57 引入的闪退根因】，改任何注入代码前先读完这段 ⚠⚠⚠
    //
    //  错误做法（批57 的写法）：
    //      HMODULE k32 = GetModuleHandleA("kernel32.dll");
    //      FARPROC fn  = GetProcAddress(k32, "SetEnvironmentVariableA");
    //      把 fn 塞进远端 stub 直接 call
    //
    //  为什么必死：
    //    GetModuleHandleA / GetProcAddress 返回的是【本进程】的虚拟地址。
    //    Windows 10+ 开了 ASLR，kernel32 在【目标进程】里的基址通常
    //    和本进程【不一样】。把本进程地址搬过去 call，落到的是目标里
    //    一段毫不相干的数据/代码 → 当场跳飞。
    //
    //  症状链（与实测日志逐条吻合）：
    //    ① 远端 stub 线程跑起来就崩，GetExitCodeThread 拿到异常码 != 1
    //       → RemoteSetEnv 返回 false，且【不打印任何错误】
    //       → 屏幕上只出现"环境变量未送达"（静默）
    //    ② 目标进程已在崩溃流程中/已死
    //       → 后续 CreateRemoteThread 报 5
    //       → NtCreateThreadEx 报 0xC000010A(INVALID_IMAGE_FORMAT)
    //    ③ 主注入根本没机会执行 → 用户看到的全是"创建线程失败"
    //
    //  正确做法：在【目标进程里】自己解析。
    //    Module32First/Next 枚举的是【目标】的模块表 → modBaseAddr 是
    //    目标里的 kernel32 基址；再把本进程的 RVA（偏移量）加上去，
    //    得到的目标地址必然正确 —— RVA 是模块内相对偏移，与 ASLR 无关。
    // ============================================================

    // ============================================================
    //  [批154] 目标模块基址 —— 失败必须说清原因，且能自救
    // ------------------------------------------------------------
    //  旧实现失败时静默 return 0，上层只能打出"找不到 kernel32 基址"，
    //  完全分不清是【权限不足】还是【进程刚起模块表未就绪】。
    //  实测（朋友机器）：游戏/Steam 提权运行而启动器没提权 →
    //  CreateToolhelp32Snapshot 直接 ACCESS_DENIED → 注入中止。
    //  现在：① 重试 5 次（进程冷启动常见 ERROR_BAD_LENGTH）
    //        ② 打印真实错误码并给出可执行建议
    //        ③ 系统 DLL 兜底：同一开机周期内各进程基址相同（Force ASLR 除外）
    // ============================================================
    // ============================================================
    //  [批155] 游戏就绪判定：client.dll 已加载且尺寸稳定
    // ------------------------------------------------------------
    //  用户需求：不要在"游戏外面"注入。游戏刚启动、还在加载画面时，
    //  模块表未就绪（ERROR_BAD_LENGTH）/ 仍在增长，旧版在这里直接失败
    //  还看不出原因。现在：client.dll 出现且两次采样尺寸一致才算就绪。
    // ============================================================
    bool ClientDllLoaded(DWORD pid, DWORD* outSize)
    {
        if (outSize) *outSize = 0;
        HANDLE snap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE)
            return false;                      // 快照失败 = 还没就绪
        MODULEENTRY32 me{};
        me.dwSize = sizeof(MODULEENTRY32);
        bool found = false;
        DWORD size = 0;
        if (Module32First(snap, &me))
        {
            do {
                if (_stricmp(me.szModule, "client.dll") == 0)
                {
                    found = true;
                    size = me.modBaseSize;
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        if (outSize) *outSize = size;
        return found;
    }

    // 游戏是否"可以注入"：client.dll 已加载且 0.5 秒内尺寸不再变化。
    //  返回 false 时把原因写进 why（供提示语使用）。
    bool GameReadyForInject(DWORD pid, std::string& why)
    {
        DWORD sz1 = 0;
        if (!ClientDllLoaded(pid, &sz1) || sz1 == 0)
        {
            why = "尚未检测到 client.dll（游戏还在启动/加载中）";
            return false;
        }
        Sleep(500);
        DWORD sz2 = 0;
        if (!ClientDllLoaded(pid, &sz2) || sz2 != sz1)
        {
            why = "游戏模块仍在加载（client.dll 尺寸未稳定）";
            return false;
        }
        return true;
    }

    bool IsSystemDll(const char* modName)
    {
        return _stricmp(modName, "kernel32.dll") == 0 ||
               _stricmp(modName, "kernelbase.dll") == 0 ||
               _stricmp(modName, "ntdll.dll") == 0;
    }

    DWORD RemoteModuleBase(DWORD pid, const char* modName)
    {
        MODULEENTRY32 me{};
        me.dwSize = sizeof(MODULEENTRY32);
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            HANDLE snap = CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap == INVALID_HANDLE_VALUE)
            {
                const DWORD err = GetLastError();
                if (err == ERROR_BAD_LENGTH && attempt < 4)
                {
                    Sleep(100);          // 进程刚起：模块表还没就绪
                    continue;
                }
                printf("[-] 枚举目标模块表失败（%s）：错误码 %lu\n",
                       err == ERROR_ACCESS_DENIED
                           ? "拒绝访问 —— 请右键选择“以管理员身份运行”"
                           : "Toolhelp 快照失败",
                       (unsigned long)err);
                break;
            }
            DWORD base = 0;
            if (Module32First(snap, &me))
            {
                do {
                    if (_stricmp(me.szModule, modName) == 0)
                    {
                        base = (DWORD)(uintptr_t)me.modBaseAddr;
                        break;
                    }
                } while (Module32Next(snap, &me));
            }
            CloseHandle(snap);
            if (base)
                return base;
            if (attempt < 4)
                Sleep(100);
        }

        // 兜底：系统 DLL 在同一开机周期内各进程基址相同（Force ASLR 除外）。
        //   只对 kernel32/kernelbase/ntdll 生效 —— 游戏模块（client.dll）绝不能用。
        if (IsSystemDll(modName))
        {
            HMODULE localMod = GetModuleHandleA(modName);
            if (localMod)
            {
                printf("[!] 快照不可用，改用本进程 %s 基址 0x%X"
                       "（同一开机周期内系统 DLL 基址相同）\n",
                       modName, (unsigned)(uintptr_t)localMod);
                return (DWORD)(uintptr_t)localMod;
            }
        }
        return 0;
    }

    // 在【目标进程】里定位一个 API 的真实地址。
    //   localFnAddr : 本进程 GetProcAddress 的结果（只用来算 RVA）
    //   remoteModBase: 目标进程里该模块的基址
    // 返回值可以直接写进远端 stub。
    DWORD RemapToRemote(HANDLE proc, DWORD pid, const char* modName,
                        FARPROC localFnAddr, const char* fnName)
    {
        if (!localFnAddr) return 0;

        HMODULE localMod = GetModuleHandleA(modName);
        if (!localMod) return 0;

        const DWORD remoteBase = RemoteModuleBase(pid, modName);
        if (!remoteBase)
        {
            printf("[!] 读不到目标进程 %s 基址，无法定位 %s\n", modName, fnName);
            return 0;
        }

        // RVA = 本进程函数地址 - 本进程模块基址（模块内偏移，与 ASLR 无关）
        const DWORD rva = (DWORD)(uintptr_t)localFnAddr - (DWORD)(uintptr_t)localMod;
        const DWORD remoteFn = remoteBase + rva;

        printf("[*] 解析 %s：本地=0x%08lX 目标=0x%08lX（%s 基址 0x%08lX + RVA 0x%X）\n",
               fnName,
               (unsigned long)(uintptr_t)localFnAddr,
               (unsigned long)remoteFn,
               modName, (unsigned long)remoteBase, (unsigned)rva);
        (void)proc;
        return remoteFn;
    }

    // 组合版：给"本进程已有的函数指针"，返回"目标进程里的对应地址"
    DWORD RemapApi(HANDLE proc, DWORD pid, const char* modName,
                   FARPROC localFnAddr, const char* fnName)
    {
        return RemapToRemote(proc, pid, modName, localFnAddr, fnName);
    }

    // ============================================================
    //  在【目标进程】里设置环境变量
    // ------------------------------------------------------------
    //  为什么必须远程做：环境变量是 per-process 的。注入后 DLL 跑在
    //  left4dead2.exe 里，读的是【游戏自己】的环境块。启动器里
    //  SetEnvironmentVariableA 设一百遍也传不过去。
    //
    //  CreateRemoteThread 只能传 1 个参数，而 SetEnvironmentVariableA
    //  要 2 个 —— 所以在远端现编一小段 stub，两个指针都是绝对地址、
    //  拿到基址后回填进 push imm32，不依赖参数。
    //
    //  stub（x86，23 字节）：
    //      push <value>            ; lpValue
    //      push <name>             ; lpName
    //      mov eax, <fn>           ; ⚠ 必须走寄存器
    //      call eax
    //      mov eax,1
    //      ret                     ; ⚠ 裸 ret，见下方
    //
    //  ⚠ 不能用 `call dword ptr [fn]`（FF 15）：那是【间接】调用，CPU 会
    //    去 fn 地址读 4 字节当跳转目标，而 GetProcAddress 返回的是函数
    //    入口本身 —— 那 4 字节是函数代码，不是指针。照写会跳到垃圾地址，
    //    【目标进程当场崩】。
    //
    //  ⚠⚠【批58 修正】调用约定 —— 这才是昨天"退出码 100"的真凶
    //    SetEnvironmentVariableA 是 WINAPI = __stdcall，**被调用方自己
    //    清栈**（内部 ret 8）。而早先的 stub 在 call 之后又写了
    //    `add esp,8`，等于多弹 8 字节：
    //        call eax      ; ESP 已被 callee 还原（+8）
    //        add esp, 8    ; ← 多余！ESP 再 -8，跳过返回地址
    //        mov eax, 1
    //        ret           ; 从错误位置取返回地址 → 跳飞
    //    症状极具迷惑性：线程"正常退出"、退出码是个普通数字（实测 100），
    //    不报任何异常，但栈已被破坏，随后游戏进入不可用状态 ——
    //    下一个 CreateRemoteThread 立刻错误 5，NtCreateThreadEx 报
    //    0xC000010A。看起来像"权限/杀软问题"，真凶却在几十行之前。
    //
    //    现在收尾用【裸 ret】。栈账（x86/32，CreateRemoteThread 入口）：
    //        push value      ESP -= 4
    //        push name       ESP -= 4
    //        call eax        ESP -= 4（压返回地址）；callee stdcall 清 8
    //        mov eax, 1      不变
    //        ret             ESP += 4（弹返回地址）
    //        净变化 -4-4-4+8+4 = 0  →  ESP 回到入口值，平衡。
    //
    //    ⚠ 顺带记一笔：`ret 8` 在这里【也是错的】。callee 已经清过栈，
    //    再让 ret 多弹 8 就等于凭空多吃 8 字节 —— 与 `add esp,8` 是同一类
    //    错误，只是方向相反。凡是 callee 遵循 stdcall，收尾就只能是裸 ret。
    //
    //  ⚠ 教训：verify_stub.py 当时把这段判成 PASS —— 因为它只逐条比对
    //    "实际指令 == 我手写的期望表"，而【期望表本身就是错的】。
    //    这类验证器保护的不是代码，是我的错误。已重写为：正确答案来自
    //    独立的 ABI 事实（MSDN 的 stdcall 契约），并用栈模拟验平衡。
    // ============================================================
    constexpr size_t kStubLen = 23;

    bool RemoteSetEnv(HANDLE h, DWORD pid, const char* name, const std::string& value)
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        FARPROC localFn = k32 ? GetProcAddress(k32, "SetEnvironmentVariableA") : nullptr;
        if (!localFn) { printf("[!] 找不到 SetEnvironmentVariableA\n"); return false; }

        // [批58] 必须换成【目标进程】里的地址，直接用 localFn 会打死游戏。
        const DWORD fnAddr = RemapApi(h, pid, "kernel32.dll", localFn,
                                       "SetEnvironmentVariableA");
        if (!fnAddr) { printf("[!] 目标进程地址解析失败\n"); return false; }

        const size_t nameLen = strlen(name) + 1;
        const size_t valLen  = value.size() + 1;
        const size_t nameOff = kStubLen;
        const size_t valOff  = nameOff + nameLen;
        const size_t total   = valOff + valLen;

        unsigned char* buf = (unsigned char*)VirtualAllocEx(
            h, nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!buf)
        {
            printf("[!] 目标进程分配 stub 失败（错误 %lu）\n", GetLastError());
            return false;
        }

        const DWORD nameAddr = (DWORD)(uintptr_t)(buf + nameOff);
        const DWORD valAddr  = (DWORD)(uintptr_t)(buf + valOff);

        // [批58] 修正后的 stub —— 23 字节。
        //   关键：没有 `add esp,8`。stdcall 的 callee 自己清栈，
        //   再手动清一次就把返回地址弹掉了（昨天退出码 100 的根因）。
        //   收尾用裸 `ret`，栈账恰好归零（见上方注释）。
        unsigned char stub[kStubLen] = {
            0x68, 0, 0, 0, 0,                 // +0  push <value>
            0x68, 0, 0, 0, 0,                 // +5  push <name>
            0xB8, 0, 0, 0, 0,                 // +10 mov eax, <fn>
            0xFF, 0xD0,                       // +15 call eax
            0xB8, 0x01, 0x00, 0x00, 0x00,     // +17 mov eax, 1
            0xC3                              // +22 ret
        };
        memcpy(stub + 1,  &valAddr,  4);       // push value 的 imm32
        memcpy(stub + 6,  &nameAddr, 4);       // push name  的 imm32
        memcpy(stub + 11, &fnAddr,   4);       // mov eax   的 imm32

        SIZE_T w = 0;
        const bool ok = WriteProcessMemory(h, buf, stub, kStubLen, &w)
                     && WriteProcessMemory(h, buf + nameOff, name, nameLen, &w)
                     && WriteProcessMemory(h, buf + valOff, value.c_str(), valLen, &w);
        if (!ok)
        {
            printf("[!] 写入远端 stub 失败（错误 %lu）\n", GetLastError());
            VirtualFreeEx(h, buf, 0, MEM_RELEASE);
            return false;
        }

        HANDLE th = CreateRemoteThreadAny(h, (LPVOID)buf, nullptr);
        if (!th)
        {
            printf("[!] 远程 SetEnvironmentVariable 线程失败（错误 %lu）\n",
                   GetLastError());
            VirtualFreeEx(h, buf, 0, MEM_RELEASE);
            return false;
        }

        WaitForSingleObject(th, 5000);
        // ⚠ lpExitCode 传 nullptr 在 MSDN 上写着可选，实际会无条件解引用
        //   —— 在这个位置触发过 0xC0000005，把整个启动器带崩。必须给变量。
        DWORD rc = 0;
        GetExitCodeThread(th, &rc);
        CloseHandle(th);
        VirtualFreeEx(h, buf, 0, MEM_RELEASE);

        // [批58] 远端线程的退出码是唯一能证明"stub 有没有跑崩"的证据。
        //   0xC0000005 / 0xC000001D 等高位值 = 远端异常（跑崩了）。
        //   之前这里直接 return rc == 1，失败时【一个字都不打印】，
        //   导致"目标已死但看起来像权限问题"，排查绕了大弯。
        if (rc != 1)
        {
            if (rc >= 0xC0000000u)
                printf("[!] 远端 stub 线程异常退出，NTSTATUS=0x%08lX（目标进程可能已崩）\n",
                       (unsigned long)rc);
            else
                printf("[!] 远端 stub 线程退出码 %lu（期望 1）\n", (unsigned long)rc);
            return false;
        }
        return true;
    }

    // ============================================================
    //  注入
    // ============================================================

    // CreateRemoteThread 被拦时的直连方案。
    //
    // 为什么要这一层：杀毒软件常见的做法是在【用户层】hook
    // CreateRemoteThread —— 拿到参数后直接返回 FALSE + SetLastError(5)。
    // 这种情况下 OpenProcess / VirtualAllocEx / WriteProcessMemory 全部
    // 成功，只有创建线程被拒，看起来像"权限不足"，其实是软件层拦截。
    // NtCreateThreadEx 直接进内核，绕过用户层 hook。
    //
    // ⚠ 如果杀毒是在【内核回调】层拦（PsSetCreateThreadNotifyRoutine 之类），
    //   这里同样会失败并返回 STATUS_ACCESS_DENIED(0xC0000022) ——
    //   那说明软件层无解，只能加杀毒排除项。两种结果都能从返回值判断。
    using NtCreateThreadEx_t = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE,
                                            PVOID, PVOID, ULONG,
                                            SIZE_T, SIZE_T, SIZE_T, PVOID);

    HANDLE RemoteThreadViaNt(HANDLE proc, LPVOID start, LPVOID arg, LONG& ntStatus)
    {
        static NtCreateThreadEx_t fn = []() -> NtCreateThreadEx_t {
            HMODULE nt = GetModuleHandleA("ntdll.dll");
            return nt ? (NtCreateThreadEx_t)GetProcAddress(nt, "NtCreateThreadEx")
                      : nullptr;
        }();
        if (!fn) { ntStatus = -1; return nullptr; }

        HANDLE th = nullptr;
        ntStatus = fn(&th, THREAD_ALL_ACCESS, nullptr, proc, start, arg,
                      0, 0, 0, 0, nullptr);
        return th;
    }

    // 建远程线程：先走常规 API，失败再走 Nt 直连。
    // 返回的句柄两套路径一致，调用方不用区分。
    HANDLE CreateRemoteThreadAny(HANDLE proc, LPVOID start, LPVOID arg)
    {
        HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                       (LPTHREAD_START_ROUTINE)start,
                                       arg, 0, nullptr);
        if (th) return th;

        const DWORD err = GetLastError();
        printf("[!] CreateRemoteThread 失败（错误 %lu），改走 NtCreateThreadEx ...\n", err);

        LONG ntStatus = 0;
        th = RemoteThreadViaNt(proc, start, arg, ntStatus);
        if (th)
        {
            // 能走到这里说明拦截发生在用户层，不是内核回调。
            printf("[*] NtCreateThreadEx 成功（CreateRemoteThread 被用户层拦了）\n");
            return th;
        }

        printf("[!] NtCreateThreadEx 也失败（NTSTATUS=0x%08lX）\n", (unsigned)ntStatus);
        if ((unsigned)ntStatus == 0xC0000022U)   // STATUS_ACCESS_DENIED
        {
            printf("    两次都是拒绝访问 —— 拦截在【内核层】，软件层绕不过去。\n");
            printf("    只能加杀毒排除项，没有别的办法。\n");
        }
        else if ((unsigned)ntStatus == 0xC000010AU)  // STATUS_INVALID_IMAGE_FORMAT
        {
            // [批58] 这个码不是权限问题。实测它出现在"目标进程已被前一步
            //   的远端线程打死"之后：进程对象已进入退出流程，再建线程
            //   一律被内核拒绝。真正的原因在【更早】那一步的日志里。
            printf("    0xC000010A = INVALID_IMAGE_FORMAT，不是权限问题。\n");
            printf("    通常意味着目标进程已经崩溃/正在退出 —— 请往上翻日志，\n");
            printf("    找第一次出现\"远端 stub 线程异常退出\"的位置，那才是根因。\n");
        }
        SetLastError(err);
        return nullptr;
    }

    bool Inject(HANDLE proc, DWORD pid, const std::string& dllPath)
    {
        const size_t bytes = dllPath.size() + 1;
        LPVOID remote = VirtualAllocEx(proc, nullptr, bytes,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote) { printf("[!] 目标进程分配内存失败\n"); return false; }

        SIZE_T wrote = 0;
        if (!WriteProcessMemory(proc, remote, dllPath.c_str(), bytes, &wrote))
        {
            printf("[!] WriteProcessMemory 失败（错误 %lu）\n", GetLastError());
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            return false;
        }

        // [批58] LoadLibraryA 也必须换成【目标进程】里的地址。
        //   老注释写"所有同版本进程里地址相同"——那是 Win7 时代的假设，
        //   Win10 开 ASLR 后不再成立。它一直没炸纯属两个进程碰巧同基址，
        //   换个启动顺序就炸。这里与 RemoteSetEnv 用同一套重映射。
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        FARPROC localLoadLib = k32 ? GetProcAddress(k32, "LoadLibraryA") : nullptr;
        if (!localLoadLib)
        {
            printf("[!] 找不到 LoadLibraryA\n");
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            return false;
        }

        const DWORD loadLib = RemapApi(proc, pid, "kernel32.dll",
                                       localLoadLib, "LoadLibraryA");
        if (!loadLib)
        {
            printf("[!] 目标进程 LoadLibraryA 地址解析失败\n");
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            return false;
        }

        HANDLE th = CreateRemoteThreadAny(proc, (LPVOID)(uintptr_t)loadLib, remote);
        if (!th)
        {
            const DWORD err = GetLastError();
            printf("[!] 创建远程线程失败（错误 %lu）\n", err);
            // 错误 5 = 拒绝访问。前面 OpenProcess / VirtualAllocEx /
            // WriteProcessMemory 全都成功，只有"创建远程线程"被拒，
            // 说明不是权限不够，是有人在 API 层拦注入。
            if (err == ERROR_ACCESS_DENIED)
            {
                printf("\n    排查方向（按可能性排序）：\n");
                printf("    ① 杀毒软件拦截 —— 本 EXE 被标记过就会这样，\n");
                printf("       点「允许」往往只放行静态扫描、没放行注入行为。\n");
                printf("       以【管理员】PowerShell 执行一次：\n");
                printf("         Add-MpPreference -ExclusionPath \"%s\"\n",
                       NarrowToUtf8(ExeDir()).c_str());
                printf("         Add-MpPreference -ExclusionPath \"%%TEMP%%\\hanxuesnow\"\n");
                printf("       或直接右键运行本目录的 add_exclusion.bat\n");
                printf("    ② 游戏以管理员身份运行、本程序没有\n");
                printf("       → 右键本程序「以管理员身份运行」\n");
                printf("    ③ 第三方安全软件（360/火绒等）的注入防护 → 同样加白名单\n\n");
            }
            VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
            return false;
        }

        WaitForSingleObject(th, 10000);

        // 取 LoadLibraryA 的返回值 = 远端模块基址。非 0 才是真的加载成功，
        // 比"线程跑完了"可靠得多。
        DWORD remoteModule = 0;
        GetExitCodeThread(th, &remoteModule);
        CloseHandle(th);
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);

        if (remoteModule == 0)
        {
            // [批58] 高位值 = 远端线程异常，说明不是"DLL 没找到"，
            //   是调用过程本身就崩了。分开报，别混成一句"加载失败"。
            if (remoteModule >= 0xC0000000u)
                printf("[!] 远端 LoadLibrary 线程异常退出，NTSTATUS=0x%08lX\n",
                       (unsigned long)remoteModule);
            else
                printf("[!] LoadLibraryA 返回 NULL —— DLL 没能加载。\n"
                       "    最常见原因：缺少 MSVC 运行库（MSVCP140.dll / VCRUNTIME140.dll）。\n");
            return false;
        }
        printf("[*] 远端模块基址 0x%08lX\n", remoteModule);
        return true;
    }
} // namespace

// ============================================================
//  入口
// ============================================================
int main()
{
    // 本文件按 -utf-8 编译，printf 出去是 UTF-8 字节；cmd 窗口默认代码页
    // 是 GBK(936)，按 GBK 解释就是一片乱码。把【输出】代码页切成 UTF-8。
    // 标题另用 W 版：SetConsoleTitleA 走 ANSI，同样会把 UTF-8 解成乱码。
    SetConsoleOutputCP(CP_UTF8);

    // ⚠ 必须关掉 stdout 全缓冲：一旦中途崩溃，缓冲里的文字会随进程消失，
    //   控制台/重定向文件里一个字都看不到，表现为"什么都没干就闪了"。
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    SetConsoleTitleW(L"寒山雪 Hanshanxue v1.0");
    printf("==============================================\n");
    printf("  寒山雪 Hanshanxue  v1.0\n");
    printf("  本工具由 AI 辅助生成，仅供学习研究使用\n");
    printf("  请勿用于商业分发或破坏游戏公平性的场景\n");
    printf("==============================================\n\n");

    // ---- ① 取出载荷 ----
    printf("[stage 1/5] extracting payload ...\n");
    std::string dllPath;
    if (!ExtractPayload(dllPath))
        return FailAndPause("载荷释放失败，已中止。");

    // ---- ② EXE 目录（交给目标进程，决定配置/日志落盘位置）----
    printf("[stage 2/5] resolving home dir ...\n");
    const std::string home = ExeDir();

    // ---- ③ 等游戏 ----
    printf("[stage 3/5] waiting for left4dead2.exe ...\n");
    // [批155] 按用户要求：不再长时间等待游戏进程。
    //   游戏没开 → 直接提示"请在游戏内注入"并退出。
    DWORD pid = FindGamePid();
    if (!pid)
    {
        return FailAndPause(
            "未检测到游戏进程。\n"
            "    请先启动游戏并进到主菜单，然后再运行本程序（注入要在游戏内进行）。",
            dllPath);
    }
    printf("[*] 已找到游戏进程 pid=%lu\n", pid);

    // [批155] 游戏必须"就绪"（client.dll 加载完成且稳定）才允许注入。
    //   游戏刚起还在加载画面时注入 = 旧版"找不到 kernel32 基址"失败的
    //   主要来源。没就绪 → 直接提示，不重试不等待。
    {
        std::string why;
        if (!GameReadyForInject(pid, why))
        {
            char msg[256] = {};
            _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                "游戏还没就绪：%s。\n"
                "    请先进入游戏（看到主菜单或对局画面），再运行本程序。",
                why.c_str());
            return FailAndPause(msg, dllPath);
        }
        printf("[*] 游戏已就绪（client.dll 加载完成），开始注入\n");
    }

    // ---- ④ 注入 ----
    printf("[stage 4/5] injecting ...\n");
    {
        Target target;
        if (!target.Open(pid))
            {
                const DWORD err = GetLastError();
                char msg[256] = {};
                if (err == ERROR_ACCESS_DENIED)
                    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "OpenProcess 被拒绝（错误码 5）—— 游戏是【提权运行】的，"
                        "本程序必须同样以管理员身份运行。");
                else
                    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                        "OpenProcess 失败（错误码 %lu）—— 权限不足或进程已退出。",
                        (unsigned long)err);
                return FailAndPause(msg, dllPath);
            }

        // ============================================================
        //  [批68] 注入前检查：目标里是否已经有一份旧载荷
        // ------------------------------------------------------------
        //  载荷按固定名 payload.dll 写盘，被占用（游戏已加载）就换名成
        //  payload_<pid>_<n>.dll —— 所以【不退出游戏也能注入成功】。
        //  但旧的那份不会被卸载：游戏进程里会同时存在两份 DLL，
        //  各自挂着自己的 CreateMove 钩子与渲染，谁最后写 cmd 谁赢。
        //  这正是"有时好用有时不好用"的一个确定来源。
        //
        //  这里不做自动卸载（DLL 没有导出卸载函数，远程调用地址不可靠），
        //  只把事实明确打出来 —— 让"要不要重启游戏"变成有依据的决定，
        //  而不是靠猜。
        // ============================================================
        {
            int existing = 0;
            bool enumOk = false;
            HANDLE snap = CreateToolhelp32Snapshot(
                TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap == INVALID_HANDLE_VALUE)
            {
                // [批154] 枚举失败 ≠ "目标干净"：旧代码这里会打印
                //   "目标进程干净"，把人误导成一切正常，实际是权限问题。
                printf("[!] 无法枚举目标模块表（错误码 %lu）—— "
                       "“是否已有旧载荷”无法判断，继续注入\n",
                       (unsigned long)GetLastError());
            }
            else
            {
                MODULEENTRY32 me{};
                me.dwSize = sizeof(me);
                if (Module32First(snap, &me))
                {
                    enumOk = true;
                    do {
                        if (_strnicmp(me.szModule, "payload", 7) == 0)
                            ++existing;
                    } while (Module32Next(snap, &me));
                }
                CloseHandle(snap);
            }

            if (existing > 0)
            {
                printf("[!] 检测到游戏里已有 %d 份旧载荷（游戏没有重启）\n", existing);
                printf("    新版本会接管功能，但旧实例【不会卸载】：\n");
                printf("    两份 DLL 各有自己的钩子与菜单，行为可能不确定。\n");
                printf("    要得到确定结果：退出游戏 → 重新运行本程序。\n");
            }
            else if (enumOk)
            {
                printf("[*] 目标进程干净（无旧载荷），本次是首次注入\n");
            }
        }

        // 环境变量必须在【加载 DLL 之前】设好：DllMain 一跑就去读路径。
        //
        // [批58] 这里原来是 `A && B` —— A 失败时 B 根本不会执行，
        //   HANXUESNOW_EXE 会静默缺失。改成各自独立求值。
        const bool envHome = RemoteSetEnv(target.proc, pid, "HANXUESNOW_HOME", home);
        const bool envExe  = RemoteSetEnv(target.proc, pid, "HANXUESNOW_EXE",  home);
        if (envHome && envExe)
        {
            printf("[*] 已通知目标进程配置目录\n");
        }
        else
        {
            // 非致命：DLL 会退化到"与自己同目录"，配置落在 %TEMP%。
            printf("[!] 环境变量未送达（HOME=%d EXE=%d），配置将落在 %%TEMP%%\n",
                   envHome ? 1 : 0, envExe ? 1 : 0);
        }

        if (!Inject(target.proc, pid, dllPath))
            return FailAndPause("注入失败，已中止。", dllPath);
    }

    // ---- ⑤ 清掉磁盘上的载荷 ----
    printf("[stage 5/5] cleaning temp payload ...\n");
    if (DeleteFileA(dllPath.c_str()))
        printf("[*] 临时载荷已清理（内存中仍在运行）。\n");
    else
        printf("[*] 临时载荷将在下次启动时被清理（当前占用中）。\n");

    printf("\n[*] 关闭本窗口不影响功能运行。\n");
    printf("[*] 配置与日志位于：%s\n", NarrowToUtf8(home).c_str());
    Sleep(2500);
    return 0;
}

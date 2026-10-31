// ============================================================
// main.cpp —— 注入器（l4d2.exe）
// ------------------------------------------------------------
// 流程：
//   1) 定位同目录的载荷 DLL（l4d2_payload.dll，必需）
//   2) 等待游戏进程 left4dead2.exe 启动
//   3) 以远程线程 LoadLibrary 方式注入载荷 DLL（自瞄内置于载荷，无独立模块）
//   4) 打印游戏内操作指引
//
// 说明：
//   · 载荷 DLL 全功能进程内运行（ESP/菜单/自瞄/子弹追踪一体，无共享内存 IPC）；
//   · ESP/自瞄设置自动保存到 DLL 目录的 esp_config.json（外部编辑保存后约 1s 热加载生效），再次注入时自动恢复；
//   · 重复注入会被检测并跳过（避免重复初始化）。
// ============================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    const char* kTargetProc   = "left4dead2.exe";
    const char* kPayloadDll   = "l4d2_payload.dll";

    // 按进程名查找 PID（快照遍历）
    DWORD FindProcessId(const char* name)
    {
        DWORD pid = 0;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return 0;

        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe))
        {
            do
            {
                if (_stricmp(pe.szExeFile, name) == 0)
                {
                    pid = pe.th32ProcessID;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        return pid;
    }

    // 检查目标进程是否已加载指定模块
    bool IsModuleLoaded(DWORD pid, const char* moduleName)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) return false;

        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        bool found = false;
        if (Module32First(snap, &me))
        {
            do
            {
                if (_stricmp(me.szModule, moduleName) == 0)
                {
                    found = true;
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    // 本程序所在目录
    std::string GetSelfDir()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string path = buf;
        size_t pos = path.find_last_of("\\/");
        return (pos == std::string::npos) ? std::string(".") : path.substr(0, pos);
    }

    // 远程线程 LoadLibraryA 注入
    bool InjectDll(HANDLE hProc, const char* dllPath)
    {
        const SIZE_T len = strlen(dllPath) + 1;

        // 1) 在目标进程分配内存并写入 DLL 路径
        LPVOID remote = VirtualAllocEx(hProc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote)
        {
            printf("[错误] VirtualAllocEx 失败（错误码 %lu）\n", GetLastError());
            return false;
        }

        if (!WriteProcessMemory(hProc, remote, dllPath, len, nullptr))
        {
            printf("[错误] WriteProcessMemory 失败（错误码 %lu）\n", GetLastError());
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
            return false;
        }

        // 2) 远程线程执行 LoadLibraryA(dllPath)
        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        FARPROC pLoadLibrary = GetProcAddress(hKernel, "LoadLibraryA");
        if (!pLoadLibrary)
        {
            printf("[错误] 无法获取 LoadLibraryA 地址\n");
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
            return false;
        }

        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
            (LPTHREAD_START_ROUTINE)pLoadLibrary, remote, 0, nullptr);
        if (!hThread)
        {
            printf("[错误] CreateRemoteThread 失败（错误码 %lu）\n", GetLastError());
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
            return false;
        }

        // 3) 等待注入线程完成（LoadLibrary 返回模块句柄）
        DWORD wait = WaitForSingleObject(hThread, 20000);
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        CloseHandle(hThread);
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

        if (wait != WAIT_OBJECT_0)
        {
            printf("[错误] 等待注入线程超时\n");
            return false;
        }
        if (exitCode == 0)
        {
            printf("[错误] 目标进程 LoadLibrary 返回 NULL（DLL 加载失败）\n");
            return false;
        }
        return true;
    }

    // 注入单个 DLL：已加载则跳过；注入后复核模块是否真正加载
    bool InjectAndVerify(HANDLE hProc, DWORD pid, const char* dllName, const std::string& dllPath)
    {
        if (IsModuleLoaded(pid, dllName))
        {
            printf("      [提示] 游戏已加载过 %s，跳过本次注入。\n", dllName);
            return true;
        }

        if (!InjectDll(hProc, dllPath.c_str()))
        {
            printf("      [错误] %s 注入失败。\n", dllName);
            return false;
        }

        // 注入后复核模块是否真正加载
        Sleep(500);
        if (IsModuleLoaded(pid, dllName))
        {
            printf("      注入成功！\n");
            return true;
        }
        printf("      [警告] 注入线程已返回，但未检测到 %s 加载，请查看游戏目录下的日志。\n", dllName);
        return false;
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("L4D2 注入器（进程内模式）");

    printf("==============================================\n");
    printf(" L4D2 注入器（进程内模式）\n");
    printf("==============================================\n\n");

    // ---- 1) 定位载荷 DLL ----
    const std::string dllPath    = GetSelfDir() + "\\" + kPayloadDll;
    printf("[1/4] 定位载荷 DLL...\n      路径: %s\n", dllPath.c_str());
    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        printf("\n[错误] 未找到 %s。\n", kPayloadDll);
        printf("       请先编译 l4d2_payload 项目（DLL），并将其与本程序放在同一目录。\n");
        printf("\n按回车键退出...");
        getchar();
        return 1;
    }

    // ---- 2) 等待游戏进程 ----
    printf("[2/4] 等待游戏进程 %s...\n", kTargetProc);
    DWORD pid = 0;
    while (!(pid = FindProcessId(kTargetProc)))
    {
        printf("      ... 未检测到游戏，请先启动 Left 4 Dead 2（2 秒后重试）\n");
        Sleep(2000);
    }
    printf("      已找到进程，PID = %lu\n", pid);

    // ---- 3) 打开进程并注入 ----
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);
    if (!hProc)
    {
        printf("\n[错误] OpenProcess 失败（错误码 %lu）。\n", GetLastError());
        printf("       请尝试以管理员身份运行本程序。\n");
        printf("\n按回车键退出...");
        getchar();
        return 1;
    }

    printf("[3/4] 注入载荷（ESP/菜单/自瞄/子弹追踪 一体）...\n");
    if (!InjectAndVerify(hProc, pid, kPayloadDll, dllPath))
    {
        printf("\n[错误] 载荷注入失败。请确认游戏为 32 位版本，并以管理员身份重试。\n");
        CloseHandle(hProc);
        printf("\n按回车键退出...");
        getchar();
        return 1;
    }

    CloseHandle(hProc);

    // ---- 4) 操作指引 ----
    printf("[4/4] 完成。\n\n");
    printf("----------------------------------------------\n");
    printf(" 游戏内操作：\n");
    printf("   · INSERT 键：随时开/关菜单（游戏内渲染）\n");
    printf("   · 菜单 → “ESP 视觉 / 视觉 / 自定义颜色”页：调整 ESP 显示与颜色\n");
    printf("   · 菜单 → “自瞄”页：开关/触发键/FOV（自瞄内置于载荷，无需额外 DLL）\n");
    printf("   · 设置修改后自动保存到 esp_config.json（手动编辑保存后约 1s 游戏内热加载）\n");
    printf("   · 卸载：菜单 → “注入控制”页 → 卸载注入（卸载后需重启游戏才能再次注入）\n");
    printf("----------------------------------------------\n");
    printf(" 风险提示：注入类工具在官方 VAC 服务器存在封禁风险，\n");
    printf("           请仅在本地/单机或社区服务器使用。\n");
    printf("----------------------------------------------\n\n");

    printf("按回车键退出...");
    getchar();
    return 0;
}

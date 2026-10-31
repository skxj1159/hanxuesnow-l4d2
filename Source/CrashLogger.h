#pragma once
// ============================================================
//  CrashLogger.h —— 崩溃现场记录器（VEH，批34）
// ------------------------------------------------------------
//  关卡加载等场景闪退时，把异常码 + EIP + 返回地址栈（模块+偏移）
//  写入 DLL 同目录 payload_crash.log。一次复现即可用 l4d2_payload.map
//  反查具体函数。只记录、不吞异常（CONTINUE_SEARCH）。
//  覆盖：0xc0000409(fail-fast/GS)、0xc0000005(AV)、0xc0000374(堆)、
//        0xc0000028(坏栈)、0xc00000FD(栈溢出)。
// ============================================================

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "Globals.h"        // [批166] g_hbPhase/g_hbEnt（崩溃现场定位）

namespace crashlog
{
    // ============================================================
    //  [批42] 两个防线（缺一不可）
    //  ① s_busy：VEH 重入自锁。VEH 处理期间若自身再次触发异常（栈扫描
    //     读到守卫页、文件 I/O 等），会递归嵌套 → 栈溢出 → 进程必死。
    //     重入时立刻返回，不再做任何读写。
    //  ② s_records：单进程记录上限。异常风暴（如引擎接口被错误参数
    //     调用后每帧复发）会把日志刷到 13MB，I/O 本身就拖死进程；
    //     超限后只计数不再落盘，日志尾部留一行汇总。
    // ============================================================
    inline volatile LONG g_busy     = 0;
    inline volatile LONG g_records  = 0;
    inline constexpr LONG kMaxRecords = 64;

    inline void DescribeAddr(void* addr, char* out, size_t cap)
    {
        if (out == nullptr || cap == 0)
            return;
        out[0] = 0;
        if (addr == nullptr)
        {
            _snprintf_s(out, cap, _TRUNCATE, "null");
            return;
        }
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)addr, &mod) && mod != nullptr)
        {
            char name[MAX_PATH] = {};
            GetModuleFileNameA(mod, name, MAX_PATH);
            const char* base = std::strrchr(name, '\\');
            base = (base != nullptr) ? (base + 1) : name;
            _snprintf_s(out, cap, _TRUNCATE, "%s+0x%X", base,
                        (unsigned)((uintptr_t)addr - (uintptr_t)mod));
        }
        else
        {
            _snprintf_s(out, cap, _TRUNCATE, "0x%p(unmapped)", addr);
        }
    }

    inline LONG WINAPI CrashLogger(EXCEPTION_POINTERS* ep)
    {
        // ---- ① 重入自锁：处理期间再次进 VEH（自身读到坏地址）→ 直接放行 ----
        if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        // ---- ② 记录上限：风暴中不再无限写盘（超过只计数）----
        if (InterlockedIncrement(&g_records) > kMaxRecords)
        {
            InterlockedExchange(&g_busy, 0);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (ep == nullptr || ep->ExceptionRecord == nullptr)
        {
            InterlockedExchange(&g_busy, 0);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code != 0xC0000409u && code != 0xC0000005u && code != 0xC0000374u &&
            code != 0xC0000028u && code != 0xC00000FDu)
        {
            InterlockedExchange(&g_busy, 0);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // 输出路径：DLL 同目录 payload_crash.log（独立文件，避免与
        // payload.log 的互斥锁/句柄冲突）
        char path[MAX_PATH] = {};
        {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&CrashLogger, &self);
            char selfPath[MAX_PATH] = {};
            GetModuleFileNameA(self, selfPath, MAX_PATH);
            char* slash = std::strrchr(selfPath, '\\');
            if (slash != nullptr)
                *slash = 0;
            _snprintf_s(path, MAX_PATH, _TRUNCATE, "%s\\payload_crash.log",
                        selfPath);
        }
        // 去重：同一 eip 800ms 内只记一次——首轮异常（被 SEH 吞掉的引擎
        // 空指针）会每帧重复；只按 eip 比对（esp 每层不同，比不上的话
        // 8 格环会被冲刷成"每次都记"）。环扩到 32 格。
        {
            static uintptr_t s_lastEip[32] = {};
            static DWORD     s_lastMs[32]  = {};
            static int       s_next = 0;
            const uintptr_t eipKey = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
            const DWORD now = GetTickCount();
            for (int i = 0; i < 32; ++i)
            {
                if (s_lastEip[i] == eipKey && now - s_lastMs[i] < 800)
                {
                    InterlockedExchange(&g_busy, 0);
                    return EXCEPTION_CONTINUE_SEARCH;
                }
            }
            s_lastEip[s_next] = eipKey;
            s_lastMs[s_next]  = now;
            s_next = (s_next + 1) & 31;
        }

        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") != 0 || f == nullptr)
        {
            InterlockedExchange(&g_busy, 0);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        char eip[192] = {};
        char access[192] = {};
        DescribeAddr(ep->ExceptionRecord->ExceptionAddress, eip, sizeof(eip));
        DescribeAddr((void*)ep->ExceptionRecord->ExceptionInformation[0],
                     access, sizeof(access));

        // [批166] 记录分级：phase/ent 标出崩溃发生在哪个阶段（10=取骨
        //   直调、1~3=CreateMove、20=渲染…），并给上一条标注"是否已被
        //   SEH 接住" —— 进程若在上一条后继续存活 >2s，说明异常被我们
        //   自己的 __try 兜住了（非致命）；紧跟本条的多半是致命链。
        //   以前 41/57 条都是被接住的非致命异常，用户根本分不出来。
        static DWORD s_prevAt = 0;
        static int   s_seq    = 0;
        {
            const DWORD gapMs = (s_prevAt != 0) ? (GetTickCount() - s_prevAt) : 0;
            if (s_prevAt != 0)
            {
                fprintf(f, "[crash-status] 上一条（#%d）%s（距今 %lu ms）\n",
                        s_seq,
                        gapMs > 2000 ? "已被 SEH 接住（进程继续运行）"
                                     : "疑似未接住（紧跟本条）",
                        (unsigned long)gapMs);
            }
            s_prevAt = GetTickCount();
            ++s_seq;
            fprintf(f, "===== crash #%d %lu ms code=0x%08X phase=%d ent=%d =====\n",
                    s_seq, GetTickCount(), code,
                    (int)g_hbPhase, (int)g_hbEnt);
        }
        fprintf(f, "eip=%s access=%s\n", eip, access);

        CONTEXT* c = ep->ContextRecord;
        if (c != nullptr)
        {
            fprintf(f, "ctx: eip=0x%08X esp=0x%08X ebp=0x%08X eax=0x%08X ecx=0x%08X edx=0x%08X\n",
                    (unsigned)c->Eip, (unsigned)c->Esp, (unsigned)c->Ebp,
                    (unsigned)c->Eax, (unsigned)c->Ecx, (unsigned)c->Edx);

            // ① EBP 链（最多 32 帧；防死循环/防越界）
            fprintf(f, "-- ebp chain --\n");
            {
                uintptr_t ebp = c->Ebp;
                int frames = 0;
                while (ebp > 0x10000u && frames < 32)
                {
                    uintptr_t savedEbp = 0;
                    uintptr_t ret = 0;
                    __try
                    {
                        savedEbp = *reinterpret_cast<const uintptr_t*>(ebp);
                        ret = *reinterpret_cast<const uintptr_t*>(ebp + 4);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        break;
                    }
                    if (ret < 0x10000u)
                        break;
                    char rb[192] = {};
                    DescribeAddr((void*)ret, rb, sizeof(rb));
                    fprintf(f, "  #%02d %s (ebp=0x%08X)\n", frames, rb,
                            (unsigned)ebp);
                    if (savedEbp <= ebp)
                        break;
                    ebp = savedEbp;
                    ++frames;
                }
            }

            // ② 裸栈扫描（最多 96 个落在模块内的疑似返回地址）
            fprintf(f, "-- stack scan --\n");
            {
                const uintptr_t sp = c->Esp;
                int hits = 0;
                for (int i = 0; i < 512 && hits < 96; ++i)
                {
                    uintptr_t v = 0;
                    __try
                    {
                        v = *reinterpret_cast<const uintptr_t*>(
                                sp + (uintptr_t)i * 4);
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        break;
                    }
                    if (v < 0x10000u)
                        continue;
                    HMODULE m = nullptr;
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)v, &m) && m != nullptr)
                    {
                        char rb[192] = {};
                        DescribeAddr((void*)v, rb, sizeof(rb));
                        fprintf(f, "  S%03d %s\n", i, rb);
                        ++hits;
                    }
                }
            }
        }
        fprintf(f, "===== end =====\n");
        fclose(f);
        InterlockedExchange(&g_busy, 0);
        return EXCEPTION_CONTINUE_SEARCH;   // 不吞异常，交还系统
    }

    inline void InstallCrashLogger()
    {
        static bool s_done = false;
        if (s_done)
            return;
        s_done = true;
        AddVectoredExceptionHandler(1, CrashLogger);
    }
} // namespace crashlog

using crashlog::InstallCrashLogger;

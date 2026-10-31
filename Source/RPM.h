#pragma once
#include <Windows.h>
#include <cstdint>

// 进程内直读基础设施工具（payload 注入后 hProcess = GetCurrentProcess()，
// ReadProcessMemory 对自身进程等价 memcpy；保留模板以便跨进程调试路径复用）
extern HANDLE hProcess;

template<typename T>
T ReadMemory(uintptr_t address) {
    T buffer{};
    ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(address), &buffer, sizeof(T), nullptr);
    return buffer;
}

// 进程内写（WriteProcessMemory 对自身进程等价 memcpy；堆数据页本身可写）
template<typename T>
bool WriteMemory(uintptr_t address, const T& value) {
    if (!address)
        return false;
    SIZE_T written = 0;
    return WriteProcessMemory(hProcess, reinterpret_cast<LPVOID>(address),
                              &value, sizeof(T), &written) && written == sizeof(T);
}

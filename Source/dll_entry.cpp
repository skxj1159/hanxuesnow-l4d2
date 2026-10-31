// ============================================================
//  dll_entry.cpp —— 载荷 DLL 的进程入口（**只做编排，不写任何逻辑**）
// ------------------------------------------------------------
//  ⚠ 为什么不叫 main.cpp：
//  本仓库的 main.cpp 已被**注入器**（l4d2.exe，独立 exe 工程）占用 ——
//  它做的是找进程 / LoadLibrary 注入 / 打印指引。DLL 的入口是另一个
//  工程（l4d2_payload.dll）里的 DllMain，两者不能共用一个文件。
//  用户要求的「根目录 main.cpp 只有 DllMain」在结构上对应本文件。
//
//  唯一职责：初始化接口 → 加载配置 → 挂钩子。
//  任何 if / 循环 / 读内存出现在这里都是架构腐化。
//
//  【初始化顺序不可调换 —— 有硬依赖关系】
//      ① Interfaces::Init()      必须最先：后面所有层都要 GetInterface
//      ② NetVars::Init()         依赖 ①（要 VClient016 遍历 ClientClass）
//      ③ EntityList::Init()      依赖 ①（要 dwEntityList）
//      ④ Render3D::Init()        依赖 ①
//      ⑤ ConfigMgr::Load()       无依赖，但必须在 ⑥ 之前
//                                （首个回调帧可能就读配置）
//      ⑥ Hooks::Init()           必须最后：挂上钩子才会有回调
//
//  反过来做（先挂钩子后初始化接口）会在第一个回调帧里取到空指针 ——
//  这是最难查的一类崩溃，因为崩在别人的栈上。
// ============================================================

#include <Windows.h>

#include "SDK/Interfaces.h"
#include "SDK/NetVars.h"
#include "SDK/Entity.h"
#include "Config/Config.h"
#include "Render/Render3D.h"
#include "Hooks/Hooks.h"

namespace
{
    bool Bootstrap()
    {
        // ① 接口
        if (!Interfaces::Init())
            return false;

        // ② 动态偏移 —— 必须在实体读取之前。
        //    拿不到就整套功能不可用，没有"降级继续"的必要，
        //    直接失败比半死不活好排查。
        if (!NetVars::Init())
            return false;

        // ③ 实体列表
        if (!sdk::EntityList::Init())
            return false;

        // ④ 3D 渲染后端（失败会自动降级到屏幕线，不阻断）
        Render3D::Init();

        // ⑤ 配置
        ConfigMgr::Load();

        return true;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        // 禁止隐式 TLS 回调，省一份加载开销，也避免与引擎的线程局部冲突
        DisableThreadLibraryCalls(hModule);

        if (!Bootstrap())
            return FALSE;

        // ⑥ 钩子 —— 必须最后
        if (!Hooks::Init())
            return FALSE;
        break;

    case DLL_PROCESS_DETACH:
        // 卸载顺序 = 初始化的逆序：
        //   先停钩子（不然卸载后还会来回调帧，用到已释放的东西）
        //   再存盘（此时配置结构还在）
        Hooks::Shutdown();
        ConfigMgr::SaveNow();
        Render3D::Shutdown();
        Interfaces::Shutdown();
        break;
    }
    return TRUE;
}

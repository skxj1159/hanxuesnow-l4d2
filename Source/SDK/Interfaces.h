#pragma once
// ============================================================
//  SDK/Interfaces.h —— 全局接口指针（**只读环境层**）
// ------------------------------------------------------------
//  【分层铁律】本文件只负责"拿到接口指针 + 存起来"，不含任何功能。
//
//  【循环依赖处理 —— 这是本层的关键设计】
//  接口类型（IVEngineClient / IEngineTrace / IVModelInfoClient …）
//  的完整定义来自引擎 SDK，彼此之间存在大量互相引用（IVEngineClient
//  里有 IEngineTrace 的方法、IEngineTrace 里要 CTraceFilter…）。
//  如果在这里 include 它们的完整定义：
//     Interfaces.h → 引擎 SDK → … → 可能又需要我们的类型
//  于是形成环。
//
//  解法（三管齐下）：
//  ① 本文件对所有接口类型只做**前向声明**（struct IVEngineClient;），
//     指针类型不需要完整定义就能声明和传递；
//  ② 真正需要调用方法的行Feature 层，通过各自的头文件 include
//     对应的引擎 SDK 头，并在那一层做 reinterpret_cast；
//  ③ 一个手写的薄封装（IVEngineTrace / IVModelInfo 的 Helper 函数）
//     放在 SDK/ 里，用**裸偏移 + void*** 表达，不碰引擎类型定义。
//
//  这样 Interfaces.h 的依赖集合是空的 —— 它是依赖图的根。
// ============================================================

#include <cstdint>
#include <Windows.h>

// ============================================================
//  接口类型前向声明
//  指针只需要前向声明即可声明全局变量。
//  完整定义由 Features/ 层在需要调用方法时自行 include。
// ============================================================
struct IVEngineClient;
struct IEngineTrace;
struct IClientEntityList;
struct IVModelInfoClient;
struct ISurface;
struct IVGui;
struct IClientRender;
struct IGameClient;
struct IEngineSound;
struct IClientCmdKeyValues;

// ============================================================
//  全局接口池
// ============================================================
namespace Interfaces
{
    // ---- 生命周期 ----
    bool Init();      // 一次性解析全部接口；失败项留 null 并打日志
    void Shutdown();  // 清空指针（卸载路径用）

    // ---- 访问器（定义统一在 Interfaces.cpp）----
    //   ⚠ 不能写成 inline 函数 + 函数内 static：那样每个 TU 都有
    //   自己的一份 static 指针，SetEngine 写的是 A 的、Engine() 读
    //   的是 B 的，永远是 null。普通函数声明 + 单点定义才保证全程序
    //   只有一份实例。
    //   返回的是 **void***，调用方自行 reinterpret_cast 到需要的类型
    //   —— 这是"SDK 层不认识引擎类型"的代价与收益。
    IVEngineClient*      Engine();
    IEngineTrace*        Trace();
    IClientEntityList*   EntityList();
    IVModelInfoClient*   ModelInfo();
    ISurface*            Surface();
    IVGui*               Gui();
    IClientRender*       ClientRender();
    IClientCmdKeyValues* ClientCmdKeys();

    // ---- 底层原始指针（.cpp 里通过 Set* 写入）----
    void SetEngine(void*);
    void SetTrace(void*);
    void SetEntityList(void*);
    void SetModelInfo(void*);
    void SetSurface(void*);
    void SetGui(void*);
    void SetClientRender(void*);
    void SetClientCmdKeys(void*);

    // ---- 必需项自检：这些缺任何一个功能都跑不起来 ----
    bool HasEngine();
    bool HasTrace();

    // ---- 模块基址（SDK 层各处都要用，缓存起来省重复 GetModuleHandle）----
    void* ClientDll();
    void* EngineDll();
    void* SurfaceDll();     // vguimatsurface.dll
}

// ============================================================
//  CreateInterface 原语（SDK 层唯一对外的函数形态）
// ------------------------------------------------------------
//  单独抽出来是因为 Interfaces.cpp / NetVars.cpp / Entity.cpp 都要用，
//  放在一处实现避免三份重复。
// ============================================================
void* GetInterface(HMODULE module, const char* name);

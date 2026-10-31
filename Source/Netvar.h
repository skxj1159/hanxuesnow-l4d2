#pragma once
// ============================================================
// Netvar.h —— 客户端 netvar 动态解析（header-only，进程内使用）
// ------------------------------------------------------------
// 通过 client.dll 的 VClient016 接口拿到 ClientClass 链，在
// RecvTable 里递归查属性偏移——不依赖任何硬编码偏移，客户端更新
// 后仍有效。结构布局与 dt_recv.h / client_class.h 一致（32 位）。
// 来源：AimHelper.cpp 内同款实现抽出复用（glow 等字段落点）。
// ============================================================

#include <windows.h>
#include <cstring>

namespace netvar
{
    struct RecvTableX;

    struct RecvPropX
    {
        const char*  m_pVarName;            // 0x00
        int          m_RecvType;            // 0x04
        int          m_Flags;               // 0x08
        int          m_StringBufferSize;    // 0x0C
        bool         m_bInsideArray;        // 0x10
        char         _pad10[3];             // 0x11
        const void*  m_pExtraData;          // 0x14
        RecvPropX*   m_pArrayProp;          // 0x18
        void*        m_ArrayLengthProxy;    // 0x1C
        void*        m_ProxyFn;             // 0x20
        void*        m_DataTableProxyFn;    // 0x24
        RecvTableX*  m_pDataTable;          // 0x28
        int          m_Offset;              // 0x2C
        int          m_ElementStride;       // 0x30
        int          m_nElements;           // 0x34
        const char*  m_pParentArrayPropName;// 0x38
    };

    struct RecvTableX
    {
        RecvPropX*  m_pProps;        // 0x00
        int         m_nProps;        // 0x04
        void*       m_pDecoder;      // 0x08
        const char* m_pNetTableName; // 0x0C
    };

    struct ClientClassX
    {
        void*         m_pCreateFn;      // 0x00
        void*         m_pCreateEventFn; // 0x04
        const char*   m_pNetworkName;   // 0x08
        RecvTableX*   m_pRecvTable;     // 0x0C
        ClientClassX* m_pNext;          // 0x10
        int           m_ClassID;        // 0x14
    };

    // 标准接口工厂：经 CreateInterface 导出按 "VClient016" 这类链名取接口
    // （与 AimHelper.cpp 已验证实现一致）
    inline void* GetInterface(HMODULE module, const char* name)
    {
        if (!module || !name)
            return nullptr;
        using CreateInterfaceFn = void* (*)(const char*, int*);
        const auto fn =
            reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
        return fn ? fn(name, nullptr) : nullptr;
    }

    // 递归查表：命中返回 offset + prop->m_Offset；嵌套表下钻失败自动回退
    inline uintptr_t FindPropR(const RecvTableX* table, const char* name, uintptr_t offset)
    {
        if (!table)
            return 0;
        for (int i = 0; i < table->m_nProps; ++i)
        {
            const RecvPropX* prop = &table->m_pProps[i];
            if (!prop->m_pVarName)
                continue;

            if (_stricmp(prop->m_pVarName, name) == 0)
                return offset + static_cast<uintptr_t>(prop->m_Offset);

            if (prop->m_pDataTable)
            {
                const uintptr_t r = FindPropR(
                    prop->m_pDataTable, name, offset + static_cast<uintptr_t>(prop->m_Offset));
                if (r)
                    return r;
            }
        }
        return 0;
    }

    // ------------------------------------------------------------
    //  [批75] DigProp —— 先定位子表，再取子表内属性
    // ------------------------------------------------------------
    //  FindPropR 是"按属性名找"，找不到"父表下的某个子属性"这种语义。
    //  L4D2 的实际层级是：
    //      localdata(0) → m_Local(6724) → m_vecPunchAngle(112)
    //  也就是说 m_Local 嵌在 localdata 里，不在类顶层。只扫顶层永远
    //  扫不到 —— 这正是"去后坐力开关有效但屏幕照抖"的根因。
    //
    //  本函数递归下钻任意深度（上限 depth 防环），命中 want 子表后，
    //  返回值 = 到该子表的累积偏移 + 子表内 inner 属性的偏移。
    inline uintptr_t DigProp(const RecvTableX* tbl, const char* want,
                             const char* inner, uintptr_t base, int depth)
    {
        if (!tbl || depth > 4)
            return 0;

        for (int i = 0; i < tbl->m_nProps; ++i)
        {
            const RecvPropX* prop = &tbl->m_pProps[i];
            if (!prop->m_pVarName || !prop->m_pDataTable)
                continue;

            const uintptr_t here = base + static_cast<uintptr_t>(prop->m_Offset);

            if (_stricmp(prop->m_pVarName, want) == 0)
            {
                const uintptr_t innerRel = FindPropR(prop->m_pDataTable, inner, 0);
                if (innerRel)
                    return here + innerRel;
            }

            const uintptr_t r = DigProp(prop->m_pDataTable, want, inner, here, depth + 1);
            if (r)
                return r;
        }
        return 0;
    }

    // 取 ClientClass 链头（client.dll）
    // ⚠ 限定名调用：SDK/Interfaces.h 也有一个全局 GetInterface(HMODULE,const char*)，
    //   同时 include 两者时非限定名会产生重载歧义
    inline ClientClassX* GetAllClasses()
    {
        void* client = netvar::GetInterface(GetModuleHandleA("client.dll"), "VClient016");
        if (!client)
            return nullptr;

        using GetAllClassesFn = ClientClassX* (__thiscall*)(void*);
        const auto vtable = *reinterpret_cast<void***>(client);
        const auto getAllClasses = reinterpret_cast<GetAllClassesFn>(vtable[7]);
        if (!getAllClasses)
            return nullptr;
        return getAllClasses(client);
    }

    // 按类名在指定类的表里解析属性偏移（0=未找到）
    inline uintptr_t OffsetInClass(ClientClassX* head, const char* className, const char* propName)
    {
        for (ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            if (!cc->m_pNetworkName || _stricmp(cc->m_pNetworkName, className) != 0)
                continue;
            return FindPropR(cc->m_pRecvTable, propName, 0);
        }
        return 0;
    }

    // 按 RecvTable 名（如 "DT_Witch"）解析属性偏移：类名在本作二进制里
    // 不可靠（无 CWitch/CInfected 字符串），DT 表名是稳定锚点（0=未找到）
    inline uintptr_t OffsetInTable(ClientClassX* head, const char* tableName, const char* propName)
    {
        for (ClientClassX* cc = head; cc; cc = cc->m_pNext)
        {
            const RecvTableX* t = cc->m_pRecvTable;
            if (!t || !t->m_pNetTableName || _stricmp(t->m_pNetTableName, tableName) != 0)
                continue;
            return FindPropR(t, propName, 0);
        }
        return 0;
    }
}

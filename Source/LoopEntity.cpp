#include "Include.h"

#include "PayloadControl.h"   // PayloadLog（本地玩家获取诊断日志）

#include <atomic>    // 实体指针表激活索引（跨线程发布）

std::mutex g_entitiesMutex;
std::vector<std::pair<EntityInfo, EntityState>> g_entities;

namespace
{
    // ---- 实体指针表批量读取（性能）----
    // BoneSystem / AimBridge 按索引取实体指针用：
    // LocalPlayerTableTick（后台线程，30ms 一轮）整表刷新一次，
    // 单次 16KB 拷贝代替 1024 次单点读。双缓冲（写非激活缓冲，
    // 成功后发布激活索引）保证读取方见到的表始终完整
    constexpr int    kMaxEntitySlots    = 1024;
    constexpr size_t kEntityTableStride = 0x10;   // 槽位步进（索引 × 0x10）
    constexpr size_t kEntityTableBytes  = kMaxEntitySlots * kEntityTableStride;  // 16KB

    // 整表读取失败的降级尺寸：数组尾部紧邻未提交页时整表读会失败，
    // 逐级收缩读取范围（前段槽位仍有效，后续槽位清零）
    constexpr size_t kTableFallbacks[] = { 0x2000, 0x1000, 0x400 };

    uint8_t          g_entityTable[2][kEntityTableBytes];
    std::atomic<int> g_entityTableActive{ -1 };   // 有效表索引；-1 = 尚未建立

    // 从表缓冲按索引取实体指针（零 RPM）
    uintptr_t ReadEntityFromTable(const uint8_t* table, int index) {
        uintptr_t ent = 0;
        std::memcpy(&ent, table + static_cast<size_t>(index) * kEntityTableStride, sizeof(ent));
        return ent;
    }
}

// 批量刷新实体指针表：成功返回写入缓冲索引（已发布），全部失败返回 -1
// （激活索引保持不变，扫描方沿用上一份表）
static int FetchEntityTable() {
    const int writeIdx = (g_entityTableActive.load(std::memory_order_relaxed) == 0) ? 1 : 0;
    uint8_t* dst = g_entityTable[writeIdx];
    const uintptr_t listBase = clientBase + Offsets::dwEntityList;

    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(listBase),
                          dst, kEntityTableBytes, nullptr)) {
        g_entityTableActive.store(writeIdx, std::memory_order_release);
        return writeIdx;
    }
    for (size_t size : kTableFallbacks) {
        std::memset(dst, 0, kEntityTableBytes);
        if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(listBase),
                              dst, size, nullptr)) {
            g_entityTableActive.store(writeIdx, std::memory_order_release);
            return writeIdx;
        }
    }
    return -1;
}

uintptr_t GetEntity(int index) {
    if (index < 0 || index >= kMaxEntitySlots || !clientBase)
        return 0;

    // 以「单槽直读」为准：只读目标槽的 4 字节，绝不会因为整表读取降级
    // （数组尾部跨未提交页 → 退到 0x2000/0x1000/0x400 且尾部清零）而
    // 把高槽位实体读成 0。之前优先查批量表，正是"快照里有这只特感、
    // 取矩形时 GetEntity 却返回 0，于是没有框"的根因。
    const uintptr_t addr = clientBase + Offsets::dwEntityList +
                           static_cast<uintptr_t>(index) * kEntityTableStride;
    uintptr_t ent = 0;
    SIZE_T    got = 0;
    if (ReadProcessMemory(hProcess, reinterpret_cast<LPCVOID>(addr),
                          &ent, sizeof(ent), &got) && got == sizeof(ent))
        return ent;          // 读成功即以直读为准（0 也表示该槽确实为空）

    // 直读整槽失败（页未提交等极端情况）才退回批量表
    const int active = g_entityTableActive.load(std::memory_order_acquire);
    if (active >= 0)
        return ReadEntityFromTable(g_entityTable[active], index);

    return 0;
}

// 渲染帧内复用：本帧实体快照。
// 数据源 = LiveEsp::LiveFrameBegin（渲染线程每帧直读后 swap 进 g_entities，
// 是唯一权威写入方）；本函数与写入方同在渲染线程（DrawCallBack 串行调用），
// 直读引用无竞态。跨线程只读场景（卸载清理）请自行加
// g_entitiesMutex 后再使用本引用。
const std::vector<std::pair<EntityInfo, EntityState>>& GetFrameSnapshot()
{
    return g_entities;
}

// ============================================================
// LocalPlayerTableTick：后台线程职责（ESP 数据链已由 LiveEsp 接管）
// ------------------------------------------------------------
// LiveFrameBegin（渲染帧直读）是实体快照的唯一权威写入方，本线程只做
// 两项低频维护（30ms 一轮）：
//   1) localPlayer / localPlayerIndex 指针维护（BoneSystem::GetEntityBones
//      与免渲染期的引用有效性依赖）；
//   2) 实体指针表批量刷新（g_entityTable，GetEntity 按索引取实体指针用）。
// ============================================================
void LocalPlayerTableTick() {
    uintptr_t localPtr = ReadMemory<uintptr_t>(clientBase + Offsets::dwLocalPlayer);
    if (!localPtr) {
        localPlayer = 0;
        localPlayerIndex = -1;
        return;
    }
    if (!localPlayer)
        PayloadLog("[player] 本地玩家已获取：0x%llX（后台维护链路）",
            (unsigned long long)localPtr);
    localPlayer = localPtr;

    // 实体指针表批量刷新（一次 16KB RPM；失败沿用激活表）
    FetchEntityTable();

    // 从表中定位本地玩家索引（LiveFrameBegin 每帧也会更新，此处兜底）
    const int readIdx = g_entityTableActive.load(std::memory_order_acquire);
    if (readIdx >= 0) {
        for (int i = 0; i < kMaxEntitySlots; ++i) {
            if (ReadEntityFromTable(g_entityTable[readIdx], i) == localPlayer) {
                localPlayerIndex = i;
                break;
            }
        }
    }
}

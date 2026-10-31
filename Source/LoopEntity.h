#pragma once

#include "Include.h"

#include <mutex>
#include <vector>

struct EntityInfo;
struct EntityState;
enum class EntityType;

uintptr_t GetEntity(int index);

// 后台线程低频维护（30ms）：仅刷新 localPlayer 与实体指针表。
// 实体快照唯一权威写入方 = LiveEsp::LiveFrameBegin（渲染帧直读）
void LocalPlayerTableTick();

// 本帧实体快照（渲染线程专用：LiveFrameBegin 每帧直读写入，本函数直读
// 引用；写入方与读取方同在渲染线程，无竞态。跨线程只读需自行加锁）
const std::vector<std::pair<EntityInfo, EntityState>>& GetFrameSnapshot();

extern std::mutex g_entitiesMutex;
extern std::vector<std::pair<EntityInfo, EntityState>> g_entities;

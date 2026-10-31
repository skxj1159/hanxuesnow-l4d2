#pragma once

#include "Include.h"
enum class EntityType {
    Unknown,
    Common,
    Witch
};

struct EntityInfo {
    Vec3 position;
    Vec3 velocity;    // 当前速度（u/s；移动预判数据源，0=读取失败）
    int team;
    int health;
    int zombieClass;
    int index;        // 实体列表索引（自瞄目标索引/本地玩家定位用）
    EntityType type;
    // Storm Identifier（LiveEsp::LiveFrameBegin 调 StormIdentify 的结果）。
    // <0 = 场景道具（-73/-105…），-1 = 未识别。渲染层与自瞄都读这一个值，
    // 保证"画出来的"和"打的"是同一套分类，不会出现透视是特感、自瞄当小僵尸。
    int stormId = -1;
    int survivorId = -1;  // 幸存者角色 ID 0..7（entity+7308；非幸存者=-1）
};

struct EntityState {
    int  health;
    BYTE lifeState;
    bool isGhost;    // 幽灵状态（m_isGhost==1：玩家控制特感尚未出生到世界）
};

// 三个有效性判据。定义内联在这里：旧的实现放在 _removed_esp/DrawEsp.cpp，
// 该目录不参与编译，符号实际无处可链接（只剩陈旧 .obj 里的残留）。
inline bool IsValidSurvivor(const EntityInfo& info, const EntityState& state)
{
    return info.team == 2 && state.lifeState == 0 && info.health > 0;
}
inline bool IsValidSpecialInfected(const EntityInfo& info, const EntityState& state)
{
    return info.team == 3 && info.zombieClass >= 1 && info.zombieClass <= 6 &&
           state.lifeState == 0;
}
// 坦克：ZC_TANK=8。ZC_WITCH=7 归女巫（分类失败时 zombieClass 可能残留 7，
// 用血量区分：坦克 4000，女巫 1000，避免把女巫当坦克）
inline bool IsTank(const EntityInfo& info)
{
    if (info.zombieClass == 8) return true;
    if (info.zombieClass == 7 && info.health > 1000) return true;
    return false;
}

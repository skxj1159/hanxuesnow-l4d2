// ============================================================
//  IdentifierSystem.cpp —— Storm 式实体识别实现
//  移植自 Storm Estimate_Velocity.hpp:8-111（Get_Identifier）
// ============================================================

#include "Include.h"
#include "IdentifierSystem.h"

#include <cctype>
#include <cstring>

namespace
{
    bool ContainsLowerNoCase(const char* haystack, const char* needle)
    {
        if (!haystack || !needle || !needle[0]) return false;
        const size_t n = std::strlen(needle);
        for (const char* p = haystack; *p; ++p)
        {
            size_t k = 0;
            while (k < n && p[k] &&
                   std::tolower((unsigned char)p[k]) ==
                   std::tolower((unsigned char)needle[k]))
                ++k;
            if (k == n) return true;
        }
        return false;
    }

    // ---- Storm 的核心集合（Estimate_Velocity.hpp:21）----
    // 只有这些 ID 会进入"敌人/幸存者"分支，其余走装备分支。
    bool IsPrimaryTarget(int id)
    {
        switch (id)
        {
        case sid::Boomer: case sid::Rock:     case sid::Charger:
        case sid::Survivor: case sid::Hunter: case sid::Infected:
        case sid::Jockey: case sid::Smoker:  case sid::Spitter:
        case 275:          case sid::Tank:    case sid::Witch:
            return true;
        default:
            return false;
        }
    }

    // Storm 装备白名单（Estimate_Velocity.hpp:77）：73/105/109/121/256/260
    bool IsEquipment(int id)
    {
        switch (id)
        {
        case sid::Aid: case sid::Adrenaline: case sid::Defibrillator:
        case sid::Pain: case sid::Ammo: case 260:
            return true;
        default:
            return false;
        }
    }
}

int IdentifierFromZombieClass(int zombieClass)
{
    switch (zombieClass)
    {
    case 1: return sid::Smoker;     // C_Smoker
    case 2: return sid::Boomer;     // C_Boomer（Storm 记作 0，不是 2）
    case 3: return sid::Hunter;
    case 4: return sid::Spitter;
    case 5: return sid::Jockey;
    case 6: return sid::Charger;    // Storm 记作 99，不是 6
    case 8: return sid::Tank;
    default: return -1;
    }
}

int EquipmentIdentifier(int subtype)
{
    switch (subtype)
    {
    case 12: return sid::Aid;           // 急救包
    case 15: return sid::Pain;          // 止疼药
    case 23: return sid::Adrenaline;    // 肾上腺素
    case 24: return sid::Defibrillator; // 除颤器
    default: return -1;
    }
}

int IdentifierFromModelName(const char* modelName)
{
    if (!modelName || !modelName[0]) return -1;

    // ---- 场景道具优先 ----
    // 这些模型的路径里带独立关键字，先判以免 "ammo" 之类误伤 infected。
    if (ContainsLowerNoCase(modelName, "props") ||
        ContainsLowerNoCase(modelName, "rock") ||
        ContainsLowerNoCase(modelName, "stone"))
        return sid::Rock;
    if (ContainsLowerNoCase(modelName, "defib")) return sid::Defibrillator;
    if (ContainsLowerNoCase(modelName, "adrenaline")) return sid::Adrenaline;
    if (ContainsLowerNoCase(modelName, "painkiller") ||
        ContainsLowerNoCase(modelName, "pain_killer") ||
        ContainsLowerNoCase(modelName, "/pain"))
        return sid::Pain;
    if (ContainsLowerNoCase(modelName, "firstaid") ||
        ContainsLowerNoCase(modelName, "first_aid") ||
        ContainsLowerNoCase(modelName, "/aid"))
        return sid::Aid;
    if (ContainsLowerNoCase(modelName, "ammo")) return sid::Ammo;

    // ---- 人物 ----
    if (ContainsLowerNoCase(modelName, "witch"))   return sid::Witch;
    // 注意顺序：tank 必须在 infected 之前判，否则
    // "infected/tank.mdl" 会被下面那条 infected 关键字吃掉。
    if (ContainsLowerNoCase(modelName, "tank"))    return sid::Tank;
    if (ContainsLowerNoCase(modelName, "smoker"))  return sid::Smoker;
    if (ContainsLowerNoCase(modelName, "boomer") ||
        ContainsLowerNoCase(modelName, "bloater")) return sid::Boomer;
    if (ContainsLowerNoCase(modelName, "hunter"))  return sid::Hunter;
    if (ContainsLowerNoCase(modelName, "spitter")) return sid::Spitter;
    if (ContainsLowerNoCase(modelName, "jockey"))  return sid::Jockey;
    if (ContainsLowerNoCase(modelName, "charger")) return sid::Charger;

    // 幸存者模型路径为 survivors/<name>.mdl；感染者普通小僵尸为 infected/common_*.mdl
    if (ContainsLowerNoCase(modelName, "survivor")) return sid::Survivor;
    if (ContainsLowerNoCase(modelName, "common") ||
        ContainsLowerNoCase(modelName, "infected") ||
        ContainsLowerNoCase(modelName, "zombie"))
        return sid::Infected;

    return -1;
}

int StormIdentify(uintptr_t ent, const char* modelName,
                  int localTeam, const Vec3& localPos, const Vec3& entPos,
                  int zombieClass, StormTarget& out)
{
    out = StormTarget{};
    out.self = ent;

    // Storm 存的是**平方**距离，排序时不开根号（同distance^2 的比较等价）
    const float dx = localPos.x - entPos.x;
    const float dy = localPos.y - entPos.y;
    const float dz = localPos.z - entPos.z;
    out.distanceSq = dx * dx + dy * dy + dz * dz;

    int id = IdentifierFromModelName(modelName);

    // 模型名没认出，用阵营 + zombieClass 兜底（等价的第二条取数路径）
    if (id < 0)
    {
        if (localTeam == 3 && zombieClass >= 1 && zombieClass <= 8)
            id = IdentifierFromZombieClass(zombieClass);
        else if (localTeam == 2)
            id = sid::Survivor;
    }

    if (id < 0)
        return out.id = -1;

    if (IsPrimaryTarget(id))
    {
        // Storm 的收尾（Estimate_Velocity.hpp:68）：
        //   return (Identifier - 232) % 43 ? Identifier : 232;
        // 即 275 被强制归为 232（幸存者）。我们保留同样语义。
        if ((id - sid::Survivor) % 43 == 0)
            id = sid::Survivor;

        // 感染者再按 zombieClass 反查一次（Storm 在 team==3 时无条件走这段，
        // 因为幸存者被控制成特感后模型名不变、只有 zombieClass 变）
        if (localTeam == 3 && zombieClass >= 1 && zombieClass <= 8)
        {
            const int byClass = IdentifierFromZombieClass(zombieClass);
            if (byClass >= 0)
                id = byClass;
        }

        out.id          = id;
        out.team        = (localTeam == 3) ? 3 : 2;
        out.zombieClass = zombieClass;
        if (id == sid::Survivor)
            out.survivorId = zombieClass;   // 调用方在命中后回填真实角色 ID
        return out.id;
    }

    if (IsEquipment(id))
    {
        // Storm 要求装备未被拾取（*Entity+312 == INVALID_HANDLE_VALUE）才画。
        // 我们没有该句柄的统一判据，用"仍在场上且血量语义不成立"代替：
        // 装备不应有存活动画/攻击序列，这里交给调用方用 lifeState 把关，
        // 本函数只负责给出**带符号**的道具 ID。
        out.id   = -id;
        out.team = 1;      // 与 Storm 一致：道具在它那里被改写成 1
        return out.id;
    }

    return out.id = -1;
}
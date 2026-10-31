#pragma once
// ============================================================
//  Visibility —— ESP 可见性判定（掩体两色）
// ------------------------------------------------------------
//  语义（用户定义）：
//    · 僵尸在掩体外                → 可见（用各类自定义色）
//    · 被可穿透物挡住（木墙/石膏等）→ 可见（能看见也能穿透）
//    · 被不可穿透物挡住（混凝土等） → 不可见（用 g_colHidden）
//    · 玻璃（看得见但打不穿）       → 不可见
//
//  实现：EngineTraceClient003::TraceRay（槽 5，实测结构来自已删的
//  自瞄体系，备份在 Source/_removed_aim_20261018/Aim/AimEngine.h）。
//  射线从本地眼位到目标胸口，按命中表面材质名分类：
//    可穿透白名单：wood/plaster/drywall/grate/fence/foliage/cardboard…
//    玻璃：glass（无论命中谁）
//    命中其他实体（僵尸/队友，非世界）：视为软遮挡，可见
//
//  性能：TraceRay 有成本 → 每实体 100ms 限频缓存，帧内只读缓存值。
// ============================================================

#include "Include.h"

// 引擎 trace 接口就绪后返回 true；未就绪时 VisIsVisible 恒 true（全按可见画）
bool VisInit();
bool VisReady();

// entIndex：限频缓存键；eye：本地眼位；target：目标胸口世界坐标
bool VisIsVisible(int entIndex, const Vec3& eye, const Vec3& target);

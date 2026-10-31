#pragma once
#include <string>

void HandleMenuToggle();
void DrawMenu();
void DrawCallBack();

// 页面渲染（扁平导航：一页 = 一屏两列卡片，无子标签）
// 2026-10-18 随 Storm 全部功能移植重排为 7 页：
//   视觉 / 瞄准 / 过检测 / 战斗 / 移动 / 准星 / 系统
// 原「颜色」页已并入视觉页（Storm 配色硬编码，只有只读图例）。
void DrawPageVisual();
void DrawPageAim();
void DrawPageAntiCheat();
void DrawPageCombat();
void DrawPageMove();
void DrawPageCrosshair();
void DrawPageSystem();

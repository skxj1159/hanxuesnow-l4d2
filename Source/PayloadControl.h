#pragma once
// ============================================================
// PayloadControl —— 注入载荷控制接口
// 由 dllmain.cpp 实现（仅内部/注入模式编译）。
// Menu 通过它提供"卸载注入"能力。
// ============================================================

// 停止全部功能：实体线程、自瞄、渲染 hook，保存 ESP 配置，
// 并恢复游戏窗口过程。之后 DLL 保持常驻但不做任何事（重启游戏彻底清除）。
void PayloadRequestUnload();

// 公共日志：写入 DLL 目录下 payload.log（任意线程可调用，文件未打开时静默）
void PayloadLog(const char* fmt, ...);

// [批111] 关闭日志的常驻文件句柄。仅供 DllMain(DLL_PROCESS_DETACH)
// 调用，确保游戏退出时尾部日志落盘、DLL 卸载后不留悬空句柄。
// 不要在别处调用：正常路径下句柄由 PayloadLog 惰性管理。
void PayloadLogClose();

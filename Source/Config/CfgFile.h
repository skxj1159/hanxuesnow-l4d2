// ============================================================
//  Config/CfgFile.h —— 人类可读、可分享的 cfg 配置
// ------------------------------------------------------------
//  为什么单独做一套：
//    esp_config.json / aim_config.ini / config.json 三路是内部
//    持久化格式，机器读写没问题，但**不适合分享** —— JSON 里全是
//    英文键名与嵌套结构，别人拿到手没法改，也没法只改一两项。
//
//  cfg 的目标：
//    · 纯文本、一行一项、键名与菜单语义对应，能直接手改
//    · 一份文件 = 一套完整玩法设置，发给别人就能用
//    · 导入时对每一项做范围钳位，防止别人的极端值把本地搞坏
//    · 未知键只计入 ignored，不报错 —— 版本不同也能尽量读
//
//  存放位置：%LOCALAPPDATA%\l4d2_payload\cfg\
//    分享 = 把这个目录里的 .cfg 发出去
//    导入 = 把别人的 .cfg 丢进这个目录，菜单里点一下
// ============================================================
#pragma once

#include <string>
#include <vector>

namespace CfgFile
{
    // cfg 目录（不存在会自动创建）
    std::string Dir();

    // 默认文件名：Dir()\hanxuesnow.cfg
    std::string DefaultPath();

    // 写出一份完整 cfg。成功返回 true，失败时 *err 带原因。
    bool Save(const std::string& path, std::string* err = nullptr);

    // 读取并应用。返回是否成功打开文件；
    // *applied = 实际应用的项数，*ignored = 未知/非法项数。
    bool Load(const std::string& path, int* applied = nullptr,
              int* ignored = nullptr, std::string* err = nullptr);

    // 目录下所有 .cfg 的文件名（不含路径），按字母序。
    std::vector<std::string> List();
}

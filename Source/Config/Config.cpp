// ============================================================
//  Config/Config.cpp —— 全局配置：预设切换 + 键表驱动持久化
// ------------------------------------------------------------
//  【本批（第一批：底层与配置）的职责】
//    1) g_Config 单例定义
//    2) ApplyPreset：Default / Legit / Aggressive / Silent 四预设
//    3) Save / Load：键表驱动精简 JSON（自包含，零第三方库）
//    4) Tick（热加载）/ SaveNow / RegisterHotkeys 占位
//
//  【为什么不复用旧的 EspConfig（893 行）】
//  旧持久化表与运行时变量是两套手抄代码，改一个功能要同步三处，
//  已经漂移过。这里用【键表】：一张表同时描述"存什么"和"存到哪"，
//  Save/Load 都是遍历同一张表 —— 天然不会漏字段、不会漂移。
//  新功能加配置 = 结构体加一个字段 + 键表加一行，两步完成。
//
//  【文件位置】%LOCALAPPDATA%\l4d2_payload\config.json
//  （DLL 内拿不到可靠的自身路径：GetModuleFileName 需要 DLL 句柄，
//   从 DllMain 传参会让 Config 层依赖入口层，违反分层。
//   LOCALAPPDATA 稳定、随用户走，卸载时删目录即可。）
// ============================================================

#include "Config.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ============================================================
//  全局配置实例（头文件 extern 声明，全程序仅此一份）
// ============================================================
Config g_Config;

namespace
{
    // ============================================================
    //  键表：type 0=bool  1=int  2=float
    // ============================================================
    struct KV
    {
        const char* key;
        int   type;
        void* p;
    };

    const KV kKeys[] =
    {
        // ---- ESP ----
        { "esp.box",           0, &g_Config.esp.box },
        { "esp.name",          0, &g_Config.esp.name },
        { "esp.healthBar",     0, &g_Config.esp.healthBar },
        { "esp.healthText",    0, &g_Config.esp.healthText },
        { "esp.distance",      0, &g_Config.esp.distance },
        { "esp.boxFill",       0, &g_Config.esp.boxFill },
        { "esp.showAllies",    0, &g_Config.esp.showAllies },
        { "esp.equipment",     0, &g_Config.esp.equipment },
        { "esp.boxPadding",    2, &g_Config.esp.boxPadding },
        { "esp.healthBarW",    2, &g_Config.esp.healthBarW },
        { "esp.nameSize",      2, &g_Config.esp.nameSize },
        { "esp.distanceSize",  2, &g_Config.esp.distanceSize },
        { "esp.healthSize",    2, &g_Config.esp.healthSize },
        { "esp.equipmentDist", 2, &g_Config.esp.equipmentDist },
        { "esp.geometrySource",1, &g_Config.esp.geometrySource },
        // ---- Aimbot ----
        { "aim.enabled",    0, &g_Config.aim.enabled },
        { "aim.key",        1, &g_Config.aim.key },
        { "aim.maxDistance",2, &g_Config.aim.maxDistance },
        { "aim.hitboxTable",0, &g_Config.aim.hitboxTable },
        { "aim.priority",   0, &g_Config.aim.priority },
        { "aim.hitVerify",  0, &g_Config.aim.hitVerify },
        { "aim.recoilComp", 0, &g_Config.aim.recoilComp },
        { "aim.showRange",  0, &g_Config.aim.showRange },
        // ---- Silent ----
        { "silent.enabled",  0, &g_Config.silent.enabled },
        { "silent.fov",      2, &g_Config.silent.fov },
        { "silent.autoFire", 0, &g_Config.silent.autoFire },
        // ---- Humanize ----
        { "humanize.enabled",        0, &g_Config.humanize.enabled },
        { "humanize.reactionMs",     1, &g_Config.humanize.reactionMs },
        { "humanize.reactionMsMax",  1, &g_Config.humanize.reactionMsMax },
        { "humanize.smoothFactor",   2, &g_Config.humanize.smoothFactor },
        { "humanize.jitterAngle",    2, &g_Config.humanize.jitterAngle },
        { "humanize.deadzone",       2, &g_Config.humanize.deadzone },
        { "humanize.strafe",         0, &g_Config.humanize.strafe },
        { "humanize.moveFix",        0, &g_Config.humanize.moveFix },
        // ---- Combat ----
        { "combat.noSpread",       0, &g_Config.combat.noSpread },
        { "combat.bhop",           0, &g_Config.combat.bhop },
        { "combat.autoFire",       0, &g_Config.combat.autoFire },
        { "combat.autoFireMs",     1, &g_Config.combat.autoFireMs },
        { "combat.penetration",    0, &g_Config.combat.penetration },
        { "combat.penetrationDmg", 2, &g_Config.combat.penetrationDmg },
        // ---- AntiCheat ----
        { "anti.extraCommands",    0, &g_Config.anti.extraCommands },
        { "anti.extraCount",       1, &g_Config.anti.extraCount },
        { "anti.extraAction",      1, &g_Config.anti.extraAction },
        { "anti.interpolateExtra", 0, &g_Config.anti.interpolateExtra },
        { "anti.seqShift",         0, &g_Config.anti.seqShift },
        { "anti.seqReserve",       1, &g_Config.anti.seqReserve },
        { "anti.noClockCorr",      0, &g_Config.anti.noClockCorr },
        // ---- Crosshair ----
        { "crosshair.enabled",    0, &g_Config.crosshair.enabled },
        { "crosshair.radius",     1, &g_Config.crosshair.radius },
        { "crosshair.rotationR",  1, &g_Config.crosshair.rotationR },
        { "crosshair.segments",   1, &g_Config.crosshair.segments },
        { "crosshair.iterations", 1, &g_Config.crosshair.iterations },
        { "crosshair.speed",      2, &g_Config.crosshair.speed },
    };

    constexpr int kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

    // ============================================================
    //  配置文件路径：%LOCALAPPDATA%\l4d2_payload\config.json
    // ============================================================
    std::string ConfigPath()
    {
        char dir[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH) || !dir[0])
            return std::string();   // 拿不到环境变量：放弃持久化（不致命）
        return std::string(dir) + "\\l4d2_payload\\config.json";
    }

    // ============================================================
    //  序列化（写）
    // ============================================================
    bool WriteJson(const std::string& path)
    {
        // 目录不存在则创建（单层，CreateDirectoryA 够用）
        const size_t slash = path.find_last_of('\\');
        if (slash != std::string::npos)
            CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);

        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
            return false;

        fputs("{\n", f);
        for (int i = 0; i < kKeyCount; ++i)
        {
            const KV& kv = kKeys[i];
            char line[128];
            switch (kv.type)
            {
            case 0:  // bool
                snprintf(line, sizeof(line), "  \"%s\": %s%s\n",
                         kv.key, *(bool*)kv.p ? "true" : "false",
                         i + 1 < kKeyCount ? "," : "");
                break;
            case 1:  // int
                snprintf(line, sizeof(line), "  \"%s\": %d%s\n",
                         kv.key, *(int*)kv.p,
                         i + 1 < kKeyCount ? "," : "");
                break;
            default: // float
                snprintf(line, sizeof(line), "  \"%s\": %.3f%s\n",
                         kv.key, *(float*)kv.p,
                         i + 1 < kKeyCount ? "," : "");
                break;
            }
            fputs(line, f);
        }
        fputs("}\n", f);
        fclose(f);
        return true;
    }

    // ============================================================
    //  反序列化（读）—— 不需要真正的 JSON 解析器：
    //  本文件是自家 WriteJson 写出的平铺键值，"key": value 定位即可。
    //  键表里没有的键自动忽略 → 旧版本配置文件向后兼容。
    // ============================================================
    bool ReadJson(const std::string& path)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
            return false;

        std::string text;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        fclose(f);

        for (int i = 0; i < kKeyCount; ++i)
        {
            const KV& kv = kKeys[i];
            char pat[80];
            snprintf(pat, sizeof(pat), "\"%s\"", kv.key);
            const char* p = std::strstr(text.c_str(), pat);
            if (!p) continue;                       // 键缺失：保留默认值
            p = std::strchr(p + std::strlen(pat), ':');
            if (!p) continue;
            ++p;                                    // 跳过 ':'，指向值
            switch (kv.type)
            {
            case 0:  // bool（true / false / 0 之外数字）
                *(bool*)kv.p = (std::strncmp(p, "true", 4) == 0);
                break;
            case 1:
                *(int*)kv.p = std::atoi(p);
                break;
            default:
                *(float*)kv.p = (float)std::atof(p);
                break;
            }
        }
        return true;
    }
} // anonymous namespace

// ============================================================
//  ConfigMgr
// ============================================================

namespace ConfigMgr
{
    // ------------------------------------------------------------
    //  预设切换
    // ------------------------------------------------------------
    //  做法：先整体重置到结构体默认值（头文件里的初始化器就是
    //  Default 预设），再叠加预设差异 —— 保证预设之间不残留上一次
    //  的脏开关。menuOpen / espPreview 属于界面状态，切换时保留。
    // ------------------------------------------------------------
    void ApplyPreset(Preset p)
    {
        const bool keepMenu  = g_Config.menuOpen;
        const bool keepPrev  = g_Config.espPreview;

        g_Config = Config{};   // 回 Default 基线

        switch (p)
        {
        case Preset::Legit:
            // 拟人化优先：小范围、强平滑、随机反应延迟、不开无扩散
            g_Config.aim.enabled      = true;
            g_Config.aim.maxDistance  = 35.0f;
            g_Config.aim.hitVerify    = true;
            g_Config.aim.recoilComp   = true;
            g_Config.humanize.enabled    = true;
            g_Config.humanize.reactionMs    = 60;
            g_Config.humanize.reactionMsMax = 160;
            g_Config.humanize.smoothFactor  = 0.55f;
            g_Config.humanize.jitterAngle   = 0.8f;
            g_Config.humanize.deadzone      = 0.4f;
            g_Config.combat.noSpread     = false;
            g_Config.anti.extraCommands  = false;   // 拟人包连补包都收着
            break;

        case Preset::Aggressive:
            // 大范围、无平滑、伤害模型全开
            g_Config.aim.enabled      = true;
            g_Config.aim.maxDistance  = 80.0f;
            g_Config.aim.hitVerify    = false;
            g_Config.aim.recoilComp   = true;
            g_Config.silent.fov       = 360.0f;
            g_Config.combat.noSpread    = true;
            g_Config.combat.penetration = true;
            g_Config.anti.extraCommands = true;
            break;

        case Preset::Silent:
            // 全静默：视角不跟手，只改 CUserCmd
            g_Config.aim.enabled      = false;
            g_Config.silent.enabled   = true;
            g_Config.silent.fov       = 180.0f;
            g_Config.silent.autoFire  = true;
            g_Config.combat.noSpread  = true;
            g_Config.humanize.strafe  = true;   // 静默常态配完美横移
            break;

        case Preset::Default:
        default:
            break;   // 结构体默认值即 Default 预设
        }

        g_Config.menuOpen   = keepMenu;
        g_Config.espPreview = keepPrev;

        // [批46] 预设同样走钳位（将来新增预设若忘了边界，这里兜住）
        g_Config.combat.Clamp();

        // 预设切换立即落盘，下次注入直接是这套
        SaveNow();
    }

    const char* PresetName(Preset p)
    {
        switch (p)
        {
        case Preset::Legit:      return "Legit";
        case Preset::Aggressive: return "Aggressive";
        case Preset::Silent:     return "Silent";
        case Preset::Default:
        default:                 return "Default";
        }
    }

    // ------------------------------------------------------------
    //  持久化
    // ------------------------------------------------------------
    bool Save()
    {
        const std::string path = ConfigPath();
        if (path.empty()) return false;
        return WriteJson(path);
    }

    bool Load()
    {
        const std::string path = ConfigPath();
        if (path.empty()) return false;
        if (!ReadJson(path)) return false;   // 无文件：全默认值（首次运行）

        // [批46] 加载后统一钳位。配置文件是外部输入，手改 JSON 就能
        // 写出 autoFireMs=10 这种值 —— 那会直接撞穿反检测红线。
        // 因此钳位必须在【读取边界】做一次，而不是只靠菜单滑条。
        g_Config.combat.Clamp();
        return true;
    }

    // 热加载轮询：渲染线程每帧调。当前版本菜单里改配置即改内存，
    // 保存由菜单关闭 / 预设切换 / 卸载时触发，暂无外部改文件需求，
    // 故不做文件时间戳轮询。接上菜单热重载后在这里加：
    //   if (GetTickCount64() - s_lastCheck < 500) return;
    //   s_lastCheck = ...; 比对 WriteTime 变了 → ReadJson。
    // [批111·死代码] 删除了 ConfigMgr::Tick()（空函数体）—— 全项目零调用。
    //   配置文件变更检测始终没有接上（没有任何地方调 Tick）。
    //   配置在菜单交互时由 CfgFile 显式读写，不依赖后台轮询。
    //   若将来要加"外部改了就热重载"，重新写一个真正的 Tick 即可。

    void SaveNow()
    {
        Save();
    }

    // [批52] 带返回值的保存：菜单需要用它给出真实的成功/失败提示。
    // Save() 内部失败（路径取不到 LOCALAPPDATA / fopen 失败）都返回
    // false，原来的 SaveNow() 恒 void，菜单只能无脑显示"已保存"。
    bool TrySaveNow()
    {
        return Save();
    }

    // ------------------------------------------------------------
    //  热键
    // ------------------------------------------------------------
    //  RegisterHotKey 依赖消息循环，注入 DLL 里没有自己的窗口线程，
    //  不适用。按键走轮询（GetAsyncKeyState），轮询点在 Hooks 层的
    //  帧回调里（第四批接线）。此处保留入口，将来要注册系统级组合键
    //  （如 Win+X）时再填。
    void RegisterHotkeys()
    {
    }
} // namespace ConfigMgr

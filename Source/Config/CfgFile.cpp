// ============================================================
//  Config/CfgFile.cpp —— cfg 读写实现
// ------------------------------------------------------------
//  格式（每行一项，`#` 或 `//` 开头为注释）：
//      set aim_fov      4.0
//      set aim_enabled  1
//      set autoFireMs   60
//
//  解析刻意宽容：
//    · `set ` 前缀可省略（直接 `aim_fov 4.0` 也认）
//    · `=` 可代替空格（`aim_fov = 4.0`）
//    · 多余空白、行尾注释都忽略
//    · 未知键只计 ignored，不中断导入
//  这样别人手改的文件、不同版本导出的文件都能尽量读进来。
//
//  安全：每个数值字段都带 [lo, hi] 区间，导入时强制钳位。
//    别人的 cfg 可能是编辑器手滑写出 9999，直接套用会让自瞄
//    FOV 变成全屏锁定 —— 钳位把"别人的文件"限制在可玩范围内。
// ============================================================
#include "CfgFile.h"

#include "Config.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace CfgFile
{
namespace
{
    // ---- 字段描述 ----
    //  type: 'b' bool / 'i' int / 'f' float
    //  lo,hi: 允许范围（bool 忽略）
    struct Field
    {
        const char* name;
        char        type;
        void*       ptr;
        double      lo;
        double      hi;
    };

    // ⚠ 加字段只改这张表：Save / Load 都从它生成，
    //   不会出现"保存了但读不回来"的两处不一致。
    Field g_fields[] =
    {
        // ---- 自瞄 ----
        { "aim_enabled",      'b', &g_Config.aim.enabled,      0, 1 },
        { "aim_key",          'i', &g_Config.aim.key,          0, 255 },
        { "aim_fov",          'f', &g_Config.aim.fov,          0.5, 15.0 },
        { "aim_showFov",      'b', &g_Config.aim.showFov,      0, 1 },
        { "aim_silent",       'b', &g_Config.aim.silent,       0, 1 },
        { "aim_silentFov",    'f', &g_Config.aim.silentFov,    1.0, 15.0 },
        { "aim_part",         'i', &g_Config.aim.aimPart,      0, 4 },
        { "aim_distWeight",   'f', &g_Config.aim.distWeight,   0.0, 1.0 },
        { "aim_friendlyFire", 'b', &g_Config.aim.friendlyFire, 0, 1 },
        { "aim_maxDistance",  'f', &g_Config.aim.maxDistance,  1.0, 200.0 },
        { "aim_hitVerify",    'b', &g_Config.aim.hitVerify,    0, 1 },
        { "aim_recoilComp",   'b', &g_Config.aim.recoilComp,   0, 1 },
        { "aim_showRange",    'b', &g_Config.aim.showRange,    0, 1 },
        { "aim_priority",     'b', &g_Config.aim.priority,     0, 1 },
        { "aim_hitboxTable",  'b', &g_Config.aim.hitboxTable,  0, 1 },

        // ---- 静默（旧结构，保留兼容）----
        { "silent_enabled",   'b', &g_Config.silent.enabled,   0, 1 },
        { "silent_fov",       'f', &g_Config.silent.fov,       1.0, 180.0 },

        // ---- 拟人化 ----
        { "humanize_enabled",   'b', &g_Config.humanize.enabled,      0, 1 },
        { "humanize_reactionMs",'i', &g_Config.humanize.reactionMs,   0, 400 },
        { "humanize_reactionMax",'i',&g_Config.humanize.reactionMsMax,0, 600 },
        { "humanize_smooth",    'f', &g_Config.humanize.smoothFactor, 0.05, 0.95 },
        { "humanize_jitter",    'f', &g_Config.humanize.jitterAngle,  0.0, 0.5 },
        { "humanize_deadzone",  'f', &g_Config.humanize.deadzone,     0.0, 2.0 },
        { "humanize_strafe",    'b', &g_Config.humanize.strafe,       0, 1 },
        { "humanize_moveFix",   'b', &g_Config.humanize.moveFix,      0, 1 },

        // ---- 射击 / 移动 ----
        { "noSpread",      'b', &g_Config.combat.noSpread,       0, 1 },
        { "bhop",          'b', &g_Config.combat.bhop,           0, 1 },
        { "autoFire",      'b', &g_Config.combat.autoFire,       0, 1 },
        { "autoFireMs",    'i', &g_Config.combat.autoFireMs,     52, 400 },
        { "autoFireKey",   'i', &g_Config.combat.autoFireKey,    0, 255 },
        { "penetration",   'b', &g_Config.combat.penetration,    0, 1 },
        { "penetrationDmg",'f', &g_Config.combat.penetrationDmg, 1.0, 100.0 },
        { "noRecoilVis",   'b', &g_Config.combat.noRecoilVis,    0, 1 },

        // ---- 过检测 ----
        { "extraCommands",    'b', &g_Config.anti.extraCommands,    0, 1 },
        { "extraCount",       'i', &g_Config.anti.extraCount,       0, 14 },
        { "extraAction",      'i', &g_Config.anti.extraAction,      0, 64 },
        { "interpolateExtra", 'b', &g_Config.anti.interpolateExtra, 0, 1 },
        { "seqShift",         'b', &g_Config.anti.seqShift,         0, 1 },
        { "seqReserve",       'i', &g_Config.anti.seqReserve,       0, 64 },
        { "noClockCorr",      'b', &g_Config.anti.noClockCorr,      0, 1 },

        // ---- 准星 ----
        { "crosshair_enabled", 'b', &g_Config.crosshair.enabled,    0, 1 },
        { "crosshair_radius",  'i', &g_Config.crosshair.radius,     1, 64 },
        { "crosshair_rotationR",'i',&g_Config.crosshair.rotationR,  0, 128 },
        { "crosshair_segments",'i', &g_Config.crosshair.segments,   3, 64 },
        { "crosshair_iterations",'i',&g_Config.crosshair.iterations,1, 64 },
        { "crosshair_speed",   'f', &g_Config.crosshair.speed,      30.0, 3000.0 },
    };

    constexpr int kFieldCount = (int)(sizeof(g_fields) / sizeof(g_fields[0]));

    std::string EnsureDir(const std::string& dir)
    {
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }

    std::string BaseDir()
    {
        char buf[MAX_PATH] = {};
        if (!GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH) || !buf[0])
            return std::string(".");
        return std::string(buf) + "\\l4d2_payload";
    }

    bool IsSpace(char c) { return c == ' ' || c == '\t'; }

    std::string Trim(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && IsSpace(s[a])) ++a;
        while (b > a && (IsSpace(s[b - 1]) || s[b - 1] == '\r' || s[b - 1] == '\n'))
            --b;
        return s.substr(a, b - a);
    }

    // 去掉行尾注释（不在引号内的 // 或 #）
    std::string StripComment(const std::string& s)
    {
        for (size_t i = 0; i + 1 < s.size(); ++i)
        {
            if (s[i] == '/' && s[i + 1] == '/')
                return s.substr(0, i);
        }
        const size_t h = s.find('#');
        return (h == std::string::npos) ? s : s.substr(0, h);
    }

    void ApplyField(const Field& f, double v)
    {
        if (f.type == 'b')
        {
            *reinterpret_cast<bool*>(f.ptr) = (v != 0.0);
            return;
        }
        // 钳位：别人的 cfg 不能把本地设成离谱值
        if (v < f.lo) v = f.lo;
        if (v > f.hi) v = f.hi;

        if (f.type == 'i')
            *reinterpret_cast<int*>(f.ptr) = (int)(v < 0 ? v - 0.5 : v + 0.5);
        else
            *reinterpret_cast<float*>(f.ptr) = (float)v;
    }

    double ReadField(const Field& f)
    {
        if (f.type == 'b')
            return *reinterpret_cast<bool*>(f.ptr) ? 1.0 : 0.0;
        if (f.type == 'i')
            return (double)*reinterpret_cast<int*>(f.ptr);
        return (double)*reinterpret_cast<float*>(f.ptr);
    }
} // namespace

    std::string Dir()
    {
        return EnsureDir(BaseDir() + "\\cfg");
    }

    std::string DefaultPath()
    {
        return Dir() + "\\hanxuesnow.cfg";
    }

    bool Save(const std::string& path, std::string* err)
    {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp)
        {
            if (err) *err = "无法写入：" + path;
            return false;
        }

        fprintf(fp,
            "// ============================================================\r\n"
            "//  寒山雪 Hanshanxue  ——  配置文件 (cfg v1)\r\n"
            "// ------------------------------------------------------------\r\n"
            "//  用法：\r\n"
            "//    1) 这是纯文本，可以直接用记事本改\r\n"
            "//    2) 改完放进配置目录的 cfg 文件夹，菜单「系统 - 配置」里点一下导入\r\n"
            "//    3) 想分享给别人，把这个文件发过去即可\r\n"
            "//  语法：每行一项  set 键名 值   （set 可省略；# 或 // 后为注释）\r\n"
            "// ============================================================\r\n\r\n");

        // 分段输出，便于人读 —— 顺序与菜单页一致
        auto dump = [&](const char* header, const char* prefix) {
            fprintf(fp, "%s\r\n", header);
            for (int i = 0; i < kFieldCount; ++i)
            {
                const Field& f = g_fields[i];
                if (prefix && prefix[0] &&
                    std::strncmp(f.name, prefix, std::strlen(prefix)) != 0)
                    continue;
                const double v = ReadField(f);
                if (f.type == 'f')
                    fprintf(fp, "set %-24s %.3f\r\n", f.name, v);
                else
                    fprintf(fp, "set %-24s %d\r\n", f.name,
                            (int)(v < 0 ? v - 0.5 : v + 0.5));
            }
            fprintf(fp, "\r\n");
        };

        dump("// ---- 自瞄 ----", "aim_");
        dump("// ---- 静默 ----", "silent_");
        dump("// ---- 拟人化 ----", "humanize_");

        fprintf(fp, "// ---- 射击 / 移动 ----\r\n");
        for (int i = 0; i < kFieldCount; ++i)
        {
            const Field& f = g_fields[i];
            if (std::strchr(f.name, '_')) continue;      // 无前缀组
            const double v = ReadField(f);
            if (f.type == 'f')
                fprintf(fp, "set %-24s %.3f\r\n", f.name, v);
            else
                fprintf(fp, "set %-24s %d\r\n", f.name, (int)(v < 0 ? v - 0.5 : v + 0.5));
        }
        fprintf(fp, "\r\n");

        dump("// ---- 准星 ----", "crosshair_");

        fclose(fp);
        return true;
    }

    bool Load(const std::string& path, int* applied, int* ignored,
              std::string* err)
    {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp)
        {
            if (err) *err = "无法打开：" + path;
            return false;
        }

        int nApplied = 0, nIgnored = 0;
        char line[512];

        while (fgets(line, sizeof(line), fp))
        {
            std::string s = Trim(StripComment(std::string(line)));
            if (s.empty()) continue;

            // 去 `set ` 前缀
            if (s.size() > 4 &&
                (s.compare(0, 4, "set ") == 0 || s.compare(0, 4, "set\t") == 0))
                s = Trim(s.substr(4));

            // 拆 键 / 值：空格或 '=' 分隔
            size_t sep = std::string::npos;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (IsSpace(s[i]) || s[i] == '=') { sep = i; break; }
            }
            if (sep == std::string::npos) { ++nIgnored; continue; }

            const std::string key = s.substr(0, sep);
            std::string val = Trim(s.substr(sep));
            if (!val.empty() && val[0] == '=') val = Trim(val.substr(1));
            if (key.empty() || val.empty()) { ++nIgnored; continue; }

            bool matched = false;
            for (int i = 0; i < kFieldCount; ++i)
            {
                const Field& f = g_fields[i];
                if (key != f.name) continue;

                if (f.type == 'b')
                {
                    // 支持 1/0 与 true/false/on/off
                    double v = 0.0;
                    if (_stricmp(val.c_str(), "true") == 0 ||
                        _stricmp(val.c_str(), "on") == 0 ||
                        _stricmp(val.c_str(), "yes") == 0)
                        v = 1.0;
                    else
                        v = std::atof(val.c_str());
                    ApplyField(f, v);
                }
                else
                {
                    const double v = std::atof(val.c_str());
                    if (!std::isfinite(v)) { ++nIgnored; matched = true; break; }
                    ApplyField(f, v);
                }
                ++nApplied;
                matched = true;
                break;
            }
            if (!matched) ++nIgnored;
        }
        fclose(fp);

        // 导入后统一兜底钳位（与配置加载走同一道闸）
        g_Config.combat.Clamp();

        if (applied) *applied = nApplied;
        if (ignored) *ignored = nIgnored;
        return true;
    }

    std::vector<std::string> List()
    {
        std::vector<std::string> out;
        const std::string dir = Dir();
        WIN32_FIND_DATAA fd{};
        const std::string pat = dir + "\\*.cfg";
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;

        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            out.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        std::sort(out.begin(), out.end());
        return out;
    }
}

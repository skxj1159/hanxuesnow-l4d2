#include "EspConfig.h"

#include "Globals.h"          // ESP 设置与颜色全局量
#include "PayloadControl.h"   // PayloadLog

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

// ============================================================
// EspConfig —— ESP 设置持久化实现（JSON 版）
// ------------------------------------------------------------
// 并发模型：
//   · 渲染线程：EspConfigTick 热加载轮询 + 改动检测（轻量快照
//     memcmp），EspConfigSaveNow 同步落盘（卸载路径）；
//   · 保存线程：串行执行文件 IO（接收渲染线程移交的文本）；
//   · 世代号（generation）保证"先写的旧内容不会覆盖后写的新
//     内容"，未开始的旧世代写入自动作废。
//
// JSON 方案（不引入第三方库）：
//   · 序列化：由条目表驱动逐键输出（两空格缩进，便于用户手改）；
//   · 解析：迷你递归下降解析器，把嵌套对象展平为
//     (叶子键, 规范化字符串) 列表后复用既有 ApplyEntry 校验
//     （布尔 → "true"/"false"，数字原样，数组 → "a,b,c"，
//     null 忽略；携带 UTF-8 BOM / // 与 /* */ 注释容错）；
//   · 热加载：约 1s 节流轮询文件写时间（mtime），发现外部修改
//     即整体重载（解析失败保留现值）；自身写盘后的 mtime 被
//     记录并跳过，避免"保存 → 重载 → 再保存"回环。
// ============================================================

namespace
{
    constexpr ULONGLONG kSaveDebounceMs = 600;    // 改动停止约 0.6s 后落盘
    constexpr ULONGLONG kReloadPollMs   = 1000;   // 热加载轮询节流（ms）

    // ---- 保存线程共享状态（堆上常驻、不析构：注入型 DLL 依赖进程
    //      回收，避免静态析构与后台线程产生竞态）----
    struct SaverState
    {
        std::mutex              pendingMtx;     // 保护 pendingText / pendingGen
        std::condition_variable pendingCv;
        std::string             pendingText;    // 待写入内容（空 = 无待写）
        unsigned long long      pendingGen = 0;

        std::mutex              writeMtx;       // 串行化文件写入
        unsigned long long      writtenGen = 0;

        bool                    started = false;
    };

    SaverState& State()
    {
        static SaverState* s = new SaverState();   // 有意常驻，不做析构
        return *s;
    }

// ---- 设置快照（改动检测用）：纯 POD，可直接 memcmp ----
    // 数组尺寸与下面各条目表严格对应（static_assert 兜底防漂移）。
    // 2026-10-18：透视换成 Storm 体系后，colors 已无实际条目（配色硬编码），
    // 这里保留 1 个占位，避免 sizeof(x)/sizeof(x[0]) 在空表上除零。
    struct EspSnapshot
    {
        unsigned char toggles[8];    // 7 个 ESP 开关 + 小僵尸过滤
        int           styles[2];     // 填充透明度 + 占位
        float         floats[6];     // 5 绘制样式 + 框外扩（道具距离已拆除）
        float         colors[6][4];  // 5 类实体色 + 不可见色
    };

    struct BoolEntry  { const char* key; bool*    value; };
    struct IntEntry   { const char* key; int*     value; int maxValue; int minValue; };
    struct FloatEntry { const char* key; float*   value; float minValue; float maxValue; };
    struct ColorEntry { const char* key; ImColor* value; };

    // 条目分组约定（序列化输出用，勿调顺序）：
    //   每组表前 kEspXxxCount 项写入 "esp" 节，其余写入 "aim" 节。
    constexpr int kEspBoolCount  = 8;    // ESP 开关（…/预览/小僵尸过滤）
    constexpr int kEspIntCount   = 1;    // 方框填充透明度
    constexpr int kEspFloatCount = 6;    // 绘制样式（血条宽/血条长/三种字号/框外扩）

    BoolEntry g_boolEntries[] = {
        { "enableBox2D",         &enableBox2D },
        { "showAllies",          &showAllies },
        { "enableHealthBar",     &enableHealthBar },
        { "enableHealthESP",     &enableHealthESP },
        { "enableDistance",      &enableDistance },
        { "enableESPName",       &enableESPName },
        { "showEspPreview",      &showEspPreview },
        { "filterCommonInfected", &filterCommonInfected },
    };

    int g_placeholderInt = 0;   // int 表占位引用的目标（防除零）

    IntEntry g_intEntries[] = {
        // 方框填充透明度（0~255，0 = 不显示填充）—— esp 节
        { "fillAlpha", &espFillAlpha, 255, 0 },
        // 占位防 sizeof/sizeof 除零（模式同 colors 占位）
        { "__unused_int", &g_placeholderInt, 0, 0 },
    };

    FloatEntry g_floatEntries[] = {
        // ---- 绘制样式（写入 "esp" 节；勿调顺序：前 6 项）----
        { "healthBarWidth",       &healthBarWidth,       1.0f,  12.0f },  // 血条宽（px）
        { "healthBarHeightRatio", &healthBarHeightRatio, 0.20f, 1.00f },  // 血条高 = 方框高 × 比例
        { "nameTextSize",         &nameTextSize,         8.0f,  32.0f },  // 名字字号（px）
        { "distanceTextSize",     &distanceTextSize,     8.0f,  32.0f },  // 距离字号（px）
        { "healthTextSize",       &healthTextSize, 8.0f,  32.0f },  // 血量字号（px）
        // Storm Paint.hpp 原本硬编码 2px 的框外扩，现开放为参数
        { "boxPadding",         &espBoxPadding,             0.0f, 8.0f },
    };

    // 2026-10-18：24 个可调颜色已随旧透视体系删除（Storm 配色硬编码）。
    // 保留 1 个永不参与读写的占位，保证 kColorCount 计算不除零、
    // 序列化时 "colors" 节仍能输出合法空对象。
    // ---- 方框颜色（可自定义，Globals.h 全局 ImColor；colors 节持久化）----
    ColorEntry g_colorEntries[] = {
        { "colInfected", &g_colInfected },
        { "colSpecial",  &g_colSpecial  },
        { "colTank",     &g_colTank     },
        { "colWitch",    &g_colWitch    },
        { "colSurvivor", &g_colSurvivor },
        { "colHidden",   &g_colHidden   },
    };

    constexpr int kBoolCount  = (int)(sizeof(g_boolEntries) / sizeof(g_boolEntries[0]));
    constexpr int kIntCount   = (int)(sizeof(g_intEntries) / sizeof(g_intEntries[0]));
    constexpr int kFloatCount = (int)(sizeof(g_floatEntries) / sizeof(g_floatEntries[0]));
    constexpr int kColorCount = (int)(sizeof(g_colorEntries) / sizeof(g_colorEntries[0]));

    // 2026-10-18 Storm 模型：bool 22 → 21（删 silentAimOn / silentMoveFix /
    // silentSoftReturn / silentAutoFire，新增 stormOn / stormIndexTable /
    // stormPellet / stormRecoil / stormExtraCommands / stormSeqShift /
    // stormNoClockCorr / stormMoveFix / stormStrafe）
    static_assert(kBoolCount  == 8, "toggles 容量需与布尔条目数同步");
    static_assert(kIntCount   == 2, "styles 容量需与整数条目数同步");
    static_assert(kFloatCount == 6, "floats 容量需与浮点条目数同步");
    static_assert(kColorCount == 6,  "colors 容量需与颜色条目数同步");

    // DLL 所在目录（进程生命周期内不变；首次调用解析后缓存，
    // 避免热加载轮询每 1s 重复的系统调用与字符串分配）
    const std::string& GetModuleDir()
    {
        static const std::string dir = []() {
            char buf[MAX_PATH] = {};
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)&GetModuleDir, &self);
            GetModuleFileNameA(self, buf, MAX_PATH);
            std::string path = buf;
            size_t pos = path.find_last_of("\\/");
            std::string dir =
                (pos == std::string::npos) ? std::string(".") : path.substr(0, pos);

            //  [批53·单 EXE 模式] DLL 由单 EXE 启动器释放到 %TEMP% 后注入，
            //  配置必须落在 EXE 所在目录 —— 否则用户在 EXE 旁边找不到
            //  esp_config.json，配置也随临时目录被清掉。
            //  启动器注入前会设置 HANXUESNOW_HOME 指向 EXE 目录。
            char home[MAX_PATH] = {};
            if (GetEnvironmentVariableA("HANXUESNOW_HOME", home, MAX_PATH)
                && home[0])
            {
                dir = home;
            }
            return dir;
        }();
        return dir;
    }

    // 配置文件路径：DLL 同目录 \ esp_config.json（拼接一次后缓存）
    const std::string& GetConfigPath()
    {
        static const std::string path = GetModuleDir() + "\\esp_config.json";
        return path;
    }

    // 旧版 INI 路径（仅用于首次运行的一次性迁移检测）
    const std::string& GetIniPath()
    {
        static const std::string path = GetModuleDir() + "\\esp_config.ini";
        return path;
    }

    // 查询文件写时间（100ns 单位合成 64 位；0 = 不存在/查询失败）
    unsigned long long QueryMtime(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad))
            return 0;
        ULARGE_INTEGER u;
        u.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        return u.QuadPart;
    }

    bool ReadWholeFile(const std::string& path, std::string& out)
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        if (size < 0 || size > 1024 * 1024)
        {
            fclose(f);
            return false;
        }
        out.assign((size_t)size, '\0');
        fseek(f, 0, SEEK_SET);
        size_t rd = fread(&out[0], 1, (size_t)size, f);
        fclose(f);
        out.resize(rd);
        return true;
    }

    int ToByte(float v)
    {
        int i = (int)(v * 255.0f + 0.5f);
        return i < 0 ? 0 : (i > 255 ? 255 : i);
    }

    // ---- 热加载共享状态（渲染线程轮询 / 保存线程更新；原子防竞态）----
    ULONGLONG g_lastPollMs = 0;                                    // 轮询节流时间戳
    std::atomic<unsigned long long> g_lastSeenMtime{ 0 };          // 上次观测到的配置 mtime（热加载基线）
    std::atomic<unsigned long long> g_selfWriteMtime{ 0 };         // 本进程写盘后的 mtime（防自触发回环）

    // ---- 设置快照采集：直接由条目表驱动（新增条目自动纳入检测）----
    void CaptureSnapshot(EspSnapshot& s)
    {
        std::memset(&s, 0, sizeof(s));

        for (int i = 0; i < kBoolCount; ++i)
            s.toggles[i] = *g_boolEntries[i].value ? 1 : 0;
        for (int i = 0; i < kIntCount; ++i)
            s.styles[i] = *g_intEntries[i].value;
        for (int i = 0; i < kFloatCount; ++i)
            s.floats[i] = *g_floatEntries[i].value;

        for (int i = 0; i < kColorCount; ++i)
        {
            const ImVec4& c = g_colorEntries[i].value->Value;
            s.colors[i][0] = c.x;
            s.colors[i][1] = c.y;
            s.colors[i][2] = c.z;
            s.colors[i][3] = c.w;
        }
    }

    void Trim(std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) { s.clear(); return; }
        s = s.substr(b, e - b + 1);
    }

    bool ParseBool(const std::string& v, bool& out)
    {
        if (v == "1" || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "on") == 0)
        { out = true; return true; }
        if (v == "0" || _stricmp(v.c_str(), "false") == 0 || _stricmp(v.c_str(), "off") == 0)
        { out = false; return true; }
        return false;
    }

    bool ParseColor(const std::string& v, float rgba[4])
    {
        int r = 0, g = 0, b = 0, a = 255;
        int n = sscanf_s(v.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a);
        if (n < 3) return false;
        auto cl = [](int x) { return x < 0 ? 0 : (x > 255 ? 255 : x); };
        rgba[0] = cl(r) / 255.0f;
        rgba[1] = cl(g) / 255.0f;
        rgba[2] = cl(b) / 255.0f;
        rgba[3] = cl(a) / 255.0f;
        return true;
    }

    // ============================================================
    // 迷你 JSON 解析器（仅覆盖本配置所需子集 + 常见容错）
    // ------------------------------------------------------------
    // 设计：不做 AST，直接把嵌套对象展平为 (叶子键, 规范化值)
    // 列表——本配置各层级键名全局唯一，展平后即可复用原 INI 时
    // 代的 ApplyEntry 做类型转换/范围校验，逻辑零重复。
    //   值规范化：true/false 保留字面；数字/裸 token 原样；
    //   数组（颜色 [r,g,b,a]）拼接为 "r,g,b,a"；字符串去引号并
    //   反转义；null 视为"该键缺省"（保留当前/默认值）。
    // ============================================================
    struct FlatEntry { std::string key; std::string value; };
    using FlatList = std::vector<FlatEntry>;

    struct JsonParser
    {
        const char* p;
        const char* e;
        FlatList*   out;

        JsonParser(const char* data, size_t size, FlatList* flat)
            : p(data), e(data + size), out(flat) {}

        // 空白 + // 行注释 + /* */ 块注释（配置文件头部说明用）
        void SkipWs()
        {
            for (;;)
            {
                while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
                if (p + 1 < e && p[0] == '/' && p[1] == '/')
                {
                    p += 2;
                    while (p < e && *p != '\n') ++p;
                    continue;
                }
                if (p + 1 < e && p[0] == '/' && p[1] == '*')
                {
                    p += 2;
                    while (p + 1 < e && !(p[0] == '*' && p[1] == '/')) ++p;
                    p = (p + 1 < e) ? p + 2 : e;
                    continue;
                }
                break;
            }
        }

        bool ParseString(std::string& s)
        {
            if (p >= e || *p != '"') return false;
            ++p;                          // 吃掉起始引号
            s.clear();
            while (p < e && *p != '"')
            {
                char c = *p++;
                if (c == '\\' && p < e)
                {
                    const char n = *p++;
                    switch (n)
                    {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'u':             // \uXXXX：跳过 4 位十六进制（本配置无 Unicode 内容）
                        for (int k = 0; k < 4 && p < e; ++k) ++p;
                        break;
                    default: s += n; break;   // \" \\ \/ 等
                    }
                }
                else
                {
                    s += c;
                }
            }
            if (p >= e) return false;     // 未闭合
            ++p;                          // 吃掉收尾引号
            return true;
        }

        static bool IsTokenEnd(char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
                   c == ',' || c == '}' || c == ']';
        }

        // 裸 token（数字 / true / false / null）
        bool ParseBare(std::string& s)
        {
            const char* st = p;
            while (p < e && !IsTokenEnd(*p)) ++p;
            if (p == st) return false;
            s.assign(st, (size_t)(p - st));
            return true;
        }

        // 数组 → 逗号连接字符串（颜色 [r,g,b,a] → "r,g,b,a"）
        bool ParseArrayValue(std::string& s)
        {
            ++p;                          // 吃掉 '['
            s.clear();
            SkipWs();
            if (p < e && *p == ']') { ++p; return true; }   // 空数组
            for (;;)
            {
                SkipWs();
                if (p >= e) return false;

                std::string item;
                if (*p == '"')
                {
                    if (!ParseString(item)) return false;
                }
                else if (*p == '[' || *p == '{')
                {
                    return false;         // 本配置数组仅含标量，不支持嵌套
                }
                else
                {
                    if (!ParseBare(item)) return false;
                }

                if (!s.empty()) s += ',';
                s += item;

                SkipWs();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }

        bool ParseObject()
        {
            ++p;                          // 吃掉 '{'
            SkipWs();
            if (p < e && *p == '}') { ++p; return true; }   // 空对象
            for (;;)
            {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (p >= e || *p != ':') return false;
                ++p;                      // 吃掉 ':'
                SkipWs();
                if (p >= e) return false;

                const char c = *p;
                if (c == '{')
                {
                    // 嵌套对象：递归展平（键名全局唯一，无需前缀）
                    if (!ParseObject()) return false;
                }
                else if (c == '[')
                {
                    std::string value;
                    if (!ParseArrayValue(value)) return false;
                    out->push_back(FlatEntry{ key, value });
                }
                else if (c == '"')
                {
                    std::string value;
                    if (!ParseString(value)) return false;
                    out->push_back(FlatEntry{ key, value });
                }
                else
                {
                    std::string value;
                    if (!ParseBare(value)) return false;
                    if (value != "null")
                        out->push_back(FlatEntry{ key, value });   // null：该键保留现值
                }

                SkipWs();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }

        bool Parse()
        {
            // 跳过 UTF-8 BOM（部分编辑器保存时自动添加）
            if (e - p >= 3 &&
                (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
                p += 3;
            SkipWs();
            if (p >= e || *p != '{') return false;
            if (!ParseObject()) return false;
            SkipWs();
            return p >= e;                // 尾部仅允许空白/注释
        }
    };

    // 单条目应用（键 → 校验 → 写入全局量）；applied/invalid 统计
    void ApplyEntry(const std::string& key, const std::string& value, int& applied, int& invalid)
    {
        bool b = false;
        for (const BoolEntry& e : g_boolEntries)
        {
            if (key == e.key)
            {
                if (ParseBool(value, b)) { *e.value = b; ++applied; }
                else ++invalid;
                return;
            }
        }

        for (const IntEntry& e : g_intEntries)
        {
            if (key == e.key)
            {
                char* end = nullptr;
                long v = strtol(value.c_str(), &end, 10);
                if (end && end != value.c_str() && *end == '\0')
                {
                    if (v < e.minValue) v = e.minValue;
                    if (v > e.maxValue) v = e.maxValue;
                    *e.value = (int)v;
                    ++applied;
                }
                else ++invalid;
                return;
            }
        }

        for (const FloatEntry& e : g_floatEntries)
        {
            if (key == e.key)
            {
                char* end = nullptr;
                float v = strtof(value.c_str(), &end);
                if (end && end != value.c_str() && *end == '\0' && isfinite(v))
                {
                    if (v < e.minValue) v = e.minValue;
                    if (v > e.maxValue) v = e.maxValue;
                    *e.value = v;
                    ++applied;
                }
                else ++invalid;
                return;
            }
        }

        float rgba[4] = {};
        for (const ColorEntry& e : g_colorEntries)
        {
            if (key == e.key)
            {
                if (ParseColor(value, rgba))
                {
                    *e.value = ImColor(rgba[0], rgba[1], rgba[2], rgba[3]);
                    ++applied;
                }
                else ++invalid;
                return;
            }
        }

        ++invalid;   // 未知键
    }

    // （迁移用）旧版 INI 文本逐行解析：key=value → ApplyEntry
    void ApplyIniText(const std::string& text, int& applied, int& invalid)
    {
        size_t pos = 0;
        while (pos <= text.size())
        {
            size_t eol = text.find('\n', pos);
            size_t end = (eol == std::string::npos) ? text.size() : eol;
            std::string line = text.substr(pos, end - pos);
            pos = (eol == std::string::npos) ? text.size() + 1 : eol + 1;

            Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[')
                continue;

            size_t eq = line.find('=');
            if (eq == std::string::npos) { ++invalid; continue; }

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);
            Trim(key);
            Trim(value);
            if (key.empty()) { ++invalid; continue; }

            ApplyEntry(key, value, applied, invalid);
        }
    }

    // 读取并应用 JSON 配置；返回 false = 读取或解析失败（调用方保留现值）
    bool LoadJsonFile(const std::string& path, int& applied, int& invalid)
    {
        std::string text;
        if (!ReadWholeFile(path, text))
        {
            PayloadLog("[config] 警告：无法读取配置文件 %s", path.c_str());
            return false;
        }

        FlatList flat;
        flat.reserve(64);   // 加载期一次性分配，解析完成后即释放（非每帧路径）
        JsonParser jp(text.data(), text.size(), &flat);
        if (!jp.Parse())
        {
            PayloadLog("[config] 警告：%s JSON 解析失败，保留当前设置", path.c_str());
            return false;
        }

        applied = 0;
        invalid = 0;
        for (const FlatEntry& fe : flat)
            ApplyEntry(fe.key, fe.value, applied, invalid);
        return true;
    }

    // 缩进写出键行（除末行外均带逗号，保证 JSON 无尾逗号）
    void AppendKeyLines(std::string& out, const std::vector<std::string>& lines)
    {
        for (size_t i = 0; i < lines.size(); ++i)
        {
            out += "    ";
            out += lines[i];
            out += (i + 1 < lines.size()) ? ",\n" : "\n";
        }
    }

    // ---- 序列化（保存线程/构建期调用，非每帧路径）----
    // 数字统一 %.3f 定点输出：读回与内存值一致（往返无损），
    // 且避免 %g 的科学计数法影响可读性。
    std::string BuildJsonText()
    {
        std::string out;
        out.reserve(2400);
        out += "// L4D2 Payload ESP 配置（JSON 格式，UTF-8）\n";
        out += "// 菜单修改后自动保存；手动编辑保存后游戏内自动热加载（约 1s 内生效）。\n";
        out += "// 2026-10-18：透视已整体换成 Storm Paint.hpp 体系。\n";
        out += "// 配色为硬编码语义色（不可调），故 colors 节为空对象。\n";
        out += "{\n";

        char buf[160];
        std::vector<std::string> lines;
        lines.reserve(kBoolCount + kIntCount + kFloatCount);

        // ---- "esp" 节 ----
        out += "  \"esp\": {\n";
        lines.clear();
        for (int i = 0; i < kEspBoolCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %s",
                g_boolEntries[i].key, *g_boolEntries[i].value ? "true" : "false");
            lines.emplace_back(buf);
        }
        for (int i = 0; i < kEspIntCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %d",
                g_intEntries[i].key, *g_intEntries[i].value);
            lines.emplace_back(buf);
        }
        for (int i = 0; i < kEspFloatCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %.3f",
                g_floatEntries[i].key, (double)*g_floatEntries[i].value);
            lines.emplace_back(buf);
        }
        AppendKeyLines(out, lines);
        out += "  },\n";

        // ---- "aim" 节 ----
        out += "  \"aim\": {\n";
        lines.clear();
        for (int i = kEspBoolCount; i < kBoolCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %s",
                g_boolEntries[i].key, *g_boolEntries[i].value ? "true" : "false");
            lines.emplace_back(buf);
        }
        for (int i = kEspIntCount; i < kIntCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %d",
                g_intEntries[i].key, *g_intEntries[i].value);
            lines.emplace_back(buf);
        }
        for (int i = kEspFloatCount; i < kFloatCount; ++i)
        {
            snprintf(buf, sizeof(buf), "\"%s\": %.3f",
                g_floatEntries[i].key, (double)*g_floatEntries[i].value);
            lines.emplace_back(buf);
        }
        AppendKeyLines(out, lines);
        out += "  },\n";

        // ---- "colors" 节 ----
        // 2026-10-18：配色已随 Storm 体系改为硬编码语义色，不再有可调条目。
        // 这里仍然输出一个**空对象**，保证 JSON 结构稳定、老解析器不会因
        // 缺键报错；占位项（__unused_*）不写入。
        out += "  \"colors\": {\n";
        lines.clear();
        for (const ColorEntry& e : g_colorEntries)
        {
            if (e.key[0] == '_')
                continue;   // 占位项不落盘
            const ImVec4& c = e.value->Value;
            snprintf(buf, sizeof(buf), "\"%s\": [%d, %d, %d, %d]",
                e.key, ToByte(c.x), ToByte(c.y), ToByte(c.z), ToByte(c.w));
            lines.emplace_back(buf);
        }
        AppendKeyLines(out, lines);
        out += "  }\n";

        out += "}\n";
        return out;
    }

    // 先写 .tmp 再原子替换：中途失败/崩溃不会破坏旧配置；
    // 写盘后记录自身写时间，避免热加载把本次写入误判为外部修改。
    void WriteConfigFile(const std::string& text)
    {
        const std::string& path = GetConfigPath();
        const std::string tmp  = path + ".tmp";

        FILE* f = nullptr;
        if (fopen_s(&f, tmp.c_str(), "wb") != 0 || !f)
        {
            PayloadLog("[config] 警告：无法写入 %s（错误码 %lu），本次保存已跳过",
                tmp.c_str(), GetLastError());
            return;
        }
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);

        if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            PayloadLog("[config] 警告：替换配置文件失败（错误码 %lu）：%s",
                GetLastError(), path.c_str());
            return;
        }
        g_selfWriteMtime.store(QueryMtime(path));   // 防热加载自触发
    }

    // ---- 保存线程：仅在收到通知时取走最新文本写盘（自动合并多次改动）----
    void SaverLoop()
    {
        for (;;)
        {
            unsigned long long gen = 0;
            std::string text;
            {
                std::unique_lock<std::mutex> lk(State().pendingMtx);
                State().pendingCv.wait(lk, [] { return !State().pendingText.empty(); });
                text.swap(State().pendingText);
                gen = State().pendingGen;
            }
            {
                std::lock_guard<std::mutex> lk(State().writeMtx);
                if (gen < State().writtenGen) continue;   // 已被更新世代覆盖，作废
                WriteConfigFile(text);
                State().writtenGen = gen;
            }
        }
    }

    void PushSave(std::string text)
    {
        SaverState& s = State();
        if (!s.started)
        {
            s.started = true;
            std::thread(SaverLoop).detach();   // 常驻线程：随进程退出回收
        }
        {
            std::lock_guard<std::mutex> lk(s.pendingMtx);
            s.pendingText = std::move(text);
            ++s.pendingGen;
        }
        s.pendingCv.notify_one();
    }
} // namespace

void EspConfigLoad()
{
    const std::string& jsonPath = GetConfigPath();
    const std::string& iniPath  = GetIniPath();

    if (GetFileAttributesA(jsonPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        int applied = 0, invalid = 0;
        if (!LoadJsonFile(jsonPath, applied, invalid))
            return;   // 失败日志已在内部输出；保留默认值
        if (invalid > 0)
            PayloadLog("[config] 警告：%s 中有 %d 个无效/未知条目已忽略（应用 %d 项）",
                jsonPath.c_str(), invalid, applied);
        else
            PayloadLog("[config] 已加载配置 %s（应用 %d 项设置）", jsonPath.c_str(), applied);
        return;
    }

    // JSON 不存在：若检测到旧版 INI（历史版本产物），一次性迁移为 JSON
    if (GetFileAttributesA(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        std::string text;
        if (ReadWholeFile(iniPath, text))
        {
            int applied = 0, invalid = 0;
            ApplyIniText(text, applied, invalid);
            PushSave(BuildJsonText());   // 生成 JSON（异步落盘，INI 保留不动）
            PayloadLog("[config] 已从旧版 INI 迁移配置（应用 %d 项，忽略 %d 个无效条目）：%s → %s",
                applied, invalid, iniPath.c_str(), jsonPath.c_str());
            return;
        }
    }

    PayloadLog("[config] 未找到配置文件，使用默认设置（首次修改菜单后自动生成 %s）", jsonPath.c_str());
}

void EspConfigTick()
{
    // 首次调用：读取并应用配置文件（保证菜单/ESP 使用前完成）
    static bool loaded = false;
    if (!loaded)
    {
        loaded = true;
        EspConfigLoad();
        g_lastSeenMtime.store(QueryMtime(GetConfigPath()));
        g_lastPollMs = GetTickCount64();
    }

    static EspSnapshot last{};
    static bool        haveLast = false;
    static bool        dirty = false;
    static ULONGLONG   dirtyAt = 0;

    // ---- 热加载：外部修改 esp_config.json 后自动重载（1s 节流）----
    // 思路：比对文件 mtime 与两个基线——"上次观测值"（外部修改会
    // 改变它）与"自身写盘值"（菜单保存引起的 mtime 变化被跳过，
    // 避免保存 → 重载 → 再保存的回环）。
    {
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - g_lastPollMs >= kReloadPollMs)
        {
            g_lastPollMs = nowMs;
            const std::string& path = GetConfigPath();
            const unsigned long long mt = QueryMtime(path);
            if (mt != 0 && mt != g_lastSeenMtime.load() && mt != g_selfWriteMtime.load())
            {
                g_lastSeenMtime.store(mt);
                int applied = 0, invalid = 0;
                if (LoadJsonFile(path, applied, invalid))
                {
                    PayloadLog("[config] 热加载成功：%s（应用 %d 项%s）", path.c_str(), applied,
                        invalid > 0 ? "，部分条目无效已忽略" : "");
                    CaptureSnapshot(last);   // 基线对齐到文件内容，避免误判为菜单改动
                    haveLast = true;
                    dirty = false;           // 文件即最新意图，不回写
                    return;
                }
                PayloadLog("[config] 警告：热加载失败（%s），保留当前设置", path.c_str());
            }
        }
    }

    EspSnapshot cur;
    CaptureSnapshot(cur);

    if (!haveLast)
    {
        last = cur;
        haveLast = true;
        return;
    }

    if (std::memcmp(&cur, &last, sizeof(EspSnapshot)) != 0)
    {
        // 设置发生变化：更新基线并等待防抖窗口（连续拖动只保存一次）
        last = cur;
        dirty = true;
        dirtyAt = GetTickCount64();
        return;
    }

    if (dirty && GetTickCount64() - dirtyAt >= kSaveDebounceMs)
    {
        dirty = false;
        PushSave(BuildJsonText());   // 文本构建极快；文件 IO 由保存线程执行
    }
}

void EspConfigSaveNow()
{
    // 作废尚未完成的旧世代写入，同步写入当前最新设置
    unsigned long long gen = 0;
    {
        std::lock_guard<std::mutex> lk(State().pendingMtx);
        State().pendingText.clear();
        gen = ++State().pendingGen;
    }
    {
        std::lock_guard<std::mutex> lk(State().writeMtx);
        if (gen < State().writtenGen) return;
        WriteConfigFile(BuildJsonText());
        State().writtenGen = gen;
    }
}

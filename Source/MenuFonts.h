#pragma once
// ============================================================
// 菜单字体（必须在 ImGui 首帧 NewFrame 之前执行）
// ------------------------------------------------------------
// 1) 为什么单独拆一个头
//    ImGui 在 NewFrame() 里把 io.Fonts->Locked 置 true，EndFrame() 才解开。
//    菜单绘制回调（DrawCallBack）跑在 NewFrame 之后，那里调 Clear/AddFont
//    必定断言失败（Release 下直接踩已释放的 ImFont）。
//    唯一安全时机是 hkEndScene 里 InitDx9() 成功之后、NewFrame() 之前。
//
// 2) 为什么不用 AddFontFromFileTTF
//    DLL 在「...\桌面\求生之路2项目\Source\Release\」，路径含中文。
//    ImGui 内部是 fopen()，UCRT 在 "C" locale 下无法把非 ASCII 路径转成
//    宽字符 → 打开失败 → AddFont 返回 null → 只剩 AddFontDefault()（纯 ASCII），
//    于是所有中文渲染成 '?'（ImGui 的缺字回退），数字/字母却正常。
//    所以这里自己用 CreateFileW / ReadFile 读成内存，再 AddFontFromMemoryTTF。
//
// 3) 为什么必须 Clear 却又不敢乱 Clear
//    OS-ImGui 初始化时已塞进一个 18px 常规雅黑并设为 FontDefault，比设计稿
//    大两号、还是常规字重。要换成 16px 粗体就得先清空图集。
//    但 Clear 会连能用的字体一起丢掉，所以顺序是：先把两套字体文件读进内存，
//    读成功才 Clear；读失败就保持原状（宁可字大两号，也不让全屏变 ???）。
//
// 4) 为什么用全量 CJK 而不做子集
//    0x4E00-0x9FA5 共 20902 字，16px+13px 合计约 1000 万像素，落在
//    4096×4096 图集内 —— 与改造前那个 18px 全量字体占的是同一张纹理，
//    显存不增加，却省掉「子集漏字就变 ???」和源文件编码两个隐患。
// ============================================================

#include "Osimgui/Imgui/imgui.h"
#include "Osimgui/Imgui/imgui_internal.h"
#include "MenuGlyphs.h"   // 项目用字（纯数值码点区间）

#include <windows.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>

namespace Neverlus
{
    inline ImFont* g_fontBold       = nullptr;   // 16px 粗体：正文 / 行标签 / 页标题
    inline ImFont* g_fontMini       = nullptr;   // 13px 常规：说明 / 卡片标题 / 数值 / 键帽
    inline ImFont* g_fontEsp        = nullptr;   // 18px 常规：ESP 文字（原 OS-ImGui 默认字体的复刻，
                                                 //   用户要求透视保持原字体；菜单字体不动）
    inline bool    g_menuFontsReady = false;

    inline char    g_fontBoldPath[MAX_PATH] = {};
    inline char    g_fontMiniPath[MAX_PATH] = {};
    inline bool    g_fontBoldOk = false;
    inline bool    g_fontCjkOk  = false;

    // 诊断：图集尺寸与各字体字形数（首次绘制时打一条日志）
    inline int     g_atlasW = 0, g_atlasH = 0;
    inline int     g_fontBoldGlyphs = 0, g_fontMiniGlyphs = 0;
    inline bool    g_atlasBuilt = false;
    inline double  g_atlasBuildMs = 0.0;   // 一次性构建耗时（注入瞬间的卡顿就是它）

    // 非汉字部分：拉丁、标点、箭头/几何、假名、全角（昵称里的颜文字靠这些）
    inline const ImWchar* SymbolRanges()
    {
        static const ImWchar r[] = {
            0x0020, 0x00FF,   // 拉丁 + 补充（° ± × ÷）
            0x2000, 0x206F,   // 常用标点（– — … ‘’）
            0x2100, 0x27BF,   // 箭头 / 数学 / 几何 / 制表 / 星标
            0x3000, 0x30FF,   // 中文标点 + 假名
            0xFF00, 0xFFEF,   // 全角/半角形式
            0
        };
        return r;
    }

    // 16px 主字体：全量 CJK（20902 字）——ESP 昵称里任何汉字都要能画出来
    inline const ImWchar* FullRanges()
    {
        static const ImWchar r[] = {
            0x0020, 0x00FF,
            0x2000, 0x206F,
            0x2100, 0x27BF,
            0x3000, 0x30FF,
            0x4E00, 0x9FA5,   // CJK 统一表意文字（全量）
            0xFF00, 0xFFEF,
            0
        };
        return r;
    }

    // 13px 说明字体：常用字 2500 ∪ 项目用字（数值表，不受编码影响）∪ 符号区段
    // 只为界面文案服务，不必背 2 万字的字形，图集与构建耗时都能省一截
    inline const ImWchar* MiniRanges()
    {
        static ImVector<ImWchar> s_out;
        if (s_out.Size == 0)
        {
            ImFontGlyphRangesBuilder b;
            b.AddRanges(ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
            b.AddRanges(kUiGlyphRanges);
            b.AddRanges(SymbolRanges());
            b.BuildRanges(&s_out);
        }
        return s_out.Data;
    }

    inline std::wstring WideModuleDir()
    {
        wchar_t buf[MAX_PATH]{};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&WideModuleDir), &self);
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (!n) return std::wstring(L".");
        std::wstring path(buf, buf + n);
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring::npos) ? std::wstring(L".") : path.substr(0, pos);
    }

    // DLL 目录\fonts 优先，其次 %WINDIR%\Fonts
    inline std::wstring PickFontFileW(const wchar_t* name)
    {
        const std::wstring local = WideModuleDir() + L"\\fonts\\" + name;
        if (GetFileAttributesW(local.c_str()) != INVALID_FILE_ATTRIBUTES)
            return local;
        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::wstring sys = std::wstring(winDir) + L"\\Fonts\\" + name;
        return (GetFileAttributesW(sys.c_str()) != INVALID_FILE_ATTRIBUTES) ? sys : std::wstring();
    }

    inline void WPathToUtf8(const std::wstring& w, char* out, size_t cap)
    {
        out[0] = '\0';
        if (!w.empty())
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out, (int)cap, nullptr, nullptr);
    }

    // 宽字符路径整文件读取：绕开 CRT 的 fopen 对非 ASCII 路径的限制
    inline bool ReadWholeFileW(const std::wstring& path, std::vector<unsigned char>& out)
    {
        out.clear();
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER sz{};
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64LL << 20))
        {
            CloseHandle(h);
            return false;
        }

        out.resize((size_t)sz.QuadPart);
        size_t total = 0;
        DWORD got = 0;
        while (total < out.size())
        {
            if (!ReadFile(h, out.data() + total, (DWORD)(out.size() - total), &got, nullptr) || got == 0)
                break;
            total += got;
        }
        CloseHandle(h);
        out.resize(total);
        return (total == (size_t)sz.QuadPart);
    }

    // 唯一的字体构建入口。调用点：OS-ImGui 内部模式 hkEndScene → InitDx9() 之后。
    inline void SetupMenuFonts()
    {
        if (g_menuFontsReady)
            return;
        if (!ImGui::GetCurrentContext())
            return;

        ImGuiIO& io = ImGui::GetIO();
        if (!io.Fonts)
            return;
        if (io.Fonts->Locked)      // NewFrame 之后不能动图集，留到安全时机
            return;

        std::wstring boldPath = PickFontFileW(L"msyhbd.ttc");
        std::wstring regPath  = PickFontFileW(L"msyh.ttc");
        if (boldPath.empty()) boldPath = regPath;
        if (regPath.empty())  regPath  = boldPath;

        // 字体数据必须活到图集销毁，用 static 保活
        static std::vector<unsigned char> s_bold;
        static std::vector<unsigned char> s_mini;

        const bool okBold = !boldPath.empty() && ReadWholeFileW(boldPath, s_bold);
        const bool okMini = !regPath.empty()  && ReadWholeFileW(regPath,  s_mini);

        WPathToUtf8(boldPath, g_fontBoldPath, sizeof(g_fontBoldPath));
        WPathToUtf8(regPath,  g_fontMiniPath, sizeof(g_fontMiniPath));

        // 一套都没读到：保持库里的 18px 字体，绝不清理图集（宁可大两号，不可满屏 ???）
        if (!okBold && !okMini)
        {
            g_menuFontsReady = true;
            g_fontBoldOk = false;
            g_fontCjkOk  = false;
            return;
        }

        g_menuFontsReady = true;
        io.Fonts->Clear();

        ImFontConfig cfg;
        cfg.OversampleH        = 1;
        cfg.OversampleV        = 1;
        cfg.PixelSnapH         = 1;
        cfg.FontNo             = 0;      // TTC 集合内第 0 个字体
        cfg.FontDataOwnedByAtlas = false; // 数据由 s_bold / s_mini 持有

        if (okBold)
            g_fontBold = io.Fonts->AddFontFromMemoryTTF(s_bold.data(), (int)s_bold.size(),
                                                        16.0f, &cfg, FullRanges());
        if (okMini)
            g_fontMini = io.Fonts->AddFontFromMemoryTTF(s_mini.data(), (int)s_mini.size(),
                                                        13.0f, &cfg, MiniRanges());

        // ESP 字体：18px 常规雅黑 —— OS-ImGui 原始 FontDefault 的复刻
        // （原版：AddFontFromFileTTF(msyh.ttc, 18.0f, ChineseSimplifiedCommon)）。
        // 复用已读入内存的常规字体数据（s_mini 就是 msyh.ttc），
        // 范围用 MiniRanges()：常用 2500 字 + 项目用字 + 符号，视觉与原版一致。
        if (okMini)
            g_fontEsp = io.Fonts->AddFontFromMemoryTTF(s_mini.data(), (int)s_mini.size(),
                                                       18.0f, &cfg, MiniRanges());

        if (!g_fontBold)
            g_fontBold = io.Fonts->AddFontDefault();
        if (!g_fontMini)
            g_fontMini = g_fontBold;
        if (!g_fontEsp)
            g_fontEsp = g_fontBold;   // 常规字体没读到时退化：至少字重统一可读

        g_fontBoldOk   = okBold;
        g_fontCjkOk    = (okBold || okMini);
        io.FontDefault = g_fontBold;

        if (!io.Fonts->IsBuilt())
        {
            LARGE_INTEGER f{}, t0{}, t1{};
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            g_atlasBuilt = io.Fonts->Build();
            QueryPerformanceCounter(&t1);
            if (f.QuadPart > 0)
                g_atlasBuildMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        }

        g_atlasW = io.Fonts->TexWidth;
        g_atlasH = io.Fonts->TexHeight;
        g_fontBoldGlyphs = g_fontBold ? g_fontBold->Glyphs.Size : 0;
        g_fontMiniGlyphs = g_fontMini ? g_fontMini->Glyphs.Size : 0;
    }
}

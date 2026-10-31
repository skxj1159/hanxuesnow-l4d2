#include "OS-ImGui_Base.h"

/****************************************************
* Copyright (C)	: Liv
* @file			: OS-ImGui_Base.cpp
* @author		: Liv
* @email		: 1319923129@qq.com
* @version		: 1.1
* @date			: 2024/4/4 13:59
****************************************************/

namespace OSImGui
{
    bool OSImGui_Base::InitImGui(ID3D11Device* device, ID3D11DeviceContext* device_context)
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;

        ImGui::StyleColorsDark();
        io.LogFilename = nullptr;

        // 加载支持简体中文的字体（依次尝试常见 Windows 中文字体，均不可用时回退默认字体）
        {
            ImFontConfig fontCfg;
            fontCfg.FontNo = 0; // TTC 字体集合中的第 0 个字体
            const ImWchar* glyphRanges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();

            char winDir[MAX_PATH]{};
            GetWindowsDirectoryA(winDir, MAX_PATH);
            const std::string fontsDir = std::string(winDir) + "\\Fonts\\";
            const char* candidates[] = { "msyh.ttc", "Deng.ttf", "simhei.ttf", "simsun.ttc" };

            ImFont* chineseFont = nullptr;
            for (const char* name : candidates)
            {
                const std::string fullPath = fontsDir + name;
                if (GetFileAttributesA(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    continue;
                chineseFont = io.Fonts->AddFontFromFileTTF(fullPath.c_str(), 18.0f, &fontCfg, glyphRanges);
                if (chineseFont)
                    break;
            }
            io.FontDefault = chineseFont ? chineseFont : io.Fonts->AddFontDefault();
        }

        if (!ImGui_ImplWin32_Init(Window.hWnd))
            throw OSException("ImGui_ImplWin32_Init() call failed.");
        if (!ImGui_ImplDX11_Init(device, device_context))
            throw OSException("ImGui_ImplDX11_Init() call failed.");

        return true;
    }

    void OSImGui_Base::CleanImGui()
    {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();

        if(g_Device.g_pd3dDevice)
            g_Device.CleanupDeviceD3D();

        // Only destroy window in external mode.
        if (Window.hWnd && !Window.Name.empty())
        {
            DestroyWindow(Window.hWnd);
            UnregisterClassA(Window.ClassName.c_str(), Window.hInstance);
        }
    }

    std::wstring OSImGui_Base::StringToWstring(std::string& str)
    {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(str);
    }

}
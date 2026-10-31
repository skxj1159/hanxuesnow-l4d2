// Offline font-chain probe (no game needed).
// Run it from Source\Release so it sits on the same non-ASCII path as the DLL.
// NOTE: ASCII-only on purpose - a source file without BOM gets decoded as GBK,
//       which mangles CJK literals (this is what broke the old glyph table).
#include <windows.h>
#include <cstdio>

#include "MenuFonts.h"

int main()
{
    ImGui::CreateContext();

    {
        wchar_t dir[MAX_PATH]{};
        GetModuleFileNameW(nullptr, dir, MAX_PATH);
        std::wstring p(dir);
        p = p.substr(0, p.find_last_of(L"\\/")) + L"\\fonts\\msyhbd.ttc";

        char narrow[MAX_PATH]{};
        WideCharToMultiByte(CP_ACP, 0, p.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
        FILE* f = fopen(narrow, "rb");
        printf("[fopen ANSI path]   %s   <- what ImGui's own loader uses\n", f ? "OK" : "FAIL");
        if (f) fclose(f);

        std::vector<unsigned char> buf;
        const bool ok = Neverlus::ReadWholeFileW(p, buf);
        printf("[CreateFileW path]  %s   bytes=%zu\n", ok ? "OK" : "FAIL", buf.size());
    }

    Neverlus::SetupMenuFonts();

    printf("\nbold path: %s\n", Neverlus::g_fontBoldPath[0] ? Neverlus::g_fontBoldPath : "(empty)");
    printf("mini path: %s\n", Neverlus::g_fontMiniPath[0] ? Neverlus::g_fontMiniPath : "(empty)");

    printf("\nbold  %.0fpx  glyphs=%d\n",
           Neverlus::g_fontBold ? Neverlus::g_fontBold->FontSize : 0.0f,
           Neverlus::g_fontBoldGlyphs);
    printf("mini  %.0fpx  glyphs=%d\n",
           Neverlus::g_fontMini ? Neverlus::g_fontMini->FontSize : 0.0f,
           Neverlus::g_fontMiniGlyphs);
    printf("atlas %dx%d  built=%d  build=%.0fms\n",
           Neverlus::g_atlasW, Neverlus::g_atlasH,
           Neverlus::g_atlasBuilt ? 1 : 0, Neverlus::g_atlasBuildMs);
    printf("cjkOk=%d boldOk=%d\n",
           Neverlus::g_fontCjkOk ? 1 : 0, Neverlus::g_fontBoldOk ? 1 : 0);

    // Probe a few codepoints that the ImGui "common" table is missing (miao/ge/wu/you/ni/ba/frame).
    const unsigned int must[] = { 0x7784, 0x9ABC, 0x5DEB, 0x5E7D, 0x62DF, 0x9776, 0x5E27, 0x81F4 };
    if (Neverlus::g_fontBold)
    {
        printf("\nglyph lookup (bold):");
        for (unsigned int cp : must)
            printf(" U+%04X:%s", cp, Neverlus::g_fontBold->FindGlyphNoFallback((ImWchar)cp) ? "Y" : "N");
        printf("\n");
    }
    if (Neverlus::g_fontMini)
    {
        printf("glyph lookup (mini):");
        for (unsigned int cp : must)
            printf(" U+%04X:%s", cp, Neverlus::g_fontMini->FindGlyphNoFallback((ImWchar)cp) ? "Y" : "N");
        printf("\n");
    }

    printf("\nRESULT: %s\n",
           (Neverlus::g_fontBoldGlyphs > 20000 && Neverlus::g_fontMiniGlyphs > 2000)
               ? "FONT CHAIN OK" : "FONT CHAIN BROKEN");

    ImGui::DestroyContext();
    return 0;
}

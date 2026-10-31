#include "Globals.h"

HANDLE hProcess = nullptr;
uintptr_t clientBase = 0;
uintptr_t engineBase = 0;
uintptr_t localPlayer = 0;
bool menuOpen = false;

bool showAllies = false;
bool enableBox2D = false;
bool enableHealthBar = false;
bool enableHealthESP = false;
bool enableDistance = false;
bool enableESPName = false;
bool showEspPreview = false;

// 注：ResetEspColors() 及其24 个可调 ImColor 已随旧透视体系整体删除
// （Storm 用三档硬编码语义色，不可调）。备份见 _removed_esp/Globals.h。
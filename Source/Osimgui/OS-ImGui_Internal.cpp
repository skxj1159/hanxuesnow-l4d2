#include "OS-ImGui_Internal.h"
#include "OS-ImGui.h"
#include "../MenuFonts.h"
#include "../PayloadControl.h" // [批156] 渲染钩子失败原因落日志 // 菜单字体：16px 粗体 + 13px 常规（须在首帧 NewFrame 之前构建）

/****************************************************
* Copyright (C)	: Liv
* @file			: OS-ImGui_Internal.cpp
* @author		: Liv
* @email		: 1319923129@qq.com
* @version		: 1.1
* @date			: 2024/4/5 13:00
****************************************************/

#ifdef OSIMGUI_INTERNAL

typedef LRESULT(CALLBACK* WNDPROC)(HWND, UINT, WPARAM, LPARAM);
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
typedef HRESULT(__stdcall* Present) (IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
typedef HRESULT(__stdcall* Resize)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
typedef HRESULT(__stdcall* EndScene)(LPDIRECT3DDEVICE9);
typedef HRESULT(__stdcall* Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

// DirectX 9
inline EndScene oEndScene;
inline Reset oReSet;
// DirectX 11
inline Present oPresent;
inline Resize oResize;

inline WNDPROC oWndProc = NULL;
inline bool IsDx11Init = false, IsDx9Init = false;

namespace OSImGui
{
	void OSImGui_Internal::Start(HMODULE hLibModule, std::function<void()> CallBack, DirectXType DxType)
	{
		this->DxType = DxType;

		this->CallBackFn = CallBack;

		this->hLibModule = hLibModule;

		MH_Initialize();

		std::thread(&OSImGui_Internal::InitThread, this).detach();
	}

	void OSImGui_Internal::ReleaseHook()
	{
		if (this->HookData.HookList.size() > 0)
			MH_DisableHook(MH_ALL_HOOKS);

		for (auto Target : this->HookData.HookList)
		{
			MH_RemoveHook(Target);
		}
		this->HookData.HookList.clear();

		MH_Uninitialize();
	}

	bool OSImGui_Internal::CreateMyWindow()
	{
		WNDCLASSEXA wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "OS-ImGui_Internal", NULL };
		RegisterClassExA(&wc);

		this->Window.hInstance = wc.hInstance;

		this->Window.hWnd = ::CreateWindowA(wc.lpszClassName, "OS-ImGui_Overlay", WS_OVERLAPPEDWINDOW, 0, 0, 50, 50, NULL, NULL, wc.hInstance, NULL);

		this->Window.ClassName = wc.lpszClassName;

		return this->Window.hWnd != NULL;
	}

	void OSImGui_Internal::DestroyMyWindow()
	{
		if (this->Window.hWnd)
		{
			DestroyWindow(this->Window.hWnd);
			UnregisterClassA(this->Window.ClassName.c_str(), this->Window.hInstance);
			this->Window.hWnd = NULL;
		}
	}

	void OSImGui_Internal::InitThread()
	{
		if (this->DxType == DirectXType::AUTO)
		{
			if (GetModuleHandleA("d3d11.dll"))
				this->DxType = DirectXType::DX11;
			else if (GetModuleHandleA("d3d9.dll"))
				this->DxType = DirectXType::DX9;
			else
				goto END;
		}

		do
		{
			if (this->DxType == DirectXType::DX11)
			{
				if (this->InitDx11Hook())
				{
					break;
				}
			}
			else if(this->DxType == DirectXType::DX9)
			{
				if (this->InitDx9Hook())
				{
					break;
				}
			}
			else
			{
				goto END;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(100));

		} while (true);

		while (true)
		{
			if (this->FreeDLL)
			{
			END:
				// 不在 DLL 自身代码路径中 FreeLibrary（自卸载存在指令页竞态，可能崩溃）；
				// 此时 hook 已移除、ImGui 已关闭，进程退出时由系统回收本模块。
				return;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
			return true;

		return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
	}

	HRESULT __stdcall hkResize(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
	{
		if (g_Device.g_mainRenderTargetView)
		{
			g_Device.g_pd3dDeviceContext->OMSetRenderTargets(0, 0, 0);
			g_Device.g_mainRenderTargetView->Release();
		}

		HRESULT Result = oResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);

		ID3D11Texture2D* pBackBuffer = nullptr;

		pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
		g_Device.g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_Device.g_mainRenderTargetView);
		pBackBuffer->Release();

		g_Device.g_pd3dDeviceContext->OMSetRenderTargets(1, &g_Device.g_mainRenderTargetView, NULL);

		D3D11_VIEWPORT ViewPort{ 0.0f, 0.0f, (float)Width, (float)Height, 0.0f, 1.0f };

		g_Device.g_pd3dDeviceContext->RSSetViewports(1, &ViewPort);

		return Result;
	}

	HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
	{
		if (!IsDx11Init)
		{
			if (OSImGui::get().InitDx11(pSwapChain))
				IsDx11Init = true;
			else
				return oPresent(pSwapChain, SyncInterval, Flags);

			oWndProc = (WNDPROC)SetWindowLongPtrA(OSImGui::get().Window.hWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
			
			try {
				OSImGui::get().InitImGui(g_Device.g_pd3dDevice, g_Device.g_pd3dDeviceContext);
			}
			catch (OSException& e)
			{
				OSImGui::get().CleanDx11();
				IsDx11Init = false;

				std::cout << e.what() << std::endl;
				return oPresent(pSwapChain, SyncInterval, Flags);
			}
		}

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		OSImGui::get().CallBackFn();

		ImGui::Render();

		g_Device.g_pd3dDeviceContext->OMSetRenderTargets(1, &g_Device.g_mainRenderTargetView, NULL);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

		if (OSImGui::get().EndFlag)
		{
			oPresent(pSwapChain, SyncInterval, Flags);

			OSImGui::get().ReleaseHook();
			OSImGui::get().CleanImGui();
			OSImGui::get().CleanDx11();

			if (oWndProc)
				oWndProc = (WNDPROC)SetWindowLongPtrA(OSImGui::get().Window.hWnd, GWLP_WNDPROC, (LONG_PTR)(oWndProc));

			OSImGui::get().FreeDLL = true;

			return NULL;
		}

		return oPresent(pSwapChain, SyncInterval, Flags);
	}

	void OSImGui_Internal::CleanDx11()
	{
		if (g_Device.g_pd3dDevice)
			g_Device.g_pd3dDevice->Release();
		if (g_Device.g_pd3dDeviceContext)
			g_Device.g_pd3dDeviceContext->Release();
		if (g_Device.g_mainRenderTargetView)
			g_Device.g_mainRenderTargetView->Release();
	}

	bool OSImGui_Internal::InitDx11(IDXGISwapChain* pSwapChain)
	{
		if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_Device.g_pd3dDevice))))
		{
			g_Device.g_pd3dDevice->GetImmediateContext(&g_Device.g_pd3dDeviceContext);

			DXGI_SWAP_CHAIN_DESC sd;
			pSwapChain->GetDesc(&sd);

			this->Window.hWnd = sd.OutputWindow;

			this->Window.Size = Vec2((float)sd.BufferDesc.Width, (float)sd.BufferDesc.Height);

			ID3D11Texture2D* pBackBuffer;
			pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
			g_Device.g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_Device.g_mainRenderTargetView);

			pBackBuffer->Release();

			return true;
		}
		return false;
	}

	bool OSImGui_Internal::InitDx11Hook()
	{
		if (!this->CreateMyWindow())
			return false;

		HMODULE hModule = NULL;

		void* D3D11CreateDeviceAndSwapChain;
		D3D_FEATURE_LEVEL FeatureLevel;
		D3D_FEATURE_LEVEL FeatureLevels[] = { D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0 };
		DXGI_RATIONAL RefreshRate;
		DXGI_MODE_DESC ModeDesc;
		DXGI_SAMPLE_DESC SampleDesc;
		DXGI_SWAP_CHAIN_DESC SwapChainDesc;
		IDXGISwapChain* SwapChain;
		ID3D11Device* Device;
		ID3D11DeviceContext* Context;

		hModule = GetModuleHandleA("d3d11.dll");

		if (hModule == NULL)
		{
			this->DestroyMyWindow();
			return false;
		}

		D3D11CreateDeviceAndSwapChain = GetProcAddress(hModule, "D3D11CreateDeviceAndSwapChain");

		if (D3D11CreateDeviceAndSwapChain == 0)
		{
			this->DestroyMyWindow();
			return false;
		}
		// Datas init.
		RefreshRate.Numerator = 60;
		RefreshRate.Denominator = 1;
		SampleDesc.Count = 1;
		SampleDesc.Quality = 0;

		ModeDesc.Width = 100;
		ModeDesc.Height = 100;
		ModeDesc.RefreshRate = RefreshRate;
		ModeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		ModeDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		ModeDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

		SwapChainDesc.BufferDesc = ModeDesc;
		SwapChainDesc.SampleDesc = SampleDesc;
		SwapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		SwapChainDesc.BufferCount = 1;
		SwapChainDesc.OutputWindow = this->Window.hWnd;
		SwapChainDesc.Windowed = 1;
		SwapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
		SwapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

		// Create D3D11 device and swapchain.
		if (((long(__stdcall*)(
			IDXGIAdapter*,
			D3D_DRIVER_TYPE,
			HMODULE,
			UINT,
			const D3D_FEATURE_LEVEL*,
			UINT,
			UINT,
			const DXGI_SWAP_CHAIN_DESC*,
			IDXGISwapChain**,
			ID3D11Device**,
			D3D_FEATURE_LEVEL*,
			ID3D11DeviceContext**))(D3D11CreateDeviceAndSwapChain))(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, FeatureLevels, 1, D3D11_SDK_VERSION, &SwapChainDesc, &SwapChain, &Device, &FeatureLevel, &Context) < 0)
		{
			this->DestroyMyWindow();
			return false;
		}

		Address* Vtable = reinterpret_cast<Address*>(calloc(205, sizeof(Address)));

		memcpy(Vtable, *reinterpret_cast<Address**>(SwapChain), sizeof(Address) * 18);
		memcpy(Vtable + 18, *reinterpret_cast<Address**>(Device), sizeof(Address) * 43);
		memcpy(Vtable + 61, *reinterpret_cast<Address**>(Context), sizeof(Address) * 144);

		SwapChain->Release();
		Device->Release();
		Context->Release();

		this->DestroyMyWindow();

		Address* pTarget = nullptr;

		pTarget = reinterpret_cast<Address*>(Vtable[8]);
		if (MH_CreateHook(pTarget, hkPresent, (void**)(&oPresent)) == MH_OK)
			this->HookData.HookList.push_back(pTarget);

		pTarget = reinterpret_cast<Address*>(Vtable[13]);
		if (MH_CreateHook(pTarget, hkResize, (void**)(&oResize)) == MH_OK)
			this->HookData.HookList.push_back(pTarget);

		free(Vtable);

		if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
			return true;

		return false;
	}

	// ============================================================
	//  [批171] GPU 诊断计数：TDR（显卡未响应）归因用
	//  frames = EndScene 帧数；resets = 设备 Reset 次数；
	//  reinits = ImGui 上下文重建次数。每 60s 报一次。
	//  判读：resets/reinits 持续为 0 → 我们没有纹理反复重建；
	//        突然飙升 → Reset 风暴，是 TDR 的直接嫌疑。
	// ============================================================
	static long s_gfxFrames = 0, s_gfxResets = 0, s_gfxReinits = 0;
	static DWORD s_gfxLogAt = 0;

	HRESULT __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
	{
		if (!IsDx9Init)
		{
			if(OSImGui::get().InitDx9(pDevice))
			{
				IsDx9Init = true;
				++s_gfxReinits;                       // [批171] 上下文重建计数
				// 字体图集必须在 NewFrame() 之前构建：那里会把 Fonts->Locked 置 true，
				// 之后 Clear/AddFont 全部非法。此处 InitDx9 刚建完上下文、图集未构建，
				// 是唯一能替换掉默认 18px 常规雅黑、换成 16px 粗体 + 13px 常规的时机。
				Neverlus::SetupMenuFonts();
			}
			else
				return oEndScene(pDevice);
		}

		// [批171] GPU 计数与 60s 汇报
		++s_gfxFrames;
		{
			const DWORD nowG = GetTickCount();
			if (s_gfxLogAt == 0) s_gfxLogAt = nowG + 60000;
			else if (nowG >= s_gfxLogAt)
			{
				s_gfxLogAt = nowG + 60000;
				PayloadLog("[gfx] 60s 窗口：frames=%ld resets=%ld reinits=%ld",
				           s_gfxFrames, s_gfxResets, s_gfxReinits);
				s_gfxFrames = s_gfxResets = s_gfxReinits = 0;
			}
		}

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		OSImGui::get().CallBackFn();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		if (OSImGui::get().EndFlag)
		{
			oEndScene(pDevice);

			OSImGui::get().ReleaseHook();
			OSImGui::get().CleanDx9();

			if (oWndProc)
				oWndProc = (WNDPROC)SetWindowLongPtrA(OSImGui::get().Window.hWnd, GWLP_WNDPROC, (LONG_PTR)(oWndProc));

			OSImGui::get().FreeDLL = true;

			return NULL;
		}

		return oEndScene(pDevice);
	}

	HRESULT __stdcall hkReset(IDirect3DDevice9* pDevice, D3DPRESENT_PARAMETERS* pPrams)
	{
		// [批171] Reset 只【失效设备对象】（字体纹理），不销毁整个 ImGui 上下文：
		//   旧实现每次 Reset 都 CleanDx9（上下文+字体+纹理全套销毁）→
		//   下一帧再全套重建。Reset 高频时（换分辨率/alt-tab/覆盖层切换）
		//   是 GPU 纹理反复创建销毁的来源之一（TDR 嫌疑）。
		//   标准 ImGui 模式：Reset 前 InvalidateDeviceObjects，
		//   Reset 后 NewFrame 自动 CreateDeviceObjects（imgui_impl_dx9.cpp
		//   的 NewFrame 里 `if (!FontTexture) CreateDeviceObjects()`）。
		//   上下文保持不动 → 无纹理风暴。
		++s_gfxResets;
		if (IsDx9Init)
			ImGui_ImplDX9_InvalidateDeviceObjects();

		return oReSet(pDevice, pPrams);
	}

	// 仅统计可见且面积最大的顶层窗口（避免命中工具提示/隐藏窗口）
	BOOL CALLBACK EnumWindowsCallback(HWND hWnd, LPARAM lParam)
	{
		DWORD wndProcId;
		GetWindowThreadProcessId(hWnd, &wndProcId);

		if (GetCurrentProcessId() != wndProcId)
			return TRUE;

		if (!IsWindowVisible(hWnd))
			return TRUE;
		RECT rect{};
		if (!GetWindowRect(hWnd, &rect))
			return TRUE;
		LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
		if (area > *(LONG*)lParam)
		{
			*(LONG*)lParam = area;
			OSImGui::get().Window.hWnd = hWnd;

			// 内部模式（DX9）无交换链尺寸可读：用客户区尺寸作为绘制坐标系基准
			RECT client{};
			if (GetClientRect(hWnd, &client))
				OSImGui::get().Window.Size = Vec2((float)(client.right - client.left), (float)(client.bottom - client.top));
		}
		return TRUE;
	}

	HWND GetProcessWindow()
	{
		OSImGui::get().Window.hWnd = NULL;
		LONG bestArea = 0;
		EnumWindows(EnumWindowsCallback, (LPARAM)&bestArea);
		return OSImGui::get().Window.hWnd;
	}

	bool OSImGui_Internal::InitDx9(LPDIRECT3DDEVICE9 pDevice)
	{
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;

		ImGui::StyleColorsDark();
		io.LogFilename = nullptr;

		// 字体统一由 Neverlus::SetupMenuFonts() 在 hkEndScene 首次 InitDx9 之后加载
		// （16px 粗体 + 13px 常规）。这里不再预加载 18px/24px —— 预加载的字体
		// 会被那次 Clear() 连同图集一起丢掉，纯属白建一次。

		if (!ImGui_ImplWin32_Init(Window.hWnd))
			throw OSException("ImGui_ImplWin32_Init() call failed.");
		if (!ImGui_ImplDX9_Init(pDevice))
			throw OSException("ImGui_ImplDX9_Init() call failed.");

		return true;
	}

	void OSImGui_Internal::CleanDx9()
	{
		ImGui_ImplDX9_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
	}

	// ============================================================
	//  [批156] 渲染钩子失败原因：以前所有失败点都是静默 return false，
	//  InitThread 死循环重试 → 表现就是"注入成功但没菜单没任何效果"，
	//  而且日志里一个字都没有。现在每个失败点都记录原因并节流落日志，
	//  超过 60 秒未成功会明确提示"兼容性问题，请把 payload.log 发给作者"。
	// ============================================================
	static std::string s_lastFail;
	static DWORD       s_lastFailAt = 0;
	static DWORD       s_firstFailAt = 0;
	static void Dx9Fail(const char* why, HRESULT hr = S_OK)
	{
		s_lastFail = why;
		if (s_firstFailAt == 0) s_firstFailAt = GetTickCount();
		const DWORD now = GetTickCount();
		if (now - s_lastFailAt >= 5000)
		{
			s_lastFailAt = now;
			if (hr != S_OK)
				PayloadLog("[imgui] DX9 钩子重试：%s（hr=0x%08lX）", why, (unsigned long)hr);
			else
				PayloadLog("[imgui] DX9 钩子重试：%s", why);
			if (now - s_firstFailAt >= 60000)
				PayloadLog("[imgui] 渲染钩子已 60 秒未成功 —— 大概率是兼容性问题"
				           "（显卡驱动 / 远程桌面 / 覆盖软件如 Steam 覆盖层、"
				           "RivaTuner）。请把 payload.log 发给作者。");
		}
	}

	bool OSImGui_Internal::InitDx9Hook()
	{
		if (!this->CreateMyWindow())
		{
			Dx9Fail("创建临时窗口失败");
			return false;
		}

		HMODULE hModule = NULL;

		void* PDirect3DCreate9;
		LPDIRECT3DDEVICE9 Device;
		IDirect3D9* PDirect3D9;
		D3DDISPLAYMODE DisplayMode;
		D3DPRESENT_PARAMETERS D3dParam;

		// Get d3d module address
		hModule = GetModuleHandleA("d3d9.dll");
		if (hModule == NULL)
		{
			this->DestroyMyWindow();
			Dx9Fail("d3d9.dll 尚未加载（游戏还没建渲染设备）");
			return false;
		}

		PDirect3DCreate9 = GetProcAddress(hModule, "Direct3DCreate9");
		if (PDirect3DCreate9 == NULL)
		{
			this->DestroyMyWindow();
			Dx9Fail("Direct3DCreate9 导出缺失");
			return false;
		}

		PDirect3D9 = ((LPDIRECT3D9(__stdcall*)(DWORD))(PDirect3DCreate9))(D3D_SDK_VERSION);
		if (PDirect3D9 == NULL)
		{
			this->DestroyMyWindow();
			Dx9Fail("Direct3DCreate9 调用失败");
			return false;
		}

		if (PDirect3D9->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &DisplayMode) < 0)
		{
			this->DestroyMyWindow();
			Dx9Fail("GetAdapterDisplayMode 失败");
			return false;
		}

		D3dParam = { 0,0,DisplayMode.Format,0,D3DMULTISAMPLE_NONE,NULL,D3DSWAPEFFECT_DISCARD,this->Window.hWnd,1,0,D3DFMT_UNKNOWN,NULL,0,0 };

		// Create d3d device
		if (PDirect3D9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, this->Window.hWnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &D3dParam, &Device) < 0)
		{
			this->DestroyMyWindow();
			PDirect3D9->Release();
			// 常见兼容性失败点：Intel 老驱动 / 远程桌面 / 双显卡选择异常
			Dx9Fail("创建辅助 D3D9 设备失败（显卡驱动/远程桌面兼容性）");
			return false;
		}

		Address* Vtable = reinterpret_cast<Address*>(calloc(119, sizeof(Address)));
		memcpy(Vtable, *reinterpret_cast<Address**>(Device), 119 * sizeof(Address));

		PDirect3D9->Release();
		Device->Release();
		this->DestroyMyWindow();

		Address* pTarget = nullptr;

		pTarget = reinterpret_cast<Address*>(Vtable[42]);

		// [批156] EndScene 钩子是【必需】的：没有它就永远不画。
		//   旧代码失败也继续 → MH_EnableHook 照样返回成功 → 表现为
		//   "注入成功但没菜单没任何效果"（Steam 覆盖层/录像软件抢先
		//   挂钩时 MinHook 可能失败）。
		if (MH_CreateHook(pTarget, hkEndScene, (void**)(&oEndScene)) != MH_OK)
		{
			free(Vtable);
			this->DestroyMyWindow();
			Dx9Fail("MH_CreateHook(EndScene) 失败 —— 可能被 Steam 覆盖层/"
			        "RivaTuner 等抢先挂钩，请尝试关闭它们");
			return false;
		}
		this->HookData.HookList.push_back(pTarget);

		pTarget = reinterpret_cast<Address*>(Vtable[16]);

		if (MH_CreateHook(pTarget, hkReset, (void**)(&oReSet)) == MH_OK)
			this->HookData.HookList.push_back(pTarget);

		free(Vtable);

		if (GetProcessWindow() == NULL)
		{
			if (this->HookData.HookList.size() > 0)
			{
				MH_RemoveHook(this->HookData.HookList.at(0));
			}
			Dx9Fail("未找到游戏窗口（最小化/全屏切换中/窗口被隐藏）");
			return false;
		}

		if (MH_EnableHook(MH_ALL_HOOKS) == MH_OK)
		{
			PayloadLog("[imgui] DX9 钩子安装完成（共 %d 个，含 EndScene）",
			           (int)this->HookData.HookList.size());
			return true;
		}

		Dx9Fail("MH_EnableHook 失败");
		return false;
	}
}

#endif
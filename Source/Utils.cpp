#include "Include.h"

#include "PayloadControl.h"   // PayloadLog（读取失败诊断，节流）

// Viewmatrix-W2s
// 渲染帧内直读（进程内 SEH memcpy，零 RPM 系统调用开销）；失败日志节流
bool GetViewMatrix(CView& view) {
    if (!engineBase) return false;

    uintptr_t ptrToMatrix = 0;
    float     mat[16]{};
    bool      ok = false;
    __try
    {
        ptrToMatrix = *reinterpret_cast<uintptr_t*>(engineBase + Offsets::dwViewMatrix);
        if (ptrToMatrix > 0x10000u)
        {
            std::memcpy(&mat,
                        reinterpret_cast<const void*>(ptrToMatrix + Offsets::viewMatrixInnerOffset),
                        sizeof(mat));
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    if (!ok)
    {
        // 失败日志节流（5 秒一条）：换图/最小化期间会连续失败，避免每帧刷屏
        static ULONGLONG s_lastFailLogAt = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastFailLogAt >= 5000) {
            s_lastFailLogAt = now;
            PayloadLog("[view] 警告：读取视图矩阵失败（直读被 SEH 拦截，换图中）");
        }
        return false;
    }

    for (int i = 0, k = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j, ++k)
            view.Matrix[i][j] = mat[k];

    return true;
}

// ---- 帧级视图矩阵缓存 ----
// 渲染帧内所有绘制/选靶函数共用同一份矩阵（一帧一次 RPM 读取）。
// DrawCallBack 每帧调用 FrameCacheBegin()；其余代码经 GetFrameView() 取用
namespace {
    CView g_frameView{};
    bool  g_frameViewValid = false;
    bool  g_frameViewReady = false;   // 本帧是否已读取（未显式开始时按需补读）
}

void FrameCacheBegin() {
    g_frameViewValid = GetViewMatrix(g_frameView);
    g_frameViewReady = true;
}

bool IsFrameViewValid() {
    if (!g_frameViewReady) FrameCacheBegin();   // 保险：调用方忘记开始时按需读取
    return g_frameViewValid;
}

const CView& GetFrameView() {
    if (!g_frameViewReady) FrameCacheBegin();
    return g_frameView;
}


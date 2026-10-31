#pragma once

#include "Include.h"

class CView {
public:
    float Matrix[4][4]{};

    bool WorldToScreen(const Vec3& Pos, Vec2& ToPos) const {
        const float* m = &Matrix[0][0];

        float x = m[0] * Pos.x + m[1] * Pos.y + m[2] * Pos.z + m[3];
        float y = m[4] * Pos.x + m[5] * Pos.y + m[6] * Pos.z + m[7];
        float w = m[12] * Pos.x + m[13] * Pos.y + m[14] * Pos.z + m[15];

        if (w < 0.01f) return false;

        float invW = 1.0f / w;
        x *= invW;
        y *= invW;

        float screenWidth = Gui.Window.Size.x;
        float screenHeight = Gui.Window.Size.y;

        ToPos.x = (screenWidth / 2.0f) * (1.0f + x);
        ToPos.y = (screenHeight / 2.0f) * (1.0f - y);

        return true;
    }
};
bool GetViewMatrix(CView& view);

// ---- 帧级视图矩阵缓存（性能）----
// 同一渲染帧内多个绘制/选靶函数各自调用 GetViewMatrix 会产生重复的
// 跨进程读取（每次 2 次 RPM）。DrawCallBack 每帧调用一次 FrameCacheBegin()
// 后，同帧其余代码改用以下接口读取缓存，整帧只读一次：
void FrameCacheBegin();        // 每渲染帧调用一次（内部执行 RPM 读取）
bool IsFrameViewValid();       // 本帧视图矩阵是否读取成功
const CView& GetFrameView();   // 本帧视图矩阵（无效时矩阵为全零）

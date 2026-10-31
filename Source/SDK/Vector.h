#pragma once
// ============================================================
//  SDK/Vector.h —— 数学库（Vector / QAngle / Matrix）
// ------------------------------------------------------------
//  【分层铁律】纯数学，零依赖，不含任何功能逻辑。
//
//  【为什么自带 Vector 而不用 OS-ImGui 的 Vec3】
//  Osimgui/OS-ImGui_Struct.h:92-116 的 Vec3 全部运算符是**非 const
//  成员函数** → const 对象不可用 → 在"传 const 引用"的场景里到处要
//  拆成分量手写。同时它与渲染层耦合，SDK 层引用它就等于反向依赖。
//  这里自带一套全 const 的 Vector，与渲染层彻底解耦。
// ============================================================

#include <cmath>
#include <cstddef>

namespace sdk
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kRad2Deg = 57.29577951308232087680f;   // 180/pi
    inline constexpr float kDeg2Rad = 0.01745329251994329577f;      // pi/180

    // ============================================================
    //  Vector（3D 位置 / 方向）
    // ============================================================
    struct Vector
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        constexpr Vector() = default;
        constexpr Vector(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        // ---- 运算全部 const，这是与 Osimgui Vec3 的核心差异 ----
        constexpr Vector operator+(const Vector& o) const { return {x + o.x, y + o.y, z + o.z}; }
        constexpr Vector operator-(const Vector& o) const { return {x - o.x, y - o.y, z - o.z}; }
        constexpr Vector operator*(float s)       const { return {x * s, y * s, z * s}; }
        constexpr Vector operator/(float s)       const { return {x / s, y / s, z / s}; }
        constexpr Vector operator-()              const { return {-x, -y, -z}; }

        constexpr Vector& operator+=(const Vector& o) { x += o.x; y += o.y; z += o.z; return *this; }
        constexpr Vector& operator-=(const Vector& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
        constexpr Vector& operator*=(float s)       { x *= s; y *= s; z *= s; return *this; }

        constexpr bool operator==(const Vector& o) const { return x == o.x && y == o.y && z == o.z; }
        constexpr bool operator!=(const Vector& o) const { return !(*this == o); }

        constexpr float LengthSq()   const { return x * x + y * y + z * z; }
        inline    float Length()     const { return std::sqrt(LengthSq()); }
        inline    float Length2D()   const { return std::sqrt(x * x + y * y); }
        constexpr float Dot(const Vector& o) const { return x * o.x + y * o.y + z * o.z; }

        inline Vector Normalized() const
        {
            const float len = Length();
            return (len > 1e-6f) ? Vector{x / len, y / len, z / len} : Vector{};
        }
        inline void Normalize()
        {
            *this = Normalized();
        }

    // 绕轴旋转（Rodrigues 公式；定义见下方 Cross 之后）
    Vector RotateAroundAxis(const Vector& axis, float deg) const;

        static constexpr Vector Zero() { return {}; }
        static constexpr Vector Up(float s = 1.f)   { return {0.f, 0.f, s}; }
    };

inline Vector operator*(float s, const Vector& v) { return v * s; }

inline Vector Cross(const Vector& a, const Vector& b)
{
    return { a.y * b.z - a.z * b.y,
             a.z * b.x - a.x * b.z,
             a.x * b.y - a.y * b.x };
    }

inline float Distance(const Vector& a, const Vector& b) { return (b - a).Length(); }
inline float DistanceSq(const Vector& a, const Vector& b) { return (b - a).LengthSq(); }

// ---- Vector 的成员外定义 ----
// 放在 Cross 定义之后： Rodrigues 公式要用到 Cross，
// 而 struct 内只留声明（否则 Cross 还没声明就��用不了）。
inline Vector Vector::RotateAroundAxis(const Vector& axis, float deg) const
{
    const Vector k = axis.Normalized();
    const float rad = deg * kDeg2Rad;
    const float c = std::cos(rad), s = std::sin(rad);

    // Rodrigues 旋转公式： v·cos + (k×v)·sin + k·(k·v)·(1-cos)
    return *this * c + Cross(k, *this) * s
         + k * (k.Dot(*this) * (1.f - c));
}

    // ============================================================
    //  QAngle（Source 引擎角度约定：pitch / yaw / roll，单位度）
    // ============================================================
    struct QAngle
    {
        float pitch = 0.f, yaw = 0.f, roll = 0.f;

        constexpr QAngle() = default;
        constexpr QAngle(float p, float y, float r = 0.f) : pitch(p), yaw(y), roll(r) {}

        constexpr QAngle operator+(const QAngle& o) const { return {pitch + o.pitch, yaw + o.yaw, roll + o.roll}; }
        constexpr QAngle operator-(const QAngle& o) const { return {pitch - o.pitch, yaw - o.yaw, roll - o.roll}; }
        constexpr QAngle operator*(float s)      const { return {pitch * s, yaw * s, roll * s}; }

        constexpr bool operator==(const QAngle& o) const
        {
            return pitch == o.pitch && yaw == o.yaw && roll == o.roll;
        }

        inline float Length() const { return std::sqrt(pitch * pitch + yaw * yaw); }
        constexpr bool IsZero() const { return pitch == 0.f && yaw == 0.f && roll == 0.f; }
    };

    // Source 引擎的角度归一化到 [-180, 180]
    inline float NormalizeYaw(float yaw)
    {
        yaw = std::fmod(yaw + 180.f, 360.f);
        if (yaw < 0.f) yaw += 360.f;
        return yaw - 180.f;
    }

    // 角度插值（给平滑用；不是简单 lerp —— 走最短弧）
    inline float AngleDelta(float from, float to)
    {
        return NormalizeYaw(to - from);
    }

    // ============================================================
    //  AngleVectors（QAngle → 三轴向量）
    //  与引擎的 AngleVectors 同口径，pitch 为正表示向上。
    // ============================================================
    inline void AngleVectors(const QAngle& a, Vector& fwd, Vector& right, Vector& up)
    {
        const float sp = std::sin(a.pitch * kDeg2Rad), cp = std::cos(a.pitch * kDeg2Rad);
        const float sy = std::sin(a.yaw   * kDeg2Rad), cy = std::cos(a.yaw   * kDeg2Rad);
        const float sr = std::sin(a.roll  * kDeg2Rad), cr = std::cos(a.roll  * kDeg2Rad);

        fwd   = { cp * cy, cp * sy, -sp };
        right = { -1.f * sr * sp * cy + -1.f * cr * -sy,
                   -1.f * sr * sp * sy + -1.f * cr * cy,
                  -1.f * sr * cp };
        up    = { cr * sp * cy + -1.f * sr * -sy,
                   cr * sp * sy + -1.f * sr * cy,
                   cr * cp };
    }

    // ============================================================
    //  VectorAngles（方向 → QAngle，DrawTo 取弹道角用）
    // ============================================================
    inline QAngle VectorAngles(const Vector& forward)
    {
        const float yaw   = std::atan2(forward.y, forward.x) * kRad2Deg;
        const float denom = std::sqrt(forward.x * forward.x + forward.y * forward.y);
        const float pitch = std::atan2(-forward.z, denom) * kRad2Deg;
        return QAngle(pitch, yaw, 0.f);
    }

    // ============================================================
    //  Matrix（3x4 行主序 —— 骨骼变换用）
    //  索引 m[row][col]，与 Source 引擎的 matrix3x4 一致。
    // ============================================================
    struct Matrix3x4
    {
        float m[3][4] = {};

        constexpr Matrix3x4() = default;

        // 行主序展开：world = Σ(m[i][c] * center[c]) + m[i][3]
        Vector TransformPoint(const Vector& p) const
        {
            return { m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
                     m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                     m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3] };
        }
    };

    // ============================================================
    //  4x4（视图/投影矩阵，世界 → 屏幕）
    // ============================================================
    struct Matrix4x4
    {
        float m[4][4] = {};

        constexpr Matrix4x4() = default;
        constexpr explicit Matrix4x4(const float (&src)[4][4])
        {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    m[i][j] = src[i][j];
        }

        float* Row(int i)             { return m[i]; }
        const float* Row(int i) const { return m[i]; }
    };

    // ============================================================
    //  工具
    // ============================================================
    template <typename T>
    inline T Clamp(T v, T lo, T hi)
    {
        return (v < lo) ? lo : ((v > hi) ? hi : v);
    }

    inline bool IsFinite(const Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    inline bool IsFinite(const QAngle& a)
    {
        return std::isfinite(a.pitch) && std::isfinite(a.yaw) && std::isfinite(a.roll);
    }
} // namespace sdk

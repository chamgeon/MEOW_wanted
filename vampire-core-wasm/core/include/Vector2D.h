#pragma once
#include <cmath>

struct Vector2D {
    float x, y;

    constexpr Vector2D() : x(0.f), y(0.f) {}
    constexpr Vector2D(float x, float y) : x(x), y(y) {}

    inline Vector2D  operator+(const Vector2D& o) const { return {x + o.x, y + o.y}; }
    inline Vector2D  operator-(const Vector2D& o) const { return {x - o.x, y - o.y}; }
    inline Vector2D  operator*(float s)           const { return {x * s,   y * s};   }
    inline Vector2D  operator/(float s)           const { float inv = 1.f/s; return {x*inv, y*inv}; }
    inline Vector2D& operator+=(const Vector2D& o) { x += o.x; y += o.y; return *this; }
    inline Vector2D& operator-=(const Vector2D& o) { x -= o.x; y -= o.y; return *this; }
    inline Vector2D& operator*=(float s)            { x *= s;   y *= s;   return *this; }

    inline float lengthSq() const { return x*x + y*y; }
    inline float length()   const { return std::sqrt(lengthSq()); }
    inline float dot(const Vector2D& o) const { return x*o.x + y*o.y; }

    inline Vector2D normalized() const {
        float len = length();
        return len > 1e-6f ? *this / len : Vector2D{};
    }
};

inline Vector2D operator*(float s, const Vector2D& v) { return v * s; }

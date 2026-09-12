#pragma once
#include <cstdint>

namespace Config {
    constexpr float WORLD_WIDTH       = 2000.0f;
    constexpr float WORLD_HEIGHT      = 2000.0f;

    constexpr int   MAX_ENEMIES       = 10000;
    constexpr int   DEFAULT_ENEMIES   = 5000;
    constexpr float TARGET_FPS        = 60.0f;

    constexpr float ENEMY_SPEED       = 60.0f;
    constexpr float ENEMY_RADIUS      = 8.0f;
    constexpr float REPULSION_RADIUS  = ENEMY_RADIUS * 2.5f;
    constexpr float REPULSION_FORCE   = 120.0f;

    constexpr float PLAYER_SPEED      = 150.0f;
    constexpr float PLAYER_RADIUS     = 12.0f;

    constexpr int   MAX_PROJECTILES   = 500;
    constexpr float PROJECTILE_SPEED  = 400.0f;
    constexpr float PROJECTILE_RADIUS = 4.0f;
    constexpr float FIRE_RATE         = 0.15f;

    constexpr int   QT_MAX_DEPTH      = 6;
    constexpr int   QT_MAX_OBJECTS    = 8;
}

// ---------------------------------------------------------------------------
// Deterministic per-instance RNG.
//
// The engine used to call the global rand(). That is a shared, hidden piece of
// mutable state, and it broke the benchmark: two EngineCore instances ticked in
// an interleaved loop drew alternating values from the same stream, so their
// projectiles flew in different directions, killed different enemies, and the
// worlds diverged. An A/B benchmark cannot tolerate that.
//
// xorshift32 is a few instructions, has no global state, and makes every run
// byte-reproducible -- which is what lets main.cpp assert that the AoS and SoA
// paths simulate the same world.
// ---------------------------------------------------------------------------
struct Rng {
    uint32_t state;

    explicit Rng(uint32_t seed = 1337u) : state(seed ? seed : 1337u) {}

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // Uniform in [0,1).
    float unit() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }

    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

    int below(int n) { return (int)(next() % (uint32_t)n); }
};

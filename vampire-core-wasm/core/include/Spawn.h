#pragma once
#include "Config.h"
#include <cmath>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Spawn distribution: the scenario axis.
//
// WHY THIS EXISTS
//
// The Collision toggle (BruteForce vs QuadTree) is only an interesting question
// if the answer can change. Swept over entity count alone it cannot: QuadTree
// wins at every N above a few hundred and the "benchmark" degenerates into an
// answer key that any reader could have written down in advance.
//
// Spatial DISTRIBUTION is the axis where the winner genuinely moves, because the
// two kernels fail in opposite directions:
//
//   BruteForce  cost is N^2 regardless of where the entities are. Perfectly
//               indifferent to distribution -- and that indifference is exactly
//               what makes it a good control.
//
//   QuadTree    cost is (build) + (query). Under a UNIFORM field the tree is
//               balanced, every leaf holds a handful of entities, and a query
//               returns almost nothing but true neighbours. Under a CLUSTERED
//               field the same tree is pathological: whole subtrees are empty
//               while a few leaves saturate at MAX_DEPTH and degrade toward a
//               local linear scan, so the per-query candidate set inflates and
//               the constant factor climbs.
//
// So the same code, the same N, and the same kernels produce different verdicts
// depending on nothing but where the entities sit. That is a real engineering
// lesson and it is the thing the LLM optimizer agent is being asked to notice.
//
// HOW THE DISTRIBUTION IS MADE TO PERSIST
//
// Spawn placement alone would not be enough. If every enemy chased the player
// unconditionally, any initial arrangement would collapse into one ball within
// seconds and the toggle would be measuring a transient, not a scenario. The
// distribution survives because these points are HOME points (Config::AGGRO_RANGE,
// HOME_ORBIT_*): an enemy outside aggro range orbits the point it was born at
// instead of steering at the player. The sample below is therefore not just a
// starting position, it is the entity's long-run neighbourhood.
// ---------------------------------------------------------------------------

enum class SpawnDistribution : int { Uniform = 0, Clustered = 1 };

// One entity's worth of spawn randomness.
//
// EngineCore draws these ONCE and hands the same array to both EnemyContainerAoS
// and EnemyContainerSoA. Previously each container ran its own identical-by-
// convention draw loop, and "identical by convention" is a comment, not a
// guarantee -- one reordered line and the two layouts would quietly be
// benchmarking different worlds. Sharing the sample array makes world identity
// structural.
struct SpawnSample {
    float x, y;         // home point (also the initial position)
    float stateTimer;   // AI state machine phase
    float animTimer;    // animation cursor phase
};

constexpr float TAU_F = 6.28318531f;

// Centre of blob k. Index 0 is the WORLD CENTRE; 1..CLUSTER_COUNT-1 are spread
// evenly around CLUSTER_RING.
//
// Blob 0 is load-bearing, not cosmetic: the player starts at the world centre,
// and the native harness treats a run that produces zero kills as a failure
// (an equivalence check over a world where nothing ever dies has proved
// nothing). If every blob sat out on the ring, a Clustered run would hand the
// aura an empty circle and those checks would report a false negative.
inline void clusterCentre(int k, float& cx, float& cy) {
    const float wx = Config::WORLD_WIDTH  * 0.5f;
    const float wy = Config::WORLD_HEIGHT * 0.5f;
    if (k <= 0) { cx = wx; cy = wy; return; }

    const int   onRing = Config::CLUSTER_COUNT - 1;
    const float a      = TAU_F * (float)(k - 1) / (float)(onRing > 0 ? onRing : 1);
    cx = wx + std::cos(a) * Config::CLUSTER_RING;
    cy = wy + std::sin(a) * Config::CLUSTER_RING;
}

// Box-Muller (polar form), consuming exactly two uniforms.
//
// The radius is clamped at CLUSTER_SIGMA_CAP sigmas. Box-Muller is unbounded, so
// without the clamp a rare draw lands far outside the world and gets pinned to
// the wall by the world clamp below. Those pinned points would accumulate into a
// thin high-density line along the border -- a spatial artefact with no
// relationship to the scenario, which the QuadTree would then dutifully report
// as a hot leaf. Clamping keeps the blob compact and keeps the wall clamp cold.
inline void gaussianOffset(float u1, float u2, float sigma, float& ox, float& oy) {
    if (u1 < 1e-7f) u1 = 1e-7f;              // log(0) guard
    float r = sigma * std::sqrt(-2.0f * std::log(u1));
    const float cap = sigma * Config::CLUSTER_SIGMA_CAP;
    if (r > cap) r = cap;

    const float th = TAU_F * u2;
    ox = r * std::cos(th);
    oy = r * std::sin(th);
}

inline float clampWorldX(float x) {
    return x < 0.f ? 0.f : (x > Config::WORLD_WIDTH  ? Config::WORLD_WIDTH  : x);
}
inline float clampWorldY(float y) {
    return y < 0.f ? 0.f : (y > Config::WORLD_HEIGHT ? Config::WORLD_HEIGHT : y);
}

// Exactly two RNG draws, in the order the pre-distribution code used, so a
// Uniform run reproduces the old world byte for byte and every benchmark number
// already recorded against it stays valid.
inline void placeUniform(Rng& rng, float& x, float& y) {
    x = rng.range(0.f, Config::WORLD_WIDTH);
    y = rng.range(0.f, Config::WORLD_HEIGHT);
}

// Exactly two RNG draws as well, so the two distributions consume the stream at
// the same rate and a switch cannot desynchronise anything downstream.
//
// The blob is chosen by slot index rather than randomly. That makes membership a
// pure function of i, which means a recycled slot returns to the blob it came
// from and the blob populations cannot random-walk apart over a long run.
inline void placeClustered(Rng& rng, int slot, float& x, float& y) {
    const float u1 = rng.unit();
    const float u2 = rng.unit();

    float cx, cy;
    clusterCentre(slot % Config::CLUSTER_COUNT, cx, cy);

    float ox, oy;
    gaussianOffset(u1, u2, Config::CLUSTER_SIGMA, ox, oy);

    x = clampWorldX(cx + ox);
    y = clampWorldY(cy + oy);
}

// Nudge a point radially outward until it is at least SPAWN_EXCLUSION from the
// player.
//
// Without this, any home point that happens to sit under the player would revive
// its slot inside the aura, where the next pulse kills it before it has been
// drawn even once. The respawner would then burn its whole per-tick budget on
// entities nobody ever sees, and the live count -- the N that every ms/tick
// figure in this demo is a function of -- would sag for a reason unrelated to
// either toggle.
inline void pushOutOfAura(float px, float py, float& x, float& y) {
    float dx = x - px, dy = y - py;
    float d2 = dx*dx + dy*dy;
    const float minD = Config::SPAWN_EXCLUSION;
    if (d2 >= minD * minD) return;

    float nx, ny;
    if (d2 < 1e-6f) { nx = 1.f; ny = 0.f; }          // exactly on the player
    else { const float d = std::sqrt(d2); nx = dx/d; ny = dy/d; }

    // Reflect to the far side if pushing outward would leave the world, rather
    // than clamping: a clamp here would stack respawns along the wall next to a
    // cornered player, building the same fake density ridge the sigma cap exists
    // to prevent.
    float cx = px + nx * minD;
    float cy = py + ny * minD;
    if (cx < 0.f || cx > Config::WORLD_WIDTH)  cx = px - nx * minD;
    if (cy < 0.f || cy > Config::WORLD_HEIGHT) cy = py - ny * minD;

    x = clampWorldX(cx);
    y = clampWorldY(cy);
}

// Build the full spawn set for a world of `count` entities.
//
// Four draws per entity in a fixed order (placement x2, stateTimer, animTimer)
// for BOTH distributions, so switching the scenario changes where entities are
// but not how much randomness was consumed getting there.
inline void buildSpawnSet(SpawnDistribution dist, int count, uint32_t seed,
                          std::vector<SpawnSample>& out) {
    Rng rng(seed);
    out.resize((size_t)(count > 0 ? count : 0));

    for (int i = 0; i < count; ++i) {
        SpawnSample s{};
        if (dist == SpawnDistribution::Clustered) placeClustered(rng, i, s.x, s.y);
        else                                      placeUniform(rng, s.x, s.y);
        s.stateTimer = rng.range(0.2f, 1.5f);
        s.animTimer  = rng.range(0.f, 1.f);
        out[(size_t)i] = s;
    }
}

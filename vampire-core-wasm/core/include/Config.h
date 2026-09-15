#pragma once
#include <cstdint>

namespace Config {
    constexpr float WORLD_WIDTH       = 2000.0f;
    constexpr float WORLD_HEIGHT      = 2000.0f;

    constexpr int   MAX_ENEMIES       = 100000;
    constexpr int   DEFAULT_ENEMIES   = 5000;
    constexpr float TARGET_FPS        = 60.0f;

    constexpr float ENEMY_SPEED       = 60.0f;
    constexpr float ENEMY_RADIUS      = 8.0f;
    constexpr float REPULSION_RADIUS  = ENEMY_RADIUS * 2.5f;
    constexpr float REPULSION_FORCE   = 120.0f;

    // --- Aggro: what makes the density axis possible --------------------------
    // An enemy chases the player only while the player is this close; outside
    // it, the enemy holds station near its home point (see HOME_ORBIT_* below).
    //
    // Without this rule EVERY enemy targets the player from anywhere in the
    // world, so whatever spatial distribution they were spawned into collapses
    // into one dense ball within seconds. A "density" toggle under those
    // dynamics would only be measuring a transient on its way to the same
    // steady state, which is not a scenario axis at all. A finite aggro radius
    // is what lets a spawn distribution PERSIST and therefore be benchmarked.
    //
    // 320 is chosen against CLUSTER_RING below: a cluster sitting 600 away has
    // its near edge at ~380, outside aggro, so it stays packed instead of
    // draining toward the player the instant the run starts.
    constexpr float AGGRO_RANGE       = 320.0f;

    // Enemies outside aggro range orbit their home point rather than standing
    // still. Standing still would freeze the field, and a frozen field quietly
    // changes what the benchmark measures: positions stop moving, so the
    // QuadTree rebuilds an identical tree every tick and the repulsion pairs
    // never change. A slow orbit keeps the kernel doing real work on real
    // motion while holding each enemy inside its own neighbourhood, which is
    // what keeps the distribution intact.
    constexpr float HOME_ORBIT_RADIUS = 70.0f;
    constexpr float HOME_ORBIT_HZ     = 0.08f;   // ~12.5 s per revolution

    // --- Clustered spawn distribution ----------------------------------------
    // The Clustered scenario places home points in a handful of gaussian blobs
    // instead of spreading them over the whole world. Same N, same kernels, very
    // different spatial statistics -- which is the entire point: it is the axis
    // along which the BruteForce/QuadTree winner is allowed to flip.
    //
    // CLUSTER_COUNT is 5 and index 0 sits on the WORLD CENTRE. That is not
    // decoration. The player starts at the centre, and the harness equivalence
    // and population checks both FAIL when a run produces zero kills. If every
    // blob sat out on the ring, a Clustered run would have nothing inside the
    // aura and those checks would report a false negative. Blob 0 guarantees the
    // aura always has something to kill.
    //
    // CLUSTER_RING 600 vs AGGRO_RANGE 320: a ring blob's near edge lands around
    // 600 - 3*SIGMA = 270... which IS inside aggro. That is intentional. The
    // inner fringe of each blob leaks toward the player and keeps the world from
    // being a set of hermetically sealed islands, while the bulk (>1 sigma out)
    // stays home and holds the distribution.
    constexpr int   CLUSTER_COUNT     = 5;
    constexpr float CLUSTER_RING      = 600.0f;
    constexpr float CLUSTER_SIGMA     = 110.0f;
    // Box-Muller is unbounded; one unlucky draw could throw a home point far
    // outside the world and get clamped onto the wall, which would build a fake
    // high-density edge that has nothing to do with the scenario. Clamping the
    // radius in sigmas keeps every blob compact and keeps the clamp path cold.
    constexpr float CLUSTER_SIGMA_CAP = 3.0f;

    constexpr float PLAYER_SPEED      = 150.0f;
    constexpr float PLAYER_RADIUS     = 12.0f;

    constexpr int   MAX_PROJECTILES   = 500;
    constexpr float PROJECTILE_SPEED  = 400.0f;
    constexpr float PROJECTILE_RADIUS = 4.0f;
    constexpr float FIRE_RATE         = 0.15f;

    // --- Aura: the player's primary weapon -----------------------------------
    // A circular area-of-effect pulse centred on the player, on a fixed 2 s
    // cadence. AURA_DAMAGE is deliberately just over half of an enemy's 100 HP,
    // so a full-health enemy dies to the SECOND pulse it is caught in. One-shot
    // damage would clear the ring instantly and make the population swing
    // violently between pulses; two pulses gives a visible kill gradient and a
    // smoother workload for the profiler to sample.
    constexpr float AURA_RADIUS       = 220.0f;
    constexpr float AURA_INTERVAL     = 2.0f;
    constexpr float AURA_DAMAGE       = 55.0f;
    constexpr float AURA_KNOCKBACK    = 180.0f;
    constexpr float AURA_FLASH_TIME   = 0.30f;  // how long the pulse ring is drawn

    // --- Respawn -------------------------------------------------------------
    // Killing enemies without replacing them would quietly shrink N, and every
    // ms/tick number in this demo is a function of N. A falling entity count
    // makes BOTH modes look faster over time and would let a mode toggle take
    // credit for a workload that simply got smaller. The respawner holds the
    // live population at the target so the only variables left are the
    // algorithm and the memory layout.
    //
    // A slot is revived AT ITS OWN HOME POINT, not on a ring around the player.
    // This is what makes respawn distribution-preserving: the hole the aura
    // burns in the field refills at exactly the rate it drains, because every
    // dead entity goes back where it came from. The earlier ring-around-the-
    // player rule would have injected a player-centred blob into every run and
    // quietly overwritten whichever distribution was selected -- the respawner
    // would have become the dominant spatial process in the benchmark.
    //
    // SPAWN_EXCLUSION only keeps the revived entity out of the kill zone, so it
    // is never born already dead. RESPAWN_JITTER keeps repeat respawns off a
    // single pixel, which would otherwise stack N bodies at one coordinate and
    // hand the QuadTree a degenerate leaf that no real game would produce.
    constexpr float SPAWN_EXCLUSION   = AURA_RADIUS * 1.2f;
    constexpr float RESPAWN_JITTER    = 40.0f;
    // Per-tick cap. Uncapped refill snaps the count back the same frame a pulse
    // lands; the cap spreads it over a few frames so deaths are actually
    // visible, while still recovering long before the next 2 s pulse.
    constexpr int   RESPAWN_PER_TICK  = 256;

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

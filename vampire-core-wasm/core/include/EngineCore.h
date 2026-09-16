#pragma once
#include "Vector2D.h"
#include "EnemyAoS.h"
#include "EnemySoA.h"
#include "QuadTree.h"
#include "Config.h"
#include "Spawn.h"
#include <vector>

enum class CollisionMode { BruteForce, QuadTree, UniformGrid, SpatialHash };
enum class MemoryMode    { AoS, SoA };

struct Projectile {
    Vector2D position;
    Vector2D velocity;
    bool     alive;
};

struct PerfStats {
    float         fps;
    float         frameTimeMs;   // rolling mean over PERF_WINDOW ticks
    // The RAW cost of the tick that just ran. frameTimeMs is smoothed, which is
    // right for a HUD and wrong for a profiler: a mean cannot tell "steadily
    // 12 ms" apart from "8 ms with periodic 40 ms spikes", and those two have
    // completely different fixes. The JS logger accumulates this per frame to
    // reconstruct the real distribution (p50/p95/p99) before shipping it to the
    // optimizer agent.
    float         lastTickMs;
    int           aliveEnemies;
    int           activeProjectiles;
    int           kills;         // cumulative since init()
    int           auraPulses;    // cumulative since init()
    CollisionMode collisionMode;
    MemoryMode    memoryMode;
    // The scenario the numbers above were produced under. A frame time is
    // meaningless without it: 8 ms at N=5000 is a QuadTree win under Uniform and
    // a QuadTree loss under Clustered, and an optimizer agent handed the number
    // without the distribution would have no way to tell those apart.
    SpawnDistribution spawnDist;
};

class EngineCore {
public:
    EngineCore();

    // `seed` reseeds the RNG before BOTH containers are populated, so the AoS
    // and SoA paths start from byte-identical world state. Without this the two
    // benchmark columns would be simulating different scenarios and the timings
    // could not be compared. See runEquivalenceCheck() in main.cpp.
    void init(int enemyCount = Config::DEFAULT_ENEMIES, unsigned seed = 1337u);
    void tick(float dt);

    // Both reset the frame-time window: the whole point of flipping a mode is
    // to watch the cost change, and an average that still carries the old
    // mode's samples would smear the transition over the next few seconds.
    void setCollisionMode(CollisionMode m) { collisionMode_ = m; resetPerfWindow(); }
    void setMemoryMode(MemoryMode m)       { memoryMode_    = m; resetPerfWindow(); }
    void setEnemyCount(int n);

    // Rebuilds the world from the remembered seed under the new distribution.
    //
    // A rebuild, not an in-place shuffle: home points are per-entity state and
    // every live entity would otherwise keep steering at wherever it already
    // was, so the field would take tens of seconds to migrate and the operator
    // would be reading a blend of the two scenarios the whole time. The renderer
    // re-reads getPosX/getPosY every frame, so reallocating here is safe.
    void setSpawnDistribution(SpawnDistribution d);
    SpawnDistribution getSpawnDistribution() const { return spawnDist_; }

    void setPlayerInput(float dx, float dy);

    // Direct Wasm linear-memory pointers for zero-copy JS rendering.
    //
    // getPosX/getPosY follow the ACTIVE memory mode. The renderer must use
    // these, not the layout-specific accessors: in AoS mode the SoA arrays are
    // never written, so reading getSoAPosX() there would draw a field of
    // enemies frozen at their spawn points while the HUD happily reported a
    // running simulation.
    const float* getPosX()      const;
    const float* getPosY()      const;

    // Layout-specific, for tests that deliberately compare the two.
    const float* getSoAPosX()   const;
    const float* getSoAPosY()   const;
    const float* getAoSPosX()   const;
    const float* getAoSPosY()   const;

    // Per-entity alive flags, per layout. Only the native equivalence check
    // needs these. Position agreement alone is not enough once the aura can
    // kill: two layouts could hold identical coordinates while disagreeing
    // about WHICH entities are corpses, and that divergence would then feed
    // different pair counts into the repulsion kernel -- the exact failure the
    // equivalence check exists to catch.
    const uint8_t* getSoAAlive() const;
    const uint8_t* getAoSAlive() const;

    int          getEntityCount() const;

    // --- player / aura, for the renderer -------------------------------------
    float getPlayerX()    const { return playerPos_.x; }
    float getPlayerY()    const { return playerPos_.y; }
    float getAuraRadius() const { return Config::AURA_RADIUS; }
    // 0 just after a pulse -> 1 the instant before the next one. The renderer
    // uses this to ramp the ring's opacity so the 2 s cadence is legible
    // without the HUD having to run its own timer (which would drift away from
    // the simulation's clock the moment a frame is dropped).
    float getAuraPhase()  const {
        const float t = 1.0f - auraTimer_ / Config::AURA_INTERVAL;
        return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    }
    // Seconds of pulse flash remaining, for the bright expanding ring.
    float getAuraFlash()  const { return auraFlash_ > 0.f ? auraFlash_ : 0.f; }

    int   getKills()      const { return kills_; }
    int   getAliveCount() const;
    float getLastTickMs() const { return stats_.lastTickMs; }

    PerfStats    getStats() const { return stats_; }

private:
    void updateRepulsionBruteForce(float dt);
    void updateRepulsionQuadTree(float dt);
    void updateRepulsionUniformGrid(float dt);
    void updateRepulsionSpatialHash(float dt);
    void updateRepulsionSpatial(float dt, bool hashed);
    void updateProjectiles(float dt);
    void updateAura(float dt);
    void fireAura();
    void respawnDead();
    void fireProjectile();
    void resetPerfWindow();

    // Single path that (re)populates both containers from one spawn set, used by
    // init(), setEnemyCount() and setSpawnDistribution(). Having exactly one
    // such path is what guarantees the AoS and SoA worlds are identical no
    // matter which control the operator touched.
    void rebuildWorld(int count);

    CollisionMode collisionMode_ = CollisionMode::QuadTree;
    MemoryMode    memoryMode_    = MemoryMode::SoA;
    // Uniform is the default because it is the scenario the QuadTree is
    // supposed to win; the demo opens on the expected result and the operator
    // goes looking for the case that breaks it.
    SpawnDistribution spawnDist_ = SpawnDistribution::Uniform;

    EnemyContainerSoA        soa_;
    EnemyContainerAoS        aos_;
    std::vector<Projectile>  projectiles_;
    float                    fireTimer_ = 0.f;

    // The spawn set stays live for the whole run: respawnDead() revives a slot
    // AT ITS OWN HOME POINT, which is what makes the respawner preserve the
    // selected distribution instead of slowly replacing it with a blob centred
    // on the player.
    std::vector<SpawnSample> spawnSet_;

    // Remembered so a distribution or count change can rebuild a world that is
    // still reproducible. Without it, changing scenario mid-session would draw
    // from wherever the stream happened to be and the run would stop being
    // comparable to anything.
    unsigned seed_ = 1337u;

    // Simulation clock, only used to phase the home orbits. Kept here rather
    // than inside each container so both layouts read the same value.
    float simTime_ = 0.f;

    Vector2D playerPos_   = {Config::WORLD_WIDTH * 0.5f, Config::WORLD_HEIGHT * 0.5f};
    Vector2D playerInput_ = {};

    // Aura pulse cadence. Starts at a full interval so the first pulse lands
    // two seconds in rather than on frame one, which would kill the starting
    // cluster before the user has seen it.
    float auraTimer_  = Config::AURA_INTERVAL;
    float auraFlash_  = 0.f;
    int   auraPulses_ = 0;

    // The respawner recycles dead slots in place and never resizes, so the
    // container's own size IS the population target. Resizing mid-run would
    // invalidate the Wasm heap pointers the renderer re-reads every frame.
    int kills_ = 0;

    QuadTree quadTree_;

    // Per-instance, so two EngineCores never share a random stream.
    Rng rng_{1337u};

    mutable std::vector<float>   aosPosXCache_, aosPosYCache_;
    mutable std::vector<uint8_t> aosAliveCache_;

    PerfStats stats_{};

    // Rolling window, not a cumulative mean. The previous version averaged
    // every tick since init() and never reset, so after a few seconds the
    // number was so damped that toggling BruteForce -> QuadTree barely moved
    // it -- the demo's single most important interaction produced no visible
    // response.
    static constexpr int PERF_WINDOW = 30;
    float perfSamples_[PERF_WINDOW]{};
    int   perfCount_  = 0;
    int   perfCursor_ = 0;
};

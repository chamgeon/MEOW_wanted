#pragma once
#include "Vector2D.h"
#include "EnemyAoS.h"
#include "EnemySoA.h"
#include "QuadTree.h"
#include "Config.h"
#include <vector>

enum class CollisionMode { BruteForce, QuadTree };
enum class MemoryMode    { AoS, SoA };

struct Projectile {
    Vector2D position;
    Vector2D velocity;
    bool     alive;
};

struct PerfStats {
    float         fps;
    float         frameTimeMs;
    int           aliveEnemies;
    int           activeProjectiles;
    CollisionMode collisionMode;
    MemoryMode    memoryMode;
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
    int          getEntityCount() const;

    PerfStats    getStats() const { return stats_; }

private:
    void updateRepulsionBruteForce(float dt);
    void updateRepulsionQuadTree(float dt);
    void updateProjectiles(float dt);
    void fireProjectile();
    void resetPerfWindow();

    CollisionMode collisionMode_ = CollisionMode::QuadTree;
    MemoryMode    memoryMode_    = MemoryMode::SoA;

    EnemyContainerSoA       soa_;
    EnemyContainerAoS       aos_;
    std::vector<Projectile> projectiles_;
    float                   fireTimer_ = 0.f;

    Vector2D playerPos_   = {Config::WORLD_WIDTH * 0.5f, Config::WORLD_HEIGHT * 0.5f};
    Vector2D playerInput_ = {};

    QuadTree quadTree_;

    // Per-instance, so two EngineCores never share a random stream.
    Rng rng_{1337u};

    mutable std::vector<float> aosPosXCache_, aosPosYCache_;

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

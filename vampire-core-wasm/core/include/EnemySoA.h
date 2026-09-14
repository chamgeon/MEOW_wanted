#pragma once
#include "Config.h"
#include "EnemyAoS.h"   // AIState, StatusBit -- shared by both layouts
#include "Spawn.h"
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Structure-of-Arrays layout.
//
// This container holds EXACTLY the same per-entity state as EnemyAoS -- same
// fields, same simulation, same results. The only thing that changes is the
// arrangement in memory: one array per field instead of one struct per entity.
// Keeping the field sets identical is what makes the A/B comparison honest;
// if SoA quietly tracked less state it would be winning by doing less work.
//
// The payoff shows up in the repulsion kernel, which is O(N^2) and reads
// nothing but position:
//
//   AoS  every neighbour costs a 64 B cache line to deliver 8 B of position.
//        At N=10000 the array is 1.25 MB, past L2, so those are real misses.
//
//   SoA  posX and posY stream contiguously. The kernel's hot working set is
//        2 * 10000 * 4 B = 80 KB, which stays resident, and every byte pulled
//        into cache is a byte the loop actually uses.
//
// The cold fields (animation, status effects, progression) still exist and are
// still simulated -- they just live in arrays the inner loop never touches, so
// they stop evicting the data it does touch.
// ---------------------------------------------------------------------------
struct EnemyContainerSoA {
    // --- hot: read every iteration of the O(N^2) repulsion kernel ------------
    std::vector<float> posX, posY;

    // --- warm: read once per entity per frame --------------------------------
    std::vector<float> velX, velY;
    std::vector<float> knockX, knockY;

    // --- combat ---------------------------------------------------------------
    std::vector<float> health, maxHealth, radius;
    std::vector<float> contactDamage, attackCooldown, hitFlashTimer;

    // --- AI state machine -----------------------------------------------------
    // targetX/targetY are HOME points (see the note in EnemyAoS.h): persistent
    // per-entity state that decides where an out-of-aggro enemy holds station,
    // which is what lets a spawn distribution survive long enough to benchmark.
    std::vector<float>   stateTimer, targetX, targetY, aggroRange;
    std::vector<uint8_t> aiState;

    // --- status effects -------------------------------------------------------
    std::vector<float> burnTimer, burnDps;
    std::vector<float> slowTimer, slowFactor;
    std::vector<float> poisonTimer, poisonDps;

    // --- animation / rendering ------------------------------------------------
    std::vector<float>   animTimer, animSpeed;
    std::vector<int32_t> animFrame, spriteId, facing;

    // --- progression / bookkeeping --------------------------------------------
    std::vector<int32_t>  xpValue, tier, spawnTick;
    std::vector<uint32_t> statusFlags;

    // Deliberately uint8_t, not bool. std::vector<bool> is a bit-packed proxy
    // container: every alive[j] read becomes a byte load plus a shift and a
    // mask, and the proxy reference blocks auto-vectorization entirely. That
    // cost lands in the innermost loop of an O(N^2) kernel and was enough to
    // make this "cache-friendly" layout measure slower than the AoS baseline.
    std::vector<uint8_t> alive;

    int size = 0;

    // Same spawn set the AoS container is given -- see EnemyContainerAoS::init.
    void init(const std::vector<SpawnSample>& spawn);
    void update(float dt, float simTime, float playerX, float playerY);
    int  aliveCount() const;

    // Mirror of EnemyContainerAoS::respawn -- same fields, same values, same
    // order. See the comment there for why the random inputs are arguments.
    void respawn(int i, float x, float y, float homeX, float homeY,
                 float stateTimerSeed, float animTimerSeed);

    const float* posXPtr() const { return posX.data(); }
    const float* posYPtr() const { return posY.data(); }

    // Bytes of per-entity state this layout owns, for the harness banner.
    // 126 vs the AoS struct's 128 -- the only difference is the struct's tail
    // padding. The two layouts hold the same state; that is the point.
    static constexpr int bytesPerEntity() {
        return 4 * 31    // 31 float/int32/uint32 arrays
             + 1 * 2;    // aiState + alive
    }
    // Bytes the repulsion kernel actually streams per entity.
    static constexpr int hotBytesPerEntity() { return 8; }  // posX + posY
};

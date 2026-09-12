#pragma once
#include "Vector2D.h"
#include "Config.h"
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// Array-of-Structures layout: the conventional OOP baseline.
//
// WHY THIS STRUCT IS THIS BIG
//
// An earlier version of this demo carried only position, velocity, health,
// radius and alive -- 28 bytes. With that struct the AoS/SoA benchmark showed
// no difference at all, and the reason is arithmetic, not opinion:
//
//     10,000 entities * 28 B = 273 KB
//
// which fits inside a typical 1 MB+ L2 cache. Nothing ever misses, so there is
// no cache penalty to measure and the whole Data-Oriented Design argument
// silently evaporates. A 28-byte enemy is not a realistic baseline; it is a
// straw man that happens to flatter AoS.
//
// A real bullet-hell enemy carries what is below: an AI state machine, an
// animation cursor, per-entity cooldowns, status effects, knockback impulse and
// progression data. Every one of these fields is actually read and written by
// EnemyContainerAoS::update() -- none of it is dead padding inserted to rig the
// comparison. EnemyContainerSoA stores the exact same fields and runs the exact
// same simulation; the only difference is how they are arranged in memory.
//
// THE CONSEQUENCE
//
// At 128 B/entity the working set is 1.25 MB and spills out of L2. The
// repulsion kernel only ever touches position (8 of those 128 bytes), so:
//
//     AoS: every cache line fetched is 8/64 = 12.5% useful
//     SoA: posX/posY stream contiguously, 64/64 = 100% useful
//
// That ratio is the entire point of the demo, and it only becomes visible once
// the baseline is honest about how fat a real game entity is.
// ---------------------------------------------------------------------------

enum class AIState : uint8_t {
    Chase = 0,   // default: home in on the player
    Wander,      // brief drift after being knocked back
    Stunned,     // cannot act
    Enraged      // speed buff after surviving long enough
};

struct EnemyAoS {
    // --- hot: touched by movement + repulsion --------------------------------
    Vector2D position;         //  8
    Vector2D velocity;         //  8
    Vector2D knockback;        //  8  decaying impulse from projectile hits

    // --- combat --------------------------------------------------------------
    float    health;           //  4
    float    maxHealth;        //  4
    float    radius;           //  4
    float    contactDamage;    //  4
    float    attackCooldown;   //  4  seconds until it can touch-damage again
    float    hitFlashTimer;    //  4  drives the white flash on the sprite

    // --- AI state machine ----------------------------------------------------
    float    stateTimer;       //  4  seconds remaining in the current state
    float    targetX;          //  4  where it is steering (player, or wander pt)
    float    targetY;          //  4
    float    aggroRange;       //  4

    // --- status effects ------------------------------------------------------
    float    burnTimer;        //  4
    float    burnDps;          //  4
    float    slowTimer;        //  4
    float    slowFactor;       //  4  1.0 = unslowed
    float    poisonTimer;      //  4
    float    poisonDps;        //  4

    // --- animation / rendering ----------------------------------------------
    float    animTimer;        //  4  seconds into the current frame
    float    animSpeed;        //  4
    int32_t  animFrame;        //  4
    int32_t  spriteId;         //  4
    int32_t  facing;           //  4  -1 left, +1 right

    // --- progression / bookkeeping -------------------------------------------
    int32_t  xpValue;          //  4
    int32_t  tier;             //  4
    int32_t  spawnTick;        //  4
    uint32_t statusFlags;      //  4  bitfield: burning/slowed/poisoned/...
    AIState  aiState;          //  1
    uint8_t  alive;            //  1
    uint8_t  _pad[2];          //  2  round up to a 128 B power-of-two stride
};

static_assert(sizeof(EnemyAoS) == 128,
              "EnemyAoS is expected to be 128 B so the N=10000 working set "
              "(1.25 MB) deliberately exceeds a typical L2 cache.");

// Status effect bits, shared by both layouts.
namespace StatusBit {
    constexpr uint32_t Burning  = 1u << 0;
    constexpr uint32_t Slowed   = 1u << 1;
    constexpr uint32_t Poisoned = 1u << 2;
    constexpr uint32_t Enraged  = 1u << 3;
}

struct EnemyContainerAoS {
    std::vector<EnemyAoS> enemies;

    void init(int count, uint32_t seed = 1337u);
    void update(float dt, Vector2D playerPos);
    int  aliveCount() const;
};

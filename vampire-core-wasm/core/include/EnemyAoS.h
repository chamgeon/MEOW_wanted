#pragma once
#include "Vector2D.h"
#include "Config.h"
#include "Spawn.h"
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
    // HOME POINT -- the coordinate this enemy holds station around whenever the
    // player is further away than aggroRange. It is persistent state, written
    // once at spawn and read every tick thereafter.
    //
    // These two fields used to be scratch: update() wrote the player's position
    // into them and read it back three lines later, which is a local variable
    // wearing a struct field's clothes. Giving them a real job is what buys the
    // spawn-distribution axis for ZERO extra bytes per entity -- sizeof is still
    // 128 and the cache-cliff arithmetic at the top of this file still holds.
    float    targetX;          //  4  home point x
    float    targetY;          //  4  home point y
    // Read every tick (it decides chase vs. hold). It was previously assigned
    // 600 at spawn and never looked at again, so the field was dead weight in a
    // struct whose whole argument is about how much weight it carries.
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

    // Populated from a spawn set built ONCE by EngineCore and handed to both
    // layouts. See SpawnSample in Spawn.h for why this is an argument rather
    // than something each container rolls for itself.
    void init(const std::vector<SpawnSample>& spawn);
    // simTime is the simulation clock, used only to phase the home orbit. It is
    // passed in rather than accumulated here so both containers share one clock
    // and cannot drift a fraction of a revolution apart.
    void update(float dt, float simTime, Vector2D playerPos);
    int  aliveCount() const;

    // Revive slot i at (x, y) with home point (homeX, homeY) and completely
    // fresh state.
    //
    // Position and home are separate arguments because the respawner has to be
    // allowed to nudge WHERE a body appears (out of the kill zone, off a shared
    // pixel) without moving the neighbourhood that body belongs to. Folding them
    // into one pair would let those nudges accumulate into the home points over
    // a long run, and the selected distribution would slowly erode into a shell
    // around wherever the player had been standing.
    //
    // The randomised fields are passed in rather than drawn here so that
    // EngineCore can feed the SAME values to both layouts from one RNG stream.
    // If each container rolled its own, the AoS and SoA worlds would diverge
    // the first time anything died and the layout benchmark would be comparing
    // two different simulations.
    //
    // Every field is reset, not just position: a corpse still carries the burn
    // timer, stun state and knockback impulse that killed it, and reusing the
    // slot without clearing them would spawn an enemy that dies on arrival.
    void respawn(int i, float x, float y, float homeX, float homeY,
                 float stateTimerSeed, float animTimerSeed);
};

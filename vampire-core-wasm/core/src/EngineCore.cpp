#include "../include/EngineCore.h"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <chrono>


// ---- shared simulation rules --------------------------------------------
//
// Both containers below run this identical logic. It is factored into free
// functions so the AoS and SoA paths cannot silently drift apart -- if they
// did, the benchmark would be comparing two different workloads and the
// layout comparison would mean nothing.

namespace {

// Advance an AI state machine by one step. Returns the new state and writes
// the seconds it should persist. Chase is the steady state; the others decay
// back into it.
inline AIState stepAI(AIState cur, float& timer, float dt, float healthFrac, int idx) {
    timer -= dt;
    if (timer > 0.f) return cur;

    switch (cur) {
        case AIState::Stunned: timer = 0.35f; return AIState::Wander;
        case AIState::Wander:  timer = 1.50f; return AIState::Chase;
        case AIState::Enraged: timer = 2.00f; return AIState::Chase;
        case AIState::Chase:
        default:
            // A wounded enemy that has survived a while works itself up.
            if (healthFrac < 0.5f && (idx & 7) == 0) { timer = 2.0f; return AIState::Enraged; }
            timer = 1.0f;
            return AIState::Chase;
    }
}

inline float speedFor(AIState s, float slowFactor) {
    float mult = 1.0f;
    if (s == AIState::Enraged) mult = 1.6f;
    else if (s == AIState::Stunned) mult = 0.0f;
    else if (s == AIState::Wander)  mult = 0.4f;
    return Config::ENEMY_SPEED * mult * slowFactor;
}

// Damage-over-time effects. Mutates the timers and returns the health delta.
inline float tickStatus(float dt, float& burnTimer, float burnDps,
                        float& poisonTimer, float poisonDps,
                        float& slowTimer, float& slowFactor,
                        uint32_t& flags) {
    float dmg = 0.f;
    if (burnTimer > 0.f)   { burnTimer   -= dt; dmg += burnDps   * dt; }
    else flags &= ~StatusBit::Burning;

    if (poisonTimer > 0.f) { poisonTimer -= dt; dmg += poisonDps * dt; }
    else flags &= ~StatusBit::Poisoned;

    if (slowTimer > 0.f)   { slowTimer -= dt; }
    else { slowFactor = 1.0f; flags &= ~StatusBit::Slowed; }

    return dmg;
}

// Sprite animation cursor: 8 frames, wrapping.
inline void tickAnim(float dt, float& animTimer, float animSpeed, int32_t& animFrame) {
    animTimer += dt * animSpeed;
    while (animTimer >= 1.0f) { animTimer -= 1.0f; animFrame = (animFrame + 1) & 7; }
}

// Where this enemy steers on THIS tick: the player if it is inside aggro range,
// otherwise a slow orbit around the entity's own home point.
//
// This one branch is what turns spawn placement into a durable scenario. Before
// it, every enemy homed on the player from anywhere in the world, so any spatial
// arrangement -- clustered, uniform, anything -- decayed into a single ball
// within a few seconds. A distribution toggle under those dynamics would be
// measuring the shape of a transient, which is not a benchmark axis at all.
//
// The orbit matters as much as the aggro test. A held-still enemy would freeze
// the field, and a frozen field silently changes what the demo measures: the
// QuadTree would rebuild a bit-identical tree every tick, the repulsion pair set
// would never change, and both kernels would start benefiting from a stability
// no real game has. A slow orbit keeps real motion in the workload while holding
// each entity inside its own neighbourhood.
//
// The golden-angle term (2.39996323 rad) offsets each entity's orbit phase. With
// a shared phase every enemy in a blob would swing the same direction at the
// same instant -- the blob would translate rigidly instead of churning, and the
// repulsion kernel would see a near-static pair set for the same reason a frozen
// field would.
inline void steerTarget(int idx, float simTime,
                        float posX, float posY,
                        float homeX, float homeY, float aggro,
                        float playerX, float playerY,
                        float& tx, float& ty) {
    const float pdx = playerX - posX;
    const float pdy = playerY - posY;
    if (pdx*pdx + pdy*pdy <= aggro*aggro) { tx = playerX; ty = playerY; return; }

    const float ph = simTime * Config::HOME_ORBIT_HZ * TAU_F + (float)idx * 2.39996323f;
    tx = homeX + std::cos(ph) * Config::HOME_ORBIT_RADIUS;
    ty = homeY + std::sin(ph) * Config::HOME_ORBIT_RADIUS;
}

}  // namespace

// ---- EnemyContainerAoS ----

// A freshly spawned enemy and a recycled one must be indistinguishable, so
// init() draws the four random inputs and then defers to respawn() for every
// field. Duplicating the field list here is what would let the two drift apart
// -- and a respawned enemy that kept, say, a stale burnTimer would die on
// arrival, turning the respawner into an invisible treadmill that silently
// drained the population the benchmark depends on holding steady.
void EnemyContainerAoS::respawn(int i, float x, float y, float homeX, float homeY,
                                float stateTimerSeed, float animTimerSeed) {
    EnemyAoS& e = enemies[i];
    e.position       = {x, y};
    e.velocity       = {};
    e.knockback      = {};
    e.health         = 100.f;
    e.maxHealth      = 100.f;
    e.radius         = Config::ENEMY_RADIUS;
    e.contactDamage  = 5.f;
    e.attackCooldown = 0.f;
    e.hitFlashTimer  = 0.f;
    e.stateTimer     = stateTimerSeed;
    // Home is passed in, not copied from the spawn position: see the note on
    // the declaration in EnemyAoS.h.
    e.targetX        = homeX;
    e.targetY        = homeY;
    e.aggroRange     = Config::AGGRO_RANGE;
    e.burnTimer      = 0.f;  e.burnDps    = 0.f;
    e.slowTimer      = 0.f;  e.slowFactor = 1.f;
    e.poisonTimer    = 0.f;  e.poisonDps  = 0.f;
    e.animTimer      = animTimerSeed;
    e.animSpeed      = 8.f;
    // Identity fields stay a pure function of the slot index, so a recycled
    // slot keeps the same sprite it always had in BOTH layouts.
    e.animFrame      = i & 7;
    e.spriteId       = i % 4;
    e.facing         = 1;
    e.xpValue        = 3;
    e.tier           = i % 3;
    e.spawnTick      = 0;
    e.statusFlags    = 0u;
    e.aiState        = AIState::Chase;
    e.alive          = 1;
}

// No RNG here any more. Both layouts consume the SAME SpawnSample array, so
// world identity is now structural rather than a promise two duplicated loops
// make to each other in a comment.
void EnemyContainerAoS::init(const std::vector<SpawnSample>& spawn) {
    const int count = (int)spawn.size();
    enemies.assign(count, EnemyAoS{});
    for (int i = 0; i < count; ++i) {
        const SpawnSample& s = spawn[(size_t)i];
        respawn(i, s.x, s.y, s.x, s.y, s.stateTimer, s.animTimer);
    }
}

void EnemyContainerAoS::update(float dt, float simTime, Vector2D playerPos) {
    const int n = (int)enemies.size();
    for (int i = 0; i < n; ++i) {
        EnemyAoS& e = enemies[i];
        if (!e.alive) continue;

        e.health -= tickStatus(dt, e.burnTimer, e.burnDps, e.poisonTimer, e.poisonDps,
                               e.slowTimer, e.slowFactor, e.statusFlags);
        if (e.health <= 0.f) { e.alive = 0; continue; }

        e.aiState = stepAI(e.aiState, e.stateTimer, dt, e.health / e.maxHealth, i);

        // targetX/targetY are the persistent home point and are NOT written
        // here -- overwriting them would erase the distribution one tick in.
        float tx, ty;
        steerTarget(i, simTime, e.position.x, e.position.y,
                    e.targetX, e.targetY, e.aggroRange,
                    playerPos.x, playerPos.y, tx, ty);

        float dx = tx - e.position.x;
        float dy = ty - e.position.y;
        float len = std::sqrt(dx*dx + dy*dy);
        if (len > 1e-6f) { dx /= len; dy /= len; }

        const float spd = speedFor(e.aiState, e.slowFactor);
        e.velocity = {dx * spd, dy * spd};
        e.facing   = (dx < 0.f) ? -1 : 1;

        // Knockback is an impulse that decays exponentially.
        e.position.x = std::clamp(e.position.x + (e.velocity.x + e.knockback.x) * dt, 0.f, Config::WORLD_WIDTH);
        e.position.y = std::clamp(e.position.y + (e.velocity.y + e.knockback.y) * dt, 0.f, Config::WORLD_HEIGHT);
        e.knockback *= 0.90f;

        if (e.attackCooldown > 0.f) e.attackCooldown -= dt;
        if (e.hitFlashTimer  > 0.f) e.hitFlashTimer  -= dt;

        tickAnim(dt, e.animTimer, e.animSpeed, e.animFrame);
    }
}

int EnemyContainerAoS::aliveCount() const {
    int n = 0;
    for (const auto& e : enemies) n += (int)e.alive;
    return n;
}

// ---- EnemyContainerSoA ----

void EnemyContainerSoA::init(const std::vector<SpawnSample>& spawn) {
    const int count = (int)spawn.size();
    size = count;
    posX.assign(count, 0.f);   posY.assign(count, 0.f);
    velX.assign(count, 0.f);   velY.assign(count, 0.f);
    knockX.assign(count, 0.f); knockY.assign(count, 0.f);

    health.assign(count, 100.f); maxHealth.assign(count, 100.f);
    radius.assign(count, Config::ENEMY_RADIUS);
    contactDamage.assign(count, 5.f);
    attackCooldown.assign(count, 0.f);
    hitFlashTimer.assign(count, 0.f);

    stateTimer.assign(count, 0.f);
    targetX.assign(count, 0.f); targetY.assign(count, 0.f);
    aggroRange.assign(count, Config::AGGRO_RANGE);
    aiState.assign(count, (uint8_t)AIState::Chase);

    burnTimer.assign(count, 0.f);   burnDps.assign(count, 0.f);
    slowTimer.assign(count, 0.f);   slowFactor.assign(count, 1.f);
    poisonTimer.assign(count, 0.f); poisonDps.assign(count, 0.f);

    animTimer.assign(count, 0.f); animSpeed.assign(count, 8.f);
    animFrame.assign(count, 0);   spriteId.assign(count, 0);
    facing.assign(count, 1);

    xpValue.assign(count, 3); tier.assign(count, 0); spawnTick.assign(count, 0);
    statusFlags.assign(count, 0u);
    alive.assign(count, 1);

    for (int i = 0; i < count; ++i) {
        const SpawnSample& s = spawn[(size_t)i];
        respawn(i, s.x, s.y, s.x, s.y, s.stateTimer, s.animTimer);
    }
}

// Field-for-field mirror of EnemyContainerAoS::respawn.
void EnemyContainerSoA::respawn(int i, float x, float y, float homeX, float homeY,
                                float stateTimerSeed, float animTimerSeed) {
    posX[i]           = x;      posY[i]      = y;
    velX[i]           = 0.f;    velY[i]      = 0.f;
    knockX[i]         = 0.f;    knockY[i]    = 0.f;
    health[i]         = 100.f;  maxHealth[i] = 100.f;
    radius[i]         = Config::ENEMY_RADIUS;
    contactDamage[i]  = 5.f;
    attackCooldown[i] = 0.f;
    hitFlashTimer[i]  = 0.f;
    stateTimer[i]     = stateTimerSeed;
    targetX[i]        = homeX;  targetY[i]   = homeY;   // home point, as in AoS
    aggroRange[i]     = Config::AGGRO_RANGE;
    burnTimer[i]      = 0.f;    burnDps[i]   = 0.f;
    slowTimer[i]      = 0.f;    slowFactor[i]= 1.f;
    poisonTimer[i]    = 0.f;    poisonDps[i] = 0.f;
    animTimer[i]      = animTimerSeed;
    animSpeed[i]      = 8.f;
    animFrame[i]      = i & 7;
    spriteId[i]       = i % 4;
    facing[i]         = 1;
    xpValue[i]        = 3;
    tier[i]           = i % 3;
    spawnTick[i]      = 0;
    statusFlags[i]    = 0u;
    aiState[i]        = (uint8_t)AIState::Chase;
    alive[i]          = 1;
}

void EnemyContainerSoA::update(float dt, float simTime, float playerX, float playerY) {
    for (int i = 0; i < size; ++i) {
        if (!alive[i]) continue;

        health[i] -= tickStatus(dt, burnTimer[i], burnDps[i], poisonTimer[i], poisonDps[i],
                                slowTimer[i], slowFactor[i], statusFlags[i]);
        if (health[i] <= 0.f) { alive[i] = 0; continue; }

        const AIState st = stepAI((AIState)aiState[i], stateTimer[i], dt,
                                  health[i] / maxHealth[i], i);
        aiState[i] = (uint8_t)st;

        // Home point is read, never written -- mirror of the AoS path.
        float tx, ty;
        steerTarget(i, simTime, posX[i], posY[i],
                    targetX[i], targetY[i], aggroRange[i],
                    playerX, playerY, tx, ty);

        float dx = tx - posX[i];
        float dy = ty - posY[i];
        float len = std::sqrt(dx*dx + dy*dy);
        if (len > 1e-6f) { dx /= len; dy /= len; }

        const float spd = speedFor(st, slowFactor[i]);
        velX[i]  = dx * spd;
        velY[i]  = dy * spd;
        facing[i] = (dx < 0.f) ? -1 : 1;

        posX[i] = std::clamp(posX[i] + (velX[i] + knockX[i]) * dt, 0.f, Config::WORLD_WIDTH);
        posY[i] = std::clamp(posY[i] + (velY[i] + knockY[i]) * dt, 0.f, Config::WORLD_HEIGHT);
        knockX[i] *= 0.90f;
        knockY[i] *= 0.90f;

        if (attackCooldown[i] > 0.f) attackCooldown[i] -= dt;
        if (hitFlashTimer[i]  > 0.f) hitFlashTimer[i]  -= dt;

        tickAnim(dt, animTimer[i], animSpeed[i], animFrame[i]);
    }
}

int EnemyContainerSoA::aliveCount() const {
    int n = 0;
    for (int i = 0; i < size; ++i) n += (int)alive[i];
    return n;
}

// ---- EngineCore ----

EngineCore::EngineCore()
    : quadTree_(AABB{Config::WORLD_WIDTH*0.5f, Config::WORLD_HEIGHT*0.5f,
                     Config::WORLD_WIDTH*0.5f, Config::WORLD_HEIGHT*0.5f}) {}

// One spawn set, two containers. This is stronger than seeding both identically:
// there is now only one place where spawn randomness is drawn, so the AoS and
// SoA worlds cannot differ even if someone edits one container's init().
void EngineCore::rebuildWorld(int count) {
    buildSpawnSet(spawnDist_, count, seed_, spawnSet_);
    soa_.init(spawnSet_);
    aos_.init(spawnSet_);
    simTime_ = 0.f;
}

void EngineCore::init(int enemyCount, unsigned seed) {
    seed_ = seed;
    rng_  = Rng(seed);
    rebuildWorld(enemyCount);
    projectiles_.assign(Config::MAX_PROJECTILES, {});
    for (auto& p : projectiles_) p.alive = false;
    playerPos_  = {Config::WORLD_WIDTH * 0.5f, Config::WORLD_HEIGHT * 0.5f};
    auraTimer_  = Config::AURA_INTERVAL;
    auraFlash_  = 0.f;
    auraPulses_ = 0;
    kills_      = 0;
    stats_ = {};
    resetPerfWindow();
}

void EngineCore::resetPerfWindow() {
    perfCount_  = 0;
    perfCursor_ = 0;
}

void EngineCore::setEnemyCount(int n) {
    rebuildWorld(n);
    // The kill tally counts kills against a given population; carrying it
    // across a resize would attribute the old run's kills to the new N.
    kills_      = 0;
    auraPulses_ = 0;
    resetPerfWindow();
}

void EngineCore::setSpawnDistribution(SpawnDistribution d) {
    if (d == spawnDist_) return;
    spawnDist_ = d;
    rebuildWorld(getEntityCount());
    // Same reasoning as the kill reset in setEnemyCount: the tally belongs to
    // the scenario it was earned under. Carrying it across would let the HUD
    // credit the new distribution with the old one's kills.
    kills_      = 0;
    auraPulses_ = 0;
    resetPerfWindow();
}

void EngineCore::setPlayerInput(float dx, float dy) {
    float len = std::sqrt(dx*dx + dy*dy);
    if (len > 1e-6f) { dx /= len; dy /= len; }
    playerInput_ = {dx, dy};
}

void EngineCore::tick(float dt) {
    auto t0 = std::chrono::high_resolution_clock::now();

    // Deaths are counted by difference -- alive before the damage phases minus
    // alive after -- rather than incremented at each of the three sites that can
    // kill (damage-over-time, projectile, aura). Two extra O(N) scans is a
    // rounding error next to the O(N^2) kernel, and centralising it means the
    // AoS and SoA paths cannot disagree about the tally: a counter maintained
    // separately in each branch is exactly the kind of thing that drifts.
    const int aliveBefore = getAliveCount();

    // Advanced before the update so the first tick already carries a nonzero
    // orbit phase, which keeps the AoS and SoA paths aligned no matter which
    // one a given run happens to step first.
    simTime_ += dt;

    playerPos_.x = std::clamp(playerPos_.x + playerInput_.x * Config::PLAYER_SPEED * dt, 0.f, Config::WORLD_WIDTH);
    playerPos_.y = std::clamp(playerPos_.y + playerInput_.y * Config::PLAYER_SPEED * dt, 0.f, Config::WORLD_HEIGHT);

    if (memoryMode_ == MemoryMode::SoA)
        soa_.update(dt, simTime_, playerPos_.x, playerPos_.y);
    else
        aos_.update(dt, simTime_, playerPos_);

    if (collisionMode_ == CollisionMode::BruteForce)
        updateRepulsionBruteForce(dt);
    else
        updateRepulsionQuadTree(dt);

    updateAura(dt);

    fireTimer_ -= dt;
    if (fireTimer_ <= 0.f) { fireProjectile(); fireTimer_ = Config::FIRE_RATE; }
    updateProjectiles(dt);

    const int aliveAfter = getAliveCount();
    if (aliveAfter < aliveBefore) kills_ += aliveBefore - aliveAfter;

    // Last, so it is the only thing in the tick that raises the live count and
    // the subtraction above can never see a respawn as a negative kill.
    respawnDead();

    auto  t1      = std::chrono::high_resolution_clock::now();
    float frameMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    perfSamples_[perfCursor_] = frameMs;
    perfCursor_ = (perfCursor_ + 1) % PERF_WINDOW;
    if (perfCount_ < PERF_WINDOW) ++perfCount_;

    float sum = 0.f;
    for (int i = 0; i < perfCount_; ++i) sum += perfSamples_[i];
    const float meanMs = (perfCount_ > 0) ? sum / (float)perfCount_ : 0.f;

    stats_.frameTimeMs = meanMs;
    stats_.lastTickMs  = frameMs;
    // Headroom, not observed frame rate: how often this tick COULD run if the
    // simulation were the only cost. The browser's real fps is measured in JS
    // and displayed separately -- conflating the two would let a slow canvas
    // draw masquerade as a slow engine.
    stats_.fps = (meanMs > 0.f) ? 1000.f / meanMs : 0.f;
    stats_.collisionMode     = collisionMode_;
    stats_.memoryMode        = memoryMode_;
    stats_.spawnDist         = spawnDist_;
    // Reported AFTER the respawn, so this is the population the next tick will
    // actually simulate -- which is the N that the ms/tick number belongs to.
    stats_.aliveEnemies      = getAliveCount();
    stats_.kills             = kills_;
    stats_.auraPulses        = auraPulses_;
    int ap = 0; for (const auto& p : projectiles_) ap += (int)p.alive;
    stats_.activeProjectiles = ap;
}

int EngineCore::getAliveCount() const {
    return (memoryMode_ == MemoryMode::SoA) ? soa_.aliveCount() : aos_.aliveCount();
}

// ---------------------------------------------------------------------------
// Aura: the player's weapon.
//
// A pulse is a single O(N) radius test, fired once every AURA_INTERVAL. Note
// that it does NOT route through the collision-mode toggle, and that is on
// purpose: one query against a freshly built QuadTree would cost more than the
// linear scan it replaced (you pay O(N) to build the tree to serve O(1)
// queries), and folding a tree rebuild into the tick every 2 s would put a
// periodic spike into the very frame-time series this demo asks you to read.
// Keeping the aura a flat linear scan leaves the Collision toggle measuring
// exactly one thing: the repulsion kernel.
//
// It also means the aura's kill set is a pure function of position, so the AoS
// and SoA paths necessarily kill the same enemies -- see runEquivalenceCheck().
// ---------------------------------------------------------------------------
void EngineCore::updateAura(float dt) {
    if (auraFlash_ > 0.f) auraFlash_ -= dt;

    auraTimer_ -= dt;
    if (auraTimer_ > 0.f) return;

    fireAura();
    // += rather than = keeps the cadence locked to the simulation clock. With
    // a plain reset, a long frame would push every subsequent pulse later and
    // the "every 2 seconds" claim would slowly become false under load --
    // exactly when the demo is being watched most closely.
    auraTimer_ += Config::AURA_INTERVAL;
    if (auraTimer_ <= 0.f) auraTimer_ = Config::AURA_INTERVAL;  // huge dt catch-up
}

void EngineCore::fireAura() {
    ++auraPulses_;
    auraFlash_ = Config::AURA_FLASH_TIME;

    const float r   = Config::AURA_RADIUS;
    const float rSq = r * r;

    if (memoryMode_ == MemoryMode::SoA) {
        for (int i = 0; i < soa_.size; ++i) {
            if (!soa_.alive[i]) continue;
            const float dx = soa_.posX[i] - playerPos_.x;
            const float dy = soa_.posY[i] - playerPos_.y;
            const float d2 = dx*dx + dy*dy;
            if (d2 > rSq) continue;

            soa_.health[i]       -= Config::AURA_DAMAGE;
            soa_.hitFlashTimer[i] = 0.15f;

            const float d  = std::sqrt(d2);
            const float nx = (d > 1e-6f) ? dx / d : 0.f;
            const float ny = (d > 1e-6f) ? dy / d : 1.f;
            soa_.knockX[i] += nx * Config::AURA_KNOCKBACK;
            soa_.knockY[i] += ny * Config::AURA_KNOCKBACK;

            if (soa_.health[i] <= 0.f) soa_.alive[i] = 0;
        }
    } else {
        for (auto& e : aos_.enemies) {
            if (!e.alive) continue;
            const float dx = e.position.x - playerPos_.x;
            const float dy = e.position.y - playerPos_.y;
            const float d2 = dx*dx + dy*dy;
            if (d2 > rSq) continue;

            e.health        -= Config::AURA_DAMAGE;
            e.hitFlashTimer  = 0.15f;

            const float d  = std::sqrt(d2);
            const float nx = (d > 1e-6f) ? dx / d : 0.f;
            const float ny = (d > 1e-6f) ? dy / d : 1.f;
            e.knockback.x += nx * Config::AURA_KNOCKBACK;
            e.knockback.y += ny * Config::AURA_KNOCKBACK;

            if (e.health <= 0.f) e.alive = 0;
        }
    }
}

// Refill dead slots so the live population converges back to the container
// size. Without this the aura would grind N down and every ms/tick reading in
// the demo would fall for a reason that has nothing to do with the algorithm
// or the memory layout under test.
//
// WHY THE SLOT GOES BACK TO ITS OWN HOME POINT
//
// This used to place every respawn on a ring around the PLAYER. Under a single
// fixed scenario that was harmless -- it just kept a supply of enemies walking
// in. With a distribution toggle it would be fatal: the aura kills roughly a
// hundred entities a second, so within a minute a large fraction of the world
// would have been re-placed by the respawner, and every one of those placements
// would be drawn from a player-centred ring rather than from the selected
// distribution. The respawner would quietly become the dominant spatial process
// and BOTH scenarios would converge on the same player-centred blob -- the
// benchmark would then be comparing Uniform-in-name against Clustered-in-name
// while measuring one identical field.
//
// Reviving a slot at its own home point makes the respawner distribution-
// NEUTRAL by construction. Whatever the aura removes from a neighbourhood is
// returned to that same neighbourhood, so the spatial statistics are a fixed
// point of the death/respawn cycle rather than something that drifts over the
// length of a run.
void EngineCore::respawnDead() {
    const int n = getEntityCount();
    int budget  = Config::RESPAWN_PER_TICK;

    for (int i = 0; i < n && budget > 0; ++i) {
        const bool dead = (memoryMode_ == MemoryMode::SoA)
                              ? !soa_.alive[i]
                              : !aos_.enemies[i].alive;
        if (!dead) continue;

        // Four draws per respawn, in a fixed order -- unchanged in count from
        // the ring version, so this rewrite does not shift the RNG stream.
        // Both layouts kill the same enemies, so they find the same dead slots
        // in the same order and pull the same values out of the stream, which
        // is what keeps the two worlds identical across a death/respawn cycle.
        const float angle = rng_.range(0.f, TAU_F);
        const float jit   = rng_.range(0.f, Config::RESPAWN_JITTER);
        const float st    = rng_.range(0.2f, 1.5f);
        const float at    = rng_.range(0.f, 1.f);

        // Jitter keeps repeated respawns of the same slot off one exact
        // coordinate. Stacked bodies at a single point would drive the QuadTree
        // to MAX_DEPTH in one leaf and produce a query cost no real spatial
        // arrangement would -- an artefact of the respawner, attributed to the
        // scenario.
        const SpawnSample& home = spawnSet_[(size_t)i];
        float x = clampWorldX(home.x + std::cos(angle) * jit);
        float y = clampWorldY(home.y + std::sin(angle) * jit);

        // The only correction applied: never revive inside the kill zone.
        pushOutOfAura(playerPos_.x, playerPos_.y, x, y);

        // Home stays the ORIGINAL spawn point, untouched by the jitter or the
        // exclusion push, so the distribution is a fixed point of this cycle.
        if (memoryMode_ == MemoryMode::SoA)
            soa_.respawn(i, x, y, home.x, home.y, st, at);
        else
            aos_.respawn(i, x, y, home.x, home.y, st, at);
        --budget;
    }
}

void EngineCore::updateRepulsionBruteForce(float dt) {
    const float sep   = Config::REPULSION_RADIUS;
    const float sepSq = sep * sep;

    if (memoryMode_ == MemoryMode::SoA) {
        int n = soa_.size;
        for (int i = 0; i < n; ++i) {
            if (!soa_.alive[i]) continue;
            float fx = 0.f, fy = 0.f;
            for (int j = 0; j < n; ++j) {
                if (i == j || !soa_.alive[j]) continue;
                float dx = soa_.posX[i] - soa_.posX[j];
                float dy = soa_.posY[i] - soa_.posY[j];
                float d2 = dx*dx + dy*dy;
                if (d2 < sepSq && d2 > 1e-6f) {
                    float d = std::sqrt(d2);
                    float f = (sep - d) / sep * Config::REPULSION_FORCE;
                    fx += dx/d * f; fy += dy/d * f;
                }
            }
            soa_.posX[i] = std::clamp(soa_.posX[i] + fx * dt, 0.f, Config::WORLD_WIDTH);
            soa_.posY[i] = std::clamp(soa_.posY[i] + fy * dt, 0.f, Config::WORLD_HEIGHT);
        }
    } else {
        auto& e = aos_.enemies;
        int   n = (int)e.size();
        for (int i = 0; i < n; ++i) {
            if (!e[i].alive) continue;
            Vector2D force{};
            for (int j = 0; j < n; ++j) {
                if (i == j || !e[j].alive) continue;
                Vector2D d  = e[i].position - e[j].position;
                float    d2 = d.lengthSq();
                if (d2 < sepSq && d2 > 1e-6f) {
                    float dist = std::sqrt(d2);
                    float f    = (sep - dist) / sep * Config::REPULSION_FORCE;
                    force += d / dist * f;
                }
            }
            e[i].position.x = std::clamp(e[i].position.x + force.x * dt, 0.f, Config::WORLD_WIDTH);
            e[i].position.y = std::clamp(e[i].position.y + force.y * dt, 0.f, Config::WORLD_HEIGHT);
        }
    }
}

void EngineCore::updateRepulsionQuadTree(float dt) {
    const float sep   = Config::REPULSION_RADIUS;
    const float sepSq = sep * sep;

    quadTree_.clear();
    std::vector<int> neighbors;

    if (memoryMode_ == MemoryMode::SoA) {
        for (int i = 0; i < soa_.size; ++i)
            if (soa_.alive[i]) quadTree_.insert({soa_.posX[i], soa_.posY[i], i});

        for (int i = 0; i < soa_.size; ++i) {
            if (!soa_.alive[i]) continue;
            neighbors.clear();
            quadTree_.query({soa_.posX[i], soa_.posY[i], sep, sep}, neighbors);
            float fx = 0.f, fy = 0.f;
            for (int j : neighbors) {
                if (i == j) continue;
                float dx = soa_.posX[i] - soa_.posX[j];
                float dy = soa_.posY[i] - soa_.posY[j];
                float d2 = dx*dx + dy*dy;
                if (d2 < sepSq && d2 > 1e-6f) {
                    float d = std::sqrt(d2);
                    float f = (sep - d) / sep * Config::REPULSION_FORCE;
                    fx += dx/d * f; fy += dy/d * f;
                }
            }
            soa_.posX[i] = std::clamp(soa_.posX[i] + fx * dt, 0.f, Config::WORLD_WIDTH);
            soa_.posY[i] = std::clamp(soa_.posY[i] + fy * dt, 0.f, Config::WORLD_HEIGHT);
        }
    } else {
        auto& e = aos_.enemies;
        for (int i = 0; i < (int)e.size(); ++i)
            if (e[i].alive) quadTree_.insert({e[i].position.x, e[i].position.y, i});

        for (int i = 0; i < (int)e.size(); ++i) {
            if (!e[i].alive) continue;
            neighbors.clear();
            quadTree_.query({e[i].position.x, e[i].position.y, sep, sep}, neighbors);
            Vector2D force{};
            for (int j : neighbors) {
                if (i == j) continue;
                Vector2D d  = e[i].position - e[j].position;
                float    d2 = d.lengthSq();
                if (d2 < sepSq && d2 > 1e-6f) {
                    float dist = std::sqrt(d2);
                    float f    = (sep - dist) / sep * Config::REPULSION_FORCE;
                    force += d / dist * f;
                }
            }
            e[i].position.x = std::clamp(e[i].position.x + force.x * dt, 0.f, Config::WORLD_WIDTH);
            e[i].position.y = std::clamp(e[i].position.y + force.y * dt, 0.f, Config::WORLD_HEIGHT);
        }
    }
}

void EngineCore::fireProjectile() {
    for (auto& p : projectiles_) {
        if (!p.alive) {
            float angle = (float)rng_.below(628) * 0.01f;
            p.position  = playerPos_;
            p.velocity  = {std::cos(angle) * Config::PROJECTILE_SPEED,
                           std::sin(angle) * Config::PROJECTILE_SPEED};
            p.alive = true;
            return;
        }
    }
}

void EngineCore::updateProjectiles(float dt) {
    for (auto& p : projectiles_) {
        if (!p.alive) continue;
        p.position += p.velocity * dt;
        if (p.position.x < 0 || p.position.x > Config::WORLD_WIDTH ||
            p.position.y < 0 || p.position.y > Config::WORLD_HEIGHT) {
            p.alive = false; continue;
        }
        if (memoryMode_ == MemoryMode::SoA) {
            for (int i = 0; i < soa_.size; ++i) {
                if (!soa_.alive[i]) continue;
                float dx = p.position.x - soa_.posX[i];
                float dy = p.position.y - soa_.posY[i];
                float r  = Config::ENEMY_RADIUS + Config::PROJECTILE_RADIUS;
                if (dx*dx + dy*dy < r*r) {
                    soa_.health[i] -= 34.f;
                    // A hit is not just damage: it flashes the sprite, knocks
                    // the enemy back, stuns it and lights it on fire. These are
                    // the writes that keep the "cold" fields honest -- they are
                    // simulated state, not padding.
                    soa_.hitFlashTimer[i] = 0.12f;
                    soa_.aiState[i]       = (uint8_t)AIState::Stunned;
                    soa_.stateTimer[i]    = 0.15f;
                    soa_.knockX[i]       += p.velocity.x * 0.25f;
                    soa_.knockY[i]       += p.velocity.y * 0.25f;
                    soa_.burnTimer[i]     = 1.0f;
                    soa_.burnDps[i]       = 4.0f;
                    soa_.statusFlags[i]  |= StatusBit::Burning;
                    if (soa_.health[i] <= 0.f) soa_.alive[i] = 0;
                    p.alive = false; break;
                }
            }
        } else {
            for (auto& e : aos_.enemies) {
                if (!e.alive) continue;
                Vector2D d = p.position - e.position;
                float    r = e.radius + Config::PROJECTILE_RADIUS;
                if (d.lengthSq() < r*r) {
                    e.health        -= 34.f;
                    e.hitFlashTimer  = 0.12f;
                    e.aiState        = AIState::Stunned;
                    e.stateTimer     = 0.15f;
                    e.knockback     += p.velocity * 0.25f;
                    e.burnTimer      = 1.0f;
                    e.burnDps        = 4.0f;
                    e.statusFlags   |= StatusBit::Burning;
                    if (e.health <= 0.f) e.alive = 0;
                    p.alive = false; break;
                }
            }
        }
    }
}

const float* EngineCore::getPosX() const {
    return (memoryMode_ == MemoryMode::SoA) ? getSoAPosX() : getAoSPosX();
}
const float* EngineCore::getPosY() const {
    return (memoryMode_ == MemoryMode::SoA) ? getSoAPosY() : getAoSPosY();
}

const float* EngineCore::getSoAPosX() const { return soa_.posXPtr(); }
const float* EngineCore::getSoAPosY() const { return soa_.posYPtr(); }

const float* EngineCore::getAoSPosX() const {
    aosPosXCache_.resize(aos_.enemies.size());
    for (size_t i = 0; i < aos_.enemies.size(); ++i)
        aosPosXCache_[i] = aos_.enemies[i].position.x;
    return aosPosXCache_.data();
}

const float* EngineCore::getAoSPosY() const {
    aosPosYCache_.resize(aos_.enemies.size());
    for (size_t i = 0; i < aos_.enemies.size(); ++i)
        aosPosYCache_[i] = aos_.enemies[i].position.y;
    return aosPosYCache_.data();
}

const uint8_t* EngineCore::getSoAAlive() const { return soa_.alive.data(); }

// Gathered into a flat array for the same reason as the position caches: the
// AoS flag lives at a 128 B stride inside the struct and cannot be handed out
// as a contiguous buffer. This is test-only code, so the copy is irrelevant.
const uint8_t* EngineCore::getAoSAlive() const {
    aosAliveCache_.resize(aos_.enemies.size());
    for (size_t i = 0; i < aos_.enemies.size(); ++i)
        aosAliveCache_[i] = aos_.enemies[i].alive;
    return aosAliveCache_.data();
}

int EngineCore::getEntityCount() const {
    return (memoryMode_ == MemoryMode::SoA) ? soa_.size : (int)aos_.enemies.size();
}

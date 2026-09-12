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

}  // namespace

// ---- EnemyContainerAoS ----

void EnemyContainerAoS::init(int count, uint32_t seed) {
    Rng rng(seed);
    enemies.assign(count, EnemyAoS{});
    for (int i = 0; i < count; ++i) {
        EnemyAoS& e = enemies[i];
        e.position       = {rng.range(0.f, Config::WORLD_WIDTH), rng.range(0.f, Config::WORLD_HEIGHT)};
        e.velocity       = {};
        e.knockback      = {};
        e.health         = 100.f;
        e.maxHealth      = 100.f;
        e.radius         = Config::ENEMY_RADIUS;
        e.contactDamage  = 5.f;
        e.attackCooldown = 0.f;
        e.hitFlashTimer  = 0.f;
        e.stateTimer     = rng.range(0.2f, 1.5f);
        e.targetX        = e.position.x;
        e.targetY        = e.position.y;
        e.aggroRange     = 600.f;
        e.burnTimer      = 0.f;  e.burnDps   = 0.f;
        e.slowTimer      = 0.f;  e.slowFactor = 1.f;
        e.poisonTimer    = 0.f;  e.poisonDps = 0.f;
        e.animTimer      = rng.range(0.f, 1.f);
        e.animSpeed      = 8.f;
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
}

void EnemyContainerAoS::update(float dt, Vector2D playerPos) {
    const int n = (int)enemies.size();
    for (int i = 0; i < n; ++i) {
        EnemyAoS& e = enemies[i];
        if (!e.alive) continue;

        e.health -= tickStatus(dt, e.burnTimer, e.burnDps, e.poisonTimer, e.poisonDps,
                               e.slowTimer, e.slowFactor, e.statusFlags);
        if (e.health <= 0.f) { e.alive = 0; continue; }

        e.aiState = stepAI(e.aiState, e.stateTimer, dt, e.health / e.maxHealth, i);

        e.targetX = playerPos.x;
        e.targetY = playerPos.y;

        float dx = e.targetX - e.position.x;
        float dy = e.targetY - e.position.y;
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

void EnemyContainerSoA::init(int count, uint32_t seed) {
    Rng rng(seed);
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
    aggroRange.assign(count, 600.f);
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
        posX[i]       = rng.range(0.f, Config::WORLD_WIDTH);
        posY[i]       = rng.range(0.f, Config::WORLD_HEIGHT);
        targetX[i]    = posX[i];
        targetY[i]    = posY[i];
        stateTimer[i] = rng.range(0.2f, 1.5f);
        animTimer[i]  = rng.range(0.f, 1.f);
        animFrame[i]  = i & 7;
        spriteId[i]   = i % 4;
        tier[i]       = i % 3;
    }
}

void EnemyContainerSoA::update(float dt, float playerX, float playerY) {
    for (int i = 0; i < size; ++i) {
        if (!alive[i]) continue;

        health[i] -= tickStatus(dt, burnTimer[i], burnDps[i], poisonTimer[i], poisonDps[i],
                                slowTimer[i], slowFactor[i], statusFlags[i]);
        if (health[i] <= 0.f) { alive[i] = 0; continue; }

        const AIState st = stepAI((AIState)aiState[i], stateTimer[i], dt,
                                  health[i] / maxHealth[i], i);
        aiState[i] = (uint8_t)st;

        targetX[i] = playerX;
        targetY[i] = playerY;

        float dx = targetX[i] - posX[i];
        float dy = targetY[i] - posY[i];
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

void EngineCore::init(int enemyCount, unsigned seed) {
    // Both containers are seeded identically, so the two layouts spawn the
    // SAME world. They each own their RNG, so the order in which they are
    // populated cannot perturb the other.
    rng_ = Rng(seed);
    soa_.init(enemyCount, seed);
    aos_.init(enemyCount, seed);
    projectiles_.assign(Config::MAX_PROJECTILES, {});
    for (auto& p : projectiles_) p.alive = false;
    stats_ = {};
    resetPerfWindow();
}

void EngineCore::resetPerfWindow() {
    perfCount_  = 0;
    perfCursor_ = 0;
}

void EngineCore::setEnemyCount(int n) {
    soa_.init(n);
    aos_.init(n);
    resetPerfWindow();
}

void EngineCore::setPlayerInput(float dx, float dy) {
    float len = std::sqrt(dx*dx + dy*dy);
    if (len > 1e-6f) { dx /= len; dy /= len; }
    playerInput_ = {dx, dy};
}

void EngineCore::tick(float dt) {
    auto t0 = std::chrono::high_resolution_clock::now();

    playerPos_.x = std::clamp(playerPos_.x + playerInput_.x * Config::PLAYER_SPEED * dt, 0.f, Config::WORLD_WIDTH);
    playerPos_.y = std::clamp(playerPos_.y + playerInput_.y * Config::PLAYER_SPEED * dt, 0.f, Config::WORLD_HEIGHT);

    if (memoryMode_ == MemoryMode::SoA)
        soa_.update(dt, playerPos_.x, playerPos_.y);
    else
        aos_.update(dt, playerPos_);

    if (collisionMode_ == CollisionMode::BruteForce)
        updateRepulsionBruteForce(dt);
    else
        updateRepulsionQuadTree(dt);

    fireTimer_ -= dt;
    if (fireTimer_ <= 0.f) { fireProjectile(); fireTimer_ = Config::FIRE_RATE; }
    updateProjectiles(dt);

    auto  t1      = std::chrono::high_resolution_clock::now();
    float frameMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    perfSamples_[perfCursor_] = frameMs;
    perfCursor_ = (perfCursor_ + 1) % PERF_WINDOW;
    if (perfCount_ < PERF_WINDOW) ++perfCount_;

    float sum = 0.f;
    for (int i = 0; i < perfCount_; ++i) sum += perfSamples_[i];
    const float meanMs = (perfCount_ > 0) ? sum / (float)perfCount_ : 0.f;

    stats_.frameTimeMs = meanMs;
    // Headroom, not observed frame rate: how often this tick COULD run if the
    // simulation were the only cost. The browser's real fps is measured in JS
    // and displayed separately -- conflating the two would let a slow canvas
    // draw masquerade as a slow engine.
    stats_.fps = (meanMs > 0.f) ? 1000.f / meanMs : 0.f;
    stats_.collisionMode     = collisionMode_;
    stats_.memoryMode        = memoryMode_;
    stats_.aliveEnemies      = (memoryMode_ == MemoryMode::SoA) ? soa_.aliveCount() : aos_.aliveCount();
    int ap = 0; for (const auto& p : projectiles_) ap += (int)p.alive;
    stats_.activeProjectiles = ap;
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

int EngineCore::getEntityCount() const {
    return (memoryMode_ == MemoryMode::SoA) ? soa_.size : (int)aos_.enemies.size();
}

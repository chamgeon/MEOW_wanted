#include "../include/EngineCore.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
using namespace emscripten;

// ---------------------------------------------------------------------------
// Zero-copy bridge to JS.
//
// Nothing is serialised across the boundary. C++ hands JS the integer address
// of a contiguous float array inside the Wasm heap, and JS reads it directly
// out of HEAPF32 at (ptr >> 2). At 10,000 entities that is the difference
// between two pointer reads per frame and 20,000 marshalled doubles.
//
// The addresses are only valid until the next allocation that grows the heap,
// so JS must re-read them every frame rather than caching them. setEnemyCount()
// in particular reallocates every array.
// ---------------------------------------------------------------------------

// These follow the ACTIVE memory mode. Binding getSoAPosX unconditionally --
// as an earlier version did -- meant that switching the demo to AoS rendered
// the SoA arrays, which are not updated in that mode: the enemies froze at
// their spawn points while the HUD reported a healthy simulation. The toggle
// looked like it did nothing but change a number.
static int getPosXPtr(const EngineCore& self) {
    return (int)(uintptr_t)(self.getEntityCount() > 0 ? self.getPosX() : nullptr);
}
static int getPosYPtr(const EngineCore& self) {
    return (int)(uintptr_t)(self.getEntityCount() > 0 ? self.getPosY() : nullptr);
}

// EngineCore::init is init(int count, unsigned seed = 1337). embind does not
// honour C++ default arguments -- it binds the declared arity -- so exposing
// the method directly would make the existing JS call engine.init(5000) throw
// on argument count. Wrap it to keep the one-argument JS contract, and expose
// the seeded form separately for anyone reproducing a specific run.
static void initDefault(EngineCore& self, int count) { self.init(count); }
static void initSeeded(EngineCore& self, int count, unsigned seed) { self.init(count, seed); }

EMSCRIPTEN_BINDINGS(VampireCore) {
    enum_<CollisionMode>("CollisionMode")
        .value("BruteForce", CollisionMode::BruteForce)
        .value("QuadTree",   CollisionMode::QuadTree);

    enum_<MemoryMode>("MemoryMode")
        .value("AoS", MemoryMode::AoS)
        .value("SoA", MemoryMode::SoA);

    // The scenario axis. Unlike the two mode toggles, flipping this rebuilds the
    // world (home points are per-entity state), so the JS side must treat it as
    // a reset: clear the metric history, or the graph will show the new
    // scenario's cost averaged against the old one's.
    enum_<SpawnDistribution>("SpawnDistribution")
        .value("Uniform",   SpawnDistribution::Uniform)
        .value("Clustered", SpawnDistribution::Clustered);

    value_object<PerfStats>("PerfStats")
        .field("fps",               &PerfStats::fps)
        .field("frameTimeMs",       &PerfStats::frameTimeMs)
        // Raw, unsmoothed. The JS logger needs the per-frame distribution to
        // compute p95/p99; frameTimeMs is a 30-tick mean and hides exactly the
        // spikes an optimizer agent most needs to see.
        .field("lastTickMs",        &PerfStats::lastTickMs)
        .field("aliveEnemies",      &PerfStats::aliveEnemies)
        .field("activeProjectiles", &PerfStats::activeProjectiles)
        .field("kills",             &PerfStats::kills)
        .field("auraPulses",        &PerfStats::auraPulses);

    class_<EngineCore>("EngineCore")
        .constructor<>()
        .function("init",             &initDefault)
        .function("initSeeded",       &initSeeded)
        .function("tick",             &EngineCore::tick)
        .function("setCollisionMode", &EngineCore::setCollisionMode)
        .function("setMemoryMode",    &EngineCore::setMemoryMode)
        .function("setEnemyCount",    &EngineCore::setEnemyCount)
        .function("setSpawnDistribution", &EngineCore::setSpawnDistribution)
        .function("getSpawnDistribution", &EngineCore::getSpawnDistribution)
        .function("setPlayerInput",   &EngineCore::setPlayerInput)
        .function("getEntityCount",   &EngineCore::getEntityCount)
        .function("getStats",         &EngineCore::getStats)
        .function("getPosXPtr",       &getPosXPtr)
        .function("getPosYPtr",       &getPosYPtr)
        // Player + aura, read once per frame by the renderer. These are plain
        // scalar calls rather than heap pointers on purpose: there are six of
        // them per frame against 10,000 entity positions, so the embind
        // marshalling cost is irrelevant and the clarity is worth more.
        .function("getPlayerX",       &EngineCore::getPlayerX)
        .function("getPlayerY",       &EngineCore::getPlayerY)
        .function("getAuraRadius",    &EngineCore::getAuraRadius)
        .function("getAuraPhase",     &EngineCore::getAuraPhase)
        .function("getAuraFlash",     &EngineCore::getAuraFlash)
        .function("getKills",         &EngineCore::getKills)
        .function("getAliveCount",    &EngineCore::getAliveCount)
        .function("getLastTickMs",    &EngineCore::getLastTickMs);
}
#endif  // __EMSCRIPTEN__

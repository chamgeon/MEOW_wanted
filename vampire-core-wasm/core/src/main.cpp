// Native benchmark harness.
//
// This target exists so the engine can be built, profiled and validated with a
// normal toolchain (gcc/clang/MSVC + perf/vtune/cachegrind) without Emscripten.
// The Wasm build uses bindings.cpp instead and has no main().
//
//   cmake -B build-native -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-native --config Release --parallel
//   ./build-native/core_bench          (MSVC: .\build-native\Release\core_bench.exe)
//
// All output is pure ASCII on purpose: this runs in the Windows console, which
// is cp949 on Korean systems and turns UTF-8 punctuation into mojibake.

#include "../include/EngineCore.h"
#include "../include/EnemySoA.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

// Cache topology query. The whole AoS-vs-SoA argument is a claim about where
// the working set stops fitting, so the harness asks the OS where that line
// actually is on this machine rather than assuming a number.
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#elif defined(__APPLE__)
    #include <sys/sysctl.h>
#elif defined(__linux__)
    #include <unistd.h>
#endif

// A Debug build runs an order of magnitude slower and its timings say nothing
// about the algorithms being compared, so the harness refuses to stay quiet
// about it. MSVC users hit this by omitting `--config Release`.
#if defined(NDEBUG)
    #define VC_OPTIMIZED 1
#else
    #define VC_OPTIMIZED 0
#endif

#if defined(_MSC_VER)
    #define VC_COMPILER "MSVC"
#elif defined(__clang__)
    #define VC_COMPILER "clang"
#elif defined(__GNUC__)
    #define VC_COMPILER "gcc"
#else
    #define VC_COMPILER "unknown"
#endif

namespace {

using Clock = std::chrono::high_resolution_clock;

void printBuildBanner() {
    std::printf("Vampire-Core native harness  [%s, %s]\n",
                VC_COMPILER, VC_OPTIMIZED ? "optimized" : "DEBUG / UNOPTIMIZED");

    if (!VC_OPTIMIZED) {
        std::printf(
            "\n"
            "  ##################################################################\n"
            "  #  WARNING: unoptimized build. The timings below are worthless.  #\n"
            "  #                                                                #\n"
            "  #  Visual Studio (multi-config):                                 #\n"
            "  #    cmake --build build-native --config Release --parallel      #\n"
            "  #    .\\build-native\\Release\\core_bench.exe                       #\n"
            "  #                                                                #\n"
            "  #  Make / Ninja (single-config):                                 #\n"
            "  #    cmake -B build-native -DCMAKE_BUILD_TYPE=Release            #\n"
            "  ##################################################################\n"
            "\n");
    }
}

double timeTicks(EngineCore& engine, int iterations) {
    engine.tick(0.016f);  // warm caches / first-touch allocations
    const auto t0 = Clock::now();
    for (int i = 0; i < iterations; ++i) engine.tick(0.016f);
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / iterations;
}

const char* name(CollisionMode m) {
    switch (m) {
        case CollisionMode::BruteForce: return "BruteForce";
        case CollisionMode::QuadTree: return "QuadTree";
        case CollisionMode::UniformGrid: return "UniformGrid";
        case CollisionMode::SpatialHash: return "SpatialHash";
    }
    return "Unknown";
}
const char* name(MemoryMode m)    { return m == MemoryMode::AoS          ? "AoS"        : "SoA"; }
const char* name(SpawnDistribution d) {
    return d == SpawnDistribution::Uniform ? "Uniform" : "Clustered";
}

// ---------------------------------------------------------------------------
// Spatial concentration: the number that decides whether the distribution
// toggle is real.
//
// The statistic is "how many entities share a cell with the average entity",
// where a cell is the QuadTree's smallest possible leaf (WORLD / 2^MAX_DEPTH).
// That is deliberately not the same thing as "entities per cell": a field can
// have a low mean occupancy and still be pathological for a tree if all the
// occupancy sits in a few cells. Weighting by occupancy answers the question the
// QuadTree actually cares about -- what does a typical QUERY have to wade
// through -- rather than the question a uniformity test would ask.
//
//     conc = sum(c_i^2) / sum(c_i)
//
// For a Poisson (uniform) field with mean lambda per cell this tends to
// 1 + lambda; concentrating the same N into a fraction f of the area multiplies
// it by roughly 1/f. So the ratio between the two distributions is a direct,
// dimensionless measure of how much harder the clustered field is to partition,
// and it is computed from the ACTUAL simulated positions rather than from the
// spawn parameters -- which matters, because repulsion pushes entities apart and
// could in principle have flattened the clusters back out before the benchmark
// ever timed them.
// ---------------------------------------------------------------------------
double spatialConcentration(const EngineCore& engine) {
    constexpr int kGrid = 1 << Config::QT_MAX_DEPTH;   // 64 -> 31.25 u cells
    static std::vector<int> cells;
    cells.assign(kGrid * kGrid, 0);

    const float* px = engine.getPosX();
    const float* py = engine.getPosY();
    const int    n  = engine.getEntityCount();
    if (n <= 0 || !px || !py) return 0.0;

    const float sx = (float)kGrid / Config::WORLD_WIDTH;
    const float sy = (float)kGrid / Config::WORLD_HEIGHT;

    for (int i = 0; i < n; ++i) {
        int cx = (int)(px[i] * sx);
        int cy = (int)(py[i] * sy);
        cx = (std::min)((std::max)(cx, 0), kGrid - 1);
        cy = (std::min)((std::max)(cy, 0), kGrid - 1);
        ++cells[(size_t)cy * kGrid + cx];
    }

    double num = 0.0, den = 0.0;
    for (int c : cells) { num += (double)c * (double)c; den += (double)c; }
    return den > 0.0 ? num / den : 0.0;
}

// Build an engine in a known scenario and let it reach its steady state.
//
// The settle ticks are not politeness. Spawn placement is the initial condition,
// not the thing under test: repulsion, aggro and the respawner all act on the
// field, and a distribution that dissolved within a second would be a decoration
// rather than a scenario. Measuring after settling is what makes the gate below
// an honest test of the mechanism instead of a restatement of the spawn code.
void settleEngine(EngineCore& engine, int n, SpawnDistribution d,
                  MemoryMode mm, CollisionMode cm, int settleTicks) {
    engine.init(n);
    engine.setSpawnDistribution(d);
    engine.setMemoryMode(mm);
    engine.setCollisionMode(cm);
    for (int i = 0; i < settleTicks; ++i) engine.tick(0.05f);
}

// Reported in bytes; 0 means "could not determine on this platform".
struct CacheSizes { size_t l1d = 0, l2 = 0, l3 = 0; };

CacheSizes queryCacheSizes() {
    CacheSizes c;
#if defined(_WIN32)
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if (len) {
        std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buf(
            len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
        if (GetLogicalProcessorInformation(buf.data(), &len)) {
            for (const auto& info : buf) {
                if (info.Relationship != RelationCache) continue;
                const CACHE_DESCRIPTOR& d = info.Cache;
                // Take the largest instance seen at each level: on hybrid CPUs
                // the P-cores have the bigger caches and they are what the
                // benchmark thread will most likely land on.
                if (d.Level == 1 && d.Type != CacheInstruction) c.l1d = (std::max)(c.l1d, (size_t)d.Size);
                else if (d.Level == 2) c.l2 = (std::max)(c.l2, (size_t)d.Size);
                else if (d.Level == 3) c.l3 = (std::max)(c.l3, (size_t)d.Size);
            }
        }
    }
#elif defined(__APPLE__)
    size_t v = 0, sz = sizeof(v);
    if (sysctlbyname("hw.l1dcachesize", &v, &sz, nullptr, 0) == 0) c.l1d = v;
    sz = sizeof(v); if (sysctlbyname("hw.l2cachesize", &v, &sz, nullptr, 0) == 0) c.l2 = v;
    sz = sizeof(v); if (sysctlbyname("hw.l3cachesize", &v, &sz, nullptr, 0) == 0) c.l3 = v;
#elif defined(__linux__)
    long v;
    if ((v = sysconf(_SC_LEVEL1_DCACHE_SIZE)) > 0) c.l1d = (size_t)v;
    if ((v = sysconf(_SC_LEVEL2_CACHE_SIZE))  > 0) c.l2  = (size_t)v;
    if ((v = sysconf(_SC_LEVEL3_CACHE_SIZE))  > 0) c.l3  = (size_t)v;
#endif
    return c;
}

void printKB(const char* label, size_t bytes) {
    if (bytes) std::printf("  %-4s %6.0f KB\n", label, bytes / 1024.0);
    else       std::printf("  %-4s      ?\n", label);
}

// ---------------------------------------------------------------------------
// Cache-cliff microbenchmark.
//
// The engine benchmark below uses one fixed entity size, which means its result
// depends on whether 10000 * 128 B happens to land above or below THIS CPU's
// L2. On a machine with a large L2 it under-reports the effect, and on a small
// one it over-reports. Either way a single data point is weak evidence.
//
// So: hold the entity count, the pair count and the arithmetic constant, and
// vary nothing but the per-entity stride. Every row below executes the exact
// same number of the exact same floating-point operations. The only thing that
// changes is how far apart in memory consecutive positions sit. Whatever curve
// appears is therefore a memory-system effect and nothing else, and the knee in
// it is this machine's cache cliff, measured rather than assumed.
// ---------------------------------------------------------------------------
namespace cliff {

constexpr int   N     = 8000;
constexpr int   ITERS = 3;   // timed passes per repetition
constexpr int   REPS  = 3;   // repetitions; the minimum is reported
constexpr float SEP   = 20.f;
constexpr float SEPSQ = SEP * SEP;
constexpr float FORCE = 50.f;

template <int STRIDE>
struct Padded {
    float x, y;
    char  pad[STRIDE - 8];
};

// Why min-of-REPS and not the mean: this measures a property of the memory
// system, and every source of noise on a desktop -- scheduler preemption, a
// background process evicting our lines, frequency scaling -- can only make a
// run slower, never faster. The minimum is therefore the closest estimate of
// the machine's actual capability, and it is what keeps the curve monotonic
// enough to read. A mean would let one descheduled run invert two adjacent
// rows and make the table say something untrue.
template <typename Fn>
double bestOf(Fn&& kernel) {
    kernel();  // warm up: first-touch page faults, cache fill, branch history
    double best = 1e30;
    for (int r = 0; r < REPS; ++r) {
        const auto t0 = Clock::now();
        for (int k = 0; k < ITERS; ++k) kernel();
        const auto t1 = Clock::now();
        best = (std::min)(best,
            std::chrono::duration<double, std::milli>(t1 - t0).count() / ITERS);
    }
    return best;
}

template <int STRIDE>
double timeAoS(const std::vector<float>& sx, const std::vector<float>& sy) {
    std::vector<Padded<STRIDE>> e(N);
    for (int i = 0; i < N; ++i) { e[i].x = sx[i]; e[i].y = sy[i]; }
    volatile float sink = 0.f;

    return bestOf([&] {
        for (int i = 0; i < N; ++i) {
            float fx = 0.f, fy = 0.f;
            for (int j = 0; j < N; ++j) {
                if (i == j) continue;
                const float dx = e[i].x - e[j].x;
                const float dy = e[i].y - e[j].y;
                const float d2 = dx*dx + dy*dy;
                if (d2 < SEPSQ && d2 > 1e-6f) {
                    const float d = std::sqrt(d2);
                    const float f = (SEP - d) / SEP * FORCE;
                    fx += dx/d*f; fy += dy/d*f;
                }
            }
            sink = sink + fx + fy;
        }
    });
}

double timeSoA(const std::vector<float>& px, const std::vector<float>& py) {
    volatile float sink = 0.f;
    return bestOf([&] {
        for (int i = 0; i < N; ++i) {
            float fx = 0.f, fy = 0.f;
            for (int j = 0; j < N; ++j) {
                if (i == j) continue;
                const float dx = px[i] - px[j];
                const float dy = py[i] - py[j];
                const float d2 = dx*dx + dy*dy;
                if (d2 < SEPSQ && d2 > 1e-6f) {
                    const float d = std::sqrt(d2);
                    const float f = (SEP - d) / SEP * FORCE;
                    fx += dx/d*f; fy += dy/d*f;
                }
            }
            sink = sink + fx + fy;
        }
    });
}

}  // namespace cliff

void runCacheCliff(const CacheSizes& cache) {
    using namespace cliff;

    std::printf("\n=== Cache cliff: identical work, only the stride changes ===\n\n");
    std::printf("N = %d. Every row below performs the same %d pair tests with the\n",
                N, N * N);
    std::printf("same arithmetic. The ONLY variable is how many bytes apart two\n");
    std::printf("consecutive entity positions sit in memory. Reported value is the\n");
    std::printf("fastest of %d repetitions after a warm-up pass.\n\n", REPS);

    Rng rng(2024u);
    std::vector<float> px(N), py(N);
    for (int i = 0; i < N; ++i) {
        px[i] = rng.range(0.f, Config::WORLD_WIDTH);
        py[i] = rng.range(0.f, Config::WORLD_HEIGHT);
    }

    const double soaMs = timeSoA(px, py);

    std::printf("%-14s  %10s  %10s  %9s  %s\n",
                "layout", "workset", "ms/tick", "vs SoA", "");
    std::printf("%-14s  %10s  %10s  %9s\n",
                "--------------", "----------", "----------", "---------");
    std::printf("%-14s  %7.0f KB  %10.3f  %8.2fx\n",
                "SoA (8 B)", 8.0 * N / 1024.0, soaMs, 1.0);

    struct Row { int stride; double ms; };
    Row rows[] = {
        {  16, timeAoS< 16>(px, py) },
        {  32, timeAoS< 32>(px, py) },
        {  64, timeAoS< 64>(px, py) },
        { 128, timeAoS<128>(px, py) },
        { 256, timeAoS<256>(px, py) },
        { 512, timeAoS<512>(px, py) },
    };

    int    kneeStride = 0;
    double kneeJump   = 1.0;
    double prevMs     = soaMs;

    for (const Row& r : rows) {
        const double kb   = (double)r.stride * N / 1024.0;
        const double jump = r.ms / prevMs;
        if (jump > kneeJump) { kneeJump = jump; kneeStride = r.stride; }
        prevMs = r.ms;

        char note[64] = "";
        if (cache.l2 && kb * 1024.0 > (double)cache.l2)
            std::snprintf(note, sizeof(note), "  past L2");

        char label[32];
        std::snprintf(label, sizeof(label), "AoS (%d B)", r.stride);
        std::printf("%-14s  %7.0f KB  %10.3f  %8.2fx%s\n",
                    label, kb, r.ms, r.ms / soaMs, note);
    }

    std::printf("\n");
    if (kneeStride) {
        std::printf("  Measured knee: going from %d B to %d B per entity cost %.2fx.\n",
                    kneeStride / 2, kneeStride, kneeJump);
        std::printf("  That is this machine's cache cliff, and it is where the AoS\n");
        std::printf("  baseline starts paying for bytes the kernel never reads.\n\n");
    }
    std::printf("  The point: an entity's cost is not its own size, it is how much\n");
    std::printf("  cache its neighbours evict. SoA keeps the hot fields dense, so it\n");
    std::printf("  stays flat no matter how much cold state each entity accumulates\n");
    std::printf("  over a project's lifetime -- which is the direction real game\n");
    std::printf("  entities always grow.\n");
}

void printLayoutFacts() {
    std::printf("Memory layout under test\n");
    std::printf("  AoS  sizeof(EnemyAoS)        = %3zu B/entity\n", sizeof(EnemyAoS));
    std::printf("  SoA  same fields, split      = %3d B/entity\n",
                EnemyContainerSoA::bytesPerEntity());
    std::printf("  repulsion reads position only=   %d B/entity\n\n",
                EnemyContainerSoA::hotBytesPerEntity());

    std::printf("  Both layouts store and simulate identical state. Only the\n");
    std::printf("  arrangement differs, so any gap below is a layout effect.\n\n");

    std::printf("  The O(N^2) repulsion kernel touches nothing but position:\n");
    std::printf("    AoS: a 64 B cache line delivers %.0f%% useful bytes (stride %zu B)\n",
                8.0 / (double)sizeof(EnemyAoS) * 100.0, sizeof(EnemyAoS));
    std::printf("    SoA: posX/posY stream contiguously -> 100%% useful\n\n");

    std::printf("%8s  %12s  %12s   %s\n", "entities", "AoS set", "SoA hot set", "expectation");
    std::printf("%8s  %12s  %12s   %s\n", "--------", "------------", "------------", "-----------");
    for (int n : {1000, 2000, 5000, 10000}) {
        const double aosKB = (double)sizeof(EnemyAoS) * n / 1024.0;
        const double soaKB = (double)EnemyContainerSoA::hotBytesPerEntity() * n / 1024.0;
        const char*  note  = (aosKB < 256.0)  ? "both fit L2, expect a tie"
                           : (aosKB < 1024.0) ? "AoS nearing L2, small gap"
                                              : "AoS past L2, gap should open";
        std::printf("%8d  %9.0f KB  %9.0f KB   %s\n", n, aosKB, soaKB, note);
    }
    const CacheSizes c = queryCacheSizes();
    std::printf("\nThis CPU's cache hierarchy (queried from the OS):\n");
    printKB("L1d", c.l1d);
    printKB("L2",  c.l2);
    printKB("L3",  c.l3);
    if (c.l2) {
        std::printf("  -> AoS stops fitting in L2 at about %.0f entities.\n",
                    (double)c.l2 / (double)sizeof(EnemyAoS));
        std::printf("  -> SoA position data stays L2-resident until about %.0f.\n",
                    (double)c.l2 / (double)EnemyContainerSoA::hotBytesPerEntity());
    }
}

void runBenchmark() {
    std::printf("\n=== Repulsion benchmark (ms per tick, lower is better) ===\n\n");
    std::printf("%8s  %-11s  %8s  %8s  %10s\n",
                "entities", "collision", "AoS ms", "SoA ms", "SoA speedup");
    std::printf("%8s  %-11s  %8s  %8s  %10s\n",
                "--------", "-----------", "--------", "--------", "-----------");

    for (int n : {1000, 2000, 5000, 10000}) {
        for (CollisionMode cm : {CollisionMode::BruteForce, CollisionMode::QuadTree}) {
            double ms[2] = {0.0, 0.0};
            int    k     = 0;
            for (MemoryMode mm : {MemoryMode::AoS, MemoryMode::SoA}) {
                EngineCore engine;
                engine.init(n);
                engine.setCollisionMode(cm);
                engine.setMemoryMode(mm);

                // Brute force is O(N^2); keep the wall time sane at high N.
                const int iters = (cm == CollisionMode::BruteForce && n >= 5000) ? 3 : 20;
                ms[k++] = timeTicks(engine, iters);
            }
            std::printf("%8d  %-11s  %8.3f  %8.3f  %9.2fx\n",
                        n, name(cm), ms[0], ms[1],
                        ms[1] > 0.0 ? ms[0] / ms[1] : 0.0);
        }
        std::printf("\n");
    }
}

// The QuadTree is the load-bearing data structure for the O(N log N) claim.
// If it silently drops or duplicates entries, the benchmark above compares two
// different workloads and means nothing. These checks guard that.
int runCorrectnessChecks() {
    int failures = 0;

    // 1. Dense cluster must not be dropped. Enemies all converge on the player,
    //    so a tight cluster is the demo's steady state, not an edge case.
    {
        QuadTree qt(AABB{1000.f, 1000.f, 1000.f, 1000.f});
        const int kInserted = 200;
        for (int i = 0; i < kInserted; ++i)
            qt.insert({1000.f + i * 0.001f, 1000.f + i * 0.001f, i});

        std::vector<int> out;
        qt.query(AABB{1000.f, 1000.f, 50.f, 50.f}, out);
        const std::set<int> unique(out.begin(), out.end());

        const bool ok = (int)unique.size() == kInserted;
        std::printf("[check] dense cluster: inserted %d, retrieved %d unique ... %s\n",
                    kInserted, (int)unique.size(), ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // 2. A point sitting exactly on a subdivision boundary must be reported once.
    //    Duplicates multiply the repulsion force applied to that entity.
    {
        QuadTree qt(AABB{1000.f, 1000.f, 1000.f, 1000.f});
        for (int i = 0; i < 9; ++i) qt.insert({500.f + i, 500.f + i, i});  // force subdivide
        qt.insert({1000.f, 1000.f, 999});                                  // dead centre

        std::vector<int> out;
        qt.query(AABB{1000.f, 1000.f, 1.f, 1.f}, out);
        int hits = 0;
        for (int v : out) hits += (v == 999);

        std::printf("[check] boundary point: reported %d time(s) ... %s\n",
                    hits, hits == 1 ? "PASS" : "FAIL");
        if (hits != 1) ++failures;
    }

    // 3. Every live entity must stay inside the world bounds after a tick.
    {
        EngineCore engine;
        engine.init(2000);
        engine.setCollisionMode(CollisionMode::QuadTree);
        engine.setMemoryMode(MemoryMode::SoA);
        for (int i = 0; i < 30; ++i) engine.tick(0.016f);

        const float* px = engine.getSoAPosX();
        const float* py = engine.getSoAPosY();
        int escaped = 0;
        for (int i = 0, n = engine.getEntityCount(); i < n; ++i) {
            if (px[i] < 0.f || px[i] > Config::WORLD_WIDTH ||
                py[i] < 0.f || py[i] > Config::WORLD_HEIGHT) ++escaped;
        }
        std::printf("[check] world bounds: %d entities escaped ... %s\n",
                    escaped, escaped == 0 ? "PASS" : "FAIL");
        if (escaped != 0) ++failures;
    }

    return failures;
}

// Guards the bug where the demo's Memory toggle appeared to do nothing.
//
// The renderer reads getPosX()/getPosY(). Those must follow the active memory
// mode: in AoS mode the SoA arrays are never written, so a renderer pointed at
// them draws enemies frozen at their spawn points while the HUD reports a
// running simulation. That is worse than a crash -- the demo keeps working,
// the numbers keep updating, and only the thing being demonstrated is wrong.
int runRendererPointerCheck() {
    int failures = 0;

    for (MemoryMode mm : {MemoryMode::AoS, MemoryMode::SoA}) {
        EngineCore engine;
        engine.init(500);
        engine.setMemoryMode(mm);
        engine.setCollisionMode(CollisionMode::QuadTree);

        const int n = engine.getEntityCount();
        std::vector<float> before(engine.getPosX(), engine.getPosX() + n);

        for (int i = 0; i < 20; ++i) engine.tick(0.016f);

        const float* after = engine.getPosX();
        int moved = 0;
        for (int i = 0; i < n; ++i) if (before[i] != after[i]) ++moved;

        // Enemies all home in on the player, so essentially every one of them
        // must have moved. A stuck renderer shows exactly zero.
        const bool ok = moved > n / 2;
        std::printf("[check] renderer follows %s mode: %d/%d entities moved ... %s\n",
                    name(mm), moved, n, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    return failures;
}

// The single most important check in this file.
//
// The benchmark's headline claim is "SoA is faster than AoS". That claim is
// only meaningful if both layouts are running the SAME simulation. If they
// diverge -- different spawn points, different entities dying, a field updated
// in one path but not the other -- then the ms/tick columns are timing two
// different workloads and the speedup number is fiction.
//
// So: run both layouts from the same seed, tick them identically, and require
// the resulting positions to agree. Both paths execute the same float
// operations in the same order, so the tolerance is for compiler reassociation
// only, not for behavioural drift.
int runEquivalenceCheck() {
    // Long enough to cover several aura pulses. AURA_INTERVAL is 2.0 s, so
    // 120 ticks at dt = 0.05 is 6 simulated seconds and three pulses -- which
    // means the comparison below runs across multiple full kill-and-respawn
    // cycles, not just across movement. dt = 0.05 is also the clamp the browser
    // loop applies to a dropped frame, so this is a dt the engine really sees.
    const int   kEnemies = 2000;
    const int   kTicks   = 120;
    const float kDt      = 0.05f;
    const float kTol     = 1e-3f;

    int failures = 0;

    // Swept over distribution as well as collision mode. The clustered scenario
    // is the one that can break this: it drives a very different kill pattern
    // (a dense blob sitting inside the aura instead of a thin stream walking
    // into it), so if the AoS and SoA paths were going to disagree about a
    // knife-edge death, that is where it would surface. A check that only ever
    // ran the easy scenario would certify a world the demo never shows.
  for (SpawnDistribution sd : {SpawnDistribution::Uniform, SpawnDistribution::Clustered}) {
    for (CollisionMode cm : {CollisionMode::BruteForce, CollisionMode::QuadTree}) {
        EngineCore a, b;
        a.init(kEnemies);  a.setSpawnDistribution(sd);
        a.setMemoryMode(MemoryMode::AoS);  a.setCollisionMode(cm);
        b.init(kEnemies);  b.setSpawnDistribution(sd);
        b.setMemoryMode(MemoryMode::SoA);  b.setCollisionMode(cm);

        for (int i = 0; i < kTicks; ++i) { a.tick(kDt); b.tick(kDt); }

        const float* ax = a.getAoSPosX();  const float* ay = a.getAoSPosY();
        const float* bx = b.getSoAPosX();  const float* by = b.getSoAPosY();

        float worst = 0.f;
        for (int i = 0; i < kEnemies; ++i) {
            worst = std::max(worst, std::fabs(ax[i] - bx[i]));
            worst = std::max(worst, std::fabs(ay[i] - by[i]));
        }

        // The kill set, not just the coordinates.
        //
        // Once the aura can kill, position agreement is no longer sufficient.
        // If the two layouts disagree about which entities are corpses, they
        // feed different live counts into the O(N^2) kernel from that tick on,
        // and the ms/tick columns stop being a layout comparison -- the faster
        // column is simply the one simulating fewer enemies.
        //
        // Exact equality is demanded on purpose. The two repulsion kernels are
        // separate code and a compiler may reassociate them differently, so a
        // position sitting exactly on the aura boundary could in principle fall
        // on opposite sides in the two builds. The mismatch count is printed
        // rather than a bare PASS/FAIL so that such a knife-edge single flip is
        // visibly different from systematic divergence -- but either way it is
        // a failure, because after one flip the two worlds are not the same
        // world and nothing below can be compared.
        const uint8_t* aa = a.getAoSAlive();
        const uint8_t* ba = b.getSoAAlive();
        int aliveMismatch = 0;
        for (int i = 0; i < kEnemies; ++i)
            if ((aa[i] != 0) != (ba[i] != 0)) ++aliveMismatch;

        const int killsA = a.getKills();
        const int killsB = b.getKills();

        const bool posOk   = (worst <= kTol);
        const bool killOk  = (killsA == killsB);
        const bool aliveOk = (aliveMismatch == 0);
        const bool ok      = posOk && killOk && aliveOk;

        std::printf("[check] AoS/SoA equivalence (%-10s %-9s): worst pos delta %.6f,"
                    " kills %d vs %d, alive mismatches %d ... %s\n",
                    name(cm), name(sd), worst, killsA, killsB, aliveMismatch,
                    ok ? "PASS" : "FAIL");

        // A run in which nothing died would pass every assertion above while
        // proving nothing about the aura, so the absence of kills is itself a
        // failure of the test rather than a clean result.
        if (killsA == 0) {
            std::printf("[check]   -> no kills in %d ticks; the aura did not fire or"
                        " reached nothing. This check proved nothing. ... FAIL\n", kTicks);
            ++failures;
        }

        if (!ok) ++failures;
    }
  }

    return failures;
}

// The aura kills; the respawner refills. If the refill cannot keep up, the live
// population drifts down over a run -- and since every ms/tick figure in this
// demo is a function of N, a drifting N makes the engine look like it is
// getting faster when all that happened is that there is less to simulate.
//
// This is the single easiest way for the benchmark to start lying without
// anything looking broken on screen, so it gets its own check.
int runPopulationStabilityCheck() {
    const int   kEnemies = 4000;
    const int   kTicks   = 240;   // 12 simulated seconds -> 6 aura pulses
    const float kDt      = 0.05f;

    // The metric is DRIFT, not the instantaneous minimum.
    //
    // A pulse legitimately empties the ring around the player in one tick, and
    // respawnDead() refills at RESPAWN_PER_TICK per tick, so the live count
    // genuinely dips right after each pulse. That transient is by design and
    // asserting against it would only measure the refill budget. What must not
    // happen is for those dips to fail to recover -- a population that ends the
    // run lower than it started is a slow leak, and it is exactly the failure
    // that would make every ms/tick number in the demo trend downward for a
    // reason unrelated to the algorithm under test.
    //
    // So: compare the mean live count over the first quarter of the run against
    // the last quarter. Both windows span more than one full 2 s aura cycle, so
    // each averages over the same mix of dip and recovery and the comparison is
    // like-for-like.
    const float kDriftTol = 0.02f;   // 2% of the target population

    int failures = 0;

    // Both distributions, because the respawn rule changed WITH the
    // distribution axis and the clustered case is the harder one: a blob sitting
    // on the player loses a large fraction of itself to every pulse, so it leans
    // on RESPAWN_PER_TICK far harder than a thin uniform stream does. If the
    // refill budget were too small, this is the scenario that would expose it --
    // and a population quietly sagging under Clustered would make the QuadTree
    // look better there for a reason that has nothing to do with partitioning.
  for (SpawnDistribution sd : {SpawnDistribution::Uniform, SpawnDistribution::Clustered}) {
    for (MemoryMode mm : {MemoryMode::AoS, MemoryMode::SoA}) {
        EngineCore engine;
        engine.init(kEnemies);
        engine.setSpawnDistribution(sd);
        engine.setMemoryMode(mm);
        engine.setCollisionMode(CollisionMode::QuadTree);

        const int quarter = kTicks / 4;
        double    headSum = 0.0, tailSum = 0.0;
        int       lowest  = kEnemies;

        for (int i = 0; i < kTicks; ++i) {
            engine.tick(kDt);
            const int alive = engine.getAliveCount();
            lowest = (std::min)(lowest, alive);
            if (i < quarter)             headSum += alive;
            if (i >= kTicks - quarter)   tailSum += alive;
        }

        const double head  = headSum / quarter;
        const double tail  = tailSum / quarter;
        const double drift = (head - tail) / (double)kEnemies;
        const int    kills = engine.getKills();

        // Both directions matter. A population that grew would mean the
        // respawner is handing out slots that were never vacated.
        const bool ok = (std::fabs(drift) <= kDriftTol) && (kills > 0);

        std::printf("[check] population stability (%s %-9s): %d kills, mean live"
                    " %.0f -> %.0f (drift %+.2f%%), low-water %d/%d ... %s\n",
                    name(mm), name(sd), kills, head, tail, drift * 100.0,
                    lowest, kEnemies, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }
  }

    return failures;
}

// ---------------------------------------------------------------------------
// The honesty gate for the scenario axis.
//
// The claim this demo makes about distribution is: "the same N, the same
// kernels, a different spatial arrangement, and the winner changes." If the two
// distributions do not in fact produce measurably different fields -- or if the
// clustered one dissolves after a few seconds of repulsion -- then the sweep
// below is comparing a scenario against itself, every timing difference in it is
// noise, and any conclusion an optimizer agent draws from it is invented.
//
// So the axis has to prove itself before any of its numbers are allowed to
// count. Two things are asserted:
//
//   1. Clustered is materially more concentrated than Uniform. "Materially" is
//      pinned to a number (2x) rather than left to the eye.
//   2. It is STILL more concentrated after the field has been simulated, not
//      merely at the instant it was spawned. This is the one that would actually
//      have failed before AGGRO_RANGE existed: with every enemy chasing the
//      player from anywhere in the world, both distributions collapsed into the
//      same ball within seconds and t=0 was the only moment they differed.
// ---------------------------------------------------------------------------
int runDistributionGate() {
    const int    kEnemies  = 5000;
    const int    kSettle   = 120;    // 6 simulated seconds, 3 aura pulses
    const double kMinRatio = 2.0;

    std::printf("\n=== Scenario gate: is the distribution toggle real? ===\n\n");
    std::printf("Concentration = entities sharing the average entity's QuadTree leaf\n");
    std::printf("(%d x %d cells of %.1f u). Higher means harder to partition.\n\n",
                1 << Config::QT_MAX_DEPTH, 1 << Config::QT_MAX_DEPTH,
                Config::WORLD_WIDTH / (float)(1 << Config::QT_MAX_DEPTH));

    char settledHdr[32];
    std::snprintf(settledHdr, sizeof(settledHdr), "after %d ticks", kSettle);
    std::printf("%-11s  %12s  %14s\n", "spawn", "at spawn", settledHdr);
    std::printf("%-11s  %12s  %14s\n", "-----------", "------------", "--------------");

    double settled[2] = {0.0, 0.0};
    int    k = 0;
    for (SpawnDistribution d : {SpawnDistribution::Uniform, SpawnDistribution::Clustered}) {
        EngineCore fresh;
        settleEngine(fresh, kEnemies, d, MemoryMode::SoA, CollisionMode::QuadTree, 0);
        const double c0 = spatialConcentration(fresh);

        EngineCore run;
        settleEngine(run, kEnemies, d, MemoryMode::SoA, CollisionMode::QuadTree, kSettle);
        const double cN = spatialConcentration(run);

        settled[k++] = cN;
        std::printf("%-11s  %12.2f  %14.2f\n", name(d), c0, cN);
    }

    const double ratio = (settled[0] > 0.0) ? settled[1] / settled[0] : 0.0;
    const bool   ok    = ratio >= kMinRatio;

    std::printf("\n[check] clustered/uniform concentration after %d ticks:"
                " %.2fx (need >= %.1fx) ... %s\n",
                kSettle, ratio, kMinRatio, ok ? "PASS" : "FAIL");

    if (!ok) {
        std::printf("[check]   -> the two scenarios are not distinguishable, so the\n");
        std::printf("[check]      sweep below is timing one field twice. Check that\n");
        std::printf("[check]      AGGRO_RANGE still holds enemies at their home points\n");
        std::printf("[check]      and that respawnDead() is still reviving them there.\n");
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The distribution sweep: the measurement the whole axis exists to produce.
//
// Memory layout is pinned to SoA so exactly one variable moves per row pair.
// BruteForce is the control: its cost is N^2 wherever the entities are, so any
// row-to-row change in the BF column is machine noise and can be read as the
// sweep's own error bar. Everything the QuadTree column does beyond that is the
// distribution talking.
//
// The winner is printed per row rather than asserted, because which way it goes
// is the finding. A sweep that hard-coded the expected answer would be an
// assertion with a table attached.
// ---------------------------------------------------------------------------
void runDistributionSweep() {
    std::printf("\n=== Distribution sweep (SoA fixed; ms per tick, lower is better) ===\n\n");
    std::printf("BruteForce is the control: N^2 regardless of layout in space, so its\n");
    std::printf("variation across distributions is this sweep's noise floor. QuadTree\n");
    std::printf("is the variable: a balanced tree is cheap, a lopsided one is not.\n\n");

    std::printf("%8s  %-11s  %10s  %10s  %10s  %s\n",
                "entities", "spawn", "BF ms", "QT ms", "QT/BF", "winner");
    std::printf("%8s  %-11s  %10s  %10s  %10s  %s\n",
                "--------", "-----------", "----------", "----------", "----------", "------");

    for (int n : {2000, 5000, 10000}) {
        for (SpawnDistribution d : {SpawnDistribution::Uniform, SpawnDistribution::Clustered}) {
            double ms[2] = {0.0, 0.0};
            int    k     = 0;
            for (CollisionMode cm : {CollisionMode::BruteForce, CollisionMode::QuadTree}) {
                EngineCore engine;
                // Settle first, then time. Timing from tick 0 would average the
                // spawn-state field together with the steady-state one, and the
                // spawn state is the one moment the two distributions are
                // guaranteed to differ -- it would flatter the axis.
                settleEngine(engine, n, d, MemoryMode::SoA, cm, 20);
                const int iters = (cm == CollisionMode::BruteForce && n >= 5000) ? 3 : 15;
                ms[k++] = timeTicks(engine, iters);
            }
            const double bf = ms[0], qt = ms[1];
            std::printf("%8d  %-11s  %10.3f  %10.3f  %9.2fx  %s\n",
                        n, name(d), bf, qt, bf > 0.0 ? qt / bf : 0.0,
                        qt < bf ? "QuadTree" : "BruteForce");
        }
        std::printf("\n");
    }

    std::printf("  Read the QT/BF column, not the absolute times. It is the QuadTree's\n");
    std::printf("  advantage expressed against a control that cannot be affected by\n");
    std::printf("  the thing being varied, so it isolates the distribution effect from\n");
    std::printf("  everything else this machine is doing.\n");
}

}  // namespace

int main() {
    printBuildBanner();
    printLayoutFacts();

    int failures = runCorrectnessChecks();
    failures += runRendererPointerCheck();
    failures += runEquivalenceCheck();
    failures += runPopulationStabilityCheck();

    // The gate runs before the sweep it guards, so a reader who stops at the
    // first FAIL has already been told not to trust the table underneath it.
    const int gateFailures = runDistributionGate();
    failures += gateFailures;

    runBenchmark();
    if (gateFailures == 0) {
        runDistributionSweep();
    } else {
        std::printf("\n=== Distribution sweep SKIPPED ===\n\n");
        std::printf("  The scenario gate failed, so the two distributions are not\n");
        std::printf("  measurably different fields. Running the sweep anyway would\n");
        std::printf("  produce a table of noise that looks exactly like a result.\n");
    }
    runCacheCliff(queryCacheSizes());

    if (failures > 0) {
        std::printf("*** %d check(s) FAILED. Either the QuadTree is not visiting the\n"
                    "*** same set of pairs as BruteForce, or the AoS and SoA paths are\n"
                    "*** no longer simulating the same world, or the respawner is\n"
                    "*** letting the aura grind the population down, or the spawn\n"
                    "*** distributions have stopped being distinguishable. In every\n"
                    "*** case the ms/tick columns above are timing two different\n"
                    "*** workloads and cannot be used as evidence that anything is\n"
                    "*** faster.\n\n",
                    failures);
    }
    return failures == 0 ? 0 : 1;
}

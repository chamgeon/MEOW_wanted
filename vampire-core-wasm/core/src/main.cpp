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

const char* name(CollisionMode m) { return m == CollisionMode::BruteForce ? "BruteForce" : "QuadTree"; }
const char* name(MemoryMode m)    { return m == MemoryMode::AoS          ? "AoS"        : "SoA"; }

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
    const int   kEnemies = 2000;
    const int   kTicks   = 60;
    const float kTol     = 1e-3f;

    int failures = 0;

    for (CollisionMode cm : {CollisionMode::BruteForce, CollisionMode::QuadTree}) {
        EngineCore a, b;
        a.init(kEnemies);  a.setMemoryMode(MemoryMode::AoS);  a.setCollisionMode(cm);
        b.init(kEnemies);  b.setMemoryMode(MemoryMode::SoA);  b.setCollisionMode(cm);

        for (int i = 0; i < kTicks; ++i) { a.tick(0.016f); b.tick(0.016f); }

        const float* ax = a.getAoSPosX();  const float* ay = a.getAoSPosY();
        const float* bx = b.getSoAPosX();  const float* by = b.getSoAPosY();

        float worst = 0.f;
        for (int i = 0; i < kEnemies; ++i) {
            worst = std::max(worst, std::fabs(ax[i] - bx[i]));
            worst = std::max(worst, std::fabs(ay[i] - by[i]));
        }

        const bool ok = (worst <= kTol);
        std::printf("[check] AoS/SoA equivalence (%-10s): worst position delta"
                    " %.6f ... %s\n", name(cm), worst, ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    return failures;
}

}  // namespace

int main() {
    printBuildBanner();
    printLayoutFacts();

    int failures = runCorrectnessChecks();
    failures += runRendererPointerCheck();
    failures += runEquivalenceCheck();
    runBenchmark();
    runCacheCliff(queryCacheSizes());

    if (failures > 0) {
        std::printf("*** %d check(s) FAILED. Either the QuadTree is not visiting the\n"
                    "*** same set of pairs as BruteForce, or the AoS and SoA paths are\n"
                    "*** no longer simulating the same world. In either case the\n"
                    "*** ms/tick columns above are timing two different workloads and\n"
                    "*** cannot be used as evidence that anything is faster.\n\n",
                    failures);
    }
    return failures == 0 ? 0 : 1;
}

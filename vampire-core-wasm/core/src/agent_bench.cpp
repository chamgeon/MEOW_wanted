#include "../include/EngineCore.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const int count = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int ticks = argc > 2 ? std::atoi(argv[2]) : 5;
    const int collision = argc > 3 ? std::atoi(argv[3]) : 0;
    const int distribution = argc > 4 ? std::atoi(argv[4]) : 0;
    const int memory = argc > 5 ? std::atoi(argv[5]) : 1;
    const float speed = argc > 6 ? static_cast<float>(std::atof(argv[6])) : 1.0f;
    const float frameDt = argc > 7 ? static_cast<float>(std::atof(argv[7])) : 0.016f;
    if (count < 1 || count > Config::MAX_ENEMIES || ticks < 1 || ticks > 100) return 2;
    if (collision < 0 || collision > 3 || (distribution != 0 && distribution != 1) ||
        (memory != 0 && memory != 1) || speed < 0.25f || speed > 4.0f ||
        frameDt < 0.001f || frameDt > 0.05f) return 2;

    EngineCore engine;
    engine.init(count, 1337u);
    engine.setSpawnDistribution(distribution == 0 ? SpawnDistribution::Uniform : SpawnDistribution::Clustered);
    engine.setCollisionMode(static_cast<CollisionMode>(collision));
    engine.setMemoryMode(memory == 0 ? MemoryMode::AoS : MemoryMode::SoA);
    engine.setPlayerInput(0.f, 0.f);

    // First-touch work is excluded from timing, but both builds see the same trace.
    engine.tick(frameDt * speed);
    std::printf("{\"count\":%d,\"samples\":[", count);
    for (int i = 0; i < ticks; ++i) {
        const auto start = std::chrono::steady_clock::now();
        engine.tick(frameDt * speed);
        const auto stop = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(stop - start).count();
        std::printf("%s%.6f", i ? "," : "", ms);
    }
    std::printf("],\"alive\":%d,\"kills\":%d,\"positions\":[",
                engine.getStats().aliveEnemies, engine.getKills());
    const float* x = engine.getPosX();
    const float* y = engine.getPosY();
    for (int i = 0; i < count; ++i)
        std::printf("%s[%.6f,%.6f]", i ? "," : "", x[i], y[i]);
    std::puts("]}");
    return 0;
}

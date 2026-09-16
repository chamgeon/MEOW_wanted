"""Reviewed, extensible C++ candidate generators for the active repulsion path.

Model output chooses and explains candidates; it is never run as a shell command
or compiled as arbitrary C++. Each recipe starts from the same source snapshot.
"""

from __future__ import annotations

from pathlib import Path


ENGINE = Path("core/src/EngineCore.cpp")
TREE = Path("core/src/QuadTree.cpp")


def _replace_method(source: str, method: str, replacement: str) -> str:
    signature = f"void EngineCore::{method}(float dt) {{"
    if source.count(signature) != 1:
        raise ValueError(f"Expected one {method} implementation")
    start = source.index(signature)
    cursor = start + len(signature)
    depth = 1
    while depth and cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
        cursor += 1
    if depth:
        raise ValueError(f"Unclosed {method} implementation")
    return source[:start] + replacement + source[cursor:]


def eligible(mode: str) -> list[str]:
    if mode == "BruteForce":
        return ["QuadTree", "UniformGrid", "SpatialHash"]
    if mode == "QuadTree":
        return ["QuadTreeReuse", "UniformGrid", "SpatialHash"]
    raise ValueError(f"Unknown collision mode: {mode}")


def comparison_algorithms(mode: str) -> list[str]:
    """All other built-in collision paths, never the active one."""
    modes = ["BruteForce", "QuadTree", "UniformGrid", "SpatialHash"]
    if mode not in modes:
        raise ValueError(f"Unknown collision mode: {mode}")
    return [candidate for candidate in modes if candidate != mode]


def _spatial_method(method: str, hashed: bool) -> str:
    storage = (
        "std::unordered_map<long long, std::vector<int>> cells;"
        if hashed else "std::vector<std::vector<int>> cells(static_cast<size_t>(cols) * rows);"
    )
    key = (
        "(static_cast<long long>(cy) << 32) | static_cast<unsigned>(cx)"
        if hashed else "cy * cols + cx"
    )
    lookup = (
        "auto it = cells.find(key(cx, cy));\n"
        "        return it == cells.end() ? nullptr : &it->second;"
        if hashed else "return &cells[key(cx, cy)];"
    )
    # Both versions mutate the cell holding an already-updated entity, so the
    # index reflects BruteForce's in-place (Gauss-Seidel) update order.
    return f"""void EngineCore::{method}(float dt) {{
    const float sep = Config::REPULSION_RADIUS;
    const float sepSq = sep * sep;
    const int cols = static_cast<int>(std::ceil(Config::WORLD_WIDTH / sep)) + 1;
    const int rows = static_cast<int>(std::ceil(Config::WORLD_HEIGHT / sep)) + 1;
    const bool useSoA = memoryMode_ == MemoryMode::SoA;
    const int n = useSoA ? soa_.size : static_cast<int>(aos_.enemies.size());
    auto alive = [&](int i) {{ return useSoA ? soa_.alive[i] != 0 : aos_.enemies[i].alive; }};
    auto x = [&](int i) {{ return useSoA ? soa_.posX[i] : aos_.enemies[i].position.x; }};
    auto y = [&](int i) {{ return useSoA ? soa_.posY[i] : aos_.enemies[i].position.y; }};
    auto cx = [&](float v) {{ return std::clamp(static_cast<int>(v / sep), 0, cols - 1); }};
    auto cy = [&](float v) {{ return std::clamp(static_cast<int>(v / sep), 0, rows - 1); }};
    auto key = [&](int cx, int cy) {{ return {key}; }};
    {storage}
    auto lookup = [&](int cx, int cy) -> const std::vector<int>* {{
        if (cx < 0 || cx >= cols || cy < 0 || cy >= rows) return nullptr;
        {lookup}
    }};
    for (int j = 0; j < n; ++j)
        if (alive(j)) cells[key(cx(x(j)), cy(y(j)))].push_back(j);

    std::vector<int> neighbors;
    for (int i = 0; i < n; ++i) {{
        if (!alive(i)) continue;
        const int oldX = cx(x(i)), oldY = cy(y(i));
        neighbors.clear();
        for (int yy = oldY - 1; yy <= oldY + 1; ++yy)
            for (int xx = oldX - 1; xx <= oldX + 1; ++xx)
                if (const auto* bucket = lookup(xx, yy))
                    neighbors.insert(neighbors.end(), bucket->begin(), bucket->end());
        // Preserve the original j=0..n-1 accumulation order.
        std::sort(neighbors.begin(), neighbors.end());
        float fx = 0.f, fy = 0.f;
        for (int j : neighbors) {{
            if (i == j || !alive(j)) continue;
            const float dx = x(i) - x(j), dy = y(i) - y(j);
            const float d2 = dx*dx + dy*dy;
            if (d2 < sepSq && d2 > 1e-6f) {{
                const float d = std::sqrt(d2);
                const float f = (sep - d) / sep * Config::REPULSION_FORCE;
                fx += dx/d * f; fy += dy/d * f;
            }}
        }}
        const float nx = std::clamp(x(i) + fx * dt, 0.f, Config::WORLD_WIDTH);
        const float ny = std::clamp(y(i) + fy * dt, 0.f, Config::WORLD_HEIGHT);
        if (useSoA) {{ soa_.posX[i] = nx; soa_.posY[i] = ny; }}
        else {{ aos_.enemies[i].position.x = nx; aos_.enemies[i].position.y = ny; }}
        const int newX = cx(nx), newY = cy(ny);
        if (newX != oldX || newY != oldY) {{
            auto& oldBucket = cells[key(oldX, oldY)];
            oldBucket.erase(std::remove(oldBucket.begin(), oldBucket.end(), i), oldBucket.end());
            cells[key(newX, newY)].push_back(i);
        }}
    }}
}}"""


def generate(snapshot: dict[Path, str], mode: str, candidate: str) -> dict[Path, str]:
    """Return a fresh file map. Never mutate the caller's baseline snapshot."""
    if candidate not in eligible(mode):
        raise ValueError(f"Candidate {candidate} is not applicable to {mode}")
    result = snapshot.copy()
    method = "updateRepulsionBruteForce" if mode == "BruteForce" else "updateRepulsionQuadTree"
    source = result[ENGINE]
    if candidate == "QuadTree":
        result[ENGINE] = _replace_method(
            source, method,
            f"void EngineCore::{method}(float dt) {{\n    updateRepulsionQuadTree(dt);\n}}",
        )
    elif candidate in {"UniformGrid", "SpatialHash"}:
        if candidate == "SpatialHash" and "#include <unordered_map>" not in source:
            source = source.replace("#include <algorithm>", "#include <algorithm>\n#include <unordered_map>", 1)
        result[ENGINE] = _replace_method(source, method, _spatial_method(method, candidate == "SpatialHash"))
    else:
        tree = result[TREE]
        old_clear = "for (auto& c : children_) c.reset();"
        if tree.count(old_clear) != 1:
            raise ValueError("QuadTree clear() layout changed")
        tree = tree.replace(old_clear, "for (auto& c : children_) if (c) c->clear();", 1)
        for idx in range(4):
            needle = f"children_[{idx}] = std::make_unique<QuadTree>("
            if tree.count(needle) != 1:
                raise ValueError("QuadTree subdivide() layout changed")
            # Retain nodes between ticks instead of allocating fresh children.
            tree = tree.replace(needle, f"if (!children_[{idx}]) children_[{idx}] = std::make_unique<QuadTree>(", 1)
        result[TREE] = tree
    return result

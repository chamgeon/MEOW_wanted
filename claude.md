# Role & Project Goal
You are a senior C++ game engine programmer and systems performance architect.
Your task is to build a high-performance interactive tech demo named **"Vampire-Core Wasm"** from scratch.
This project proves low-level optimization skills (Data-Oriented Design, dynamic spatial partitioning, cache efficiency) and AI-driven automated code profiling/refactoring for Nexon engineering recruiting and hackathon submission.

---

## 1. Problem Statement & Tech Architecture
- **Problem**: In games with massive entities (e.g., Vampire Survivors-like bullet hell), CPU spikes and frame drops frequently occur during entity collision and repulsive separation loops ($O(N^2)$ brute-force checks and AoS cache misses). Manual code-level bottleneck diagnosis requires heavy engineering resources.
- **Solution**:
  1. A C++ core engine compiled to WebAssembly (Wasm) via Emscripten that simulates 5,000–10,000 enemies at 60 FPS.
  2. Toggleable calculation modes to benchmark performance in real time:
     - Collision Detection: [Brute Force $O(N^2)$] vs [Dynamic Quad-tree $O(N \log N)$]
     - Memory Layout: [Array of Structures (AoS)] vs [Structure of Arrays (SoA / Data-Oriented Design)]
  3. A FastAPI backend integrating an LLM Code Profiler Agent that receives runtime performance stats (FPS, frame time in ms, active entity count) and C++ source snippets, returning an architectural bottleneck diagnosis and optimized C++ refactoring diffs.

---

## 2. Directory Structure to Scaffold
Create the project following this layout:

```text
vampire-core-wasm/
├── core/                        # Pure C++ Simulation Engine
│   ├── CMakeLists.txt
│   ├── include/
│   │   ├── Config.h             # Simulation constants (world size, max enemies)
│   │   ├── Vector2D.h           # Inline SIMD-friendly 2D vector math
│   │   ├── EnemyAoS.h           # Array of Structures baseline entity
│   │   ├── EnemySoA.h           # Structure of Arrays cache-friendly container
│   │   ├── QuadTree.h           # Dynamic 2D spatial partitioning
│   │   └── EngineCore.h         # Game loop tick, repulsion, projectiles
│   └── src/
│       ├── Vector2D.cpp
│       ├── QuadTree.cpp
│       ├── EngineCore.cpp
│       └── bindings.cpp         # Emscripten embind & direct memory export
├── web/                         # Frontend Viewer & Profiler HUD
│   ├── package.json
│   ├── vite.config.ts
│   ├── src/
│   │   ├── App.tsx
│   │   ├── components/
│   │   │   ├── GameCanvas.tsx   # Canvas 2D / WebGL direct buffer renderer
│   │   │   ├── MetricGraph.tsx  # Real-time FPS, frame time (ms) chart
│   │   │   └── CodePanel.tsx    # Live C++ snippet viewer & AI analysis trigger
│   │   └── wasm/
│   │       ├── core_engine.js   # Compiled Emscripten glue code
│   │       └── core_engine.wasm
├── server/                      # AI Optimizer Backend
│   ├── requirements.txt         # fastapi, uvicorn, pydantic, openai/anthropic
│   ├── main.py                  # Profiling endpoint /api/optimize
│   └── prompts.py               # Low-level systems engineering system prompt
└── Makefile                     # One-click build: make wasm, make front, make server
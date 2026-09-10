# Running the live demo on Windows

The web client is finished, but it needs the Wasm module. `web/public/` is
empty until you build it, and the app renders a "Wasm not loaded" banner in
that state. Everything below runs in PowerShell.

## 1. Install the Emscripten SDK (once)

```powershell
cd D:\GitHub
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
.\emsdk install latest
.\emsdk activate latest
```

## 2. Activate it (every new shell)

```powershell
cd D:\GitHub\emsdk
.\emsdk_env.ps1
emcc --version        # should print a version, not "not recognized"
```

`emsdk_env.ps1` only edits the current shell's PATH. Open a new terminal and
you have to run it again.

## 3. Build the Wasm module

```powershell
cd D:\GitHub\AIcpp\vampire-core-wasm
powershell -ExecutionPolicy Bypass -File .\build-wasm.ps1
```

That writes `web\public\core_engine.js` and `web\public\core_engine.wasm`.

The script calls `emcc` directly rather than `emcmake cmake`. On Windows CMake
defaults to the Visual Studio generator, which cannot drive clang/emcc, so
`emcmake cmake` fails unless Ninja or MinGW make happens to be on PATH. With
four translation units there is nothing for a build system to do here.

(The `wasm` target in the `Makefile` still uses `emcmake` and works fine under
Git Bash / WSL, where the default generator is Unix Makefiles.)

## 4. Start the client

```powershell
cd D:\GitHub\AIcpp\vampire-core-wasm\web
npm install
npm run dev
```

Open http://localhost:5173.

## What to look at

Controls: WASD or arrow keys move the player. The enemy slider goes to 10,000.

The HUD separates two numbers that are easy to conflate:

- **Sim ms** is `EngineCore::tick` only, measured inside C++ over a rolling
  30-frame window. This is the number the optimization work moves.
- **Frame ms** is the whole JS frame: tick plus the canvas blit. It includes
  costs the C++ side cannot see.

Both reset when you flip a toggle, so the transition is visible immediately
instead of being smeared over the next few seconds by a stale average.

Suggested sequence:

1. Push the slider to 10,000 and leave Collision on QuadTree. Should hold 60.
2. Flip Collision to BruteForce. Sim ms jumps by roughly an order of magnitude
   -- 50M pair checks per frame -- while Frame ms rises by the same absolute
   amount, since the renderer's cost did not change. That gap is the point:
   the algorithmic win is entirely in the sim column.
3. Flip Memory between SoA and AoS with Collision on BruteForce, where the
   pair loop is memory-bound. The size of this gap depends on your L2: at
   10,000 entities AoS is 1.28 MB, so if your L2 is larger than that it still
   fits and the two columns tie. Drag the slider up until AoS falls out of
   cache and the gap opens. `core_bench` prints your machine's actual cache
   sizes and the measured stride knee.
4. On QuadTree the memory toggle barely matters, because the quadtree already
   cut the number of pairs down to where the loop is latency-bound rather than
   bandwidth-bound. Two optimizations that stack in principle can hide each
   other in practice, and the demo shows that honestly.

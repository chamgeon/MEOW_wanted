# Vampire-Core Wasm — one-click build targets.
#
# On Windows run these from Git Bash or WSL (GNU make required). The individual
# commands under each target also work verbatim if you prefer to paste them.

WASM_OUT := web/public

.PHONY: all bench wasm front dev server clean help

help:
	@echo "make bench   - build + run the native C++ benchmark (no Emscripten needed)"
	@echo "make wasm    - compile core/ to WebAssembly into $(WASM_OUT)/ (needs emsdk)"
	@echo "make dev     - start the Vite dev server on :5173"
	@echo "make front   - production build of the web app"
	@echo "make server  - start the FastAPI AI profiler on :8000"
	@echo "make clean   - remove build artifacts"

# --- Native benchmark + correctness checks (plain gcc/clang/MSVC) ---
# Start here: this runs today without the Emscripten SDK.
bench:
	cd core && cmake -B build-native -DCMAKE_BUILD_TYPE=Release
	cd core && cmake --build build-native --parallel
	./core/build-native/core_bench

# --- Wasm (requires Emscripten SDK activated: source /path/to/emsdk/emsdk_env.sh) ---
wasm:
	mkdir -p $(WASM_OUT)
	cd core && emcmake cmake -B build -DCMAKE_BUILD_TYPE=Release
	cd core && cmake --build build --parallel
	cp core/build/core_engine.js   $(WASM_OUT)/
	cp core/build/core_engine.wasm $(WASM_OUT)/
	@echo "Wasm build done -> $(WASM_OUT)"

# --- Frontend production build ---
front:
	cd web && npm install && npm run build

# --- Start dev frontend (hot-reload) ---
dev:
	cd web && npm install && npm run dev

# --- Start AI backend ---
server:
	cd server && pip install -r requirements.txt
	cd server && uvicorn main:app --reload --port 8000

# --- Clean build artifacts ---
clean:
	rm -rf core/build core/build-native
	rm -f  $(WASM_OUT)/core_engine.js $(WASM_OUT)/core_engine.wasm
	rm -rf web/dist
	@echo "Clean done"

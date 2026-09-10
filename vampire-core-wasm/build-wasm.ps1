# ---------------------------------------------------------------------------
# Vampire-Core Wasm - Windows one-shot Wasm build.
#
#   powershell -ExecutionPolicy Bypass -File .\build-wasm.ps1
#
# Prerequisite: the Emscripten SDK must be activated in THIS shell, i.e. `em++`
# resolves. See README-BUILD.md for the emsdk install steps.
#
# This calls em++ directly instead of going through CMake on purpose. On
# Windows, `emcmake cmake` inherits CMake's default generator, which is Visual
# Studio - and the VS generator cannot drive clang/emcc. Working around that
# needs Ninja or MinGW make on PATH. A direct compiler invocation has no
# generator to get wrong, and this project is only four translation units.
#
# em++, NOT emcc. emcc compiles .cpp files as C++ but links with the C driver,
# so libc++ is left off the link line and every `new`, `delete`, and thrown
# exception in QuadTree.cpp / EngineCore.cpp comes back as an undefined symbol
# from wasm-ld. The compile step succeeds, which makes the failure look like a
# source problem when it is purely a driver choice.
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root "web\public"

if (-not (Get-Command em++ -ErrorAction SilentlyContinue)) {
    Write-Host "ERROR: em++ not found on PATH." -ForegroundColor Red
    Write-Host "Activate the Emscripten SDK in this shell first:"
    Write-Host "    cd D:\GitHub\emsdk"
    Write-Host "    .\emsdk_env.ps1"
    exit 1
}

New-Item -ItemType Directory -Force -Path $out | Out-Null

$sources = @(
    "core\src\Vector2D.cpp",
    "core\src\QuadTree.cpp",
    "core\src\EngineCore.cpp",
    "core\src\bindings.cpp"
) | ForEach-Object { Join-Path $root $_ }

$flags = @(
    "-O3",
    "-std=c++17",
    "-msimd128",
    "--bind",
    "-I", (Join-Path $root "core\include"),
    "-sWASM=1",
    "-sMODULARIZE=1",
    "-sEXPORT_NAME=CoreEngineModule",
    "-sALLOW_MEMORY_GROWTH=1",
    "-sINITIAL_MEMORY=67108864",
    "-sENVIRONMENT=web",
    "-sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAP32']"
)

Write-Host "Building Wasm -> $out\core_engine.js" -ForegroundColor Cyan
& em++ @flags @sources -o (Join-Path $out "core_engine.js")
if ($LASTEXITCODE -ne 0) { Write-Host "Build FAILED" -ForegroundColor Red; exit $LASTEXITCODE }

Get-ChildItem $out -Filter "core_engine.*" | ForEach-Object {
    "{0,-20} {1,10:N0} bytes" -f $_.Name, $_.Length
}
Write-Host "OK. Now: cd web ; npm install ; npm run dev" -ForegroundColor Green

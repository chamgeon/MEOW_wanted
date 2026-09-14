# ---------------------------------------------------------------------------
# Vampire-Core Wasm - Windows one-shot Wasm build (the `make wasm` equivalent).
#
#   cd D:\GitHub\wanted_demo\vampire-core-wasm
#   powershell -ExecutionPolicy Bypass -File .\build-wasm.ps1
#
# WHY THIS EXISTS INSTEAD OF `make wasm`
#
# The Makefile target uses `mkdir -p` and `cp` and needs GNU make. A Developer
# PowerShell has none of the three, so `make wasm` cannot run there at all.
#
# WHY em++ DIRECTLY INSTEAD OF emcmake + CMake
#
# On Windows, `emcmake cmake` inherits CMake's default generator, which is
# Visual Studio - and the VS generator cannot drive clang/emcc. Working around
# that needs Ninja or MinGW make on PATH. A direct compiler invocation has no
# generator to get wrong, and this project is only four translation units.
#
# WHY em++, NOT emcc
#
# emcc compiles .cpp files as C++ but links with the C driver, so libc++ is left
# off the link line and every `new`, `delete`, and thrown exception in
# QuadTree.cpp / EngineCore.cpp comes back as an undefined symbol from wasm-ld.
# The compile step succeeds, which makes the failure look like a source problem
# when it is purely a driver choice.
#
# All output here is pure ASCII on purpose: this runs in a cp949 Korean console
# where non-ASCII punctuation renders as mojibake.
# ---------------------------------------------------------------------------

param(
    # Where the Emscripten SDK lives. Left empty, the script probes the usual
    # places (see Step 1). Pass this if emsdk is somewhere unusual.
    [string] $EmsdkRoot = "",
    # Skip the post-build embind symbol scan. Only useful when iterating on the
    # script itself.
    [switch] $NoVerify
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root "web\public"

function Step($n, $msg) { Write-Host "[$n/5] $msg" -ForegroundColor Cyan }
function Fail($msg)     { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# --- 1. Locate and activate the Emscripten SDK -----------------------------
#
# The activation must be DOT-SOURCED (`. $env`), not invoked with `&`. emsdk_env
# works by setting PATH and EMSDK in the caller's scope; with `&` those land in
# a child scope, vanish on return, and em++ is still missing two lines later -
# a failure that reads like "emsdk is broken" rather than "wrong call operator".
Step 1 "Locating Emscripten SDK"

if (Get-Command em++ -ErrorAction SilentlyContinue) {
    Write-Host "      em++ already on PATH - skipping activation."
} else {
    $candidates = @()
    if ($EmsdkRoot)  { $candidates += $EmsdkRoot }
    if ($env:EMSDK)  { $candidates += $env:EMSDK }
    $candidates += (Join-Path $root "core\emsdk")
    $candidates += "D:\GitHub\emsdk"
    $candidates += "C:\emsdk"

    # A bare `git clone` of emsdk has emsdk_env.ps1 but NO upstream\emscripten -
    # the toolchain is only downloaded by `emsdk install`. Activating such a
    # clone succeeds silently and leaves em++ unresolvable, so require the
    # compiler directory, not just the env script.
    $sdk = $null
    foreach ($c in $candidates) {
        if ($c -and (Test-Path (Join-Path $c "upstream\emscripten\em++.bat"))) { $sdk = $c; break }
    }

    if (-not $sdk) {
        $clone = $candidates | Where-Object { $_ -and (Test-Path (Join-Path $_ "emsdk.ps1")) } | Select-Object -First 1
        Write-Host "ERROR: no installed Emscripten SDK found." -ForegroundColor Red
        Write-Host "       Probed:"
        foreach ($c in $candidates) { if ($c) { Write-Host "         $c" } }
        if ($clone) {
            Write-Host ""
            Write-Host "       An emsdk CLONE exists at $clone but the toolchain is not installed."
            Write-Host "       Run once (this downloads ~1 GB):"
            Write-Host "         cd $clone"
            Write-Host "         .\emsdk.ps1 install latest"
            Write-Host "         .\emsdk.ps1 activate latest"
        } else {
            Write-Host ""
            Write-Host "       Install it once:"
            Write-Host "         git clone https://github.com/emscripten-core/emsdk D:\GitHub\emsdk"
            Write-Host "         cd D:\GitHub\emsdk"
            Write-Host "         .\emsdk.ps1 install latest"
            Write-Host "         .\emsdk.ps1 activate latest"
        }
        exit 1
    }

    Write-Host "      Using $sdk"
    . (Join-Path $sdk "emsdk_env.ps1") | Out-Null
    if (-not (Get-Command em++ -ErrorAction SilentlyContinue)) {
        Fail "activated $sdk but em++ still does not resolve. Try '.\emsdk.ps1 activate latest' in that directory."
    }
}

# --- 2. Collect sources ----------------------------------------------------
#
# main.cpp is deliberately absent: it is the NATIVE correctness harness
# (equivalence + population-stability checks) and carries its own main(), which
# would fight bindings.cpp for the module entry point.
Step 2 "Collecting sources"

$sources = @(
    "core\src\Vector2D.cpp",
    "core\src\QuadTree.cpp",
    "core\src\EngineCore.cpp",
    "core\src\bindings.cpp"
) | ForEach-Object {
    $p = Join-Path $root $_
    if (-not (Test-Path $p)) { Fail "missing source file: $p" }
    $p
}
Write-Host ("      {0} translation units." -f $sources.Count)

# --- 3. Compile + link -----------------------------------------------------
#
# EXPORTED_RUNTIME_METHODS is written without brackets or quotes on purpose.
# The bracketed form -sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAP32'] has to
# survive PowerShell's argument parser and then cmd's, and the quotes can reach
# emcc as part of the literal name. The comma-separated form means the same
# thing to emcc and has nothing for either shell to mangle.
Step 3 "Compiling to WebAssembly"

New-Item -ItemType Directory -Force -Path $out | Out-Null

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
    "-sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAP32"
)

$jsOut   = Join-Path $out "core_engine.js"
$wasmOut = Join-Path $out "core_engine.wasm"

Write-Host "      -> $jsOut"
& em++ @flags @sources -o $jsOut
if ($LASTEXITCODE -ne 0) { Fail "em++ exited with $LASTEXITCODE" }

# --- 4. Report artifacts ---------------------------------------------------
Step 4 "Artifacts"
foreach ($f in @($jsOut, $wasmOut)) {
    if (-not (Test-Path $f)) { Fail "expected output missing: $f" }
    $i = Get-Item $f
    Write-Host ("      {0,-20} {1,10:N0} bytes   {2}" -f $i.Name, $i.Length, $i.LastWriteTime.ToString("HH:mm:ss"))
}

# --- 5. Verify the binary actually carries the current bindings ------------
#
# A build that silently reuses a stale artifact, or a copy that lands in the
# wrong directory, produces a browser session where the HUD updates normally
# and only the NEW bindings are missing. That surfaces as
# "engine.getAuraPhase is not a function" hundreds of frames into a rAF
# callback, which reads like a frontend bug and costs an hour.
#
# embind writes every exported name as a literal string into the .wasm, so an
# ASCII scan of the bytes proves the file on disk is the one just compiled.
if (-not $NoVerify) {
    Step 5 "Verifying embind exports"
    $expected = @(
        "getPlayerX", "getPlayerY", "getAuraRadius", "getAuraPhase", "getAuraFlash",
        "getKills", "getAliveCount", "getLastTickMs",
        "lastTickMs", "auraPulses",
        "setCollisionMode", "setMemoryMode", "getPosXPtr", "getPosYPtr",
        # The scenario axis. Newest bindings go at the END of this list on
        # purpose: the whole point of the scan is to catch a stale artifact, and
        # the most recently added export is the one a stale build is missing.
        "setSpawnDistribution", "getSpawnDistribution"
    )
    $blob = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($wasmOut))
    $missing = @($expected | Where-Object { -not $blob.Contains($_) })
    if ($missing.Count -gt 0) {
        Write-Host ("      MISSING: {0}" -f ($missing -join ", ")) -ForegroundColor Red
        Fail "the .wasm in web\public does not export the current bindings. Check that bindings.cpp compiled and that nothing overwrote web\public afterwards."
    }
    Write-Host ("      All {0} expected export names present." -f $expected.Count)
} else {
    Step 5 "Verification skipped (-NoVerify)"
}

Write-Host ""
Write-Host "Build OK." -ForegroundColor Green
Write-Host "Next:  cd web ; npm install ; npm run dev"

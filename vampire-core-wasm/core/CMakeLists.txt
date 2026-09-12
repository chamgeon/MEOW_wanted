cmake_minimum_required(VERSION 3.20)
project(VampireCore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Single-config generators (Make, Ninja) need a build type at configure time.
# Multi-config generators (Visual Studio, Xcode) pick it at build time instead,
# via `cmake --build <dir> --config Release`.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Simulation code shared by both the Wasm module and the native harness.
set(ENGINE_SOURCES
    src/Vector2D.cpp
    src/QuadTree.cpp
    src/EngineCore.cpp
)

add_library(engine STATIC ${ENGINE_SOURCES})
target_include_directories(engine PUBLIC include)

if(MSVC)
    target_compile_options(engine PRIVATE /W4)
else()
    target_compile_options(engine PRIVATE -Wall -Wextra)
endif()

if(EMSCRIPTEN)
    # ---- WebAssembly module (entry point is embind, no main()) ----
    add_executable(core_engine src/bindings.cpp)
    target_link_libraries(core_engine PRIVATE engine)

    target_compile_options(engine      PRIVATE -O3 -msimd128)
    target_compile_options(core_engine PRIVATE -O3 -msimd128)

    target_link_options(core_engine PRIVATE
        -O3
        --bind
        -msimd128
        -sWASM=1
        -sMODULARIZE=1
        -sEXPORT_NAME=CoreEngineModule
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sENVIRONMENT=web
        "SHELL:-sEXPORTED_RUNTIME_METHODS=['HEAPF32','HEAP32']"
    )
    set_target_properties(core_engine PROPERTIES
        OUTPUT_NAME "core_engine"
        SUFFIX ".js"
    )
    message(STATUS "Configured for Emscripten -> core_engine.js + core_engine.wasm")
else()
    # ---- Native benchmark + correctness harness ----
    add_executable(core_bench src/main.cpp)
    target_link_libraries(core_bench PRIVATE engine)

    # No hand-rolled -O2 / /O2 here on purpose. CMake already supplies the right
    # optimization flags per configuration, and forcing /O2 on MSVC collides
    # with the /RTC1 runtime checks it injects into Debug builds (error D8016).
    message(STATUS "Configured for native build -> core_bench")

    if(CMAKE_CONFIGURATION_TYPES)
        message(STATUS "Multi-config generator: pass --config Release when building,"
                       " otherwise the benchmark measures an unoptimized Debug build.")
    endif()
endif()

cmake_minimum_required(VERSION 3.22.1)

project("dlmslosss" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Fetch Google Oboe (low-latency audio) ─────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG        1.8.1
)
FetchContent_MakeAvailable(oboe)

# ─── DSP + JNI shared library ──────────────────────────────────────────────
add_library(
    dlmslosss            # Library name (loaded from Kotlin via System.loadLibrary)
    SHARED

    # JNI bridge
    native-lib.cpp

    # DSP engine
    DlmsEngine.cpp
    BiquadFilter.cpp
    AudioEngine.cpp
    DwpParser.cpp
)

# Include Oboe headers
target_include_directories(dlmslosss PRIVATE
    ${oboe_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Link Oboe, Android log, and OpenSL ES (used internally by Oboe)
target_link_libraries(dlmslosss
    oboe
    android
    log
    OpenSLES
)

# Enable link-time optimization for release
target_compile_options(dlmslosss PRIVATE
    -O2
    -ffast-math        # Safe for DSP: enables vectorization
    -DANDROID
)

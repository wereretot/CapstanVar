cmake_minimum_required(VERSION 3.20)

# Must be set BEFORE project() so subproject policy checks use this minimum
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

project(CapstanVar VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    # /arch:AVX2 enables AVX2 codegen without enabling fast-math — strict
    # IEEE-754 is MSVC's default; /fp:precise pinned for symmetry with the
    # non-MSVC NAGRA_FP_PRECISE target define further down.
    add_compile_options(/O2 /W4 /MP /arch:AVX2 /fp:precise)
    # Note: the three CRT macros _CRT_SECURE_NO_WARNINGS / NOMINMAX /
    # WIN32_LEAN_AND_MEAN used to live here as a global add_compile_definitions
    # call. They LEAKED into FetchContent deps and broke SFML's compile
    # on Windows — SleepImpl.cpp needs full windows.h to find TIMECAPS /
    # timeGetDevCaps from mmsystem.h, and MainWin32.cpp needs MSVC's
    # __argc / __argv CRT globals (both unavailable under WIN32_LEAN_AND_MEAN).
    # They are now set target-private further down.
else()
    # No -ffast-math: the in-process verify_* smoke tests used by CI
    # (./CapstanVar --verify, see .github/workflows/ci.yml) require strict
    # IEEE-754 semantics. See docs/TAPE_PHYSICS_REFACTOR.md §X. NAGRA_FP_PRECISE
    # is scoped to this target further down (target_compile_definitions
    # PRIVATE) so FetchContent deps don't accidentally inherit it.
    # Note: -march=native is preserved here because CI runners are
    # Skylake-or-newer — switch to -march=x86-64-v3 (Haswell baseline)
    # before redistributing binaries.
    add_compile_options(-O3 -march=native -Wall -Wextra)
endif()

# ── FetchContent ──────────────────────────────────────────────────────────────
include(FetchContent)

option(CAPSTANVAR_VERBOSE_DEP_FETCH "Show FetchContent dependency download progress" ON)
set(CAPSTANVAR_GIT_LOW_SPEED_LIMIT 1024 CACHE STRING "Bytes/second threshold for stalled dependency downloads")
set(CAPSTANVAR_GIT_LOW_SPEED_TIME 30 CACHE STRING "Seconds below the low-speed threshold before Git aborts dependency downloads")

if(CAPSTANVAR_VERBOSE_DEP_FETCH)
    set(FETCHCONTENT_QUIET OFF CACHE BOOL "Show FetchContent population progress" FORCE)
endif()

set(CAPSTANVAR_FETCH_GIT_CONFIG
    "http.lowSpeedLimit=${CAPSTANVAR_GIT_LOW_SPEED_LIMIT}"
    "http.lowSpeedTime=${CAPSTANVAR_GIT_LOW_SPEED_TIME}"
)

macro(capstanvar_make_available)
    foreach(dep ${ARGN})
        string(TOLOWER "${dep}" dep_lower)
        FetchContent_GetProperties(${dep})
        if(NOT ${dep_lower}_POPULATED)
            message(STATUS "Dependency ${dep}: fetching/populating with FetchContent")
        endif()
        FetchContent_MakeAvailable(${dep})
    endforeach()
endmacro()

# SFML 2.6.2
set(SFML_BUILD_AUDIO    OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_NETWORK  OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SFML_BUILD_DOC      OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SFML
    GIT_REPOSITORY https://github.com/SFML/SFML.git
    GIT_TAG        2.6.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# Dear ImGui 1.89.9 (compatible with imgui-sfml 2.6.x)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.89.9
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# ImGui-SFML — pinned to exact commit on 2.6.x branch (SFML 2 compatible)
set(IMGUI_SFML_FIND_SFML    OFF CACHE BOOL "" FORCE)
set(IMGUI_SFML_IMGUI_TARGET imgui CACHE STRING "" FORCE)
FetchContent_Declare(imgui_sfml
    GIT_REPOSITORY https://github.com/SFML/imgui-sfml.git
    GIT_TAG        64e09feda29266572799e5ded66eac5cb0a749aa
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# nlohmann/json
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# libsndfile
# Use libsndfile's bundled codec decoders (FLAC, Vorbis, MP3) rather than
# the system packages so the CI artifact is self-contained and runs on
# end-user machines that don't have libflac-dev / libvorbis-dev /
# libmpg123-dev installed. With ENABLE_EXTERNAL_LIBS=ON (the libsndfile
# default), libsndfile's CMake configure searches for system libFLAC,
# libvorbis (and via libogg), and libmpg123 via pkg-config / find_package;
# when those searches fail on runners that don't have the -dev packages
# installed (Linux and Windows CI runners only ship ALSA / X11 / GL deps),
# libsndfile prints "External lib X not found" warnings and proceeds with
# an inconsistent fallback state. We've already observed that state on
# CI runs as the runtime failure that the user reported: sf_open on .ogg
# / .flac / .mp3 returning NULL despite the binary launching. Forcing
# ENABLE_EXTERNAL_LIBS=OFF makes libsndfile build its own FLAC / Vorbis /
# MP3 implementations from src/ and bake them into libsndfile's static
# lib unconditionally -- the CI artefact then resolves all codec symbols
# internally and decodes .ogg / .flac / .mp3 on user machines without
# requiring apt / vcpkg / brew / MSI installs of codec dev packages.
# ENABLE_MPEG stays ON because libsndfile has its own bundled MP3 decoder
# independent of libmpg123 -- that flag gates whether the bundled path
# gets compiled at all. ENABLE_EXPERIMENTAL is OFF (WavPack / SD2 /
# old-Windows-only codecs are not needed by CapstanVar).
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK   OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_MPEG ON CACHE BOOL "" FORCE)           # bundled MP3 decoder
set(ENABLE_EXPERIMENTAL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# libopus + libopusfile -- native .opus decoding fallback path.
# libsndfile 1.2.x with ENABLE_EXTERNAL_LIBS=OFF (the d57fdcf bundled-codec
# config) does NOT bundle an Opus decoder, so .opus files cannot be decoded
# by the libsndfile path alone. libopusfile 0.12 is the canonical native
# decoder for Opus-in-Ogg streams; it depends on libopus + libogg (both
# fetched transitively when libopusfile's own CMakeLists runs). Tags use
# xiph's `v`-prefix convention (verified at github.com/xiph/libopus/tags
# and libopusfile/tags -- the `libopusfile-0.12` form used in package
# manager labels is the same tag, just with a vendor prefix). The
# OPUSFILE_BUILD_HTTP=OFF option drops the optional curl + openssl dep
# that libopusfile uses for URL-streamed ogg: opus sources -- CapstanVar
# only opens local files via std::ifstream, so the network codepath can
# be excluded and we avoid carrying OpenSSL into the Linux CI runner.
set(OPUS_BUILD_TESTING           OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS          OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libopus
    GIT_REPOSITORY https://github.com/xiph/libopus.git
    GIT_TAG        v1.5.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

set(OPUSFILE_BUILD_TESTING       OFF CACHE BOOL "" FORCE)
set(OPUSFILE_BUILD_PROGRAMS      OFF CACHE BOOL "" FORCE)
set(OPUSFILE_BUILD_HTTP          OFF CACHE BOOL "" FORCE) # strip curl + openssl dep
FetchContent_Declare(libopusfile
    GIT_REPOSITORY https://github.com/xiph/libopusfile.git
    GIT_TAG        v0.12
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
    GIT_CONFIG     ${CAPSTANVAR_FETCH_GIT_CONFIG}
)

# Fetch SFML + imgui first, then imgui-sfml (needs IMGUI_DIR)
capstanvar_make_available(SFML imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
capstanvar_make_available(imgui_sfml nlohmann_json libsndfile libopus libopusfile)

# xiph/libopusfile exposes target `opusfile` but exposes no `opusfile::opusfile`
# namespaced alias. Add one here so the project's target_link_libraries can
# write `opusfile::opusfile` consistent with libsndfile's `SndFile::sndfile`
# form. Idempotent on re-runs (CMake re-runs at configure time) and on future
# libopusfile versions that may start shipping the alias themselves.
if(TARGET opusfile AND NOT TARGET opusfile::opusfile)
    add_library(opusfile::opusfile ALIAS opusfile)
endif()

if(MSVC)
    # xiph's MSVC-side sources (snprintf, strncpy, strcpy, fopen, etc.)
    # need the C4996 deprecation warning suppressed to compile cleanly
    # under MSVC 2022 default warning level. _CRT_SECURE_NO_WARNINGS
    # is applied PRIVATE to each fetched target so it doesn't leak into
    # CapstanVar's own translation units (which would break the
    # target-private scope we lock it to further down in this file).
    target_compile_definitions(opusfile PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(opus    PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ── Audio backend selection ───────────────────────────────────────────────────
# Priority: system PortAudio → ALSA (Linux) → CoreAudio stub (macOS) → WinMM (Windows)
find_package(PkgConfig QUIET)
set(AUDIO_LIBS "")
set(AUDIO_DEFS "")

if(PKG_CONFIG_FOUND)
    pkg_check_modules(PORTAUDIO portaudio-2.0)
endif()

if(PORTAUDIO_FOUND)
    message(STATUS "Audio backend: system PortAudio")
    set(AUDIO_LIBS   ${PORTAUDIO_LIBRARIES})
    set(AUDIO_DEFS   NAGRA_AUDIO_PORTAUDIO)
    include_directories(${PORTAUDIO_INCLUDE_DIRS})
elseif(UNIX AND NOT APPLE)
    find_package(ALSA REQUIRED)
    message(STATUS "Audio backend: ALSA")
    set(AUDIO_LIBS   ALSA::ALSA)
    set(AUDIO_DEFS   NAGRA_AUDIO_ALSA)
elseif(APPLE)
    message(STATUS "Audio backend: CoreAudio")
    set(AUDIO_LIBS   "-framework CoreAudio" "-framework AudioUnit" "-framework CoreFoundation")
    set(AUDIO_DEFS   NAGRA_AUDIO_COREAUDIO)
elseif(WIN32)
    message(STATUS "Audio backend: WinMM")
    set(AUDIO_LIBS   winmm)
    set(AUDIO_DEFS   NAGRA_AUDIO_WINMM)
endif()

# Portable thread abstraction. Threads::Threads maps to libpthread.so on
# Linux glibc, libSystem (bundled) on macOS, and a no-op stub on MSVC
# where std::thread is built into the runtime. The previous literal
# `pthread` link in target_link_libraries was correct on Linux/macOS but
# caused `fatal error LNK1181: cannot open input file 'pthread.lib'` on
# MSVC — there is no pthread library on Windows. Threads::Threads is the
# canonical CMake idiom for this and is now the source of the actual
# link dependency.
find_package(Threads REQUIRED)

# ── Sources ───────────────────────────────────────────────────────────────────
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

set(APP_SOURCES
    src/main.cpp
    src/engine.cpp
    src/mod_transport.cpp
    src/mod_magnetic.cpp
    src/mod_electronics.cpp
    src/mod_environment.cpp
    src/audio_io.cpp
    src/preset_manager.cpp
    src/ui.cpp
    src/ui_panels.cpp
    src/render_engine.cpp
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tinyfiledialogs.c")
    list(APPEND APP_SOURCES tinyfiledialogs.c)
    add_compile_definitions(NAGRA_HAS_TFD)
    message(STATUS "tinyfiledialogs found — native file dialogs enabled")
else()
    message(STATUS "tinyfiledialogs NOT found — file open/save will use stdin fallback")
endif()

add_executable(CapstanVar ${APP_SOURCES} ${IMGUI_SOURCES})

# Forward-compat hook for precision-sensitive DSP code (env module is the
# expected first consumer). Defined target-scoped so FetchContent deps
# (SFML / ImGui / PortAudio / libsndfile / nlohmann_json) don't inherit it.
target_compile_definitions(CapstanVar PRIVATE NAGRA_FP_PRECISE ${AUDIO_DEFS})

if(MSVC)
    # MSVC CRT macros + math-constants feature-test macro — target-private
    # so FetchContent deps (SFML / ImGui / libsndfile / PortAudio /
    # nlohmann_json / imgui-sfml) and audio-backend targets don't inherit
    # them. WIN32_LEAN_AND_MEAN in particular strips the windows.h surface
    # that SFML's SleepImpl.cpp (needs TIMECAPS / timeGetDevCaps from
    # mmsystem.h) and SFML/Main/MainWin32.cpp (needs MSVC's __argc /
    # __argv CRT globals) depend on; propagating it broke Windows CI
    # builds. _USE_MATH_DEFINES is required BEFORE any #include <cmath>
    # for the POSIX-style `M_*` constants (used by include/dsp_types.hpp
    # at lines 366/372/383/389 for M_SQRT2) to be visible under MSVC —
    # without it those translates fail with `error C2065: 'M_SQRT2':
    # undeclared identifier`. Defining it via /D (this target_compile_
    # definitions call) lets it take effect before the first preprocessor
    # pass, so any include order is fine.
    target_compile_definitions(CapstanVar PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
endif()

target_include_directories(CapstanVar PRIVATE
    include/
    ${imgui_SOURCE_DIR}
    ${imgui_sfml_SOURCE_DIR}
)

target_link_libraries(CapstanVar PRIVATE
    ImGui-SFML::ImGui-SFML
    sfml-graphics
    sfml-window
    sfml-system
    nlohmann_json::nlohmann_json
    SndFile::sndfile
    Opus::opus
    opusfile::opusfile
    ${AUDIO_LIBS}
    Threads::Threads
)

if(WIN32)
    set_target_properties(CapstanVar PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(CapstanVar PRIVATE ole32 comdlg32)
endif()

install(TARGETS CapstanVar DESTINATION bin)

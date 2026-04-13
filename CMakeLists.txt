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

# ── Build options ─────────────────────────────────────────────────────────────
option(CAPSTANVAR_BUILD_EXECUTABLE "Build the standalone application" ON)
option(CAPSTANVAR_BUILD_LIBRARY    "Build as a reusable library" OFF)

if(NOT CAPSTANVAR_BUILD_EXECUTABLE AND NOT CAPSTANVAR_BUILD_LIBRARY)
    message(FATAL_ERROR "At least one of CAPSTANVAR_BUILD_EXECUTABLE or CAPSTANVAR_BUILD_LIBRARY must be enabled")
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /W4 /MP /arch:AVX2)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-O3 -march=native -Wall -Wextra -ffast-math)
endif()

# ── FetchContent ──────────────────────────────────────────────────────────────
include(FetchContent)

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
)

# Dear ImGui 1.89.9 (compatible with imgui-sfml 2.6.x)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.89.9
    GIT_SHALLOW    TRUE
)

# ImGui-SFML — pinned to exact commit on 2.6.x branch (SFML 2 compatible)
set(IMGUI_SFML_FIND_SFML    OFF CACHE BOOL "" FORCE)
set(IMGUI_SFML_IMGUI_TARGET imgui CACHE STRING "" FORCE)
FetchContent_Declare(imgui_sfml
    GIT_REPOSITORY https://github.com/SFML/imgui-sfml.git
    GIT_TAG        64e09feda29266572799e5ded66eac5cb0a749aa
    GIT_SHALLOW    TRUE
)

# nlohmann/json
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# libsndfile
# Enable additional audio codecs: FLAC, Vorbis, Opus, etc.
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK   OFF CACHE BOOL "" FORCE)
set(ENABLE_EXTERNAL_LIBS ON CACHE BOOL "" FORCE)  # Enable FLAC, Vorbis, Opus
set(ENABLE_MPEG ON CACHE BOOL "" FORCE)           # Enable MP3 and other MPEG formats
set(ENABLE_EXPERIMENTAL ON CACHE BOOL "" FORCE)   # Enable experimental codecs

FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
)

# Fetch SFML + imgui first, then imgui-sfml (needs IMGUI_DIR)
FetchContent_MakeAvailable(SFML imgui)
set(IMGUI_DIR ${imgui_SOURCE_DIR})
FetchContent_MakeAvailable(imgui_sfml nlohmann_json libsndfile)

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

# ── Include directories (shared) ──────────────────────────────────────────────
set(CAPSTANVAR_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${imgui_SOURCE_DIR}
    ${imgui_sfml_SOURCE_DIR}
)

# ── Core library sources (DSP engine + audio I/O) ────────────────────────────
set(LIB_SOURCES
    src/engine.cpp
    src/mod_transport.cpp
    src/mod_magnetic.cpp
    src/mod_electronics.cpp
    src/audio_io.cpp
    src/preset_manager.cpp
)

# ── Build the library ─────────────────────────────────────────────────────────
if(CAPSTANVAR_BUILD_LIBRARY)
    add_library(CapstanVarLib STATIC ${LIB_SOURCES})
    add_library(CapstanVar::CapstanVarLib ALIAS CapstanVarLib)
    
    target_compile_definitions(CapstanVarLib PRIVATE ${AUDIO_DEFS})
    target_include_directories(CapstanVarLib PUBLIC 
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${imgui_SOURCE_DIR}
        ${imgui_sfml_SOURCE_DIR}
    )
    
    # Link dependencies
    target_link_libraries(CapstanVarLib PRIVATE
        nlohmann_json::nlohmann_json
        SndFile::sndfile
        ${AUDIO_LIBS}
        pthread
    )
    
    # Set output name and properties
    set_target_properties(CapstanVarLib PROPERTIES
        OUTPUT_NAME CapstanVar
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# ── Build the executable ──────────────────────────────────────────────────────
if(CAPSTANVAR_BUILD_EXECUTABLE)
    set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    )

    set(APP_SOURCES
        src/main.cpp
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

    # If library is also built, link against it; otherwise compile everything together
    if(CAPSTANVAR_BUILD_LIBRARY)
        add_executable(CapstanVar ${APP_SOURCES} ${IMGUI_SOURCES})
        target_link_libraries(CapstanVar PRIVATE
            CapstanVarLib
            ImGui-SFML::ImGui-SFML
            sfml-graphics
            sfml-window
            sfml-system
        )
    else()
        # Standalone build: compile all sources directly into executable
        add_executable(CapstanVar ${APP_SOURCES} ${IMGUI_SOURCES} ${LIB_SOURCES})
        target_compile_definitions(CapstanVar PRIVATE ${AUDIO_DEFS})
        target_include_directories(CapstanVar PRIVATE ${CAPSTANVAR_INCLUDE_DIRS})
        target_link_libraries(CapstanVar PRIVATE
            ImGui-SFML::ImGui-SFML
            sfml-graphics
            sfml-window
            sfml-system
            nlohmann_json::nlohmann_json
            SndFile::sndfile
            ${AUDIO_LIBS}
            pthread
        )
    endif()

    if(WIN32)
        set_target_properties(CapstanVar PROPERTIES WIN32_EXECUTABLE TRUE)
        target_link_libraries(CapstanVar PRIVATE ole32 comdlg32)
    endif()

    install(TARGETS CapstanVar DESTINATION bin)
endif()

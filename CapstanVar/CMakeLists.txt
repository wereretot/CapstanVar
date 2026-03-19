cmake_minimum_required(VERSION 3.20)
project(CapstanVar VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build type default ────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/O2 /W4 /MP /arch:AVX2)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-O3 -march=native -Wall -Wextra -ffast-math)
endif()

# ── Dependencies via FetchContent ─────────────────────────────────────────────
include(FetchContent)

# ── SFML 2.6.2  (must be declared before imgui-sfml) ─────────────────────────
# imgui-sfml 2.6.x branch targets SFML 2; SFML 3 has breaking API changes.
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

# ── Dear ImGui 1.89.9 ─────────────────────────────────────────────────────────
# imgui-sfml 2.6.x was authored against the 1.89 series.
# v1.90+ changed font and IO APIs; pin to last 1.89 release for compatibility.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.89.9
    GIT_SHALLOW    TRUE
)

# ── ImGui-SFML  ("2.6.x" is a branch name, NOT a release tag) ────────────────
# The master branch has moved to SFML 3; 2.6.x is the SFML 2-compatible branch.
set(IMGUI_SFML_FIND_SFML    OFF CACHE BOOL "" FORCE)  # we supply SFML ourselves
set(IMGUI_SFML_IMGUI_TARGET imgui  CACHE STRING "" FORCE)

FetchContent_Declare(imgui_sfml
    GIT_REPOSITORY https://github.com/SFML/imgui-sfml.git
    GIT_TAG        64e09feda29266572799e5ded66eac5cb0a749aa  # tip of 2.6.x branch
    GIT_SHALLOW    TRUE
)

# ── nlohmann/json ─────────────────────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# ── libsndfile ────────────────────────────────────────────────────────────────
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(ENABLE_CPACK   OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libsndfile
    GIT_REPOSITORY https://github.com/libsndfile/libsndfile.git
    GIT_TAG        1.2.2
    GIT_SHALLOW    TRUE
)

# ── PortAudio ─────────────────────────────────────────────────────────────────
set(PA_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PA_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(PA_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(portaudio
    GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
    GIT_TAG        v19.7.0
    GIT_SHALLOW    TRUE
)

# ── Fetch: SFML and imgui first so imgui-sfml can find them ──────────────────
FetchContent_MakeAvailable(SFML imgui)

# imgui-sfml's CMakeLists searches for imgui via IMGUI_DIR
set(IMGUI_DIR ${imgui_SOURCE_DIR})
FetchContent_MakeAvailable(imgui_sfml)

FetchContent_MakeAvailable(nlohmann_json portaudio libsndfile)

# ── ImGui core sources ────────────────────────────────────────────────────────
# imgui-sfml's CMake target (ImGui-SFML::ImGui-SFML) compiles imgui-SFML.cpp
# and links the imgui sources itself, so we only need the four core files here.
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
)

# ── Application sources ───────────────────────────────────────────────────────
set(APP_SOURCES
    src/main.cpp
    src/engine.cpp
    src/mod_transport.cpp
    src/mod_magnetic.cpp
    src/mod_electronics.cpp
    src/audio_io.cpp
    src/preset_manager.cpp
    src/ui.cpp
    src/ui_panels.cpp
    src/render_engine.cpp
)

# tinyfiledialogs — optional single-file native file dialogs
# Download from https://sourceforge.net/projects/tinyfiledialogs/
# and place tinyfiledialogs.h + tinyfiledialogs.c in the project root.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tinyfiledialogs.c")
    list(APPEND APP_SOURCES tinyfiledialogs.c)
    add_compile_definitions(NAGRA_HAS_TFD)
    message(STATUS "tinyfiledialogs found — native file dialogs enabled")
else()
    message(STATUS "tinyfiledialogs NOT found — file open/save will use stdin fallback")
endif()

add_executable(CapstanVar ${APP_SOURCES} ${IMGUI_SOURCES})

target_include_directories(CapstanVar PRIVATE
    include/
    ${imgui_SOURCE_DIR}
    ${imgui_sfml_SOURCE_DIR}
)

target_link_libraries(CapstanVar PRIVATE
    ImGui-SFML::ImGui-SFML      # exports sfml-graphics + sfml-window + sfml-system
    sfml-graphics
    sfml-window
    sfml-system
    nlohmann_json::nlohmann_json
    PortAudio::PortAudio
    SndFile::sndfile
)

# ── Platform specifics ────────────────────────────────────────────────────────
if(WIN32)
    set_target_properties(CapstanVar PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_libraries(CapstanVar PRIVATE winmm ole32 comdlg32)
elseif(APPLE)
    target_link_libraries(CapstanVar PRIVATE
        "-framework CoreAudio" "-framework AudioUnit" "-framework CoreFoundation")
elseif(UNIX)
    target_link_libraries(CapstanVar PRIVATE asound pthread)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS CapstanVar DESTINATION bin)

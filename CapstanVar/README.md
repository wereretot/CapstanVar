# CapstanVar  ·  Analog Forensics  ·  Modular DSP

A physically-modelled analogue tape machine simulator — C++20, fully
cross-platform (Windows / Linux / macOS), no Python runtime required.

---

## Architecture

```
NagraApp (ui.cpp)          ← ImGui + SFML window, all UI panels
    │
    ├── TapeEngine (engine.cpp)
    │       ├── TransportDynamics (mod_transport.cpp)   wow/flutter/inertia/dropouts
    │       ├── MagneticPath      (mod_magnetic.cpp)    saturation/print-through/demag
    │       └── ElectronicComponents (mod_electronics.cpp) hiss/hum/azimuth/head-bump
    │
    ├── AudioIO (audio_io.cpp)     ← PortAudio real-time playback + squeal shuttle
    ├── PresetManager (preset_manager.cpp)  ← built-ins + session + JSON import/export
    └── RenderEngine (render_engine.cpp)    ← offline render, multi-thread, libsndfile
```

### Dependencies (all fetched automatically via CMake FetchContent)

| Library | Purpose |
|---------|---------|
| Dear ImGui | Immediate-mode GUI |
| ImGui-SFML | ImGui ↔ SFML backend |
| SFML 2.6 | Window, events, OpenGL context |
| PortAudio | Real-time audio I/O |
| libsndfile | WAV/FLAC/AIFF read-write |
| nlohmann/json | Preset JSON serialisation |

Plus **tinyfiledialogs** (single `.h` + `.c` file, bundled in project root).

The first CMake configure needs GitHub access while `FetchContent` clones the
dependencies. If configure appears to pause right after compiler feature
detection, it is usually fetching `SFML` first. Re-run CMake with normal
terminal output visible and check for `Dependency SFML: fetching/populating
with FetchContent`.

---

## Build on Linux

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install cmake ninja-build g++ \
     libasound2-dev libx11-dev libxrandr-dev libxcursor-dev \
     libgl-dev libfreetype-dev libudev-dev libpthread-stubs0-dev

# Fedora / RHEL
sudo dnf install cmake ninja-build gcc-c++ \
     alsa-lib-devel libX11-devel libXrandr-devel \
     mesa-libGL-devel freetype-devel systemd-devel
```

### Compile

```bash
cd nagra
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/CapstanVar
```

---

## Build on Windows

### Prerequisites

- **Visual Studio 2022** (Community or later) with "Desktop development with C++" workload
- **CMake 3.20+** (bundled with VS, or from cmake.org)
- **Git** (for FetchContent)

### Compile (Developer Command Prompt)

```bat
cd nagra
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\CapstanVar.exe
```

Or open `build/CapstanVar.sln` in Visual Studio and press **Build → Build Solution**.

### MinGW / MSYS2

```bash
pacman -S mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-gcc mingw-w64-x86_64-portaudio
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build -j$(nproc)
```

---

## Adding tinyfiledialogs

Download from https://sourceforge.net/projects/tinyfiledialogs/ and place:

```
nagra/
  tinyfiledialogs.h
  tinyfiledialogs.c
```

Add to `CMakeLists.txt` `APP_SOURCES`:

```cmake
tinyfiledialogs.c
```

---

## Preset file format (`.ftsp`)

Presets are plain JSON, forward-compatible. All numeric fields are optional
(missing keys fall back to defaults):

```json
{
  "__version__": 1,
  "__app__": "ForensicTapeStudio",
  "__name__": "My Custom Preset",
  "oxide_type": "CrO2",
  "ips_base": 7.5,
  "drive": 1.45,
  "hiss": 0.000125,
  ...
}
```

---

## Key design decisions vs Python original

| Area | Python | C++ |
|------|--------|-----|
| GUI | tkinter (OS widgets) | Dear ImGui (GPU-rendered, portable) |
| Audio | sounddevice / PortAudio | PortAudio directly |
| File I/O | pydub + ffmpeg | libsndfile (no external runtime) |
| DSP | NumPy vectorised arrays | SIMD-friendly scalar loops (-O3 -march=native) |
| Filters | scipy.signal lfilter | Transposed Direct Form II biquads |
| RNG | numpy.random | xorshift64 per-module (deterministic, seeded) |
| Presets | Python dict literals | C++ struct macros → std::vector |
| Threading | Python threads (GIL) | std::thread / std::async (true parallelism) |
| Oversampling | scipy resample_poly | Linear interp upsample (swap for SRC lib) |

---

## Extending

- **Add a new module**: create `include/mod_X.hpp` + `src/mod_X.cpp`, add an
  instance to `TapeEngine`, call from `dsp_process()`.
- **Add a preset**: add a `BEGIN_PRESET / P(...) / END_PRESET` block in
  `preset_manager.cpp::_build_builtins()`.
- **Add a control**: add a field to `EngineParams`, a `_slider()` call in the
  appropriate tab, and use the field in the DSP module.

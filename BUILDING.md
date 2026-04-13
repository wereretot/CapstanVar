# Build Options

CapstanVar can be built as:
- **Standalone application** (executable)
- **Static library** (for integration into other projects)
- **Both** (library + executable that links to it)

## Configuration Flags

Control the build using CMake options:

| Flag | Default | Description |
|------|---------|-------------|
| `CAPSTANVAR_BUILD_EXECUTABLE` | `ON` | Build the standalone GUI application |
| `CAPSTANVAR_BUILD_LIBRARY` | `OFF` | Build the static library for reuse |

## Build Examples

### Executable Only (Default)
```bash
cmake -B build -DCAPSTANVAR_BUILD_EXECUTABLE=ON -DCAPSTANVAR_BUILD_LIBRARY=OFF
cmake --build build
```

### Library Only
```bash
cmake -B build -DCAPSTANVAR_BUILD_EXECUTABLE=OFF -DCAPSTANVAR_BUILD_LIBRARY=ON
cmake --build build
```

### Both Library and Executable
```bash
cmake -B build -DCAPSTANVAR_BUILD_EXECUTABLE=ON -DCAPSTANVAR_BUILD_LIBRARY=ON
cmake --build build
```

## Using the Library

When `CAPSTANVAR_BUILD_LIBRARY=ON`, the build produces `libCapstanVar.a` which contains:
- `TapeEngine` - Core DSP processing engine
- `AudioIO` - Audio I/O and transport controls  
- All modulation modules (transport, magnetic, electronics)
- Preset management

### Linking in Your Project

```cmake
# Option 1: Add as subdirectory
add_subdirectory(CapstanVar)
target_link_libraries(your_target PRIVATE CapstanVar::CapstanVarLib)

# Option 2: Use as installed package
find_package(CapstanVar REQUIRED)
target_link_libraries(your_target PRIVATE CapstanVar::CapstanVarLib)
```

### Basic Usage

```cpp
#include <engine.hpp>
#include <audio_io.hpp>

// Create engine and load audio file
TapeEngine engine;
engine.load_file("tape_source.wav");

// Create audio I/O
AudioIO audio(engine);
audio.open();

// Start playback
audio.play_forward();

// Modify parameters in realtime
{
    std::lock_guard<std::mutex> lock(engine.lock);
    engine.params.wow_dep = 0.5f;
    engine.params flutter_dep = 0.1f;
    engine.params.drive = 1.5f;
}

// Stop and cleanup
audio.stop();
audio.close();
```

## Build Artifacts

- **Executable**: `build/CapstanVar`
- **Library**: `build/libCapstanVar.a`
- **Headers**: `include/*.hpp`

## Notes

- The library is compiled with `-fPIC` for use in shared libraries
- Audio backend is automatically selected (PortAudio > ALSA > CoreAudio > WinMM)
- All DSP code requires C++20 and SSE/AVX optimizations (`-march=native`)

# CapstanVar - Quick Build Reference

## Build the standalone app (default)
```bash
cmake -B build && cmake --build build
./build/CapstanVar
```

## Build as library for use in other projects
```bash
cmake -B build -DCAPSTANVAR_BUILD_LIBRARY=ON -DCAPSTANVAR_BUILD_EXECUTABLE=OFF
cmake --build build
# Output: build/libCapstanVar.a
```

## Build both library and app
```bash
cmake -B build -DCAPSTANVAR_BUILD_LIBRARY=ON -DCAPSTANVAR_BUILD_EXECUTABLE=ON
cmake --build build
# Outputs: build/libCapstanVar.a + build/CapstanVar
```

## Link the library in your project
```cmake
add_subdirectory(CapstanVar)
target_link_libraries(your_app PRIVATE CapstanVar::CapstanVarLib)
```

See `BUILDING.md` for complete documentation.

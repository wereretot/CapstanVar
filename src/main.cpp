#include "ui.hpp"
#include "mod_environment.hpp"
#include "preset_manager.hpp"
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <string>

#ifdef _WIN32
#include <windows.h>
// MSVC (and ucrt-based MinGW) expose __argc / __argv globals from the CRT,
// so a WinMain entry point can still inspect command-line arguments without
// having to call CommandLineToArgvW. On non-Windows we read argc/argv in the
// usual way.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int   argc = __argc;
    char** argv = __argv;
#else
int main(int argc, char** argv) {
#endif
    std::set_new_handler([] {
        std::fputs("[FATAL] Out of memory\n", stderr);
        std::fflush(stderr);
        std::set_new_handler(nullptr);
    });
    try {
        // --verify : headless self-test mode used by CI (GitHub Actions
        // `CapstanVar[.exe] --verify`). Skips AppDirs, SFML window, and
        // CapstanApp construction entirely — runs the three in-process
        // numerical audits (EnvironmentModule ref-invariant, env Hot
        // Attic worked example, PresetManager storage-preset round-trip)
        // and exits 0 iff all PASS. Use of stderr-only output keeps the
        // signal easy to grep in CI logs without a stdout/stderr split.
        const bool verify_mode =
            (argc >= 2) && (std::strcmp(argv[1], "--verify") == 0);
        if (verify_mode) {
            int failures = 0;
            auto run = [&](const char* name, bool ok) {
                std::fprintf(stderr, "[--verify] %s %s\n",
                             ok ? "PASS" : "FAIL", name);
                if (!ok) ++failures;
            };
            // Phase 5 — reference-state audibility-floor audit.
            // At T_c=20 °C / RH_pct=50 % / α=1.0 / age=0 the env module's
            // effective_p must equal base_p within numerical tolerance.
            run("verify_reference_invariant",
                EnvironmentModule::verify_reference_invariant());

            // Phase 5b — aggressive non-reference Hot Attic worked example.
            // Validates temp/hum/alpha math, thermal offsets, multiplicative
            // bias drift, and IMMUNE-field passthrough at α=10000.
            run("verify_hot_attic_worked_example",
                EnvironmentModule::verify_hot_attic_worked_example());

            // Phase 5c — round-trip smoke test for the 4 storage presets.
            // Mirrors storage_profiles() in include/mod_environment.hpp.
            run("verify_storage_preset_round_trip",
                PresetManager::verify_storage_preset_round_trip());

            std::fprintf(stderr, "[--verify] %d failure(s)\n", failures);
            return failures == 0 ? 0 : 1;
        }

        // Normal interactive startup. Same three audits fire here so a
        // crash/misbehaviour is caught at first launch too, not only on CI.
        EnvironmentModule::verify_reference_invariant();
        EnvironmentModule::verify_hot_attic_worked_example();
        PresetManager::verify_storage_preset_round_trip();

        CapstanApp app;
        app.run();
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[CRASH] %s\n", e.what());
        return 1;
    }
    return 0;
}

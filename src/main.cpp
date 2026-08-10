#include "ui.hpp"
#include "mod_environment.hpp"
#include <cstdio>
#include <exception>
#include <new>

#ifdef _WIN32
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    std::set_new_handler([] {
        std::fputs("[FATAL] Out of memory\n", stderr);
        std::fflush(stderr);
        std::set_new_handler(nullptr);
    });
    try {
        // Phase 5 — Environment & aging reference-state sanity check.
        // At T_c=20 °C / RH_pct=50 % / α=1.0 / age=0 the env module's
        // effective_p must equal base_p within numerical tolerance.
        // Run once at startup; cheap (<1ms, single block) and self-contained.
        // See docs/TAPE_PHYSICS_REFACTOR.md §X "Audibility preservation".
        EnvironmentModule::verify_reference_invariant();

        // Phase 5b — Hot Attic worked-example audit at α=10000. The
        // reference audit proves the audibility floor; this audit
        // proves the AGGRESSIVE non-reference operating point still
        // follows the documented formulas (temp/hum/alpha math,
        // thermal offsets, multiplicative bias drift, IMMUNE-field
        // passthrough). See docs/TAPE_PHYSICS_REFACTOR.md §X
        // "Phase 5 worked example" and the new
        // verify_hot_attic_worked_example() doc-comment.
        EnvironmentModule::verify_hot_attic_worked_example();

        // Phase 5c — Round-trip smoke test for the 4 storage presets
        // (Controlled / Consumer Closet / Hot Attic / Cold Warehouse)
        // added to PresetManager::_build_builtins(). Mirrors the
        // storage_profiles() catalog in include/mod_environment.hpp
        // and verifies each preset's env_* axes round-trip through
        // ep_to_json / ep_from_json, while env_age_acceleration /
        // env_age_seconds / env_tape_health / env_failure_modes
        // stay at struct defaults. See docs/PRESET_SCHEMA.md
        // "Phase 5 storage presets" and the doc-comment on
        // PresetManager::verify_storage_preset_round_trip() in
        // include/preset_manager.hpp.
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

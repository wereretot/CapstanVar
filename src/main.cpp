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

        CapstanApp app;
        app.run();
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[CRASH] %s\n", e.what());
        return 1;
    }
    return 0;
}

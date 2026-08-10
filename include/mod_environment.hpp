#pragma once
// ── mod_environment — Phase 5 ─────────────────────────────────────────────────
// EnvironmentModule sits at the top of the DSP chain (when wired in
// TapeEngine::dsp_process in a follow-up commit). It accumulates a
// wall-clock-derived environmental age for the simulated tape, derives
// per-field damage from temperature and humidity, and returns an
// "effective_p" EngineParams that downstream modules consume instead of
// the user's base_p.
//
// Composition rule is Option (a) instant-override preservation:
//   effective_p.x = max(base_p.x, env_derived_x)             (damage floors)
//   effective_p.bias *= max(0.5, 1.0 - (T_c - 20) * 0.005)    (multiplicative)
// plus thermal offsets, with eq_curve / format_id / oxide_type /
// input_gain marked IMMUNE (passed through verbatim).
//
// At the reference state (T_c=20 °C, RH_pct=50 %, α=1.0, age=0) every
// env-derived term evaluates to zero and effective_p equals base_p
// within numerical tolerance. That invariant is the audibility-
// preservation guarantee and is asserted by
// EnvironmentModule::verify_reference_invariant(); main.cpp calls it
// once at startup.
//
// See docs/TAPE_PHYSICS_REFACTOR.md §X and docs/PRESET_SCHEMA.md for
// the full rationale: per-field formula table, the kEnvHalfLifeYears
// pinning policy, the failure-mode catalog, and the audibility-
// preservation 5-condition list.
#include "dsp_types.hpp"

namespace env_constants {
// kEnvHalfLifeYears is pinned at compile time per the Phase 5 design
// (see docs/TAPE_PHYSICS_REFACTOR.md §X "half_life" note). Keeping the
// rate of env_tape_health decay a single named constant lets users
// repro the audibility floor deterministically across builds.
//
// If a future Phase-N needs per-preset half-life calibration, expose
// env_half_life_yr with an explicit __version__ bump to 3 under the
// audibility-preservation policy. Until then, this constant governs.
inline constexpr float kEnvHalfLifeYears = 10.0f;
// Seconds per sim-year. Julian year (365.25 d × 86400 s) for fidelity
// with the formula derivations (which treat age as sim-years).
inline constexpr double kSecondsPerYear = 365.25 * 86400.0;
} // namespace env_constants

// Phase 5 — Storage-profile catalog. Single source of truth for the
// four built-in storage environments surfaced in src/ui.cpp's
// Environment tab. Each profile pins only the static environment axes
// (temperature / humidity); env_age_acceleration stays user-driven and
// the persisted wear fields (env_age_seconds, env_tape_health,
// env_failure_modes) are NOT reset by the storage-preset selection —
// see docs/PRESET_SCHEMA.md "Phase 5 storage presets" rationale.
//
// Phase 5c mirrors these as `.cvpr` files via PresetManager. The UI
// reads storage_profiles() directly so the tab works before Phase 5c
// lands (and so the static data here stays the audit source).
struct StorageProfile {
    const char* id;                 // stable key (e.g. "HotAttic")
    const char* display_name;       // human-readable label
    float       env_temperature_c;  // °C,  [0..60]
    float       env_humidity_pct;   // RH%, [0..100]
    const char* description;        // tooltip text in the Environment tab
};
inline const std::vector<StorageProfile>& storage_profiles() {
    static const std::vector<StorageProfile> P = {
        {"Controlled",
         "Controlled (climate-controlled vault)",
         20.0f, 50.0f,
         "20 °C / 50% RH \u2014 reference state; env module produces zero damage."},
        {"ConsumerCloset",
         "Consumer Closet (typical bedroom)",
         25.0f, 60.0f,
         "25 °C / 60% RH \u2014 mild; hiss climbs after ~3 sim-years, dropout creeps after ~5."},
        {"HotAttic",
         "Hot Attic (abandoned for years)",
         35.0f, 70.0f,
         "35 °C / 70% RH \u2014 aggressive; sticky shed audible within ~1 sim-year; mould risk."},
        {"ColdWarehouse",
         "Cold Warehouse (unheated storage)",
         10.0f, 40.0f,
         "10 °C / 40% RH \u2014 below reference; ageing axis dormant, only thermal wow/flutter drift."},
    };
    return P;
}

class EnvironmentModule {
public:
    EnvironmentModule() = default;

    // Apply Option (a) modulation for one audio block of `frames` samples.
    // Mutates the persisted env_* fields on `p`:
    //   - env_age_seconds  += Δt × α × temp_rate × hum_rate
    //   - env_tape_health   = exp(-age_years / kEnvHalfLifeYears)
    // Returns the composed effective_p (a copy of `p` with the per-field
    // damage, thermal offsets, and bias/head_bump modulation applied).
    // Downstream modules in the DSP chain should consume the returned
    // effective_p instead of `p` so that PRESERVATION holds.
    //
    // At reference state, returned effective_p equals `p` within
    // ε = ~1e-6 (numerical tolerance); see verify_reference_invariant.
    //
    // ── Thread safety ────────────────────────────────────────────────────────
    // Engine writes params under mutex; dsp_process reads it via const&.
    // This routine mutates p.env_age_seconds / p.env_tape_health DIRECTLY
    // (no internal lock). env_failure_modes is also persisted on `p` and
    // belongs to the same concurrency domain — mutated by the UI's
    // TRIGGER BREAK path on the GUI thread, read by the audio thread.
    // All three persisted env_* fields share engine.lock as their
    // sequencing point. When wired into the audio thread process() MUST
    // be invoked either under engine.lock OR with a local copy of `p`
    // — otherwise the GUI thread or a TRIGGER BREAK could observe
    // torn writes mid-block. main.cpp's startup audit is
    // single-threaded and safe.
    EngineParams process(EngineParams& p, int frames);

    // One-shot reference-state sanity audit for CI / startup hooks.
    // Builds a reference-state EngineParams (T_c=20, RH_pct=50, α=1,
    // age=0), runs process() over BLOCK_SIZE frames, and verifies the
    // returned effective_p equals the input base_p field-by-field
    // within `eps`. Prints diagnostic on stderr (PASS / FAIL with the
    // first divergent field). Returns true on PASS.
    //
    // Safe to call once at startup; cheap (one process + one comparison
    // pass) and self-contained. stderr-only output simplifies CI
    // capture (some pipelines only capture stderr; mixing stdout for
    // PASS loses the signal).
    static bool verify_reference_invariant(float eps = 1e-5f);
};

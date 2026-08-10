#include "mod_environment.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

using env_constants::kEnvHalfLifeYears;
using env_constants::kSecondsPerYear;

// Phase 5 option (a) instant-override preservation. Reads temperature/
// humidity/age axes from `p`, accumulates env_age_seconds, recomputes
// env_tape_health, and returns an effective_p that downstream modules
// consume in place of `p`. The persisted env_* fields on `p` are mutated
// in-place so the caller (e.g. TapeEngine::dsp_process) sees the
// updated wear in the live UI state.
//
// At reference state (T_c=20, RH_pct=50, α=1, age=0) every env-derived
// term is zero, env_age_seconds is unchanged (hum_rate==0 short-
// circuits accumulation), and the returned effective_p matches `p`
// within numerical tolerance. The static audit
// EnvironmentModule::verify_reference_invariant() exercises this.
//
// Method name `process()` keeps consistency with existing modules
// (MagneticPath::process, ElectronicComponents::process); the
// semantics differ (this one mutates persisted env_* fields on `p`)
// but the call-site vocabulary matches the project convention.
//
// See:
//   docs/TAPE_PHYSICS_REFACTOR.md §X "Phase 5 aging math"
//   docs/TAPE_PHYSICS_REFACTOR.md §X "Phase 5 per-field modulation"
//   docs/PRESET_SCHEMA.md        §"Option (a) instant-override"
EngineParams EnvironmentModule::process(EngineParams& p, int frames) {
    // Shortcut: at reference state (and any state where hum_rate==0)
    // the accumulation term is zero and the per-field damage floors are
    // zero. Without thermal offsets above reference the result equals
    // `p` entirely. We still always run the full path so that any
    // future formula extension is captured uniformly.
    const float    T_c    = p.env_temperature_c;
    const float    RH_pct = p.env_humidity_pct;
    const float    alpha  = p.env_age_acceleration;
    constexpr float T_ref = 20.0f;

    // ── Aging math ───────────────────────────────────────────────────────────
    // Arrhenius-style doubling every 10°C above reference. At T_c == 20
    // this evaluates to exactly 1.0 (preserves audibility: see docs §X
    // "Audibility verification hinge").
    const float temp_rate = std::pow(2.0f, (T_c - T_ref) / 10.0f);

    // RH rate: linear penalty above 50%, quadratic surcharge above 70%.
    // At RH_pct == 50 the first term is 0; below 50% both terms clamp to
    // 0. Below 50% RH means the ageing axis is dormant (see the
    // conditional in sched — Cold Warehouse storage preset).
    const float hum_rate = std::max(0.0f, (RH_pct - 50.0f) / 50.0f)
                         + std::pow(std::max(0.0f, (RH_pct - 70.0f) / 30.0f), 2.0f);

    // Δt_block is wall-clock seconds of audio playback covered by one
    // block. age_acceleration multiplies wall-clock into sim-seconds
    // (α=1000 ⇒ 1000 sim-sec per wall-clock second; see docs §X worked
    // example).
    const double dt_block     = static_cast<double>(frames) * (1.0 / static_cast<double>(SR));
    const double accum_now    = dt_block
                              * static_cast<double>(alpha)
                              * static_cast<double>(temp_rate)
                              * static_cast<double>(hum_rate);

    // Persisted fields: mutate the caller's struct so the live UI reads
    // updated wear. Effective_p (built below) mirrors these.
    p.env_age_seconds  += accum_now;
    const float age_years = static_cast<float>(p.env_age_seconds / kSecondsPerYear);
    p.env_tape_health    = std::exp(-age_years / kEnvHalfLifeYears);

    // ── Per-field modulation (Option a) ──────────────────────────────────────
    // Compose a copy and apply damage floors / thermal offsets /
    // multiplicative drift. IMMUNE fields pass through at the bottom.
    EngineParams eff = p;

    // Damage floors — env_derived is monotonic in age, humidity, and
    // |T_c - 20|. max() preserves any hand-tuned base value.
    eff.hiss            = std::max(p.hiss,
                                   0.01f * (std::exp(age_years / 20.0f) - 1.0f));
    eff.dropout_rate    = std::max(p.dropout_rate,
                                   std::min(0.5f, age_years / 5.0f));
    eff.oxide_shedding  = std::max(p.oxide_shedding,
                                   std::min(0.3f, age_years / 8.0f * hum_rate));
    eff.sticky_shed     = std::max(p.sticky_shed,
                                   std::min(1.0f,
                                            std::exp((T_c - T_ref) / 8.0f) *
                                            std::max(0.0f, hum_rate) *
                                            (age_years / 2.0f)));
    eff.mains_hum       = std::max(p.mains_hum,
                                   0.005f * std::exp((T_c - T_ref) / 10.0f) *
                                   hum_rate * (age_years / 10.0f));
    eff.demagnetization = std::max(p.demagnetization,
                                   std::min(0.5f,
                                            std::exp((T_c - T_ref) / 5.0f) *
                                            age_years));
    eff.print_through   = std::max(p.print_through,
                                   std::min(0.05f, age_years / 20.0f));
    eff.azimuth_drift   = std::max(p.azimuth_drift,
                                   std::min(0.5f, (age_years / 15.0f) * temp_rate));

    // Thermal offsets — additive. At reference |T_c - 20| == 0 so both
    // contributions vanish and the assignment is a no-op additive floor.
    const float thermal_offset = std::fabs(T_c - T_ref);
    eff.wow_dep     = p.wow_dep     + thermal_offset * 0.05f;
    eff.flutter_dep = p.flutter_dep + thermal_offset * 0.05f;

    // tension_load: parenthesised ((max(0, T_c-20)/100)²). Below
    // reference both terms clamp to zero (cold has no contrib, per
    // docs §X "Mechanical creep only" note for Cold Warehouse).
    {
        const float tl_numer  = std::max(0.0f, T_c - T_ref);
        const float tl_factor = tl_numer / 100.0f;
        eff.tension_load      = p.tension_load + tl_factor * tl_factor;
    }

    // Multiplicative drift: bias shrinks above reference (factor < 1.0),
    // clamped to 0.5 to prevent runaway on extreme high temps. Below
    // reference the bias factor exceeds 1.0 — to keep audibility
    // preserving on the maximum side as well, clamp at the same floor.
    {
        const float bias_factor = std::max(0.5f, 1.0f - (T_c - T_ref) * 0.005f);
        eff.bias = p.bias * bias_factor;
    }

    // head_bump softens as tape dies. At reference env_tape_health==1.0,
    // so the multiplier is identity and the field is preserved.
    eff.head_bump = p.head_bump * p.env_tape_health;

    // IMMUNE fields — passed through verbatim. These preserve audibility
    // for users who tune outside the env module's reach.
    eff.eq_curve   = p.eq_curve;
    eff.format_id  = p.format_id;
    eff.oxide_type = p.oxide_type;
    eff.input_gain = p.input_gain;

    // Mirror the persisted env_* read/write fields onto eff so downstream
    // modules see the same values that process() just wrote into `p`.
    eff.env_temperature_c    = p.env_temperature_c;
    eff.env_humidity_pct     = p.env_humidity_pct;
    eff.env_age_acceleration = p.env_age_acceleration;
    eff.env_age_seconds      = p.env_age_seconds;
    eff.env_tape_health      = p.env_tape_health;
    eff.env_failure_modes    = p.env_failure_modes;

    return eff;
}

// Reference-state sanity audit. Builds the reference-state EngineParams
// (whose struct field initialisers already encode T_c=20, RH_pct=50,
// α=1.0, age=0 by default), runs one block of process(), and compares
// the returned effective_p against the input base_p field-by-field
// within eps. At reference state the math guarantees every env-derived
// term is zero, so any divergence indicates a broken audibility-
// preservation guarantee.
//
// Returns true on PASS; prints diagnostic on FAIL. Both PASS and FAIL
// messages go to stderr so CI pipelines that capture only stderr (and
// only those that capture stdout) consistently receive the verdict.
// Cheap enough to call once at startup (one process() over BLOCK_SIZE
// samples; < 1ms).
static bool field_invariant_ok(const EngineParams& eff,
                               const EngineParams& base,
                               float eps) {
    // Convert eps once to double so the comparisons inside the lambdas
    // don't rely on implicit float->double promotion.
    const double eps_d = static_cast<double>(eps);
    auto approx_eq_f = [eps_d](float a, float b) {
        return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= eps_d;
    };
    auto approx_eq_d = [eps_d](double a, double b) {
        return std::fabs(a - b) <= eps_d;
    };
    if (!approx_eq_f(eff.dropout_rate,    base.dropout_rate))    return false;
    if (!approx_eq_f(eff.oxide_shedding,  base.oxide_shedding))  return false;
    if (!approx_eq_f(eff.sticky_shed,     base.sticky_shed))     return false;
    if (!approx_eq_f(eff.mains_hum,       base.mains_hum))       return false;
    if (!approx_eq_f(eff.demagnetization, base.demagnetization)) return false;
    if (!approx_eq_f(eff.print_through,   base.print_through))   return false;
    if (!approx_eq_f(eff.azimuth_drift,   base.azimuth_drift))   return false;
    if (!approx_eq_f(eff.hiss,            base.hiss))            return false;
    if (!approx_eq_f(eff.wow_dep,         base.wow_dep))         return false;
    if (!approx_eq_f(eff.flutter_dep,     base.flutter_dep))     return false;
    if (!approx_eq_f(eff.tension_load,    base.tension_load))    return false;
    if (!approx_eq_f(eff.bias,            base.bias))            return false;
    if (!approx_eq_f(eff.head_bump,       base.head_bump))       return false;
    if (eff.eq_curve   != base.eq_curve)             return false;
    if (eff.format_id  != base.format_id)            return false;
    if (eff.oxide_type != base.oxide_type)           return false;
    if (!approx_eq_f(eff.input_gain,     base.input_gain))      return false;
    if (!approx_eq_d(eff.env_age_seconds,base.env_age_seconds)) return false;
    if (!approx_eq_f(eff.env_tape_health,base.env_tape_health))return false;
    if (eff.env_failure_modes != base.env_failure_modes) return false;
    return true;
}

bool EnvironmentModule::verify_reference_invariant(float eps) {
    EngineParams base{};   // struct defaults match doc reference state
    EnvironmentModule mod;

    // Take a fresh copy of `base` so process()'s in-place mutation of
    // env_age_seconds / env_tape_health doesn't bleed into our
    // comparison baseline.
    EngineParams input = base;
    const EngineParams eff = mod.process(input, BLOCK_SIZE);

    const bool ok = field_invariant_ok(eff, base, eps);

    if (ok) {
        std::fprintf(stderr,
                     "[mod_environment] PASS: reference-state invariant holds "
                     "(effective_p == base_p within eps=%.1e over %d frames).\n",
                     static_cast<double>(eps), BLOCK_SIZE);
        std::fflush(stderr);
        return true;
    }

    // FAIL diagnostic: print every divergent field so the implementer can
    // immediately see which formula regressed. Each float arg is pre-
    // cast to double so the format specifiers match strictly
    // (avoids -Wformat complaints on stricter warning levels).
    std::fprintf(stderr,
                 "[mod_environment] FAIL: reference-state invariant broken (eps=%.1e):\n"
                 "  dropout_rate    base=%.6f  eff=%.6f\n"
                 "  oxide_shedding  base=%.6f  eff=%.6f\n"
                 "  sticky_shed     base=%.6f  eff=%.6f\n"
                 "  mains_hum       base=%.6f  eff=%.6f\n"
                 "  demagnetization base=%.6f  eff=%.6f\n"
                 "  print_through   base=%.6f  eff=%.6f\n"
                 "  azimuth_drift   base=%.6f  eff=%.6f\n"
                 "  hiss            base=%.6f  eff=%.6f\n"
                 "  wow_dep         base=%.6f  eff=%.6f\n"
                 "  flutter_dep     base=%.6f  eff=%.6f\n"
                 "  tension_load    base=%.6f  eff=%.6f\n"
                 "  bias            base=%.6f  eff=%.6f\n"
                 "  head_bump       base=%.6f  eff=%.6f\n"
                 "  input_gain      base=%.6f  eff=%.6f\n"
                 "  env_age_seconds base=%.6f  eff=%.6f\n"
                 "  env_tape_health base=%.6f  eff=%.6f\n"
                 "  env_failure_modes base=%u  eff=%u\n",
                 static_cast<double>(eps),
                 base.dropout_rate,    eff.dropout_rate,
                 base.oxide_shedding,  eff.oxide_shedding,
                 base.sticky_shed,     eff.sticky_shed,
                 base.mains_hum,       eff.mains_hum,
                 base.demagnetization, eff.demagnetization,
                 base.print_through,   eff.print_through,
                 base.azimuth_drift,   eff.azimuth_drift,
                 base.hiss,            eff.hiss,
                 base.wow_dep,         eff.wow_dep,
                 base.flutter_dep,     eff.flutter_dep,
                 base.tension_load,    eff.tension_load,
                 base.bias,            eff.bias,
                 base.head_bump,       eff.head_bump,
                 base.input_gain,      eff.input_gain,
                 base.env_age_seconds, eff.env_age_seconds,
                 base.env_tape_health, eff.env_tape_health,
                 base.env_failure_modes, eff.env_failure_modes);
    std::fflush(stderr);
    return false;
}
// Phase 5b — non-reference worked-example audit at Hot Attic (T=35,
// RH=70, alpha=10000). The reference-state audit above exercises the
// audibility-preservation floor; this audit exercises the AGGRESSIVE
// non-reference operating point documented in
// docs/TAPE_PHYSICS_REFACTOR.md §X "Phase 5 worked example" so a
// regression in temp/hum/alpha math is caught at startup.
//
// Per-block derivations (the script in §X):
//   dt_block     = BLOCK_SIZE / SR                    ~ 0.02322   s
//   temp_rate    = pow(2, (35-20)/10)                 = 2^1.5 ~ 2.8284
//   hum_rate     = (70-50)/50 + pow(max(0,(70-70)/30), 2)  = 0.4 + 0 = 0.4
//   accum_now    = dt_block * 10000 * 2.8284 * 0.4    ~ 262.7     s
//   age_years    = accum_now / kSecondsPerYear        ~ 8.324e-6  yr
//   tape_health  = exp(-age_years/10)                 ~ 0.999999167
//   bias factor  = max(0.5, 1 - (35-20)*0.005)        = 0.925
//   wow/flutter  = base + (35-20)*0.05 = base + 0.75
//   tension_load = base + ((max(0, 35-20)/100)^2)     = base + 0.0225
// Harmonic contraction stays precise at Hot Attic: hum_rate's quadratic
// penalty kicks in at RH>=70 so the (70-70)/30 term is exactly zero —
// hum_rate is the simple linear 0.4.
//
// The four IMMUNE fields (eq_curve, format_id, oxide_type, input_gain)
// and the failure-mode bit-set are passthroughs; assertions below
// verify that contract.
bool EnvironmentModule::verify_hot_attic_worked_example() {
    // Phase 5b — non-reference worked-example audit at Hot Attic
    // (T=35, RH=70, alpha=10000). The reference-state audit verifies
    // the audibility floor; this audit exercises the AGGRESSIVE
    // non-reference operating point documented in
    // docs/TAPE_PHYSICS_REFACTOR.md §X "Phase 5 worked example" so a
    // regression in temp/hum/alpha math is caught at startup.
    //
    // Per-block derivations (script in §X):
    //   dt_block    = BLOCK_SIZE / SR               ~ 0.02322 s
    //   temp_rate   = 2^((35-20)/10)                ~ 2.828427
    //   hum_rate    = (70-50)/50 + 0^2              = 0.4
    //   accum_now   = dt_block * 10000 * 2.8284 * 0.4 ~ 262.7 s
    //   age_years   = accum_now / kSecondsPerYear  ~ 8.32e-6 yr
    //   tape_health = exp(-age_years / 10)         ~ 0.999999167
    //   bias factor = max(0.5, 1 - (35-20)*0.005)  =  0.925
    //   thermal     = |35-20| * 0.05               =  0.75
    //   tension     = base + ((max(0, 35-20)/100)^2) = base + 0.0225
    //
    // Tightened bias-factor check (Phase 5b reviewer note 1): the band
    // is now (0.924, 0.926) — ±0.001 around 0.925, catches coefficient
    // drift in the 0.005/°C multiplier. Reviewer note 4: the bias-factor
    // test also verifies the multiplicative reduction by direct
    // computation against `base.bias * (1.0f - 15.0f * 0.005f)` so a
    // future change to the bias formula moves the audit's expected
    // value along with it.
    //
    // TODO(Phase 6+): add a multi-block tape_health decay audit that
    // runs enough blocks at Hot Attic α=10000 to drop
    // env_tape_health below 0.5, then verifies the resulting age_years
    // matches `ln(2) * kEnvHalfLifeYears` (reviewer note 2; deferred
    // because 1 block keeps tape_health ≈ 0.999999167, so the
    // exponential decay contract only surfaces after hundreds of
    // blocks). Reference doc: docs/TAPE_PHYSICS_REFACTOR.md §X
    // "Phase 5 worked example".
    EngineParams base{};
    base.env_temperature_c     = 35.0f;
    base.env_humidity_pct      = 70.0f;
    base.env_age_acceleration  = 10000.0f;
    // Leave age/health/failure_modes at struct defaults (0/1/0).

    EnvironmentModule mod;
    EngineParams input = base;
    const EngineParams eff = mod.process(input, BLOCK_SIZE);

    // Tolerance guards. Loosen bias_factor band slightly (0.924-0.926)
    // than direct-float computation to absorb accumulated-rounding
    // noise from the multiplication `base.bias * bias_factor`.
    const double  age_tol_s       = 0.5;
    const double  health_tol      = 1.0e-7;
    const float   bias_factor_lo  = 0.924f;        // tightened (reviewer note 1)
    const float   bias_factor_hi  = 0.926f;        // tightened (reviewer note 1)
    const float   bias_factor_ref = base.bias * (1.0f - 15.0f * 0.005f); // reviewer note 4
    const float   bias_tol        = 1.0e-4f;
    const float   thermal_offset  = 0.75f;         // |35-20| * 0.05
    const float   thermal_tol     = 1.0e-4f;
    const float   tension_term    = 0.15f * 0.15f / (100.0f * 100.0f);  // (15/100)²
    const float   tension_tol     = 1.0e-5f;

    bool ok = true;

    // 1. env_age_seconds strictly positive AND within 0.5 s of 262.7.
    if (!(eff.env_age_seconds > 0.0)) ok = false;
    if (std::fabs(eff.env_age_seconds - 262.7) > age_tol_s) ok = false;

    // 2. env_tape_health ≈ 0.999999167 within 1e-7.
    if (std::fabs(static_cast<double>(eff.env_tape_health) - 0.999999167)
        > health_tol) ok = false;

    // 3. eff.bias factors strictly toward 0.925 of base.bias,
    //    band-tightened (0.924, 0.926) — reviewer note 1.
    {
        const float ratio = eff.bias / base.bias;
        if (!(ratio > bias_factor_lo && ratio < bias_factor_hi)) ok = false;
    }
    // 3b. SELF-CORRECTING direct-computation witness (reviewer
    //     note 4). `bias_factor_ref` is the literal expected
    //     multiplier `base.bias * (1 - 15 * 0.005)`; if a future
    //     Phase-N changes the bias formula, this assertion moves its
    //     expected value along with it (follows the formula). Note:
    //     this is NOT a duplicate of the `bias_factor_lo`/`bias_factor_hi`
    //     band assertion above — keep both.
    if (std::fabs(eff.bias - bias_factor_ref) > bias_tol) ok = false;

    // 4. Monotone damage floor: hiss never decreases.
    if (!(eff.hiss >= base.hiss)) ok = false;

    // 5. Thermal offsets for wow/flutter land within 1e-4 of +0.75.
    if (std::fabs(eff.wow_dep     - (base.wow_dep     + thermal_offset)) > thermal_tol) ok = false;
    if (std::fabs(eff.flutter_dep - (base.flutter_dep + thermal_offset)) > thermal_tol) ok = false;

    // 6. tension_load gains (15/100)² = 0.0225 above base.
    if (std::fabs(eff.tension_load - (base.tension_load + tension_term)) > tension_tol) ok = false;

    // 7. IMMUNE fields — strict equality (no env_derived overlay).
    if (eff.eq_curve    != base.eq_curve)    ok = false;
    if (eff.format_id   != base.format_id)   ok = false;
    if (eff.oxide_type  != base.oxide_type)  ok = false;
    if (eff.input_gain  != base.input_gain)  ok = false;

    // 8. failure-mode bit-set is a passthrough (process() never sets
    //    bits — that's the GUI's TRIGGER BREAK responsibility).
    if (eff.env_failure_modes != base.env_failure_modes) ok = false;

    // 9. Persisted env_* axes mirrored verbatim (sanity by construction).
    if (eff.env_temperature_c    != base.env_temperature_c)    ok = false;
    if (eff.env_humidity_pct     != base.env_humidity_pct)     ok = false;
    if (eff.env_age_acceleration != base.env_age_acceleration) ok = false;

    if (ok) {
        std::fprintf(stderr,
                     "[mod_environment] PASS: Hot Attic worked example holds "
                     "(\u03b1=10000 / T_c=35 / RH=70 / %d frames): age=%.4f s (~%.4e yr), "
                     "health=%.9f, bias_factor=%.4f.\n",
                     BLOCK_SIZE,
                     eff.env_age_seconds,
                     static_cast<double>(eff.env_age_seconds) / kSecondsPerYear,
                     static_cast<double>(eff.env_tape_health),
                     static_cast<double>(eff.bias) / static_cast<double>(base.bias));
        std::fflush(stderr);
        return true;
    }

    // FAIL diagnostic: print every divergent scalar so the implementer
    // can spot which formula regressed. Real newlines (no \n escapes).
    std::fprintf(stderr,
                 "[mod_environment] FAIL: Hot Attic worked example regression:\n"
                 "  expected age_seconds     ~262.7 s      actual=%.6f\n"
                 "  expected tape_health     ~0.999999167  actual=%.9f\n"
                 "  expected bias/0.925      ~0.925        actual=%.4f\n"
                 "  expected wow_dep delta   +0.75         actual=%.6f  (base=%.6f)\n"
                 "  expected flutter_dep del +0.75         actual=%.6f  (base=%.6f)\n"
                 "  expected tension delta   +0.0225       actual=%.6f  (base=%.6f)\n"
                 "  hiss monotone            base<=eff     base=%.6f  eff=%.6f\n"
                 "  eq_curve IMMUNE          base==eff     base=%d  eff=%d\n"
                 "  format_id IMMUNE         base==eff     base=\"%s\"  eff=\"%s\"\n"
                 "  oxide_type IMMUNE        base==eff     base=\"%s\"  eff=\"%s\"\n"
                 "  input_gain IMMUNE        base==eff     base=%.6f  eff=%.6f\n"
                 "  env_failure_modes PT     base==eff     base=%u  eff=%u\n",
                 eff.env_age_seconds,
                 static_cast<double>(eff.env_tape_health),
                 static_cast<double>(eff.bias) / static_cast<double>(base.bias),
                 eff.wow_dep     - base.wow_dep,     base.wow_dep,
                 eff.flutter_dep - base.flutter_dep, base.flutter_dep,
                 eff.tension_load- base.tension_load,base.tension_load,
                 base.hiss, eff.hiss,
                 static_cast<int>(base.eq_curve), static_cast<int>(eff.eq_curve),
                 base.format_id.c_str(),  eff.format_id.c_str(),
                 base.oxide_type.c_str(), eff.oxide_type.c_str(),
                 base.input_gain, eff.input_gain,
                 base.env_failure_modes,  eff.env_failure_modes);
    std::fflush(stderr);
    return false;
}


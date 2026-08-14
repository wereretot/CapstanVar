#include <cstdio>
#include "mod_magnetic.hpp"
#include <cmath>
#include <algorithm>
#include <random>

// Thread-local RNG for per-block noise (avoids contention)
static thread_local std::mt19937 tl_rng{std::random_device{}()};
static thread_local std::normal_distribution<float> tl_normal{0.f, 1.f};
static thread_local std::uniform_real_distribution<float> tl_uniform{0.f, 1.f};
static thread_local std::bernoulli_distribution tl_bern{0.001};

MagneticPath::MagneticPath() {
    _demag_fwd.reset();
    _demag_rev.reset();
}

void MagneticPath::reset() {
    _last_proc  = {};
    _last_bark  = {};
    _demag_fwd.reset();
    _demag_rev.reset();
    _demag_fc_last = -1.0f;
    // Phase 3: clear 4-band LR-4 cascade and Preisach magnetisation state
    for (int i = 0; i < 2; ++i) {
        _lpf1[i].reset(); _hpf1[i].reset();
        _lpf2[i].reset(); _hpf2[i].reset();
        _lpf3[i].reset(); _hpf3[i].reset();
    }
    for (int b = 0; b < 4; ++b) _m_prev[b][0] = _m_prev[b][1] = 0.0f;
    _crossover_sr_last = -1.0f;
    _oxide_last.clear();
    _drive_bias_last   = 0.0f;
}

void MagneticPath::_update_demag(float /*demag_param*/, float fc_hz) {
    // Only update when cutoff changes by more than 1 Hz — prevents per-block
    // coefficient recomputation which would reset state and cause clicks.
    if (std::abs(fc_hz - _demag_fc_last) < 1.0f) return;
    _demag_fc_last = fc_hz;
    float fc_norm  = fc_hz / (SR_F * 0.5f);
    butter_lp(fc_norm, _demag_fwd);
    _demag_rev = _demag_fwd;
    // Do NOT reset state: preserving IIR state gives a brief transient
    // but avoids the hard zero-discontinuity click that reset() causes.
}

void MagneticPath::saturate_only(Frame* buf, int n, const EngineParams& p, int oversample) {
    // Phase 3: 4-band LR-4 cascade + per-band Preisach-blended saturation.
    // Cooking the crossovers at the effective sample rate (1× base / 4× OS)
    // keeps band alignment correct across both paths that share this code.
    float sr = SR_F * std::max(oversample, 1);
    _saturate_bandwise(buf, n, p, sr);
}

// ── 4-band LR-4 cascade + per-band Preisach-blended saturator (Phase 3) ──────
// Replaces the single-band `fast_tanh`/`fast_tanh`/ceiling chain with:
//   1. A 4-band Linkwitz-Riley 4th-order crossover split at 200 Hz / 1.5 kHz
//      / 6 kHz. Each split is two cascaded 2-pole Butterworths (LP4 + HP4);
//      the two sides sum to a power-complementary unity gain.
//   2. A per-band saturator that blends fast_tanh(h*k)/k and a Preisach LUT
//      lookup, weighted by oxide.hysteresis_amt. The Preisach state per band
//      and channel persists in _m_prev so the anhysteretic loop forms over
//      successive samples.
//   3. Per-oxide knee / ceiling remap from mol_thd3_db: scales saturation
//      parameters WITHOUT touching input gain (refactor gotcha #2).
//   4. Per-oxide HF-band softening from hf_rolloff_db_oct.
//   5. A 80-sample silent spin-up on oxide or driving change (refactor
//      gotcha #1), advancing _m_prev to the bias DC steady state.
//
// Audibility preservation for the 30+ existing built-in presets: legacy
// oxides (Fe2O3/CrO2/Metal/FeCo) carry mol_thd3_db ≈ +6 and hysteresis_amt in
// [0.05, 0.20], so the 4-band cascade + hysteresis blend sits within a small
// acceptable coloration floor vs. the pre-Phase-3 single-band path. When
// hysteresis_amt == 0 the lerp bypasses the LUT and the band-summed output
// matches the legacy saturator within numerical tolerance.
void MagneticPath::_saturate_bandwise(Frame* buf, int n, const EngineParams& p, float sr_F) {
    if (n <= 0) return;
    auto it = oxide_presets().find(p.oxide_type);
    if (it == oxide_presets().end()) return;
    const auto& oxide = it->second;
    auto lut_it = preisach_luts().find(p.oxide_type);
    const PreisachLUT& lut = (lut_it != preisach_luts().end())
                           ? lut_it->second : preisach_luts().at("Fe2O3");
    const float hyst = std::clamp(oxide.hysteresis_amt, 0.0f, 1.0f);

    // (a) Re-cook LR-4 crossover biquads only when the effective sample rate
    //     changes (base SR path vs. 2× / 4× oversample path).
    if (std::abs(sr_F - _crossover_sr_last) > 0.1f) {
        float nyq = sr_F * 0.5f;
        auto cook = [&](float fc_hz, Biquad lp[2], Biquad hp[2]) {
            float fcn = std::clamp(fc_hz / nyq, 1e-4f, 0.4999f);
            butter_lp(fcn, lp[0]); butter_lp(fcn, lp[1]);
            butter_hp(fcn, hp[0]); butter_hp(fcn, hp[1]);
            lp[0].reset(); lp[1].reset();
            hp[0].reset(); hp[1].reset();
        };
        cook(200.0f,  _lpf1, _hpf1);
        cook(1500.0f, _lpf2, _hpf2);
        cook(6000.0f, _lpf3, _hpf3);
        _crossover_sr_last = sr_F;
    }

    // (b) Per-oxide knee / ceiling remap — mol_thd3_db shifts saturation
    //     parameters only; input gain is unaffected (refactor gotcha #2).
    float mol_factor = std::pow(10.0f, (oxide.mol_thd3_db - 6.0f) / 20.0f);  // 1.0 at mol=+6
    float hc_ratio   = oxide.Hc / HC_REF;
    float knee_eff   = std::clamp(std::pow(hc_ratio * mol_factor, 0.45f), 0.4f, 3.5f);
    float knee       = 1.0f / knee_eff;
    float ceiling    = (oxide.Ms * mol_factor) / std::max(p.drive * 0.5f + 0.5f, 0.1f);
    float dc_offset  = (p.bias * oxide.bias_trim - 1.0f) * 0.08f;

    // (c) Per-band HF-band softening from oxide HF rolloff. B1 / B2 keep the
    //     base knee; B3 (1.5–6 kHz) softens by ~½ oct; B4 (>6 kHz) by ~1.5 oct.
    float hf_oct  = oxide.hf_rolloff_db_oct;
    float k1 = knee;
    float k2 = knee;
    float k3 = knee * std::pow(10.0f, hf_oct * 0.5f / 20.0f);
    float k4 = knee * std::pow(10.0f, hf_oct * 1.5f / 20.0f);

    // (d) Band ceiling: uniform compensation so the saturation peak
    //     across the 4-band SUM equals the documented ceiling.
    //
    //     The kernel `fast_tanh(x·k)/k` is bounded by `1/k_b` on each
    //     band. Per-band peak contribution is therefore
    //     `band_ceiling × 1/k_b`. With a uniform band_ceiling the sum is
    //     `band_ceiling × Σ(1/k_b)`. To make that equal `ceiling`, set:
    //
    //         band_ceiling = ceiling / Σ(1/k_b)
    //
    //     Worked example (SM900, knee=0.71, hf_oct=-3.5):
    //       k1=k2=0.71, k3=0.581, k4=0.388
    //       Σ(1/k_b) = 1.408 + 1.408 + 1.722 + 2.578 = 7.116
    //       band_ceiling = ceiling / 7.116 = ceiling × 0.141
    //       per-band peaks: 0.198, 0.198, 0.242, 0.362
    //       sum = 1.000 × ceiling ✓
    //
    //     At equal knees (no HF rolloff), Σ(1/k_b) = 4/k_b and
    //     band_ceiling = ceiling × k_b / 4 — the original 1/4 split
    //     scaled by legacy knee. Behaviour matches the pre-Phase-3 path
    //     at hyst=0, equal knees, drive=1.2 (legacy: peak = ceil).
    float k_inv_sum    = 1.0f/k1 + 1.0f/k2 + 1.0f/k3 + 1.0f/k4;
    float band_ceiling = ceiling / k_inv_sum;
    const float  kn_per[4]  = { k1, k2, k3, k4 };

    // Stored into _m_prev each step: keep the LUT-domain signal (always in
    // [-M_MAX, +M_MAX] = [-1, +1]) rather than the rescaled lerp output.
    // The rescaled mll can exceed ±1 for low-knee bands, which would
    // silently saturate the LUT's boundary cell on the next sample.

    // Per-sample 4-band cascade + Preisach-blended saturation. Inlined into
    // both the spin-up loop (output discarded) and the production loop.
    auto step = [&](float in_l, float in_r, float& ol, float& or_) {
        float lp1l1 = _lpf1[1].tick(in_l, 0); float lp1l = _lpf1[0].tick(lp1l1, 0);
        float lp1r1 = _lpf1[1].tick(in_r, 1); float lp1r = _lpf1[0].tick(lp1r1, 1);
        float hp1l1 = _hpf1[1].tick(in_l, 0); float hp1l = _hpf1[0].tick(hp1l1, 0);
        float hp1r1 = _hpf1[1].tick(in_r, 1); float hp1r = _hpf1[0].tick(hp1r1, 1);
        float lp2l1 = _lpf2[1].tick(hp1l, 0); float lp2l = _lpf2[0].tick(lp2l1, 0);
        float lp2r1 = _lpf2[1].tick(hp1r, 1); float lp2r = _lpf2[0].tick(lp2r1, 1);
        float hp2l1 = _hpf2[1].tick(hp1l, 0); float hp2l = _hpf2[0].tick(hp2l1, 0);
        float hp2r1 = _hpf2[1].tick(hp1r, 1); float hp2r = _hpf2[0].tick(hp2r1, 1);
        float lp3l1 = _lpf3[1].tick(hp2l, 0); float lp3l = _lpf3[0].tick(lp3l1, 0);
        float lp3r1 = _lpf3[1].tick(hp2r, 1); float lp3r = _lpf3[0].tick(lp3r1, 1);
        float hp3l1 = _hpf3[1].tick(hp2l, 0); float hp3l = _hpf3[0].tick(hp3l1, 0);
        float hp3r1 = _hpf3[1].tick(hp2r, 1); float hp3r = _hpf3[0].tick(hp3r1, 1);
        const float bin_l[4] = { lp1l, lp2l, lp3l, hp3l };
        const float bin_r[4] = { lp1r, lp2r, lp3r, hp3r };
        ol = 0.0f; or_ = 0.0f;
        for (int b = 0; b < 4; ++b) {
            float bl = bin_l[b], br_ = bin_r[b], k = kn_per[b];
            float mfl  = fast_tanh(bl * k) / k;
            float mfr  = fast_tanh(br_ * k) / k;
            float mll_l = lut.lookup(bl,  _m_prev[b][0]);
            float mll_r = lut.lookup(br_, _m_prev[b][1]);
            float mll   = lerp(mfl, mll_l, hyst);
            float mlr   = lerp(mfr, mll_r, hyst);
            // band_ceiling already absorbed Σ(1/k_b); no per-band multiplier
            ol  += mll * band_ceiling;
            or_ += mlr * band_ceiling;
            _m_prev[b][0] = mll_l;     // bounded ±1 — fits LUT domain exactly
            _m_prev[b][1] = mll_r;
        }
    };

    // (e) Spin-up on oxide or driving change — feeds silent DC for 80 samples
    //     to converge LR-4 biquad state and Preisach m_prev to the bias
    //     operating point. Skipping this causes a startup click on the first
    //     real sample (refactor gotcha #1).
    float drive_bias_now = p.drive * p.bias;
    if (p.oxide_type != _oxide_last
     || std::abs(drive_bias_now - _drive_bias_last) > 1e-3f) {
        for (int b = 0; b < 4; ++b) _m_prev[b][0] = _m_prev[b][1] = 0.0f;
        for (int s = 0; s < 80; ++s) {
            float ol_dummy = 0.0f, or_dummy = 0.0f;
            step(dc_offset, dc_offset, ol_dummy, or_dummy);
        }
        _oxide_last      = p.oxide_type;
        _drive_bias_last = drive_bias_now;
    }

    // (f) Production pass — same per-sample path with output written back
    for (int i = 0; i < n; ++i) {
        float ol = 0.0f, or_ = 0.0f;
        step(buf[i].l + dc_offset, buf[i].r + dc_offset, ol, or_);
        buf[i].l = ol;
        buf[i].r = or_;
    }
}

void MagneticPath::process(Frame* buf, int n,
                           std::span<const Frame> full_audio,
                           std::span<const double> read_indices,
                           const EngineParams& p)
{
    // Phase 3: oxide lookup + saturation parameters moved into
    // _saturate_bandwise. We still need a fast failsafe for unknown oxide
    // keys so downstream sections don't run with a missing presets() entry.
    if (oxide_presets().find(p.oxide_type) == oxide_presets().end()) return;
    int N = (int)full_audio.size();

    // ── 1. PRINT-THROUGH ──────────────────────────────────────────────────────
    float print_amt = p.print_through;
    if (print_amt > 0.0f && N > 0) {
        // Supply reel radius at current progress
        const float REEL_FULL = 0.133f, REEL_HUB = 0.025f;
        float full_area = PI * (REEL_FULL*REEL_FULL - REEL_HUB*REEL_HUB);
        float prog      = std::clamp((float)(read_indices[n/2] / std::max(N-1, 1)), 0.f, 1.f);
        float supply_r  = std::sqrt(full_area * (1.0f - prog) / PI + REEL_HUB * REEL_HUB);
        float ips       = p.ips_base;
        float v_tape    = ips * 0.0254f;
        float layer_s   = std::clamp((TWO_PI * supply_r) / v_tape, 0.2f, 4.0f);
        int   la        = (int)(SR_F * layer_s);
        int   lb        = la;

        bool is_rev = p.is_reversed;
        for (int i = 0; i < n; ++i) {
            int raw = (int)read_indices[i];
            int pre_raw  = is_rev ? raw - la : raw + la;
            int post_raw = is_rev ? raw + lb : raw - lb;

            if (pre_raw >= 0 && pre_raw < N) {
                buf[i].l += full_audio[pre_raw].l * (print_amt * 0.70f);
                buf[i].r += full_audio[pre_raw].r * (print_amt * 0.70f);
            }
            if (post_raw >= 0 && post_raw < N) {
                buf[i].l += full_audio[post_raw].l * (print_amt * 0.30f);
                buf[i].r += full_audio[post_raw].r * (print_amt * 0.30f);
            }
        }
    }

    // ── 2. STEREO CROSSTALK ───────────────────────────────────────────────────
    float cross = p.crosstalk;
    if (cross > 0.0f) {
        for (int i = 0; i < n; ++i) {
            float l = buf[i].l, r = buf[i].r;
            buf[i].l = l * (1.0f - cross) + r * cross;
            buf[i].r = r * (1.0f - cross) + l * cross;
        }
    }

    // ── 3. MAGNETIC SATURATION (Phase 3 — 4-band cascade + Preisach) ────
    if (!p.presaturated) {
        _saturate_bandwise(buf, n, p, SR_F);
    }

    // ── 4. REPLAY HEAD DIFFERENTIATION ───────────────────────────────────────
    // Simulates replay head reading flux rate-of-change (+6dB/oct HF boost)
    // This effect becomes harsher at high IPS because more HF content is present.
    float rd = p.replay_diff;
    float ips = p.ips_base;
    if (rd > 0.0f && n > 1) {
        Frame prev = _last_proc;
        // Reduce effect at high IPS to prevent crispy HF artifacts
        // At 1.7 IPS: full effect, at 30 IPS: ~50% effect
        float ips_scale = std::clamp(1.0f - (ips - 1.7f) / 60.f, 0.5f, 1.0f);
        float rd_smooth = rd * 0.5f * ips_scale;  // Scale down effective amount
        
        for (int i = 0; i < n; ++i) {
            Frame orig = buf[i];
            // First-order difference (differentiation)
            Frame diff = {buf[i].l - prev.l, buf[i].r - prev.r};
            // Blend original with differentiated signal
            // Use a curved blend to reduce harshness at high settings
            float blend = rd_smooth * (2.0f - rd_smooth);  // Soft curve
            buf[i].l = orig.l * (1.0f - blend) + diff.l * blend;
            buf[i].r = orig.r * (1.0f - blend) + diff.r * blend;
            prev = orig;
        }
    }
    _last_proc = buf[n-1];

    // ── 5. BARKHAUSEN NOISE ───────────────────────────────────────────────────
    float bark = p.barkhausen;
    if (bark > 0.0f) {
        Frame prev = _last_bark;
        for (int i = 0; i < n; ++i) {
            float dm_l = std::abs(buf[i].l - prev.l);
            float dm_r = std::abs(buf[i].r - prev.r);
            float n1 = tl_normal(tl_rng), n2 = tl_normal(tl_rng);
            buf[i].l += n1 * dm_l * bark * 0.05f;
            buf[i].r += n2 * dm_r * bark * 0.05f;
            prev = {buf[i].l, buf[i].r};
        }
    }
    _last_bark = buf[n-1];

    // ── 6. ASPERITIES ─────────────────────────────────────────────────────────
    float asp = p.asperities;
    if (asp > 0.0f) {
        for (int i = 0; i < n; ++i) {
            float nl = tl_normal(tl_rng), nr = tl_normal(tl_rng);
            buf[i].l += nl * asp * 0.005f * std::abs(buf[i].l);
            buf[i].r += nr * asp * 0.005f * std::abs(buf[i].r);
        }
    }

    // ── 7. DEMAGNETISATION ────────────────────────────────────────────────────
    float demag = p.demagnetization;
    if (demag > 0.0f) {
        float fc_hz = std::clamp(300.0f + 17500.0f * std::pow(1.0f - demag, 2.2f),
                                 300.0f, 20000.0f);
        _update_demag(demag, fc_hz);

        if (p.is_reversed) {
            // Reverse buffer, apply filter, reverse back (causal in tape direction)
            for (int i = 0; i < n/2; ++i) std::swap(buf[i], buf[n-1-i]);
            _demag_rev.process(buf, n);
            for (int i = 0; i < n/2; ++i) std::swap(buf[i], buf[n-1-i]);
        } else {
            _demag_fwd.process(buf, n);
        }
    } else {
        _demag_fc_last = -1.0f;
        // Don't reset filter state on disable — let it decay naturally.
    }

    // ── 8. OXIDE SHEDDING ─────────────────────────────────────────────────────
    float shedding = p.oxide_shedding;
    if (shedding > 0.0f) {
        std::bernoulli_distribution shed_dist(shedding * 0.001);
        std::uniform_real_distribution<float> drop_dist(0.1f, 0.5f);
        for (int i = 0; i < n; ++i) {
            if (shed_dist(tl_rng)) {
                float g = 1.0f - drop_dist(tl_rng);
                buf[i] *= g;
            }
        }
    }
}

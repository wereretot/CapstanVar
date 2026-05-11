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

void MagneticPath::saturate_only(Frame* buf, int n, const EngineParams& p) {
    auto& oxide = oxide_presets().at(p.oxide_type);
    float drive     = p.drive;
    float bias      = p.bias * oxide.bias_trim;
    float hc_ratio  = oxide.Hc / HC_REF;
    float Ms        = oxide.Ms;
    float knee      = 1.0f / std::clamp(std::pow(hc_ratio, 0.45f), 0.4f, 3.5f);
    float dc_offset = (bias - 1.0f) * 0.08f;
    float ceiling   = Ms / std::max(drive * 0.5f + 0.5f, 0.1f);

    for (int i = 0; i < n; ++i) {
        auto h_l = buf[i].l * drive + dc_offset;
        auto h_r = buf[i].r * drive + dc_offset;
        buf[i].l = fast_tanh(h_l * knee) / knee * ceiling;
        buf[i].r = fast_tanh(h_r * knee) / knee * ceiling;
    }
}

void MagneticPath::process(Frame* buf, int n,
                           std::span<const Frame> full_audio,
                           std::span<const double> read_indices,
                           const EngineParams& p)
{
    auto it = oxide_presets().find(p.oxide_type);
    if (it == oxide_presets().end()) return;
    const auto& oxide = it->second;

    float drive     = p.drive;
    float bias      = p.bias * oxide.bias_trim;
    float hc_ratio  = oxide.Hc / HC_REF;
    float Ms        = oxide.Ms;
    int   N         = (int)full_audio.size();

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

    // ── 3. MAGNETIC SATURATION ────────────────────────────────────────────────
    if (!p.presaturated) {
        float knee_scale = 1.0f / std::clamp(std::pow(hc_ratio, 0.45f), 0.4f, 3.5f);
        float dc_offset  = (bias - 1.0f) * 0.08f;
        float ceiling    = Ms / std::max(drive * 0.5f + 0.5f, 0.1f);

        for (int i = 0; i < n; ++i) {
            float hl = buf[i].l * drive + dc_offset;
            float hr = buf[i].r * drive + dc_offset;
            buf[i].l = fast_tanh(hl * knee_scale) / knee_scale * ceiling;
            buf[i].r = fast_tanh(hr * knee_scale) / knee_scale * ceiling;
        }
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

    // ── 8. METAL LOSS — oxide/metal particle shedding (dropouts) ──────────────
    float shedding = p.tape_metal_loss;
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

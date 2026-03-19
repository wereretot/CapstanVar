#include <cstdio>
#include "mod_electronics.hpp"
#include <cmath>
#include <algorithm>
#include <random>

static thread_local std::mt19937 el_rng{std::random_device{}()};
static thread_local std::normal_distribution<float> el_normal{0.f, 1.f};

ElectronicComponents::ElectronicComponents() {
    _az_delay_buf.fill({});
}

void ElectronicComponents::reset() {
    _bump_f.reset();
    _bump_fc_last = -1.0f;
    _az_f.reset();
    _az_fc_last   = -1.0f;
    _azimuth_state  = 0.0f;
    _az_delay_smooth = 0.0f;
    _az_delay_buf.fill({});
    _pink_state[0] = _pink_state[1] = 0.0f;
    _diff_last = {};
}

float ElectronicComponents::_rand_normal() {
    if (_bm_ready) { _bm_ready = false; return _bm_spare; }
    // xorshift
    _rng ^= _rng << 13; _rng ^= _rng >> 7; _rng ^= _rng << 17;
    float u1 = (float)(_rng >> 11) * (1.f/(float)(1ULL<<53));
    _rng ^= _rng << 13; _rng ^= _rng >> 7; _rng ^= _rng << 17;
    float u2 = (float)(_rng >> 11) * (1.f/(float)(1ULL<<53));
    float mag = std::sqrt(-2.f * std::log(std::max(u1, 1e-8f)));
    _bm_spare = mag * std::cos(TWO_PI * u2); _bm_ready = true;
    return mag * std::sin(TWO_PI * u2);
}

float ElectronicComponents::_rand_uniform() {
    _rng ^= _rng << 13; _rng ^= _rng >> 7; _rng ^= _rng << 17;
    return (float)(_rng >> 11) * (1.f/(float)(1ULL<<53));
}

void ElectronicComponents::process(Frame* buf, int n,
                                   float current_time,
                                   float speed_factor,
                                   float sticky_drag,
                                   const EngineParams& p)
{
    // ── 1. HEAD BUMP ──────────────────────────────────────────────────────────
    float bump_amt = p.head_bump;
    if (bump_amt > 0.0f) {
        float bump_f = std::clamp(50.0f * speed_factor, 15.0f, 600.0f);
        float Q      = 1.5f;
        float bw     = bump_f / Q;
        float fc_key = bump_f; // use centre freq as cache key
        if (std::abs(fc_key - _bump_fc_last) > 0.5f) {
            float lo = std::clamp((bump_f - bw*0.5f) / (SR_F*0.5f), 1e-4f, 0.499f);
            float hi = std::clamp((bump_f + bw*0.5f) / (SR_F*0.5f), lo+1e-4f, 0.4999f);
            butter_bp(lo, hi, _bump_f);
            // No reset: preserve IIR state across coefficient update
            _bump_fc_last = fc_key;
        }
        // Apply: out = in + bump_sig * amt
        // Make a copy, filter it, add back
        std::vector<Frame> bump_buf(buf, buf+n);
        _bump_f.process(bump_buf.data(), n);
        for (int i = 0; i < n; ++i) {
            buf[i].l += bump_buf[i].l * bump_amt;
            buf[i].r += bump_buf[i].r * bump_amt;
        }
    }

    // ── 2. AZIMUTH LOWPASS ────────────────────────────────────────────────────
    float cutoff     = p.cutoff_base;
    float shed_factor = std::clamp(1.0f - sticky_drag * 50.0f, 0.05f, 1.0f);
    float safe_cut   = std::clamp(cutoff * speed_factor * shed_factor, 80.0f, 20000.0f);
    float fc_norm    = std::clamp(safe_cut / (SR_F * 0.5f), 1e-4f, 0.4999f);
    // Only recompute coefficients when cutoff changes by >0.5 Hz (inaudible threshold).
    // Do NOT reset state on update — that causes a click every block.
    if (std::abs(safe_cut - _az_fc_last) > 0.5f) {
        butter_lp(fc_norm, _az_f);
        // No reset: preserve state, accept brief coefficient-update transient
        _az_fc_last = safe_cut;
    }
    _az_f.process(buf, n);

    // ── 3. AZIMUTH PHASE WANDER ───────────────────────────────────────────────
    // Models head gap angle variation causing HF phase difference between channels.
    // The right channel is delayed by a slowly-wandering fractional sample count.
    //
    // Fixes vs original:
    //  - _azimuth_state is bounded with a leaky integrator (prevents fast zero-crossings)
    //  - delay_samp is smoothed per-block to prevent inter-block discontinuities
    //  - shift & frac are computed consistently from the clamped/smoothed value
    //  - frac is always in [0,1) so lerp never extrapolates
    float az_drift = p.azimuth_drift;
    if (az_drift > 0.0f) {
        // Bounded random walk: leak toward zero so state stays near [-pi, +pi]
        _azimuth_state = _azimuth_state * 0.9998f + _rand_normal() * 0.003f;
        _azimuth_state = std::clamp(_azimuth_state, -3.14159f, 3.14159f);

        // Target delay in samples: smooth sine gives slow, organic wander
        float target_delay = std::sin(_azimuth_state) * az_drift * (float)(AZ_BUF / 2);

        // Smooth the delay change per-block to prevent discontinuity clicks
        // Ramp rate: at most 0.5 sample per block (~23ms at 44100/1024)
        float max_delta = 0.5f;
        if (target_delay > _az_delay_smooth + max_delta)
            _az_delay_smooth += max_delta;
        else if (target_delay < _az_delay_smooth - max_delta)
            _az_delay_smooth -= max_delta;
        else
            _az_delay_smooth = target_delay;

        // Clamp to valid delay range
        float delay_f = std::clamp(_az_delay_smooth,
                                   -(float)(AZ_BUF - 2), (float)(AZ_BUF - 2));

        // Split into integer + fractional for interpolation
        // Always use positive frac by adjusting integer part
        int   delay_i = (int)std::floor(delay_f);
        float frac    = delay_f - (float)delay_i;   // always in [0, 1)

        int buf_len = AZ_BUF;

        // Copy original right channel BEFORE overwriting it.
        // This is critical — the delay buffer must store the unmodified signal,
        // not the already-delayed output (which would create feedback).
        std::vector<float> orig_r(n);
        for (int i = 0; i < n; ++i) orig_r[i] = buf[i].r;

        // Read delayed right channel from [_az_delay_buf | orig_r] virtual array
        for (int i = 0; i < n; ++i) {
            int src = buf_len + delay_i + i;

            auto get = [&](int k) -> float {
                if (k < 0)       k = 0;
                if (k < buf_len) return _az_delay_buf[k].r;
                int ki = k - buf_len;
                if (ki >= n)     ki = n - 1;
                return orig_r[ki];   // read from ORIGINAL, not modified buf
            };

            buf[i].r = get(src) * (1.f - frac) + get(src + 1) * frac;
        }

        // Update delay buffer with ORIGINAL right channel (not delayed output)
        if (n >= buf_len) {
            for (int i = 0; i < buf_len; ++i) {
                _az_delay_buf[i].l = buf[n - buf_len + i].l;
                _az_delay_buf[i].r = orig_r[n - buf_len + i];
            }
        } else {
            for (int i = 0; i < buf_len - n; ++i)
                _az_delay_buf[i] = _az_delay_buf[i + n];
            for (int i = 0; i < n; ++i) {
                _az_delay_buf[buf_len - n + i].l = buf[i].l;
                _az_delay_buf[buf_len - n + i].r = orig_r[i];
            }
        }
    }

    // ── 4. TAPE HISS ─────────────────────────────────────────────────────────
    float hiss_amt    = p.hiss;
    static const struct { const char* name; float factor; } oxide_noise[] = {
        {"Fe2O3", 1.00f}, {"CrO2", 0.85f}, {"Metal", 0.70f}, {"FeCo", 0.80f}
    };
    float oxide_factor = 1.0f;
    for (auto& o : oxide_noise)
        if (p.oxide_type == o.name) { oxide_factor = o.factor; break; }

    float dynamic_hiss = hiss_amt * oxide_factor / std::max(std::sqrt(speed_factor), 0.01f);

    if (dynamic_hiss > 0.0f) {
        for (int i = 0; i < n; ++i) {
            buf[i].l += _rand_normal() * dynamic_hiss;
            buf[i].r += _rand_normal() * dynamic_hiss;
        }
    }

    // ── 5. MAINS HUM ─────────────────────────────────────────────────────────
    float hum_amt = p.mains_hum;
    if (hum_amt > 0.0f) {
        for (int i = 0; i < n; ++i) {
            float t = current_time + (float)i * SR_F_INV;
            float hum = (std::sin(TWO_PI *  60.0f * t) * 1.00f
                       + std::sin(TWO_PI * 120.0f * t) * 0.50f
                       + std::sin(TWO_PI * 180.0f * t) * 0.25f
                       + std::sin(TWO_PI * 240.0f * t) * 0.08f) * hum_amt;
            buf[i].l += hum;
            buf[i].r += hum;
        }
    }

    // ── 6. PINK HISS TILT ────────────────────────────────────────────────────
    float hiss_color = p.hiss_color;
    if (hiss_color > 0.0f && dynamic_hiss > 0.0f) {
        float s0 = _pink_state[0], s1 = _pink_state[1];
        float scale = dynamic_hiss * 0.15f;
        for (int i = 0; i < n; ++i) {
            float wl = _rand_normal() * scale;
            float wr = _rand_normal() * scale;
            s0 = 0.99f * s0 + wl;
            s1 = 0.99f * s1 + wr;
            buf[i].l += s0 * hiss_color;
            buf[i].r += s1 * hiss_color;
        }
        _pink_state[0] = s0;
        _pink_state[1] = s1;
    }
}

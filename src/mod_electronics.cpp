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
    _bump_delay_buf.fill({});
    _bump_write_idx = 0;
    _az_f.reset();
    _az_fc_last   = -1.0f;
    _gap_delay_buf.fill({});
    _gap_write_idx = 0;
    _azimuth_state  = 0.0f;
    _az_delay_smooth = 0.0f;
    _az_delay_buf.fill({});
    _pink_state[0] = _pink_state[1] = 0.0f;
    _diff_last = {};
    
    // Reset HiFi state
    _hifi_phase = 0.0f;
    _hifi_fm_phase_l = 0.0f;
    _hifi_fm_phase_r = 0.0f;
    _hifi_preemph_last_l = 0.0f;
    _hifi_preemph_last_r = 0.0f;
    _hifi_deemph_last_l = 0.0f;
    _hifi_deemph_last_r = 0.0f;
    _hifi_quality = 1.0f;
    _hifi_demod_phase_l = 0.0f;
    _hifi_demod_phase_r = 0.0f;
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

// HiFi FM modulation - simulates FM audio at baseband rates
// Real VHS HiFi uses 1.3/1.7 MHz carriers, but we simulate at audio frequencies
float ElectronicComponents::_fmModulate(float audio_sample, float carrier_freq, float deviation, float& phase) {
    // Scale down to audio-range frequencies for simulation
    // Map 1.3-1.7 MHz down to ~3-5 kHz range for audible simulation
    float simulated_carrier = carrier_freq * 0.0025f; // Scale MHz to kHz range
    float simulated_deviation = deviation * 0.0025f;
    
    // FM modulation: phase accumulator
    float freq = simulated_carrier + simulated_deviation * audio_sample;
    phase += TWO_PI * freq / SR_F;
    while (phase > TWO_PI) phase -= TWO_PI;
    
    return std::cos(phase);
}

// FM demodulation using phase differentiator
float ElectronicComponents::_fmDemodulate(float fm_sample, float& last_phase) {
    // For a real FM signal, we track instantaneous frequency by phase derivative
    // Reconstruct phase from the FM sample (which is cos(phase))
    float phase = std::acos(std::clamp(fm_sample, -1.0f, 1.0f));
    
    // Handle phase wrapping
    float delta = phase - last_phase;
    if (delta > PI) delta -= TWO_PI;
    if (delta < -PI) delta += TWO_PI;
    
    last_phase = phase;
    
    // Convert frequency deviation back to audio
    return delta * SR_F / TWO_PI / 1000.0f; // Scale down
}

// Process HiFi audio path - simplified version that simulates HiFi characteristics
void ElectronicComponents::_processHiFiAudio(Frame* buf, int n, float speed_factor, const EngineParams& p) {
    if (!p.hifi_enabled) return;
    
    float tracking_factor = 1.0f - std::min(p.tracking_error * p.hifi_dropout_sens * 3.0f, 1.0f);
    float dropout_factor = 1.0f - std::min(p.dropout_rate * 4.0f, 1.0f);
    float target_quality = tracking_factor * dropout_factor;
    
    if (target_quality < _hifi_quality) {
        _hifi_quality += (target_quality - _hifi_quality) * 0.2f;
    } else {
        _hifi_quality += (target_quality - _hifi_quality) * 0.05f;
    }
    _hifi_quality = std::clamp(_hifi_quality, 0.0f, 1.0f);
    
    if (_hifi_quality < 0.1f) return;
    
    float noise_linear = std::pow(10.0f, p.hifi_noise_floor / 20.0f) * 0.01f;
    float crosstalk_linear = std::pow(10.0f, p.hifi_crosstalk / 20.0f);
    
    for (int i = 0; i < n; ++i) {
        float orig_l = buf[i].l;
        float orig_r = buf[i].r;
        
        // Crosstalk before modulation
        float ct_l = orig_l + orig_r * crosstalk_linear;
        float ct_r = orig_r + orig_l * crosstalk_linear;
        
        // Pre-emphasis (1T high shelf approx)
        float pre_l = ct_l + 0.85f * (ct_l - _hifi_preemph_last_l);
        float pre_r = ct_r + 0.85f * (ct_r - _hifi_preemph_last_r);
        _hifi_preemph_last_l = ct_l;
        _hifi_preemph_last_r = ct_r;
        
        // FM Modulation
        float fm_l = _fmModulate(pre_l, p.hifi_carrier_left_hz, p.hifi_deviation_hz, _hifi_fm_phase_l);
        float fm_r = _fmModulate(pre_r, p.hifi_carrier_right_hz, p.hifi_deviation_hz, _hifi_fm_phase_r);
        
        // Inject Tape RF Noise (scales up as quality drops)
        float noise_injector = noise_linear * (2.0f - _hifi_quality);
        fm_l += _rand_normal() * noise_injector;
        fm_r += _rand_normal() * noise_injector;
        
        // FM Demodulation
        float demod_l = _fmDemodulate(fm_l, _hifi_demod_phase_l);
        float demod_r = _fmDemodulate(fm_r, _hifi_demod_phase_r);
        
        // De-emphasis
        float deemph_l = _hifi_deemph_last_l * 0.85f + demod_l * 0.15f;
        float deemph_r = _hifi_deemph_last_r * 0.85f + demod_r * 0.15f;
        _hifi_deemph_last_l = deemph_l;
        _hifi_deemph_last_r = deemph_r;
        
        buf[i].l = deemph_l;
        buf[i].r = deemph_r;
    }
}

// Process Linear audio path
void ElectronicComponents::_processLinearAudio(Frame* buf, int n, float speed_factor, float sticky_drag, 
                                              float current_time, const EngineParams& p) {
    // ── 1. HEAD BUMP (Contour Effect Comb Filter) ─────────────────────────────
    float bump_amt = p.head_bump;
    if (bump_amt > 0.0f) {
        float bump_f_base = std::clamp(50.0f * speed_factor, 5.0f, 600.0f);
        float bump_f = bump_f_base * p.head_bump_freq_mult;
        
        // Delay calculated so null is at DC, peak is at head bump freq
        int d_samps = std::clamp((int)(SR_F / (2.0f * std::max(bump_f, 1.0f))), 1, BUMP_BUF_SIZE - 2);
        
        for (int i = 0; i < n; ++i) {
            _bump_delay_buf[_bump_write_idx] = buf[i];
            
            int read_idx = (_bump_write_idx - d_samps + BUMP_BUF_SIZE) % BUMP_BUF_SIZE;
            
            // Out = In - delayed * amt (Null at 0Hz, Peak at bump_f)
            buf[i].l += (buf[i].l - _bump_delay_buf[read_idx].l) * bump_amt * 0.5f;
            buf[i].r += (buf[i].r - _bump_delay_buf[read_idx].r) * bump_amt * 0.5f;
            
            _bump_write_idx = (_bump_write_idx + 1) % BUMP_BUF_SIZE;
        }
    }

    // ── 2. GAP LOSS AND SPACING LOSS (TAPE SPEED EFFECT) ─────────────────────
    float tape_velocity_m_s = p.ips_base * speed_factor * 0.0254f;
    float gap_width_m = 1.5e-6f; // typical playback head gap
    float gap_time = gap_width_m / std::max(tape_velocity_m_s, 0.001f);
    float gap_samples_f = gap_time * SR_F;
    
    // Spacing loss (Wallace: 54.6 * d / lambda) -> Rolloff mapped to leaky integrator
    float spacing_loss_d = 0.3e-6f; 
    float spacing_cutoff = tape_velocity_m_s / (1.5f * PI * spacing_loss_d) * 1.5f;
    float shed_factor = std::clamp(1.0f - sticky_drag * 50.0f, 0.05f, 1.0f);
    float safe_cut = std::clamp(spacing_cutoff * shed_factor, 100.0f, 20000.0f);
    
    if (std::abs(safe_cut - _az_fc_last) > 10.0f) {
        butter_lp(safe_cut / (SR_F * 0.5f), _az_f);
        _az_fc_last = safe_cut;
    }
    _az_f.process(buf, n);

    // Gap Loss FIR Filter (moving average)
    int gap_samples_int = std::clamp((int)std::ceil(gap_samples_f), 1, GAP_BUF_SIZE - 2);
    if (gap_samples_int > 1) {
        float gap_weight = 1.0f / gap_samples_int;
        for (int i = 0; i < n; ++i) {
            _gap_delay_buf[_gap_write_idx] = buf[i];
            float sum_l = 0.0f, sum_r = 0.0f;
            for (int j = 0; j < gap_samples_int; ++j) {
                int read_idx = (_gap_write_idx - j + GAP_BUF_SIZE) % GAP_BUF_SIZE;
                sum_l += _gap_delay_buf[read_idx].l;
                sum_r += _gap_delay_buf[read_idx].r;
            }
            buf[i].l = sum_l * gap_weight;
            buf[i].r = sum_r * gap_weight;
            _gap_write_idx = (_gap_write_idx + 1) % GAP_BUF_SIZE;
        }
    }

    // ── 3. AZIMUTH PHASE WANDER ───────────────────────────────────────────────
    // Models head gap angle variation causing HF phase difference between channels.
    // The right channel is delayed by a slowly-wandering fractional sample count.
    float az_drift = p.azimuth_drift;
    if (az_drift > 0.0f) {
        // Bounded random walk: leak toward zero so state stays near [-pi, +pi]
        _azimuth_state = _azimuth_state * 0.9998f + _rand_normal() * 0.003f;
        _azimuth_state = std::clamp(_azimuth_state, -3.14159f, 3.14159f);

        // Target delay in samples: smooth sine gives slow, organic wander
        float target_delay = std::sin(_azimuth_state) * az_drift * (float)(AZ_BUF / 2);

        // Smooth the delay change per-block to prevent discontinuity clicks
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
        int   delay_i = (int)std::floor(delay_f);
        float frac    = delay_f - (float)delay_i;   // always in [0, 1)

        int buf_len = AZ_BUF;

        // Copy original right channel BEFORE overwriting it.
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

    float dynamic_hiss = hiss_amt * oxide_factor / std::max(speed_factor, 0.005f);

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

void ElectronicComponents::process(Frame* buf, int n,
                                   float current_time,
                                   float speed_factor,
                                   float sticky_drag,
                                   const EngineParams& p)
{
    // Store original audio for potential HiFi blending
    std::vector<Frame> original(buf, buf + n);
    
    // Process linear audio path (always processed as fallback)
    _processLinearAudio(buf, n, speed_factor, sticky_drag, current_time, p);
    
    // Store linear-processed audio
    std::vector<Frame> linear(buf, buf + n);
    
    // If HiFi is enabled, process through HiFi FM chain
    if (p.hifi_enabled) {
        // Restore original for HiFi processing
        for (int i = 0; i < n; ++i) {
            buf[i] = original[i];
        }
        
        // Process HiFi audio
        _processHiFiAudio(buf, n, speed_factor, p);
        
        // Blend between HiFi and Linear based on HiFi quality
        // When HiFi quality drops, we fade to linear track
        for (int i = 0; i < n; ++i) {
            buf[i].l = buf[i].l * _hifi_quality + linear[i].l * (1.0f - _hifi_quality);
            buf[i].r = buf[i].r * _hifi_quality + linear[i].r * (1.0f - _hifi_quality);
        }
    }
    
    // ── HiFi PULSE-BUZZ (tracking interference) ────────────────────────────
    // Characteristic 60Hz pulse-buzz seen when helical HiFi heads lose tracking.
    // Only active when HiFi is enabled and tracking is poor.
    if (p.hifi_enabled) {
        float hifi_amt = (p.dropout_rate * 0.4f + p.tracking_error * p.hifi_dropout_sens + p.motor_drag * 0.1f) 
                         * (1.0f - _hifi_quality) * 2.0f; // Stronger when quality is poor
        if (hifi_amt > 0.001f) {
            float hifi_hz = 59.94f * speed_factor;
            float hifi_inc = hifi_hz / SR_F;
            for (int i = 0; i < n; ++i) {
                _hifi_phase = std::fmod(_hifi_phase + hifi_inc, 1.0f);
                // Pulse window (simulating head switching interference)
                if (_hifi_phase < 0.035f) {
                    float pulse = std::sin(_hifi_phase * TWO_PI * 15.0f); // metallic harmonic
                    float crackle = el_normal(el_rng) * 0.4f;
                    float buzz = (pulse + crackle) * hifi_amt * 0.25f;
                    buf[i].l += buzz;
                    buf[i].r += buzz;
                }
            }
        }
    }
}

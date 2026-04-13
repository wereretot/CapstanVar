#include <cstdio>
#include "mod_transport.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>

// ── xorshift64 RNG ────────────────────────────────────────────────────────────
static uint64_t xor64(uint64_t& s) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

TransportDynamics::TransportDynamics(uint64_t seed) {
    _rng_state = seed ? seed : 0x853c49e6748fea9bULL;

    // Seed oscillator phases from RNG
    auto rand01 = [&]{ return (float)(xor64(_rng_state) >> 11) * (1.0f / (float)(1ULL << 53)); };
    _roller_phase = rand01() * TWO_PI;
    _roller_ecc   = 0.0002f + rand01() * 0.0006f;
    _supply_phase = rand01() * TWO_PI;
}

void TransportDynamics::reset() {
    current_motor_speed = 0.0f;
    sticky_drag         = 0.0f;
    last_instant_speed  = 0.0f;
    motor_engage        = 0.0f;
    _fight_phase        = 0.0f;
    _fight_lurch_timer  = 0.0f;
    _fight_lurch_mag    = 0.0f;
    _next_dropout       = 0.0f;
    _dropout_timer      = 0.0f;
    dropout_count       = 0;
    last_dropout_time   = -999.0f;
    _dropout_rem        = 0;
    _dropout_depth      = 0.0f;
}

float TransportDynamics::_rand_uniform() {
    return (float)(xor64(_rng_state) >> 11) * (1.0f / (float)(1ULL << 53));
}

float TransportDynamics::_rand_normal() {
    if (_bm_ready) { _bm_ready = false; return _bm_spare; }
    float u, v, s;
    do { u = _rand_uniform()*2.f-1.f; v = _rand_uniform()*2.f-1.f; s = u*u+v*v; }
    while (s >= 1.f || s == 0.f);
    float mul = std::sqrt(-2.f * std::log(s) / s);
    _bm_spare = v * mul; _bm_ready = true;
    return u * mul;
}

float TransportDynamics::_reel_inertia(float progress) const {
    float p = std::clamp(progress, 0.0f, 1.0f);
    float I_s = I_REEL_EMPTY + (I_REEL_FULL - I_REEL_EMPTY) * (1.0f - p);
    float I_t = I_REEL_EMPTY + (I_REEL_FULL - I_REEL_EMPTY) * p;
    return I_s + I_t;
}

void TransportDynamics::_reel_geometry(float progress, float& supply_r, float& takeup_r) const {
    float p        = std::clamp(progress, 0.0f, 1.0f);
    float full_area = PI * (REEL_RADIUS_FULL*REEL_RADIUS_FULL - REEL_RADIUS_HUB*REEL_RADIUS_HUB);
    supply_r = std::sqrt(full_area * (1.0f - p) / PI + REEL_RADIUS_HUB * REEL_RADIUS_HUB);
    takeup_r = std::sqrt(full_area * p           / PI + REEL_RADIUS_HUB * REEL_RADIUS_HUB);
}

float TransportDynamics::_schedule_dropout(float rate) {
    float mean = std::max(0.3f, 8.0f / std::max(rate, 0.001f));
    // Exponential distribution: -mean * ln(U)
    return -mean * std::log(std::max(_rand_uniform(), 1e-7f));
}

TransportResult TransportDynamics::process(int frames, float current_time,
                                           double play_head, int total_samples,
                                           const EngineParams& p)
{
    TransportResult result;
    result.speeds.resize(frames);
    result.dropout_mask.assign(frames, 1.0f);

    const float progress = (float)(play_head / std::max((double)total_samples, 1.0));
    const float ips      = p.ips_base;

    // Reel geometry
    float supply_r, takeup_r;
    _reel_geometry(progress, supply_r, takeup_r);

    // Inertia lag
    float I_eff       = _reel_inertia(progress);
    float I_ref       = _reel_inertia(0.5f);
    float lag_coeff   = 0.0005f / (I_eff / I_ref);

    // Target speed
    float boost = p.motor_boost;
    float drag  = p.motor_drag;

    // Sticky shed drag
    float target_sticky = p.sticky_shed * 0.015f;
    sticky_drag += (target_sticky - sticky_drag) * 0.0002f;
    if (std::abs(sticky_drag) < 1e-6f) sticky_drag = 0.0f;  // snap to zero, prevent float drift

    // Tape velocity and reel rotation frequencies
    float v_tape   = ips * 0.0254f;
    float f_supply = v_tape / (TWO_PI * std::max(supply_r, 0.001f));
    float f_takeup = v_tape / (TWO_PI * std::max(takeup_r, 0.001f));

    // Fighting speed conflict
    float conflict = std::min(boost, drag);
    float fight_osc = 0.0f;
    if (conflict > 0.05f) {
        float fight_freq = 3.5f + std::sin(_fight_phase * 0.3f) * 2.0f;
        _fight_phase    += fight_freq * frames / SR_F;
        fight_osc        = std::sin(_fight_phase) * conflict * 0.4f;

        _fight_lurch_timer += (float)frames / SR_F;
        float lurch_interval = std::max(0.3f, 1.5f / (conflict + 0.1f));
        if (_fight_lurch_timer > lurch_interval) {
            _fight_lurch_mag   = (_rand_uniform() > 0.5f ? 1.f : -1.f) * conflict * 0.8f;
            _fight_lurch_timer = 0.0f;
        }
        _fight_lurch_mag *= std::pow(0.85f, (float)frames / (SR_F * 0.1f));
    } else {
        _fight_lurch_mag = 0.0f;
    }

    float target_speed = std::max(0.02f, 1.0f + boost - drag - sticky_drag
                                  + fight_osc + _fight_lurch_mag);

    // Per-sample modulation arrays
    float ips_ratio = ips / 15.0f;
    float wow_dep   = p.wow_dep / 100.0f;
    float f_pinch   = v_tape / (TWO_PI * 0.0254f);

    // Drift amplitude
    float health     = p.motor_health;
    float drift_amp  = health * 0.004f / std::max(std::pow(ips_ratio, 0.3f), 0.2f);

    // Tension oscillation constants
    float tension_load = p.tension_load;
    float _TS = 0.10f;
    float dc_tension = (takeup_r / REEL_RADIUS_FULL) * tension_load * 0.008f;

    // Flutter amplitude
    float flutter_amp = (p.flutter_dep / 150.0f) / std::max(std::sqrt(ips_ratio), 0.1f);

    // Engage target
    float engage_target = p.motor_engage;

    // Roller eccentricity scale for wow
    float roller_ecc_scale = _roller_ecc * 50.0f;

    // ── Per-sample assembly loop ──────────────────────────────────────────────
    for (int i = 0; i < frames; ++i) {
        float t = current_time + (float)i * SR_F_INV;

        // Motor engage ramp
        if (motor_engage < engage_target)
            motor_engage = std::min(engage_target, motor_engage + SPINUP_COEFF);
        else if (motor_engage > engage_target)
            motor_engage = std::max(engage_target, motor_engage - SPINDOWN_COEFF);

        // Motor drift
        float drift = drift_amp * (
            std::sin(TWO_PI * 0.70f * ips_ratio * t + _roller_phase * 1.3f)
          + 0.60f * std::sin(TWO_PI * 2.80f * ips_ratio * t + _supply_phase)
          + 0.35f * std::sin(TWO_PI * 7.30f * ips_ratio * t + 2.1f));

        // Wow components
        float roller_wow = wow_dep * roller_ecc_scale *
            std::sin(TWO_PI * f_pinch * t + _roller_phase);
        float supply_wow = wow_dep * 0.45f * (progress * 0.5f + 0.08f) *
            std::sin(TWO_PI * f_supply * t + _supply_phase);
        float takeup_wow = wow_dep * 0.30f * (progress * 0.60f + 0.04f) *
            std::sin(TWO_PI * f_takeup * t + _supply_phase + 1.41f);

        // Flutter
        float flutter = flutter_amp * (
            std::sin(TWO_PI * 15.0f  * ips_ratio * t)
          + 0.45f * std::sin(TWO_PI *  7.3f  * ips_ratio * t + 0.71f)
          + 0.20f * std::sin(TWO_PI * 22.5f  * ips_ratio * t + 1.23f)
          + 0.12f * std::sin(TWO_PI * 30.0f  * ips_ratio * t + 2.07f)
          + 0.06f * std::sin(TWO_PI * 46.2f  * ips_ratio * t + 0.44f));

        // Tension modulation
        float tension_mod = 0.0f;
        if (tension_load > 0.0f) {
            float osc =
                std::sin(TWO_PI * f_supply       * t + _roller_phase)        * 0.55f
              + std::sin(TWO_PI * f_supply * 2.f * t + _roller_phase + 1.05f) * 0.20f
              + std::sin(TWO_PI * f_supply * 3.f * t + _roller_phase + 2.13f) * 0.08f
              + std::sin(TWO_PI * f_takeup       * t + _supply_phase + PI)    * 0.30f
              + std::sin(TWO_PI * f_takeup * 2.f * t + _supply_phase + 2.80f) * 0.10f
              + std::sin(TWO_PI * 0.48f          * t + _roller_phase * 0.37f + 0.72f) * 0.15f;
            float raw = osc * tension_load * _TS + dc_tension;
            float limit = tension_load * _TS * 1.40f;
            tension_mod = std::clamp(raw, -limit, limit);
        }

        // Integrate motor speed
        float instant_target = target_speed + drift;
        current_motor_speed += (instant_target - current_motor_speed) * lag_coeff;

        float speed = current_motor_speed + roller_wow + supply_wow + takeup_wow
                    + flutter + tension_mod;

        float final_spd = std::max(0.001f, speed * motor_engage);
        result.speeds[i] = final_spd;

        // Populate history buffer for phase-accurate video sync
        // Indexed by the absolute sample position in the source file
        int64_t idx = p.is_reversed ? int64_t(play_head - i) : int64_t(play_head + i);
        speed_history[uint64_t(idx) % SPEED_HIST_SIZE] = final_spd;
    }

    last_instant_speed = result.speeds.back();
    last_conflict      = std::min(p.motor_boost, p.motor_drag);

    // ── Dropout events ────────────────────────────────────────────────────────
    const int FADE = 176;
    float rate = p.dropout_rate;
    _dropout_timer += (float)frames / SR_F;

    // Carry-over from previous block
    if (_dropout_rem > 0) {
        int carry  = std::min(_dropout_rem, frames);
        float depth = _dropout_depth;
        int fo = std::min(FADE, carry);
        for (int i = 0; i < carry - fo; ++i)
            result.dropout_mask[i] = std::min(result.dropout_mask[i], 1.0f - depth);
        for (int i = 0; i < fo; ++i) {
            float t = (float)(i) / (float)fo;
            result.dropout_mask[carry - fo + i] = std::min(
                result.dropout_mask[carry - fo + i],
                lerp(1.0f - depth, 1.0f, t));
        }
        _dropout_rem -= carry;
        if (_dropout_rem <= 0) { _dropout_rem = 0; _dropout_depth = 0.f; }
    }

    if (rate > 0.0f) {
        while (_dropout_timer >= _next_dropout) {
            float overshoot = _dropout_timer - _next_dropout;
            int s = (int)std::clamp(overshoot * SR_F, 0.0f, (float)(frames - 1));
            float max_dur  = 0.005f + rate * 0.040f;
            float dur_s    = 0.001f + _rand_uniform() * (max_dur - 0.001f);
            int   dur_samp = (int)(dur_s * SR_F);
            float depth    = (0.7f + _rand_uniform() * 0.3f) * std::clamp(rate, 0.f, 1.f);

            int fade = std::min(FADE, dur_samp / 3);
            // Build envelope inline, applying directly to mask
            for (int k = 0; k < dur_samp; ++k) {
                float env;
                if      (k < fade)              env = lerp(1.0f, 1.0f - depth, (float)k / std::max(fade, 1));
                else if (k >= dur_samp - fade)  env = lerp(1.0f - depth, 1.0f, (float)(k-(dur_samp-fade)) / std::max(fade, 1));
                else                            env = 1.0f - depth;

                int idx = s + k;
                if (idx < frames) {
                    result.dropout_mask[idx] = std::min(result.dropout_mask[idx], env);
                } else {
                    // Carry remainder
                    _dropout_rem   = dur_samp - k;
                    _dropout_depth = depth;
                    break;
                }
            }
            last_dropout_time = _dropout_timer;
            ++dropout_count;
            _next_dropout += _schedule_dropout(rate);
        }
    }

    result.sticky_drag = sticky_drag;
    return result;
}

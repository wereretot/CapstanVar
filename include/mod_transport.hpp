#pragma once
#include "dsp_types.hpp"
#include <vector>

struct TransportResult {
    std::vector<float> speeds;      // per-sample capstan speed multipliers
    float              sticky_drag; // current accumulated shed drag
    std::vector<float> dropout_mask;// per-sample gain mask (1=clean, <1=dropout)
};

class TransportDynamics {
public:
    // Physical constants — NAB 10.5" reel geometry
    static constexpr float REEL_RADIUS_FULL = 0.133f;  // m
    static constexpr float REEL_RADIUS_HUB  = 0.025f;  // m
    static constexpr float TAPE_THICKNESS   = 12e-6f;  // m
    static constexpr float I_REEL_EMPTY     = 0.00224f;
    static constexpr float I_REEL_FULL      = 0.00544f;
    static constexpr float SPINUP_COEFF     = 1.0f / (0.6f  * SR_F);
    static constexpr float SPINDOWN_COEFF   = 1.0f / (0.9f  * SR_F);

    // Telemetry (read by UI thread — written by audio thread; benign races OK)
    float last_instant_speed = 1.0f;
    float last_conflict      = 0.0f;
    int   dropout_count      = 0;
    float last_dropout_time  = -999.0f;

    // Motor state
    float current_motor_speed = 0.0f;
    float motor_engage        = 0.0f;
    float sticky_drag         = 0.0f;
    
    // Capstan Servo PID state
    float _capstan_integral   = 0.0f;
    float _capstan_prev_error = 0.0f;
    
    // Speed history for synchronous A/V warping (indexed by absolute play_head)
    static constexpr int SPEED_HIST_SIZE = 262144; // ~6 seconds at 44.1kHz
    float speed_history[SPEED_HIST_SIZE] = {};

    explicit TransportDynamics(uint64_t seed = 12345);
    void reset();

    TransportResult process(int frames, float current_time,
                            double play_head, int total_samples,
                            const EngineParams& p);

private:
    // Oscillator phases (seeded for reproducibility across workers)
    float _roller_phase;
    float _roller_ecc;
    float _supply_phase;

    // Fight dynamics state
    float _fight_phase        = 0.0f;
    float _fight_lurch_timer  = 0.0f;
    float _fight_lurch_mag    = 0.0f;

    // Dropout state
    float _next_dropout       = 0.0f;
    float _dropout_timer      = 0.0f;
    int   _dropout_rem        = 0;
    float _dropout_depth      = 0.0f;

    float _reel_inertia(float progress) const;
    void  _reel_geometry(float progress, float& supply_r, float& takeup_r) const;
    float _schedule_dropout(float rate);

    // Tiny xorshift64 for fast, reproducible pseudo-random numbers
    uint64_t _rng_state;
    float    _rand_uniform();   // [0, 1)
    float    _rand_normal();    // mean 0, sigma 1 (Box-Muller)
    bool     _bm_ready = false;
    float    _bm_spare = 0.0f;
};

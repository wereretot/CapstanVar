#pragma once
#include "dsp_types.hpp"
#include <array>

class ElectronicComponents {
public:
    ElectronicComponents();
    void reset();

    void process(Frame* buf, int n,
                 float current_time,
                 float speed_factor,
                 float sticky_drag,
                 const EngineParams& p);

private:
    // Head bump bandpass
    Biquad  _bump_f;
    float   _bump_fc_last = -1.0f;

    // Azimuth lowpass
    Biquad  _az_f;
    float   _az_fc_last   = -1.0f;

    // Azimuth phase delay buffer (128 samples headroom)
    static constexpr int AZ_BUF = 128;
    std::array<Frame, AZ_BUF> _az_delay_buf = {};
    float  _azimuth_state = 0.0f;
    float  _az_delay_smooth = 0.0f;  // smoothed delay to prevent inter-block clicks

    // Pink noise leaky integrator — one state per channel
    float  _pink_state[2] = {0.0f, 0.0f};

    // Diff (replay head) — last sample carried across blocks
    Frame  _diff_last = {};

    // Simple xorshift RNG (fast, no stdlib dependency in hot path)
    uint64_t _rng = 0x853c49e6748fea9bULL;
    float _rand_normal();
    float _rand_uniform();
    bool  _bm_ready = false;
    float _bm_spare = 0.0f;
};

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

    // Azimuth lowpass (legacy — only used when EngineParams::eq_curve == Legacy)
    Biquad  _az_f;
    float   _az_fc_last   = -1.0f;

    // Standard playback EQ shelves (Phase 2). Only used when eq_curve is one
    // of the non-Legacy values (NAB / IEC / AES / cassette). Each shelf is
    // re-cooked when fc or gain moves by more than ~0.5 Hz / 0.05 dB,
    // mirroring the existing biquad state-cook guards.
    Biquad  _eq_lf;                      // 50 Hz low-shelf (always 3180 µs LF tau)
    float   _eq_lf_fc_last = -1.0f;
    float   _eq_lf_db_last = -999.0f;
    Biquad  _eq_hf;                      // high-shelf at fc = 1/(2π·τ_hf)
    float   _eq_hf_fc_last = -1.0f;
    float   _eq_hf_db_last = -999.0f;
    EQCurve _eq_curve_last = EQCurve::Legacy;
    bool    _eq_in_use     = false;      // tracks whether standard EQ path was last taken

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

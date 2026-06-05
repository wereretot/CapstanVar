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
    // Head bump comb filter (Contour effect)
    static constexpr int BUMP_BUF_SIZE = 4096;
    std::array<Frame, BUMP_BUF_SIZE> _bump_delay_buf = {};
    int _bump_write_idx = 0;

    // Azimuth / Spacing lowpass
    Biquad  _az_f;
    float   _az_fc_last   = -1.0f;

    // Gap loss FIR
    static constexpr int GAP_BUF_SIZE = 512;
    std::array<Frame, GAP_BUF_SIZE> _gap_delay_buf = {};
    int _gap_write_idx = 0;

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
    
    // HiFi buzz state
    float _hifi_phase = 0.0f;
    
    // HiFi FM processing state
    float _hifi_fm_phase_l = 0.0f;    // FM phase accumulator for left channel
    float _hifi_fm_phase_r = 0.0f;    // FM phase accumulator for right channel
    float _hifi_preemph_last_l = 0.0f; // Pre-emphasis state
    float _hifi_preemph_last_r = 0.0f;
    float _hifi_deemph_last_l = 0.0f;  // De-emphasis state
    float _hifi_deemph_last_r = 0.0f;
    float _hifi_quality = 1.0f;        // Current HiFi signal quality (0-1)
    
    float _hifi_demod_phase_l = 0.0f; // Demodulator trackers
    float _hifi_demod_phase_r = 0.0f;
    
    // HiFi processing methods
    void _processHiFiAudio(Frame* buf, int n, float speed_factor, const EngineParams& p);
    void _processLinearAudio(Frame* buf, int n, float speed_factor, float sticky_drag, 
                           float current_time, const EngineParams& p);
    float _fmModulate(float audio_sample, float carrier_freq, float deviation, float& phase);
    float _fmDemodulate(float fm_sample, float& last_phase);
};

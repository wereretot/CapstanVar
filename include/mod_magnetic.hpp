#pragma once
#include "dsp_types.hpp"
#include <vector>
#include <span>

class MagneticPath {
public:
    MagneticPath();
    void reset();

    // Process one block in-place.
    // audio_data / read_indices allow print-through look-ahead into the full tape.
    void process(Frame* buf, int n,
                 std::span<const Frame> full_audio,
                 std::span<const double> read_indices,
                 const EngineParams& p);

    // Oversampled saturation only (called from engine when OS > 1).
    // Phase 3: saturate_only delegates to _saturate_bandwise, which shares
    // identical math with MagneticPath::process's §3 path. oversample
    // selects the effective sample rate (1× = base SR, 4× = 176.4 kHz).
    void saturate_only(Frame* buf, int n, const EngineParams& p, int oversample = 1);

private:
    // Cross-block state
    Frame  _last_proc  = {};
    Frame  _last_bark  = {};

    // Demagnetisation filter (2-pole Butterworth lowpass)
    Biquad _demag_fwd;   // causal forward direction
    Biquad _demag_rev;   // causal reverse direction
    float  _demag_fc_last = -1.0f; // detect coefficient change

    // 4-band LR-4 crossover state — Phase 3 magnetic saturator.
    // Linkwitz-Riley 4th-order is constructed by cascading two 2-pole
    // Butterworth filters; for each split frequency we carry a stereo
    // 2-pole pair on each side (LP and HP), so 3 splits × 2 sides × 2
    // biquads = 12 Biquad instances per channel handled.
    Biquad _lpf1[2];   // 200  Hz split — LR-4 LPF cascade pair
    Biquad _hpf1[2];   // 200  Hz split — LR-4 HPF cascade pair
    Biquad _lpf2[2];   // 1500 Hz split — LR-4 LPF cascade pair
    Biquad _hpf2[2];   // 1500 Hz split — LR-4 HPF cascade pair
    Biquad _lpf3[2];   // 6000 Hz split — LR-4 LPF cascade pair
    Biquad _hpf3[2];   // 6000 Hz split — LR-4 HPF cascade pair

    // Per-band magnetisation state for the Preisach-blended saturator.
    // _m_prev[band][ch] feeds the LUT lookup at sample i+1. Reset to
    // 0 on oxide change and re-converged by the 80-sample spin-up loop.
    float  _m_prev[4][2] = {{0,0},{0,0},{0,0},{0,0}};

    // Crossover-cook tracking — only recompute when the effective sample
    // rate changes (base SR vs. 2×/4× oversample path).
    float  _crossover_sr_last = -1.0f;

    // Spin-up tracking — detect oxide and driving changes so we re-run
    // the 80-sample silent pulse before audio passes through.
    std::string _oxide_last      = "";
    float       _drive_bias_last = 0.0f;

    // Bandwise magnetic saturation: 4-band LR-4 cascade + per-band satu-
    // rator, with each band blending fast_tanh ↔ PreisachLUT by the
    // oxide's hysteresis_amt. sr_F is the effective sample rate of buf.
    void _saturate_bandwise(Frame* buf, int n, const EngineParams& p, float sr_F);

    void _update_demag(float demag_param, float fc_hz);
};

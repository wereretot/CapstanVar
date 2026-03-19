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

    // Oversampled saturation only (called from engine when OS > 1)
    void saturate_only(Frame* buf, int n, const EngineParams& p);

private:
    // Cross-block state
    Frame  _last_proc  = {};
    Frame  _last_bark  = {};

    // Demagnetisation filter (2-pole Butterworth lowpass)
    Biquad _demag_fwd;   // causal forward direction
    Biquad _demag_rev;   // causal reverse direction
    float  _demag_fc_last = -1.0f; // detect coefficient change

    void _update_demag(float demag_param, float fc_hz);
};

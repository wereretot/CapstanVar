#pragma once
#include "dsp_types.hpp"
#include "mod_transport.hpp"
#include "mod_magnetic.hpp"
#include "mod_electronics.hpp"

#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <string>
#include <functional>

class TapeEngine {
public:
    // ── Audio data (loaded from file) ─────────────────────────────────────────
    std::vector<Frame> audio_data;
    int                total_samples = 0;

    // ── Playback state ────────────────────────────────────────────────────────
    double      play_head    = 0.0;
    float       current_time = 0.0f;
    bool        is_reversed  = false;
    std::atomic<bool> is_playing{false};

    // ── Shared params (written by UI, read by audio thread) ───────────────────
    mutable std::mutex lock;
    EngineParams       params;

    // ── DSP modules ───────────────────────────────────────────────────────────
    TransportDynamics  transport;
    MagneticPath       magnetic;
    ElectronicComponents electronics;

    explicit TapeEngine(uint64_t seed = 0);

    // Load a WAV/FLAC/AIFF file via libsndfile; normalises to ±1.0 at 44100 Hz stereo
    bool load_file(const std::string& path);

    // Flip audio_data in-place; resets DSP state if direction changed
    void set_reverse(bool want_reverse, bool force_reset = false);

    // Reset all DSP state (play_head → 0, IIR filters, motor ramp)
    void reset_state();    // resets DSP filters only, preserves play_head
    void reset_position(); // seeks to position 0
    void trigger_fade_in(int samples = 512) { _fade_in = samples; }

    // ── Main DSP block processor ──────────────────────────────────────────────
    // Returns false when tape reaches end.
    // oversample: 1=normal, 2/4/8=oversampled nonlinear stages
    bool dsp_process(Frame* out, int frames, int oversample = 1);

    // ── Worker factory for parallel render ───────────────────────────────────
    std::unique_ptr<TapeEngine> make_worker(int start_sample, int block_size,
                                            int oversample, uint64_t shared_seed,
                                            int n_warmup_blocks = 16);

    // Progress callback for render (0.0–1.0, called from render thread)
    std::function<void(float)> on_render_progress;

private:
    // Oversampling context — last OS_PAD samples of previous block
    static constexpr int OS_PAD = 64;
    std::vector<Frame> _os_context;
    bool               _os_context_valid = false;

    // Post-preset fade-in
    int   _fade_in    = 0;
    Frame _last_out   = {};

    // Scrape flutter state
    float _scrape_phase = 0.0f;
    float _scrape_y_prev = 0.0f;
    float _scrape_x_prev = 0.0f;

    // Oversampled saturation helper
    void _saturate_oversampled(Frame* buf, int frames, int oversample,
                               const EngineParams& p);
};

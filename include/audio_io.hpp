#pragma once
#include "engine.hpp"
#include <atomic>
#include <thread>

// ── Transport state machine ───────────────────────────────────────────────────
// A single signed _tape_speed value drives everything:
//   0      = stopped / braking
//  +1      = play forward (normal speed)
//  -1      = play reverse (normal speed)
//  +N/-N   = shuttle forward/reverse at N× tape speed
//
// This avoids the independent capstan/direction bug where separate speed
// and direction ramps multiply together to create false speed spikes.

enum class TransportMode { Stopped, Playing, Shuttle };

class AudioIO {
public:
    explicit AudioIO(TapeEngine& engine);
    ~AudioIO();

    bool open();
    void close();

    void play_forward();
    void play_reverse();
    void stop();
    void shuttle_rewind(float speed_mult = 40.f);
    void shuttle_ff    (float speed_mult = 40.f);
    void shuttle_faster(bool reverse);  // increase shuttle speed
    void stop_shuttle();
    void cycle_shuttle_speed();
    
    float shuttle_speed() const { return std::abs(_target_speed.load()); }  // current shuttle speed

    bool  is_open()      const { return _open_flag.load(); }
    bool  is_playing()   const;
    bool  is_stopped()   const;
    bool  is_rewinding() const;
    bool  is_ffing()     const;
    float current_speed_mult() const { return _current_speed_mult.load(); }
    float signed_tape_speed()  const { return _signed_tape_speed.load(); }

private:
    TapeEngine& _engine;

    // The single signed target speed — UI thread writes, DSP thread reads.
    std::atomic<float> _target_speed{0.f};

    std::atomic<bool>  _open_flag{false};
    std::thread        _thread;
    void               _dsp_thread();

    // Inertia state — owned by DSP thread only (no atomic needed)
    float _tape_speed  = 0.f;   // actual current signed speed
    float _squeal_phase= 0.f;

    // For reel animation — written by DSP thread
    std::atomic<float> _current_speed_mult{0.f};
    std::atomic<float> _signed_tape_speed  {0.f};  // signed: + = fwd, - = rev

    void* _backend = nullptr;
    bool  _open_device();
    void  _close_device();
    bool  _write_block(const float* stereo, int frames);

    // ── Physics constants (per sample) ───────────────────────────────────────
    // BRAKE_RATE:   stop from any speed in ~0.15s (emergency brake feel)
    // PLAY_SPINUP:  0→1 in 0.6s (realistic capstan motor)
    // SHUT_UP:      0→40 in 0.25s (fast shuttle motor)
    static constexpr float BRAKE_RATE  = 1.f / (0.15f * SR_F);
    static constexpr float PLAY_SPINUP = 1.f / (0.60f * SR_F);
    static constexpr float SHUT_UP     = 1.f / (0.25f * SR_F);
};

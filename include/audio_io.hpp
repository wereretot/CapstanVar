#pragma once
#include "engine.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

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

    // ── VU Metering — peak levels with decay ─────────────────────────────────
    float get_level_left()  const { return _level_left.load(); }
    float get_level_right() const { return _level_right.load(); }
    int64_t get_hardware_delay() const { return _hardware_delay.load(); }

    // ── VU Meter response speed — 0=slow, 1=medium, 2=fast ───────────────────
    void set_vu_response(int response) { _vu_response.store(response); }
    int  get_vu_response() const { return _vu_response.load(); }

    // ── Capture callback — receives processed interleaved audio ─────────────────
    // Passes processed stereo interleaved samples to a callback for export/sync
    void set_capture_callback(std::function<void(const std::vector<float>&)> cb) { _capture_callback = std::move(cb); }

private:
    TapeEngine& _engine;

    // ── VU Meter state — written by DSP thread, read by UI ───────────────────
    std::atomic<float> _level_left{0.f};
    std::atomic<float> _level_right{0.f};
    std::atomic<int64_t> _hardware_delay{0};
    float _level_peak_l = 0.f;  // DSP thread only
    float _level_peak_r = 0.f;
    int   _level_decay_cnt = 0;
    std::atomic<int> _vu_response{1};  // 0=slow, 1=medium, 2=fast
    std::function<void(const std::vector<float>&)> _capture_callback;

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

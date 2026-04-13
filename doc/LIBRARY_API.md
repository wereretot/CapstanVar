# CapstanVar Library API Documentation

A high-fidelity analog tape warping DSP engine, designed for realtime integration into audio applications.

---

## Table of Contents

1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Core Classes](#core-classes)
   - [TapeEngine](#tapeengine)
   - [AudioIO](#audioio)
   - [StreamBuffer](#streambuffer)
   - [PresetManager](#presetmanager)
4. [Data Types](#data-types)
   - [EngineParams](#engineparams)
   - [Frame](#frame)
   - [DSP Utilities](#dsp-utilities)
5. [Parameter Animation System](#parameter-animation-system)
6. [Error Logging](#error-logging)
7. [Thread Safety Model](#thread-safety-model)
8. [Offline Rendering](#offline-rendering)
9. [Performance Tuning](#performance-tuning)
10. [Built-in RNG and Reproducibility](#built-in-rng-and-reproducibility)
11. [Constants and Defaults](#constants-and-defaults)

---

## Overview

CapstanVar simulates the full signal chain of analog tape recording:

```
Input Gain → Transport (wow/flutter) → Magnetic Path (saturation, bias)
→ Electronics (head bump, azimuth, noise) → Output
```

The library operates in two modes:

- **Realtime playback** — `TapeEngine` + `AudioIO` for live, low-latency processing
- **Offline rendering** — worker engines for fast, parallel batch processing

All DSP runs at 44.1 kHz stereo. Parameters are adjustable in realtime via the `EngineParams` struct, guarded by a mutex for safe UI thread access.

---

## Quick Start

### Minimal Realtime Playback

```cpp
#include "engine.hpp"
#include "audio_io.hpp"

// 1. Create the engine
TapeEngine engine;

// 2. Load an audio file
if (!engine.load_file("source.wav")) {
    // Check error_log.hpp for details
    return -1;
}

// 3. Open the audio device
AudioIO audio(engine);
if (!audio.open()) return -1;

// 4. Start playback
audio.play_forward();

// 5. Modify parameters from any thread
{
    std::lock_guard<std::mutex> lock(engine.lock);
    engine.params.wow_dep = 0.5f;
    engine.params.drive   = 2.0f;
    engine.params.hiss    = 0.005f;
}

// 6. Stop and clean up
audio.stop();
audio.close();
```

### Minimal Offline Render

```cpp
#include "engine.hpp"

TapeEngine engine;
engine.load_file_for_render("source.wav");  // Loads fully into RAM

// Process block-by-block
std::vector<Frame> output(engine.total_samples);
int offset = 0;
Frame block[1024];

while (offset < engine.total_samples) {
    int n = std::min(1024, engine.total_samples - offset);
    engine.dsp_process(block, n);
    std::copy(block, block + n, output.begin() + offset);
    offset += n;
}
```

---

## Core Classes

### TapeEngine

The central DSP processing engine. Manages all three simulation stages (transport, magnetic, electronics) and holds playback state.

#### Constructor

```cpp
explicit TapeEngine(uint64_t seed = 0);
```

| Parameter | Description |
|-----------|-------------|
| `seed` | RNG seed for reproducible stochastic effects (wow, flutter, dropouts). `0` = auto-generate from `std::random_device`. |

#### File Loading

```cpp
bool load_file(const std::string& path);
bool load_file_for_render(const std::string& path);
```

| Method | Behavior | Memory |
|--------|----------|--------|
| `load_file()` | Opens `StreamBuffer` — streams audio from disk on demand. **Use for realtime playback.** | Ring buffer only (~10 MB default) |
| `load_file_for_render()` | Decodes entire file into `audio_data` vector. **Use for offline rendering.** | Full file in RAM |

Both return `false` on failure. Errors are posted to the global `ErrorLog`.

#### Playback Control

```cpp
void set_reverse(bool want_reverse, bool force_reset = false);
void reset_state();     // Resets DSP filters, preserves play_head
void reset_position();  // Seeks to position 0
void trigger_fade_in(int samples = 512);
```

| Method | Description |
|--------|-------------|
| `set_reverse()` | Flip audio data in-place for reverse playback. Resets DSP state if direction actually changed. |
| `reset_state()` | Zero all IIR filter states and DSP history. Play head position unchanged. |
| `reset_position()` | Set `play_head` back to 0. Does not reset filters. |
| `trigger_fade_in()` | Apply a smooth fade-in over `samples` frames after a parameter change. Prevents clicks. |

#### DSP Processing

```cpp
bool dsp_process(Frame* out, int frames, int oversample = 1);
```

Processes `frames` samples into `out`. Returns `false` when the tape reaches the end.

| Parameter | Description |
|-----------|-------------|
| `out` | Output buffer (must hold at least `frames` Frame objects) |
| `frames` | Number of stereo frames to process |
| `oversample` | Oversampling factor for nonlinear stages: `1` = normal, `2/4/8` = oversampled |

**Returns:** `true` if processing continued, `false` if end-of-tape was reached.

#### Worker Factory (for Parallel Rendering)

```cpp
std::unique_ptr<TapeEngine> make_worker(
    int start_sample,
    int block_size,
    int oversample,
    uint64_t shared_seed,
    int n_warmup_blocks = 16
);
```

Creates a copy of the engine configured for parallel offline rendering. The worker processes a sub-range of the tape starting at `start_sample`.

| Parameter | Description |
|-----------|-------------|
| `start_sample` | First sample this worker should process |
| `block_size` | Frames per `dsp_process()` call for this worker |
| `oversample` | Oversampling factor |
| `shared_seed` | RNG seed (shared across all workers for coherence) |
| `n_warmup_blocks` | Blocks of warmup processing to discard (settles IIR filters) |

**Example: Multi-threaded render**

```cpp
int total = engine.total_samples;
int threads = 4;
int block = total / threads;

std::vector<std::vector<Frame>> results(threads);
std::vector<std::thread> pool;

for (int t = 0; t < threads; ++t) {
    int start = t * block;
    int size  = (t == threads - 1) ? (total - start) : block;
    auto worker = engine.make_worker(start, size, 1, 42);
    
    pool.emplace_back([&results, t, size, w = std::move(worker)]() {
        results[t].resize(size);
        worker->dsp_process(results[t].data(), size);
    });
}

for (auto& th : pool) th.join();
```

#### Progress Callback

```cpp
std::function<void(float)> on_render_progress;
```

Called during offline rendering with values from `0.0` to `1.0`. Set before calling `dsp_process` on a worker.

#### Public Members

| Member | Type | Description |
|--------|------|-------------|
| `stream` | `StreamBuffer` | Ring-buffered audio streamer (for realtime mode) |
| `audio_data` | `std::vector<Frame>` | Fully loaded audio (for offline render mode only) |
| `total_samples` | `int` | Total frames in loaded file |
| `play_head` | `double` | Current read position in samples |
| `current_time` | `float` | Current time in seconds |
| `is_reversed` | `bool` | Reverse playback flag |
| `is_playing` | `std::atomic<bool>` | Playback running flag |
| `params` | `EngineParams` | All DSP parameters (guarded by `lock`) |
| `lock` | `mutable std::mutex` | Protects `params` for UI-thread writes |
| `transport` | `TransportDynamics` | Transport simulation module |
| `magnetic` | `MagneticPath` | Magnetic path simulation module |
| `electronics` | `ElectronicComponents` | Electronics simulation module |

---

### AudioIO

Audio device management and transport state machine. Runs a background DSP thread that reads from `TapeEngine` and writes to the system audio device.

#### Constructor

```cpp
explicit AudioIO(TapeEngine& engine);
~AudioIO();
```

Holds a reference to the engine. The engine must outlive the `AudioIO` instance.

#### Device Control

```cpp
bool open();
void close();
```

Opens/closes the system audio device. Returns `false` if the device cannot be opened.

#### Transport Controls

```cpp
void play_forward();           // Normal forward playback
void play_reverse();           // Reverse playback
void stop();                   // Emergency brake (~0.15s stop)
void shuttle_rewind(float speed_mult = 40.f);   // Fast rewind
void shuttle_ff(float speed_mult = 40.f);        // Fast forward
void shuttle_faster(bool reverse);               // Increase shuttle speed
void stop_shuttle();                             // Return to normal play speed
void cycle_shuttle_speed();                      // Cycle through preset shuttle speeds
```

**Transport Mode Model:**

The transport uses a single signed `_tape_speed` value:

| Speed Value | Mode | Behavior |
|-------------|------|----------|
| `0` | Stopped | Brake applied |
| `+1` | Play forward | Normal speed |
| `-1` | Play reverse | Normal speed |
| `+N` | Shuttle forward | N× fast forward |
| `-N` | Shuttle reverse | N× fast rewind |

This unified model avoids speed spikes that occur when independent capstan speed and direction ramps multiply together.

#### State Queries

```cpp
bool  is_open()      const;   // Device is open
bool  is_playing()   const;   // Tape is moving (any mode)
bool  is_stopped()   const;   // Tape is stationary
bool  is_rewinding() const;   // Shuttle reverse active
bool  is_ffing()     const;   // Shuttle forward active

float shuttle_speed()        const;  // Current shuttle multiplier
float current_speed_mult()   const;  // Absolute tape speed
float signed_tape_speed()    const;  // Signed: + = fwd, - = rev
```

#### VU Metering

```cpp
float get_level_left()  const;   // Peak level, left channel (0.0–1.0+)
float get_level_right() const;   // Peak level, right channel (0.0–1.0+)

void set_vu_response(int response);  // 0=slow, 1=medium, 2=fast
int  get_vu_response() const;
```

Peak levels with decay. Written by the DSP thread, read atomically by the UI thread.

#### Physics Constants

Internal ramp rates (per sample):

| Constant | Value | Description |
|----------|-------|-------------|
| `BRAKE_RATE` | `1 / (0.15 * 44100)` | Emergency stop rate |
| `PLAY_SPINUP` | `1 / (0.60 * 44100)` | Normal play motor ramp |
| `SHUT_UP` | `1 / (0.25 * 44100)` | Shuttle motor ramp |

---

### StreamBuffer

Lock-free ring-buffered audio streamer. Keeps a sliding window of decoded audio around the play position so the DSP thread never needs the full file in RAM.

#### Configuration (set before `open()`)

```cpp
StreamBuffer buf;
buf.ring_frames     = 30 * 44100;   // Ring buffer size (~10 MB default)
buf.ahead_frames    = 10 * 44100;   // Read-ahead target
buf.io_chunk_frames = 4096;          // Frames per I/O read
```

| Field | Default | Description |
|-------|---------|-------------|
| `ring_frames` | 1,323,000 (30s) | Total ring capacity. Larger = tolerates slower drives. ~21 MB/min stereo. |
| `ahead_frames` | 441,000 (10s) | How far ahead the I/O thread pre-fills. Increase for slow HDDs. |
| `io_chunk_frames` | 4096 | Frames decoded per I/O wakeup. Smaller = lower latency; larger = better throughput. |

#### Methods

```cpp
bool open(const std::string& path);   // Open audio file, start I/O thread
void close();                          // Stop I/O thread, release resources
bool is_open() const;                  // True if file is loaded
Frame read(double pos) const;          // Read at position with Catmull-Rom interpolation
void notify_position(int file_pos, bool reversed = false);  // Called by DSP thread
size_t ram_bytes() const;              // Current ring buffer memory usage
int valid_start() const;               // First valid sample in ring
int valid_end()   const;               // One past last valid sample in ring
```

#### Read Behavior

- Uses **Catmull-Rom cubic Hermite** interpolation (4 taps, C1 continuous)
- If the requested window is not yet in the ring, returns the last valid sample (sample-and-hold to prevent clicks) and signals the I/O thread to seek
- Underruns are logged to stderr (first one + every 1000th)

#### File Info (read-only after open)

```cpp
std::string _file_path;    // Loaded file path
int  total_samples;        // Total frames
int  sample_rate;          // Sample rate (Hz)
int  channels;             // Channel count
float norm_gain;           // Auto-normalization gain (peak → 0.707)
```

---

### PresetManager

Manages factory presets and user-saved configurations. Serializes `EngineParams` to/from JSON.

```cpp
PresetManager mgr;
```

#### Built-in Presets

```cpp
const std::vector<Preset>& builtin_presets() const;
std::optional<Preset>      find_builtin(const std::string& name) const;
```

Factory presets included:
- **Default** — neutral settings
- **Tape Wow & Flutter** — heavy speed modulation
- **Saturated Push** — high drive, elevated bias
- **Old Cassette** — degraded transport, elevated noise
- **Studio Machine** — tight transport, warm saturation
- **Broken Capstan** — extreme motor degradation
- **Dying Tape** — high dropouts, oxide shedding
- **Hiss Paradise** — elevated noise floor, colored hiss
- **Print Through Echo** — noticeable pre/post echo
- **Heavy Crosstalk** — inter-channel bleeding

#### Session Presets

```cpp
void save_session(const std::string& name, const EngineParams& p);
const std::vector<Preset>& session_presets() const;
std::optional<Preset>      find_session(const std::string& name) const;
```

User-saved presets during the current session. Persist these to disk with `export_preset()`.

#### File I/O

```cpp
bool export_preset(const std::string& path, const std::string& name,
                   const EngineParams& p) const;
std::optional<Preset> import_preset(const std::string& path) const;
```

Exports/imports `.cvpr` JSON files.

#### Default Parameters

```cpp
static EngineParams default_params();
```

Returns a flat `EngineParams` with all controls at neutral positions.

---

## Data Types

### EngineParams

All DSP parameters in one flat struct. Written by the UI thread (under `engine.lock`), read by the audio thread.

#### Input Stage

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `input_gain` | `1.0f` | 0 – 2.0 | Input volume/trim. `1.0` = unity. |

#### Transport (Wow, Flutter, Dropouts)

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `ips_base` | `15.0f` | 0.5 – 30 | Base tape speed in inches per second. |
| `motor_health` | `0.0f` | 0 – 10 | Motor degradation. Higher = more speed instability. |
| `motor_drag` | `0.0f` | 0 – 0.9 | Low-frequency motor slowdown. |
| `motor_boost` | `0.0f` | 0 – 0.9 | High-frequency motor agitation. |
| `wow_dep` | `0.2f` | 0 – 30 | Wow (slow speed variation) depth. |
| `flutter_dep` | `0.05f` | 0 – 10 | Flutter (fast speed variation) depth. |
| `scrape_flutter` | `0.1f` | 0 – 1 | Tape-to-head scraping noise. |
| `tension_load` | `0.01f` | 0 – 0.12 | Tape tension-induced speed variation. |
| `dropout_rate` | `0.0f` | 0 – 1 | Frequency of signal dropouts. |

#### Magnetic Path (Saturation, Bias, Degradation)

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `drive` | `1.2f` | 1 – 20 | Tape saturation drive. Higher = more harmonic distortion. |
| `bias` | `1.0f` | 0.5 – 3 | AC bias level. Affects high-frequency response and distortion character. |
| `replay_diff` | `0.3f` | 0 – 1 | Replay head differentiation (channel separation). |
| `asperities` | `0.0f` | 0 – 0.5 | Surface roughness noise (micro-contact noise). |
| `barkhausen` | `0.0f` | 0 – 0.1 | Barkhausen noise (magnetic domain jumps). |
| `crosstalk` | `0.0f` | 0 – 0.5 | Inter-channel electromagnetic leakage. |
| `print_through` | `0.0f` | 0 – 0.1 | Magnetic print-through (pre/post echo). |
| `demagnetization` | `0.0f` | 0 – 0.99 | Gradual signal loss from magnetic decay. |
| `oxide_shedding` | `0.0f` | 0 – 1 | Oxide layer flaking off the tape. |

#### Electronics (Noise, Filtering, Artifacts)

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `hiss` | `0.001f` | 0 – 0.02 | Tape hiss noise floor. |
| `hiss_color` | `0.0f` | 0 – 1 | Hiss spectral tilt. 0 = white, 1 = pink-leaning. |
| `mains_hum` | `0.0f` | 0 – 0.05 | 50/60 Hz mains hum pickup. |
| `cutoff_base` | `18000.0f` | 500 – 22000 | Electronics lowpass cutoff frequency (Hz). |
| `head_bump` | `0.5f` | 0 – 5 | Low-frequency head bump resonance. |
| `azimuth_drift` | `0.05f` | 0 – 1 | Head azimuth angle drift (HF loss, phase smear). |
| `sticky_shed` | `0.0f` | 0 – 1 | Sticky-shed syndrome drag (hydrolyzed binder). |

#### Meta Parameters (not exposed as UI sliders)

| Field | Default | Description |
|-------|---------|-------------|
| `oxide_type` | `"Fe2O3"` | Oxide preset key: `"Fe2O3"`, `"CrO2"`, `"Metal"`, `"FeCo"` |
| `is_reversed` | `false` | Reverse playback flag |
| `motor_engage` | `1.0f` | Capstan engagement factor (0 = disengaged) |
| `tape_speed_mult` | `1.0f` | Actual read stride multiplier |
| `presaturated` | `false` | Pre-saturation mode flag |

### Frame

Stereo sample pair with arithmetic operators.

```cpp
struct Frame {
    float l = 0.0f;
    float r = 0.0f;

    Frame  operator+(const Frame& o) const;
    Frame  operator*(float s)        const;
    Frame& operator+=(const Frame& o);
    Frame& operator*=(float s);
};
```

### DSP Utilities

#### Biquad (2-pole IIR Filter)

```cpp
struct Biquad {
    float b[3] = {1,0,0};   // Feedforward coefficients
    float a[3] = {1,0,0};   // Feedback coefficients
    float z1[2] = {0,0};    // Delay line state 1 (per channel)
    float z2[2] = {0,0};    // Delay line state 2 (per channel)

    void reset();                        // Clear delay lines
    float tick(float x, int ch);         // Process one sample, one channel
    void process(Frame* buf, int n);     // Process block of stereo frames
};
```

Transposed Direct Form II. Used internally by the magnetic and electronics modules.

#### Coefficient Calculators

```cpp
void butter_lp(float fc_norm, Biquad& bq);
```

2-pole Butterworth lowpass. `fc_norm` = normalized cutoff (0–1, where 1 = Nyquist).

```cpp
void butter_bp(float low_norm, float high_norm, Biquad& bq);
```

2-pole Butterworth bandpass.

#### DC Block (1-pole Highpass)

```cpp
struct DC_Block {
    float x1[2]={0,0}, y1[2]={0,0};
    float R = 0.9997f;

    float tick(float x, int ch);
    void process(Frame* buf, int n);
    void reset();
};
```

Removes DC offset. `R` controls the pole position (closer to 1 = gentler).

#### Interpolation

```cpp
Frame cubic_interp(const Frame* data, int N, double pos);
```

Catmull-Rom cubic Hermite interpolation. 4 taps, zero latency, C1 continuous.

#### Fast Math

```cpp
float fast_tanh(float x);   // Padé approximation, <0.5% error for |x| < 3
float lerp(float a, float b, float t);  // Linear interpolation
float clamp01(float x);     // Clamp to [0, 1]
```

#### Oxide Presets

```cpp
struct OxideProps {
    float Hc;          // Coercivity (Oersted)
    float Ms;          // Saturation magnetization (normalized)
    float bias_trim;   // Optimal bias multiplier
};

const std::unordered_map<std::string, OxideProps>& oxide_presets();
```

| Preset | Hc (Oe) | Ms | bias_trim |
|--------|---------|-----|-----------|
| `Fe2O3` | 250 | 1.0 | 1.00 |
| `CrO2` | 480 | 1.2 | 1.35 |
| `Metal` | 1400 | 1.8 | 1.70 |
| `FeCo` | 700 | 1.4 | 1.40 |

---

## Parameter Animation System

Blender-style keyframe curves for all `EngineParams` float fields.

### Keyframe

```cpp
struct Keyframe {
    double time;    // Position in samples
    float  value;
    float  tan_l = 0.f;  // Left bezier tangent
    float  tan_r = 0.f;  // Right bezier tangent
};
```

### AnimCurve

One animated parameter. Holds a sorted list of `Keyframe` objects.

```cpp
struct AnimCurve {
    std::string param_id;             // Identifier (e.g. "wow", "drv")
    std::string label;                // Display name
    std::vector<Keyframe> keys;       // Sorted by time
    bool enabled = true;

    int  insert(double time, float value);             // Add/update keyframe
    void remove(int idx);                               // Remove keyframe
    std::optional<float> evaluate(double t) const;      // Catmull-Rom eval
};
```

**Evaluation behavior:**
- 0 keys → `nullopt`
- 1 key → constant value
- Before first key → first key value (clamp)
- After last key → last key value (clamp)
- Catmull-Rom overshoot is clamped to the parameter's valid range

### ParamAnim

Full animation data for one session. Holds all curves and produces parameter overrides.

```cpp
struct ParamAnim {
    std::unordered_map<std::string, AnimCurve> curves;
    bool enabled = true;

    bool has_keys(const std::string& id) const;
    void insert_key(const std::string& id, const std::string& label,
                    double time_samples, float value);
    void delete_key(const std::string& id, int key_idx);
    void clear_curve(const std::string& id);
    void clear_all();

    // Apply all curves to params at time_samples
    void apply(EngineParams& p, double time_samples) const;

    json to_json() const;
    void from_json(const json& j);
};
```

#### Animated Parameters

All 26 float fields of `EngineParams` are animatable. The curve ID mapping:

| Curve ID | Parameter Field | Range |
|----------|----------------|-------|
| `ips` | `ips_base` | 0.5 – 30 |
| `mh` | `motor_health` | 0 – 10 |
| `mdrag` | `motor_drag` | 0 – 0.9 |
| `mboost` | `motor_boost` | 0 – 0.9 |
| `wow` | `wow_dep` | 0 – 30 |
| `flt` | `flutter_dep` | 0 – 10 |
| `scr` | `scrape_flutter` | 0 – 1 |
| `tens` | `tension_load` | 0 – 0.12 |
| `drop` | `dropout_rate` | 0 – 1 |
| `drv` | `drive` | 1 – 20 |
| `bias` | `bias` | 0.5 – 3 |
| `rd` | `replay_diff` | 0 – 1 |
| `asp` | `asperities` | 0 – 0.5 |
| `bark` | `barkhausen` | 0 – 0.1 |
| `xtk` | `crosstalk` | 0 – 0.5 |
| `prt` | `print_through` | 0 – 0.1 |
| `dmg` | `demagnetization` | 0 – 0.99 |
| `shed` | `oxide_shedding` | 0 – 1 |
| `hiss` | `hiss` | 0 – 0.02 |
| `hcol` | `hiss_color` | 0 – 1 |
| `hum` | `mains_hum` | 0 – 0.05 |
| `cut` | `cutoff_base` | 500 – 22000 |
| `bump` | `head_bump` | 0 – 5 |
| `azdrift` | `azimuth_drift` | 0 – 1 |
| `sticky` | `sticky_shed` | 0 – 1 |

#### Usage

```cpp
ParamAnim anim;

// Insert keyframes
anim.insert_key("wow", "Wow Depth", 0.0, 0.2f);       // Start: low wow
anim.insert_key("wow", "Wow Depth", 44100.0, 5.0f);   // 1 sec: heavy wow
anim.insert_key("wow", "Wow Depth", 88200.0, 0.2f);    // 2 sec: back to low

// Evaluate at a specific time
EngineParams params;
anim.apply(params, 44100);  // params.wow_dep is now ~5.0

// Serialize
json j = anim.to_json();
// ... save to file ...

// Deserialize
anim.from_json(j);
```

---

## Error Logging

Thread-safe centralized error log. All subsystems post errors here; the UI polls and displays them.

```cpp
ErrorLog& log = ErrorLog::get();  // Singleton
```

### Error Codes

Grouped by subsystem prefix:

| Prefix | Subsystem |
|--------|-----------|
| `AUDIO_*` | Audio device and DSP thread |
| `FILE_*` | Audio file loading |
| `RENDER_*` | Offline render engine |
| `PROJECT_*` | Project save/load |
| `PRESET_*` | Preset import/export |
| `ENGINE_*` | DSP engine internals |

### Severity Levels

| Severity | Icon | Meaning |
|----------|------|---------|
| `Info` | ℹ | Informational |
| `Warning` | ⚠ | Non-critical, doesn't stop operation |
| `Error` | ✖ | Critical failure |

### Posting Errors

```cpp
#include "error_log.hpp"

// Via macro (posts to stderr + error log):
CV_ERR(FILE_OPEN_FAILED, "myfile.wav: permission denied");

// Direct post:
ErrorLog::get().post(ErrCode::AUDIO_DEVICE_OPEN_FAILED,
                      "ALSA: device busy",
                      ErrSeverity::Error);
```

### UI Integration

```cpp
// Check if new errors exist
if (ErrorLog::get().has_new()) {
    auto entries = ErrorLog::get().snapshot();  // Thread-safe copy
    for (auto& e : entries) {
        if (!e.dismissed) {
            // Display: e.timestamp, err_code_str(e.code), e.message
        }
    }
    ErrorLog::get().clear_new_flag();
}

// Dismiss all
ErrorLog::get().dismiss_all();
```

---

## Thread Safety Model

CapstanVar uses a **multi-writer, single-reader** model with clear ownership:

### `TapeEngine::params` + `TapeEngine::lock`

| Thread | Role |
|--------|------|
| **UI thread** | Writes `engine.params` under `std::lock_guard<std::mutex>` |
| **Audio thread** | Reads `params` without locking (atomic snapshots at block boundaries) |

```cpp
// UI thread — always lock:
{
    std::lock_guard<std::mutex> lock(engine.lock);
    engine.params.wow_dep = 0.5f;
}
```

### `std::atomic<bool>` Members

These are lock-free and safe to read/write from any thread:

- `TapeEngine::is_playing`
- `AudioIO` VU meter levels (`_level_left`, `_level_right`)
- `AudioIO` speed values (`_target_speed`, `_current_speed_mult`, `_signed_tape_speed`)
- `StreamBuffer` position and seek hints

### `StreamBuffer`

| Thread | Access Pattern |
|--------|----------------|
| **I/O thread** | Writes ring buffer slots (one writer per slot) |
| **DSP thread** | Reads ring buffer slots via `read()` (one reader) |

Ring slot writes are atomic at the `Frame` level. No mutex is used — validity is determined by `_valid_start`/`_valid_end` bounds.

### `AudioIO` Internal State

The `_tape_speed`, `_squeal_phase`, and inertia variables are owned exclusively by the DSP thread (no atomics or locks needed internally).

### ErrorLog

Mutex-protected singleton. Any thread can `post()`. UI thread calls `snapshot()` for a copy.

---

## Offline Rendering

The library supports two rendering modes:

### Sequential Rendering

```cpp
TapeEngine engine;
if (!engine.load_file_for_render("input.wav")) return false;

std::vector<Frame> output(engine.total_samples);
int offset = 0;
Frame block[BLOCK_SIZE];

while (offset < engine.total_samples) {
    int n = std::min(BLOCK_SIZE, engine.total_samples - offset);
    if (!engine.dsp_process(block, n)) break;  // false = end of tape
    std::copy(block, block + n, output.begin() + offset);
    offset += n;
}
```

### Parallel Rendering with Workers

```cpp
int threads = std::thread::hardware_concurrency();
int total = engine.total_samples;
int block_size = total / threads;

struct Job {
    int start, size;
    std::unique_ptr<TapeEngine> worker;
    std::vector<Frame> output;
};

std::vector<Job> jobs(threads);
std::vector<std::thread> pool;

for (int t = 0; t < threads; ++t) {
    jobs[t].start = t * block_size;
    jobs[t].size  = (t == threads - 1) ? (total - jobs[t].start) : block_size;
    jobs[t].worker = engine.make_worker(jobs[t].start, jobs[t].size, 1, 42);

    pool.emplace_back([&jobs, t]() {
        auto& j = jobs[t];
        j.output.resize(j.size);
        j.worker->dsp_process(j.output.data(), j.size);
    });
}

for (auto& th : pool) th.join();

// Concatenate results
std::vector<Frame> final_output(total);
int write_pos = 0;
for (auto& j : jobs) {
    std::copy(j.output.begin(), j.output.end(), final_output.begin() + write_pos);
    write_pos += j.output.size();
}
```

### Oversampled Rendering

```cpp
Frame block[BLOCK_SIZE];
engine.dsp_process(block, BLOCK_SIZE, /* oversample= */ 4);
```

Nonlinear stages (saturation, bias) are processed at 4× the sample rate for improved alias rejection. Uses an oversampling context internally (`OS_PAD = 64` samples of overlap between blocks).

### Warmup Blocks

Workers discard `n_warmup_blocks` (default 16) of initial processing to settle IIR filter states before the actual output begins. This prevents transients from uninitialized filter conditions.

---

## Performance Tuning

### PerfOptions

```cpp
#include "perf_options.hpp"

PerfOptions opt;
opt.load();  // Load from config/options.json
```

| Field | Default | Description |
|-------|---------|-------------|
| `ring_seconds` | 30 | StreamBuffer ring size in seconds. ~21 MB/min stereo. |
| `ahead_seconds` | 10 | How far ahead the I/O thread pre-reads. |
| `io_chunk_frames` | 4096 | Frames decoded per I/O wakeup. |
| `interpolation` | 1 | 0=linear, 1=Catmull-Rom, 2=sinc-6tap |
| `block_size` | 1024 | DSP block size. Larger = less CPU, more latency. |
| `render_threads` | 1 | Default worker thread count for offline rendering. |
| `vu_response` | 1 | VU meter: 0=slow (classic VU), 1=medium, 2=fast (PPM) |

### Preset Profiles

```cpp
opt.apply_preset_fast_ssd();   // Low latency, SSD optimized
opt.apply_preset_default();    // Balanced defaults
opt.apply_preset_slow_hdd();   // Large buffers, spinning disk
opt.apply_preset_low_ram();    // Minimal memory footprint (~3 MB ring)
```

### RAM Estimates

| Config | Ring Size | RAM |
|--------|-----------|-----|
| `low_ram` | 10s | ~3 MB |
| `default` | 30s | ~10 MB |
| `fast_ssd` | 20s | ~7 MB |
| `slow_hdd` | 60s | ~21 MB |

---

## Built-in RNG and Reproducibility

All stochastic effects (wow, flutter, dropouts, Barkhausen noise, hiss) use a fast **xorshift64** PRNG seeded at engine construction.

### Seeding

```cpp
// Auto-seed from std::random_device (unique every run)
TapeEngine engine;

// Fixed seed for reproducible renders
TapeEngine engine(42);

// Worker with shared seed (coherent noise across workers)
auto worker = engine.make_worker(0, 10000, 1, /* shared_seed= */ 42);
```

When `seed = 0`, the engine generates a seed from `std::random_device`. Any non-zero seed produces deterministic, repeatable stochastic behavior.

### Worker Seed Coherence

`make_worker()` takes a `shared_seed` parameter. All workers rendering the same file should use the **same** seed to ensure stochastic noise patterns are continuous across worker boundaries (no seams at block junctions).

---

## Constants and Defaults

### Sample Rate and Block Size

```cpp
static constexpr int   SR          = 44100;
static constexpr float SR_F        = 44100.0f;
static constexpr float SR_F_INV    = 1.0f / 44100.0f;
static constexpr float PI          = 3.14159265358979323846f;
static constexpr float TWO_PI      = 2.0f * PI;
static constexpr int   BLOCK_SIZE  = 1024;
static constexpr int   MAX_CHANNELS = 2;
```

### Transport Physical Constants (NAB 10.5" reel)

```cpp
static constexpr float REEL_RADIUS_FULL = 0.133f;   // meters
static constexpr float REEL_RADIUS_HUB  = 0.025f;   // meters
static constexpr float TAPE_THICKNESS   = 12e-6f;   // meters
static constexpr float I_REEL_EMPTY     = 0.00224f; // kg·m²
static constexpr float I_REEL_FULL      = 0.00544f; // kg·m²
```

### Motor Ramp Rates

| Constant | Value | Time |
|----------|-------|------|
| `SPINUP_COEFF` | `1 / (0.6 × 44100)` | 0 → 1 in 0.6s |
| `SPINDOWN_COEFF` | `1 / (0.9 × 44100)` | 1 → 0 in 0.9s |

### Oversampling Context

```cpp
static constexpr int OS_PAD = 64;  // Overlap samples between blocks for OS rendering
```

---

## Application Directory Management

```cpp
#include "app_dirs.hpp"

AppDirs::init();  // Call once at startup — creates all directories
```

| Method | Linux/macOS | Windows |
|--------|-------------|---------|
| `root()` | `~/.local/share/CapstanVar/` | `%APPDATA%/CapstanVar` |
| `projects()` | `.../projects/` | `.../projects/` |
| `presets()` | `.../presets/` | `.../presets/` |
| `renders()` | `.../renders/` | `.../renders/` |
| `config_dir()` | `.../config/` | `.../config/` |

Automatically migrates legacy dotfiles (`~/.capstanvar_recents`, etc.) into the new config directory.

---

## Build-time Audio Backend Selection

The library automatically detects and links the appropriate audio backend:

| Priority | Backend | Platform |
|----------|---------|----------|
| 1 | PortAudio (system) | All (via pkg-config) |
| 2 | ALSA | Linux (no PortAudio) |
| 3 | CoreAudio | macOS |
| 4 | WinMM | Windows |

Backend selection is determined at compile time. The following preprocessor definitions are set:

- `NAGRA_AUDIO_PORTAUDIO`
- `NAGRA_AUDIO_ALSA`
- `NAGRA_AUDIO_COREAUDIO`
- `NAGRA_AUDIO_WINMM`

---

## Dependencies

The library links the following external libraries (transitively available through the build system):

| Library | Purpose |
|---------|---------|
| `libsndfile` | Audio file decoding (WAV, FLAC, OGG, OPUS, MP3) |
| `PortAudio` or platform equivalent | Audio device I/O |
| `nlohmann/json` | Preset serialization |
| `pthread` | I/O and DSP threading |

---

## Limitations and Design Decisions

1. **Fixed sample rate** — All DSP is hard-coded at 44.1 kHz. No resampling is performed.
2. **Stereo only** — Mono files are duplicated to both channels; multi-channel files use the first two channels.
3. **No realtime sample rate conversion** — The `StreamBuffer` reads at the file's native rate. Mismatched rates will play at the wrong pitch.
4. **Mutex-based parameter sharing** — The UI thread writes `params` under lock; the audio thread reads without lock (relies on `float` being atomic on the target platform). For strict correctness on platforms where `float` writes are not atomic, use a double-buffer pattern externally.
5. **Ring buffer underruns** — If the I/O thread cannot keep up with the DSP thread, `read()` returns sample-and-hold (last valid frame). This prevents clicks but introduces momentary repetition.
6. **Worker boundary coherence** — Stochastic noise is coherent across workers only when they share the same `shared_seed`. Different seeds will produce audible seams at block boundaries.

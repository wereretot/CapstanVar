#include "error_log.hpp"
#include <cstdio>
#include "engine.hpp"
#include <sndfile.h>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <cassert>
#include <vector>
#include <random>

TapeEngine::TapeEngine(uint64_t seed)
    : transport(seed), magnetic(), electronics()
{
 }

bool TapeEngine::load_file(const std::string& path) {
    // Open StreamBuffer first — uses current stream.ring_frames, stream.ahead_frames,
    // stream.io_chunk_frames set by UI from PerfOptions
    if (!stream.open(path)) {
        CV_ERR(FILE_OPEN_FAILED, path + ": StreamBuffer::open failed");
        return false;
    }

    // Populate audio_data metadata for render engine compatibility
    // (render engine uses load_file_for_render which fully loads)
    std::lock_guard<std::mutex> g(lock);
    total_samples = stream.total_samples;
    is_reversed   = false;
    params.is_reversed = false;
    reset_state();
    reset_position();
    std::fprintf(stderr, "[load_file] StreamBuffer opened: %d samples, ring=%d frames, ahead=%d frames, chunk=%d frames\n",
                 total_samples, stream.ring_frames, stream.ahead_frames, stream.io_chunk_frames);
    return true;
}

// Load for render: fills audio_data fully (render runs offline, RAM is acceptable)
bool TapeEngine::load_file_for_render(const std::string& path) {
    SF_INFO info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (!sf) {
        CV_ERR(FILE_OPEN_FAILED, std::string(path) + ": " + sf_strerror(nullptr));
        return false;
    }
    std::vector<float> raw((size_t)info.frames * info.channels);
    sf_count_t got = sf_readf_float(sf, raw.data(), info.frames);
    sf_close(sf);
    if (got <= 0) {
        CV_ERR(FILE_READ_FAILED, std::string(path) + ": sf_readf returned 0 frames");
        return false;
    }
    int ch = info.channels, n = (int)got;
    std::lock_guard<std::mutex> g(lock);
    audio_data.resize(n);
    float peak = 1e-9f;
    for (int i = 0; i < n; ++i) {
        float l = raw[i*ch], r = (ch>1)?raw[i*ch+1]:l;
        audio_data[i] = {l,r};
        peak = std::max(peak, std::max(std::abs(l),std::abs(r)));
    }
    float gain = 0.707f/peak;
    for (auto& f : audio_data) f *= gain;
    total_samples = n;
    is_reversed   = false;
    params.is_reversed = false;
    reset_state(); reset_position();
    return true;
}

void TapeEngine::set_reverse(bool want_reverse, bool force_reset) {
    std::lock_guard<std::mutex> g(lock);
    // audio_data is ALWAYS in forward order — never physically reversed.
    // is_reversed just tells dsp_process to read indices backward.
    // play_head always means "position in the original forward file".
    is_reversed = want_reverse;
    params.is_reversed = want_reverse;
    if (force_reset) reset_state();
}

void TapeEngine::reset_state() {
    // NOTE: play_head and current_time are intentionally NOT reset here.
    // reset_state() only clears DSP filter state so IIR filters, scrape
    // oscillators and motor state start clean — position is preserved.
    // Call reset_position() separately if you need to seek to zero.
    _os_context.clear();
    _os_context_valid = false;
    _fade_in          = 0;
    _last_out         = {};
    _scrape_phase     = 0.0f;
    _scrape_y_prev    = 0.0f;
    _scrape_x_prev    = 0.0f;
    transport.reset();
    electronics.reset();
    magnetic.reset();
}

void TapeEngine::reset_position() {
    play_head    = 0.0;
    current_time = 0.0f;
}

// ── Oversampled saturation helper ─────────────────────────────────────────────
void TapeEngine::_saturate_oversampled(Frame* buf, int frames, int oversample,
                                        const EngineParams& p)
{
    // Simple polyphase upsample (linear interpolation), saturate, downsample.
    // For production quality use a proper windowed-sinc FIR; this is fast enough
    // for 2–4× and meets the project's latency goals.
    int up_n = frames * oversample;
    std::vector<Frame> up(up_n + OS_PAD * oversample);

    // Prepend context if available
    int pad_out = 0;
    if (_os_context_valid) {
        // Upsample context into start of up[]
        for (int i = 0; i < OS_PAD; ++i) {
            for (int s = 0; s < oversample; ++s) {
                float t = (float)s / oversample;
                Frame a = _os_context[i];
                Frame b = (i+1 < OS_PAD) ? _os_context[i+1] : buf[0];
                up[i * oversample + s] = {lerp(a.l, b.l, t), lerp(a.r, b.r, t)};
            }
        }
        pad_out = OS_PAD * oversample;
    }

    // Upsample main block
    for (int i = 0; i < frames; ++i) {
        Frame next = (i+1 < frames) ? buf[i+1] : buf[frames-1];
        for (int s = 0; s < oversample; ++s) {
            float t = (float)s / oversample;
            up[pad_out + i * oversample + s] = {
                lerp(buf[i].l, next.l, t),
                lerp(buf[i].r, next.r, t)
            };
        }
    }

    // Save context for next block
    _os_context.assign(buf + frames - OS_PAD, buf + frames);
    _os_context_valid = true;

    // Saturate at high sample rate
    int sat_n = pad_out + up_n;
    magnetic.saturate_only(up.data(), sat_n, p);

    // Downsample (averaging decimation)
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        int base = pad_out + i * oversample;
        for (int s = 0; s < oversample; ++s) {
            l += up[base + s].l;
            r += up[base + s].r;
        }
        buf[i] = {l / oversample, r / oversample};
    }
}

bool TapeEngine::dsp_process(Frame* out, int frames, int oversample) {
    std::lock_guard<std::mutex> g(lock);

    if (!stream.is_open()) return false;
    if (!is_reversed && play_head >= total_samples - 1) return false;
    if ( is_reversed && play_head <= 0.0)               return false;

    EngineParams p = params;
    p.is_reversed  = is_reversed;
    // When tape is moving, keep engage target = 1.0 so TransportDynamics
    // internal ramp reaches full speed quickly. tape_speed_mult carries
    // the actual inertia — no double-ramping.
    if (p.tape_speed_mult > 0.001f) p.motor_engage = 1.0f;
    float speed_factor = p.ips_base / 15.0f;

    // Transport
    auto tr = transport.process(frames, current_time, play_head, total_samples, p);
    if (tr.speeds.empty()) {
        CV_ERR(ENGINE_TRANSPORT_NO_SPEEDS, "TransportDynamics::process returned empty speeds");
        return false;
    }

    // Build read indices.
    // When reversed, advance BACKWARD so play_head always
    // represents the true forward position in the original file.
    // IMPORTANT: read_indices[0] = play_head (read AT current position first)
    std::vector<double> read_indices(frames);
    double acc = play_head;
    if (p.is_reversed) {
        for (int i = 0; i < frames; ++i) {
            read_indices[i] = acc;  // Read first
            acc -= tr.speeds[i] * p.tape_speed_mult;  // Then accumulate
        }
    } else {
        for (int i = 0; i < frames; ++i) {
            read_indices[i] = acc;  // Read first
            acc += tr.speeds[i] * p.tape_speed_mult;  // Then accumulate
        }
    }

    // Read audio via StreamBuffer (Catmull-Rom interpolation)
    for (int i = 0; i < frames; ++i) {
        double pos = std::clamp(read_indices[i], 0.0, (double)(total_samples - 1));
        out[i] = stream.read(pos);
    }

    // Apply input gain/trim (before any processing to prevent internal clipping)
    if (p.input_gain != 1.0f) {
        for (int i = 0; i < frames; ++i) {
            out[i].l *= p.input_gain;
            out[i].r *= p.input_gain;
        }
    }

    // Notify stream of current position for IO thread window management
    stream.notify_position((int)play_head, p.is_reversed);

    // DEBUG: Set to true to bypass all magnetic/electronics effects
    constexpr bool BYPASS_EFFECTS = false;
    
    if constexpr (!BYPASS_EFFECTS) {
        // Oversampled saturation or normal magnetic processing
        // For streaming mode, we use a simplified magnetic path that doesn't require
        // random access to the original audio_data (which is now empty)
        if (oversample > 1) {
            _saturate_oversampled(out, frames, oversample, p);
            EngineParams p_os = p;
            p_os.presaturated = true;
            // In streaming mode, magnetic processing uses only the output buffer
            magnetic.process(out, frames, {}, {}, p_os);
        } else {
            magnetic.process(out, frames, {}, {}, p);
        }

        electronics.process(out, frames, current_time, speed_factor, tr.sticky_drag, p);
    }

    int n_out = frames;

    if constexpr (!BYPASS_EFFECTS) {
        // ── Scrape flutter ────────────────────────────────────────────────────────
        float scrape_amt = p.scrape_flutter;
        if (scrape_amt > 0.001f) {
            float ips        = p.ips_base;
            float scrape_hz  = std::clamp(2800.0f * (ips / 15.0f), 600.0f, 14000.0f);
            float phase_inc  = scrape_hz / SR_F;
            float alpha      = std::clamp(0.85f + scrape_amt * 0.1f, 0.85f, 0.97f);
            // Reduce depth at high IPS to prevent crispy artifacts
            // At 1.7 IPS: full depth, at 30 IPS: ~40% depth
            float ips_scale  = std::clamp(1.0f - (ips - 1.7f) / 60.f, 0.4f, 1.0f);
            float depth      = std::clamp(scrape_amt * 0.15f * ips_scale, 0.0f, 0.12f);

            static thread_local std::mt19937 sc_rng{42};
            static thread_local std::normal_distribution<float> sc_norm{0.f, 0.3f};

            float y_prev = _scrape_y_prev;
            float x_prev = _scrape_x_prev;

            for (int i = 0; i < n_out; ++i) {
                float ph = _scrape_phase + i * phase_inc;
                // Smoother envelope - reduced high-frequency content
                float env = 0.5f + 0.3f * std::sin(TWO_PI * ph)
                               + 0.15f * std::sin(TWO_PI * ph * 1.031f + 0.7f)
                               + 0.1f * std::sin(TWO_PI * ph * 0.973f + 1.3f)
                               + sc_norm(sc_rng) * 0.3f;
                env = std::abs(env);

                float x_cur = out[i].l;
                float y_cur = alpha * (y_prev + x_cur - x_prev);
                float mod   = y_cur * env * depth;
                out[i].l += mod;
                out[i].r += mod * 0.85f;
                x_prev = x_cur;
                y_prev = y_cur;
            }
            _scrape_phase = std::fmod(_scrape_phase + n_out * phase_inc, 1.0f);
            _scrape_y_prev = y_prev;
            _scrape_x_prev = x_prev;
        }

        // Dropout mask - apply directly (mask already has smooth fade in/out)
        for (int i = 0; i < n_out; ++i) {
            out[i] *= tr.dropout_mask[i];
        }

        // Fighting motors AM
        float conflict = transport.last_conflict;
        if (conflict > 0.05f) {
            float am_freq  = 2.0f + conflict * 3.0f;
            float am_depth = std::clamp(conflict * 0.7f, 0.0f, 0.8f);
            for (int i = 0; i < n_out; ++i) {
                float t   = current_time + (float)i * SR_F_INV;
                float am  = 1.0f - am_depth * (0.5f + 0.5f * std::sin(TWO_PI * am_freq * t));
                out[i] *= am;
            }
        }
    }

    // play_head always tracks the actual tape position in the forward file.
    // Clamp to [0, total_samples-1] in both directions.
    play_head = std::clamp(read_indices.back(), 0.0, (double)(total_samples - 1));
    current_time += (float)frames * SR_F_INV;

    // Output limiter - two-stage: soft clip then hard peak limiter
    // Stage 1: Analog-style soft clipping with tanh
    const float KNEE = 0.92f;  // Slightly lower knee for earlier saturation
    for (int i = 0; i < n_out; ++i) {
        out[i].l = fast_tanh(out[i].l / KNEE) * KNEE;
        out[i].r = fast_tanh(out[i].r / KNEE) * KNEE;
    }
    
    // Stage 2: Look-ahead peak limiter to catch any remaining peaks
    // Find max peak in this block
    float peak = 0.0f;
    for (int i = 0; i < n_out; ++i) {
        peak = std::max(peak, std::max(std::abs(out[i].l), std::abs(out[i].r)));
    }
    
    // Apply gain reduction if needed (ceiling at 0.95 to allow for reconstruction)
    const float CEILING = 0.95f;
    if (peak > CEILING) {
        float gain = CEILING / peak;
        for (int i = 0; i < n_out; ++i) {
            out[i].l *= gain;
            out[i].r *= gain;
        }
    }

    // Post-preset fade-in
    const int FADE_SAMPS = 512;
    if (_fade_in > 0) {
        int fl = std::min(_fade_in, n_out);
        int pos_start = FADE_SAMPS - _fade_in;
        for (int i = 0; i < fl; ++i) {
            float t = (float)(pos_start + i) / FADE_SAMPS;
            out[i].l = out[i].l * t + _last_out.l * (1.0f - t);
            out[i].r = out[i].r * t + _last_out.r * (1.0f - t);
        }
        _fade_in -= fl;
    }
    _last_out = out[n_out - 1];

    return true;
}

std::unique_ptr<TapeEngine> TapeEngine::make_worker(
    int start_sample, int block_size, int oversample,
    uint64_t shared_seed, int n_warmup_blocks)
{
    auto eng = std::make_unique<TapeEngine>(shared_seed);
    {
        std::lock_guard<std::mutex> g(lock);
        eng->audio_data    = audio_data;
        eng->total_samples = total_samples;
        eng->params        = params;
        eng->is_reversed   = is_reversed;
    }

    // Warm up DSP state before the slice starts.
    // For forward renders: warm up from (start - warmup) advancing forward to start.
    // For reverse renders: warm up from (start + warmup) retreating backward to start.
    int warmup_samples = n_warmup_blocks * block_size;
    eng->transport.motor_engage        = 1.f;
    eng->transport.current_motor_speed = 1.f;

    std::vector<Frame> discard(block_size);
    if (!eng->is_reversed) {
        int warmup_start = std::max(0, start_sample - warmup_samples);
        eng->play_head   = (double)warmup_start;
        eng->current_time = (float)warmup_start / SR_F;
        while (eng->play_head < (double)start_sample) {
            if (!eng->dsp_process(discard.data(), block_size, oversample)) break;
        }
    } else {
        int warmup_start = std::min(eng->total_samples - 1, start_sample + warmup_samples);
        eng->play_head   = (double)warmup_start;
        eng->current_time = (float)warmup_start / SR_F;
        while (eng->play_head > (double)start_sample) {
            if (!eng->dsp_process(discard.data(), block_size, oversample)) break;
        }
    }

    eng->play_head    = (double)start_sample;
    eng->current_time = (float)start_sample / SR_F;
    return eng;
}

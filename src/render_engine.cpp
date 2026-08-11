#include "error_log.hpp"
#include "render_engine.hpp"
#include <sndfile.h>
#include <cmath>
#include <algorithm>
#include <future>
#include <numeric>
#include <random>
#include <cassert>
#include <cstdio>
#include <chrono>

using Clock = std::chrono::steady_clock;
using Tp    = Clock::time_point;

// ── Helpers ───────────────────────────────────────────────────────────────────
static float elapsed_s(Tp start) {
    return std::chrono::duration<float>(Clock::now() - start).count();
}

// ── RenderEngine ──────────────────────────────────────────────────────────────
RenderEngine::RenderEngine(TapeEngine& src) : _src(src) {
    _worker = std::thread(&RenderEngine::_worker_loop, this);
}

RenderEngine::~RenderEngine() {
    cancel_all();
    _shutdown.store(true);
    _queue_cv.notify_one();      // wake worker so it sees _shutdown and exits
    if (_worker.joinable()) _worker.join();
}

int RenderEngine::enqueue(const RenderOptions& opts,
                           std::function<void(bool,std::string)> done_cb)
{
    {
        std::lock_guard<std::mutex> g(_queue_mtx);
        _queue.push_back({opts, std::move(done_cb)});
    }
    _queue_cv.notify_one();   // wake worker immediately, no polling delay
    return queue_size() - 1;
}

void RenderEngine::start(const RenderOptions& opts,
                          std::function<void(float)>            progress_cb,
                          std::function<void(bool,std::string)> done_cb)
{
    // Wrap legacy progress_cb into the status system
    auto wrapped_done = [progress_cb, done_cb](bool ok, std::string msg) {
        if (progress_cb) progress_cb(1.f);
        if (done_cb)     done_cb(ok, msg);
    };
    enqueue(opts, std::move(wrapped_done));
}

void RenderEngine::cancel_current() { _cancel_current.store(true); }

void RenderEngine::cancel_all() {
    _cancel_all.store(true);
    _cancel_current.store(true);
    {
        std::lock_guard<std::mutex> g(_queue_mtx);
        _queue.clear();
    }
    _queue_cv.notify_one();
}

int RenderEngine::queue_size() const {
    std::lock_guard<std::mutex> g(_queue_mtx);
    return (int)_queue.size();
}

RenderStatus RenderEngine::get_status() const {
    std::lock_guard<std::mutex> g(_status_mtx);
    return _status;
}

void RenderEngine::_update_status(std::function<void(RenderStatus&)> fn) {
    std::lock_guard<std::mutex> g(_status_mtx);
    fn(_status);
}

// ── Worker loop ───────────────────────────────────────────────────────────────
void RenderEngine::_worker_loop() {
    while (true) {
        QueuedJob job;
        int job_idx   = 0;
        int total_jobs= 0;
        {
            std::unique_lock<std::mutex> g(_queue_mtx);
            // Wait until a job arrives or shutdown is requested.
            // Uses condition_variable so enqueue() wakes us immediately —
            // no 20ms sleep while holding the lock.
            _queue_cv.wait(g, [this]{
                return !_queue.empty() || _shutdown.load() || _cancel_all.load();
            });

            if (_shutdown.load()) return;

            if (_cancel_all.load()) {
                _queue.clear();
                _cancel_all.store(false);
                _cancel_current.store(false);
                _worker_running.store(false);
                continue;
            }

            if (_queue.empty()) continue;

            job        = std::move(_queue.front());
            _queue.pop_front();
            total_jobs = 1 + (int)_queue.size();
        }

        _worker_running.store(true);
        _cancel_current.store(false);

        // Update queued names in status
        _update_status([&](RenderStatus& s) {
            s.queued_names.clear();
            std::lock_guard<std::mutex> g2(_queue_mtx);
            for (auto& q : _queue) {
                std::string n = q.opts.display_name.empty()
                    ? q.opts.path : q.opts.display_name;
                s.queued_names.push_back(n);
            }
            s.job_index  = job_idx;
            s.total_jobs = total_jobs;
        });

        try {
            _run_job(job, job_idx, total_jobs);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[RenderEngine] job failed: %s\n", e.what());
            _update_status([&](RenderStatus& s) {
                s.completed.push_back({job.opts.display_name, false, e.what()});
            });
            if (job.done_cb) job.done_cb(false, e.what());
        }

        _worker_running.store(false);
    }
}

// ── Quality settings ──────────────────────────────────────────────────────────
RenderEngine::QualitySettings RenderEngine::_quality_settings(SimQuality q) {
    switch (q) {
        case SimQuality::Draft:       return {4096, 1, false, false};
        case SimQuality::Standard:    return {2048, 1, true,  false};
        case SimQuality::High:        return {1024, 1, true,  true };
        case SimQuality::Ultra:       return { 512, 1, true,  true };
        case SimQuality::Pristine_2x: return {1024, 2, true,  true };
        case SimQuality::Extreme_4x:  return {1024, 4, true,  true };
        case SimQuality::Archival_8x: return { 512, 8, true,  true };
    }
    return {1024, 1, true, true};
}

// ── Core job runner ───────────────────────────────────────────────────────────
void RenderEngine::_run_job(const QueuedJob& job, int job_idx, int total_jobs) {
    const RenderOptions& opts = job.opts;
    const auto qs    = _quality_settings(opts.quality);
    const int  bs    = qs.block_size;
    const int  os    = qs.oversample;
    const Tp   t0    = Clock::now();

    std::string display = opts.display_name.empty() ? opts.path : opts.display_name;

    // ── Build render engine with fully loaded audio_data ────────────────────
    // Live engine uses StreamBuffer (no full file in RAM). Render workers need
    // the full file in audio_data for fast random-access slice rendering.
    auto eng = std::make_unique<TapeEngine>();
    {
        std::lock_guard<std::mutex> g(_src.lock);
        eng->params      = _src.params;
        eng->is_reversed = _src.is_reversed;
    }
    if (!eng->load_file_for_render(_src.stream._file_path)) {
        CV_ERR(RENDER_ENGINE_EMPTY, "Could not load audio for render");
        if (job.done_cb) job.done_cb(false, "Audio file could not be read for render");
        return;
    }
    eng->is_reversed = opts.reverse;
    eng->params.is_reversed = opts.reverse;

    eng->reset_state();
    eng->reset_position();           // always render from start of file
    eng->params.tape_speed_mult = 1.0f;  // always render at 1× — ignore live inertia
    eng->params.motor_engage    = 1.0f;
    eng->transport.motor_engage        = 1.f;
    eng->transport.current_motor_speed = 1.f;

    // Apply quality overrides
    if (!qs.barkhausen)    eng->params.barkhausen    = 0.f;
    if (!qs.print_through) eng->params.print_through = 0.f;

    // ── DSP warmup ──────────────────────────────────────────────────────────
    // Process a small amount of audio and discard the output to prime the
    // magnetic hysteresis, Barkhausen envelope, scrape flutter filter
    // history, and the electronics IIR chains into their steady-state
    // operating points BEFORE recording begins. This matches the warmup
    // that make_worker() already does for parallel slice workers, and
    // prevents the cold-start transient burst (heavy distortion+wow at
    // the very start of the recorded file). The warmup does not touch
    // play_head after seeking it back to 0 — filter state stays primed.
    {
        constexpr int kRenderWarmupBlocks = 8;
        std::vector<Frame> warmup_discard(bs);
        for (int i = 0; i < kRenderWarmupBlocks; ++i) {
            if (!eng->dsp_process(warmup_discard.data(), bs, os)) break;
        }
        eng->play_head    = 0.0;
        eng->current_time = 0.0f;
        // Short output-amplitude ramp as click-safety belt, in case any
        // sub-tap filter history leaks through the seeded state.
        eng->trigger_fade_in(512);
    }

    const int total_samples = eng->total_samples;
    if (total_samples == 0) {
        CV_ERR(RENDER_ENGINE_EMPTY, "No audio loaded — nothing to render");
        if (job.done_cb) job.done_cb(false, "No audio loaded");
        return;
    }
    const int total_blocks  = (total_samples + bs - 1) / bs;

    // Source RMS (before processing) for loudness matching
    float src_rms = 0.f;
    if (opts.match_loudness) {
        double acc = 0;
        for (auto& f : eng->audio_data) acc += f.l*f.l + f.r*f.r;
        src_rms = (float)std::sqrt(acc / std::max((int)eng->audio_data.size()*2, 1));
    }

    _update_status([&](RenderStatus& s) {
        s.job_index     = job_idx;
        s.total_jobs    = total_jobs;
        s.job_progress  = 0.f;
        s.phase         = "Pre-roll";
        s.blocks_done   = 0;
        s.total_blocks  = total_blocks;
        s.total_samples_job = total_samples;
        s.samples_done  = 0;
        s.sr_out        = opts.sample_rate;
        s.bit_depth     = opts.bit_depth;
        s.oversample    = os;
        s.threads       = opts.threads;
        s.reverse       = opts.reverse;
        s.output_path   = opts.path;
        s.display_name  = display;
        s.elapsed_s     = 0.f;
        s.eta_s         = 0.f;
        s.xrt           = 0.f;
    });

    // ── Pre-roll ──────────────────────────────────────────────────────────────
    std::vector<Frame> preroll_data;
    if (opts.preroll_s > 0.f) {
        const int pr_n = (int)(opts.preroll_s * SR_F);
        TapeEngine pr_eng;
        pr_eng.audio_data.assign(pr_n + bs * 4, Frame{});
        pr_eng.total_samples = (int)pr_eng.audio_data.size();
        pr_eng.params        = eng->params;
        pr_eng.transport.motor_engage        = 1.f;
        pr_eng.transport.current_motor_speed = 1.f;

        std::vector<Frame> blk(bs);
        while ((int)preroll_data.size() < pr_n && !_cancel_current.load()) {
            if (!pr_eng.dsp_process(blk.data(), bs, os)) break;
            int keep = std::min(bs, pr_n - (int)preroll_data.size());
            preroll_data.insert(preroll_data.end(), blk.begin(), blk.begin()+keep);
        }
        // Transfer motor state so main render starts from warmed-up motor
        eng->transport.motor_engage        = pr_eng.transport.motor_engage;
        eng->transport.current_motor_speed = pr_eng.transport.current_motor_speed;
    }

    if (_cancel_current.load()) {
        if (job.done_cb) job.done_cb(false, "Cancelled");
        return;
    }

    // ── Main render ───────────────────────────────────────────────────────────
    _update_status([](RenderStatus& s){ s.phase = "Rendering"; });

    std::vector<Frame> audio_out;
    audio_out.reserve(total_samples);

    // Shared atomic progress counter for multi-thread path
    std::atomic<int> blocks_done_atomic{0};

    auto report_progress = [&](int done) {
        float jp = std::clamp((float)done / std::max(total_blocks, 1), 0.f, 0.98f);
        float el = elapsed_s(t0);
        float xrt = (done > 0 && el > 0.01f)
            ? (float)(done * bs) / (SR_F * el)
            : 0.f;
        float eta = (done > 0 && el > 0.f)
            ? el / jp * (1.f - jp)
            : 0.f;
        _update_status([&](RenderStatus& s) {
            s.job_progress      = jp;
            s.overall_progress  = ((float)job_idx + jp) / std::max(total_jobs, 1);
            s.blocks_done       = done;
            s.samples_done      = done * bs;
            s.elapsed_s         = el;
            s.eta_s             = eta;
            s.xrt               = xrt;
        });
    };

    // Determine sample rate for time conversion in animation
    const bool has_anim = opts.anim.enabled && !opts.anim.curves.empty();

    if (opts.threads <= 1) {
        // ── Single-threaded ───────────────────────────────────────────────
        std::vector<Frame> blk(bs);
        int done = 0;
        std::fprintf(stderr, "[Render] Starting single-threaded render: total_samples=%d, bs=%d, os=%d\n", 
            total_samples, bs, os);
        while (!_cancel_current.load()) {
            // Apply animation curves at current play_head before each block
            if (has_anim) {
                opts.anim.apply(eng->params, eng->play_head);
                // Always keep transport internals intact
                eng->params.tape_speed_mult = 1.0f;
                eng->params.motor_engage    = 1.0f;
            }
            bool ok = eng->dsp_process(blk.data(), bs, os);
            if (!ok) {
                std::fprintf(stderr, "[Render] dsp_process returned false at done=%d, play_head=%.0f\n", 
                    done, eng->play_head);
                break;
            }
            audio_out.insert(audio_out.end(), blk.begin(), blk.end());
            ++done;
            if (done % 16 == 0) report_progress(done);
        }
        std::fprintf(stderr, "[Render] Render complete: done=%d blocks, audio_out.size()=%zu\n", 
            done, audio_out.size());
        report_progress(done);

    } else {
        // ── Multi-threaded ────────────────────────────────────────────────────
        // Divide total_samples into N slices. Each worker independently warms up
        // and renders its slice. Seam crossfades hide the warmup discontinuity.

        const int  N         = opts.threads;
        const int  slice_n   = total_samples / N;
        const int  xf_len    = 256;
        const auto seed      = static_cast<uint64_t>(total_samples) ^ 0xDEADBEEFULL;
        const bool rev       = eng->is_reversed;

        using Chunk = std::vector<Frame>;

        std::vector<std::future<Chunk>> futures;
        futures.reserve(N);

        for (int i = 0; i < N; ++i) {
            // For reverse renders, slice from the end backward so chunks
            // are collected in output order (first chunk = end of file).
            int start, end;
            if (!rev) {
                start = i * slice_n;
                end   = (i == N-1) ? total_samples : (i+1)*slice_n;
            } else {
                // Reverse: slice[0] starts near total_samples, works down to 0
                end   = total_samples - i * slice_n;
                start = (i == N-1) ? 0 : total_samples - (i+1)*slice_n;
            }

            futures.push_back(std::async(std::launch::async,
                [this, &eng, &opts, bs, os, seed, start, end, &blocks_done_atomic, total_blocks, has_anim]() -> Chunk
            {
                auto worker = eng->make_worker(start, bs, os, seed, 24);
                Chunk out;
                out.reserve(end - start);
                std::vector<Frame> wb(bs);

                while (!_cancel_current.load()) {
                    if ((int)worker->play_head >= end) break;

                    // Apply animation curves per-block — match the single-thread
                    // render path. Previously multi-thread silently dropped
                    // opts.anim.apply entirely, so any animated parameters were
                    // ignored during parallel renders.
                    if (has_anim) {
                        opts.anim.apply(worker->params, worker->play_head);
                        worker->params.tape_speed_mult = 1.0f;
                        worker->params.motor_engage    = 1.0f;
                    }

                    if (!worker->dsp_process(wb.data(), bs, os)) break;

                    // Trim last block if it overshoots the slice boundary
                    int written = (int)worker->play_head;
                    int keep    = std::min(bs, std::max(0, end - (written - bs)));
                    keep        = std::max(keep, 0);
                    out.insert(out.end(), wb.begin(), wb.begin() + std::min(keep, bs));

                    int d = blocks_done_atomic.fetch_add(1, std::memory_order_relaxed) + 1;
                    (void)d;
                }
                return out;
            }));
        }

        // Poll progress while workers run
        while (true) {
            bool all_done = true;
            for (auto& f : futures)
                if (f.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
                    { all_done = false; break; }
            report_progress(blocks_done_atomic.load());
            if (all_done) break;
            if (_cancel_current.load()) break;
        }

        // Collect chunks
        std::vector<Chunk> chunks(N);
        for (int i = 0; i < N; ++i) chunks[i] = futures[i].get();

        // Stitch with crossfade at each seam
        for (int i = 0; i < N; ++i) {
            if (i == 0) {
                audio_out.insert(audio_out.end(), chunks[i].begin(), chunks[i].end());
                continue;
            }
            // Crossfade: last xf_len samples of previous chunk with first xf_len
            // of current chunk, then append the rest
            auto& prev = audio_out;
            auto& curr = chunks[i];
            int xf = std::min({xf_len, (int)prev.size(), (int)curr.size()});
            int prev_start = (int)prev.size() - xf;
            for (int k = 0; k < xf; ++k) {
                float t = (float)(k+1) / (float)(xf+1);
                float it = 1.f - t;
                prev[prev_start + k].l = prev[prev_start+k].l * it + curr[k].l * t;
                prev[prev_start + k].r = prev[prev_start+k].r * it + curr[k].r * t;
            }
            // Append remainder of current chunk (skip the crossfaded region)
            if ((int)curr.size() > xf)
                audio_out.insert(audio_out.end(), curr.begin()+xf, curr.end());
        }
    }

    if (_cancel_current.load()) {
        CV_ERR(RENDER_CANCELLED, display + ": render cancelled by user");
        _update_status([](RenderStatus& s){ s.phase = "Cancelled"; });
        _update_status([&](RenderStatus& s){
            s.completed.push_back({display, false, "Cancelled"});
        });
        if (job.done_cb) job.done_cb(false, "Cancelled");
        return;
    }

    // Prepend pre-roll
    if (!preroll_data.empty()) {
        preroll_data.insert(preroll_data.end(), audio_out.begin(), audio_out.end());
        audio_out = std::move(preroll_data);
    }

    // Do NOT trim to total_samples.
    // When the motor runs slow (motor health, wow, low voltage),
    // tr.speeds[i] < 1.0 so more output samples are produced per tape sample
    // consumed. The correct output length is audio_out.size() — it represents
    // the full time the tape took to play through. Trimming to total_samples
    // would cut the end off whenever the motor averaged below 1.0x speed.
    // (The block-based overshoot is at most bs-1 = 1023 samples ≈ 23ms
    // which is acceptable, and removed by the output sample-rate conversion.)

    // ── Post-process ──────────────────────────────────────────────────────────
    _update_status([](RenderStatus& s){ s.phase = "Post-process"; s.job_progress = 0.98f; });

    if (opts.dc_block) {
        DC_Block dc;
        for (auto& f : audio_out) { f.l = dc.tick(f.l,0); f.r = dc.tick(f.r,1); }
    }

    if (opts.match_loudness && src_rms > 1e-6f) {
        double acc = 0;
        for (auto& f : audio_out) acc += f.l*f.l + f.r*f.r;
        float out_rms = (float)std::sqrt(acc / std::max((int)audio_out.size()*2, 1));
        if (out_rms > 1e-6f) {
            float gain = std::min(src_rms / out_rms, std::pow(10.f, 12.f/20.f));
            for (auto& f : audio_out) f *= gain;
        }
    } else if (opts.normalize) {
        float peak = 0;
        for (auto& f : audio_out)
            peak = std::max(peak, std::max(std::abs(f.l), std::abs(f.r)));
        if (peak > 1e-9f) {
            float gain = std::pow(10.f, -0.1f/20.f) / peak;
            for (auto& f : audio_out) f *= gain;
        }
    }

    for (auto& f : audio_out) {
        f.l = std::clamp(f.l, -1.f, 1.f);
        f.r = std::clamp(f.r, -1.f, 1.f);
    }

    // Resample if output SR differs from internal SR
    if (opts.sample_rate != SR)
        _resample(audio_out, SR, opts.sample_rate);

    // ── Write ─────────────────────────────────────────────────────────────────
    _update_status([](RenderStatus& s){ s.phase = "Writing"; s.job_progress = 0.99f; });
    _write_wav(opts.path, audio_out, opts.sample_rate, opts.bit_depth, opts.dither);

    float total_el = elapsed_s(t0);
    _update_status([&](RenderStatus& s) {
        s.job_progress     = 1.f;
        s.overall_progress = (float)(job_idx+1) / std::max(total_jobs,1);
        s.phase            = "Done";
        s.elapsed_s        = total_el;
        s.eta_s            = 0.f;
        s.xrt              = (total_el > 0.f)
            ? (float)total_samples / (SR_F * total_el) : 0.f;
        s.completed.push_back({display, true,
            "Written: " + opts.path + " (" +
            std::to_string((int)(audio_out.size()/1000)) + "k samp, " +
            std::to_string((int)(total_el)) + "s elapsed)"});
    });

    if (job.done_cb) job.done_cb(true, "");
}

// ── Windowed-sinc polyphase resampler (better than linear) ────────────────────
// Uses a 64-tap Blackman-windowed sinc kernel for each phase.
void RenderEngine::_resample(std::vector<Frame>& data, int from_sr, int to_sr) {
    if (from_sr == to_sr) return;
    const double ratio    = (double)to_sr / from_sr;
    const int    out_n    = (int)std::ceil(data.size() * ratio);
    const int    taps     = 64;
    const double fc       = std::min(0.5, std::min(0.5 / (1.0/ratio), 0.5));  // cutoff
    const double pi       = 3.14159265358979323846;

    // Build kernel once
    std::vector<double> h(taps);
    double sum = 0;
    for (int i = 0; i < taps; ++i) {
        double n  = i - (taps-1)*0.5;
        double bw = (n == 0) ? 2*fc : std::sin(2*pi*fc*n)/(pi*n);
        // Blackman window
        double w  = 0.42 - 0.5*std::cos(2*pi*i/(taps-1))
                         + 0.08*std::cos(4*pi*i/(taps-1));
        h[i] = bw * w;
        sum  += h[i];
    }
    for (auto& v : h) v /= sum;

    const int src_n = (int)data.size();
    std::vector<Frame> out(out_n);

    for (int i = 0; i < out_n; ++i) {
        double src_pos = (double)i / ratio;
        double l = 0, r = 0;
        for (int k = 0; k < taps; ++k) {
            double    pos = src_pos - (taps/2 - 1 - k);
            int       j   = (int)std::floor(pos);
            double    frac= pos - j;
            // Interpolate the kernel fractionally
            double    kv  = h[k];
            // Linear interpolation of adjacent taps for sub-sample accuracy
            if (k+1 < taps) kv = h[k] * (1-frac) + h[k+1] * frac;
            if (j >= 0 && j < src_n) { l += data[j].l * kv; r += data[j].r * kv; }
        }
        out[i] = {(float)l, (float)r};
    }
    data = std::move(out);
}

// ── WAV writer ────────────────────────────────────────────────────────────────
void RenderEngine::_write_wav(const std::string& path,
                               const std::vector<Frame>& data,
                               int sr, int bit_depth, bool dither)
{
    SF_INFO info{};
    info.channels   = 2;
    info.samplerate = sr;
    switch (bit_depth) {
        case 16: info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16; break;
        case 24: info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24; break;
        default: info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;  break;
    }

    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!sf) {
        CV_ERR(RENDER_OUTPUT_OPEN_FAILED, path + ": " + sf_strerror(nullptr));
        throw std::runtime_error("Cannot open output: " + path);
    }

    std::vector<float> buf(data.size() * 2);
    if (dither && bit_depth < 32) {
        // TPDF dither: two uniform noises summed = triangular PDF
        float lsb = 1.f / (float)(1 << (bit_depth - 1));
        std::mt19937_64 rng{12345ULL};
        std::uniform_real_distribution<float> d(-lsb*0.5f, lsb*0.5f);
        for (size_t i = 0; i < data.size(); ++i) {
            buf[i*2]   = data[i].l + d(rng) + d(rng);
            buf[i*2+1] = data[i].r + d(rng) + d(rng);
        }
    } else {
        for (size_t i = 0; i < data.size(); ++i) {
            buf[i*2]   = data[i].l;
            buf[i*2+1] = data[i].r;
        }
    }

    sf_writef_float(sf, buf.data(), (sf_count_t)data.size());
    sf_close(sf);
}

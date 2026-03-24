#pragma once
// ── CapstanVar performance options ───────────────────────────────────────────
// Persisted to config/options.json via AppDirs.
// All options affect the StreamBuffer sizing and render defaults.
// No option requires a restart — changed values apply on next file load.

#include "app_dirs.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

struct PerfOptions {
    // ── Stream buffer ─────────────────────────────────────────────────────────
    // ring_seconds: total ring buffer length. More = tolerates slower drives.
    // Higher values use more RAM. 1 min stereo 44100 ≈ 21 MB.
    int  ring_seconds   = 30;    // 30 s ≈ 10 MB — default
    // ahead_seconds: how far ahead the IO thread reads. Increase on slow HDDs.
    int  ahead_seconds  = 10;    // 10 s read-ahead
    // io_chunk_frames: frames decoded per IO wakeup. Smaller = lower latency,
    // more wakeups. Larger = better throughput on spinning disks.
    int  io_chunk_frames = 4096;

    // ── DSP quality ───────────────────────────────────────────────────────────
    // interpolation: 0=linear (fast), 1=Catmull-Rom (default), 2=sinc-6tap
    int  interpolation  = 1;
    // dsp_block_size: frames per DSP block (BLOCK_SIZE in audio_io).
    // Larger = less CPU, more latency. Smaller = lower latency, more CPU.
    int  block_size     = 1024;

    // ── VU Meter ──────────────────────────────────────────────────────────────
    // vu_response: VU meter response speed. 0=slow (classic VU), 1=medium, 2=fast (PPM)
    // Higher values = faster attack and decay, more responsive but jittery
    int  vu_response    = 1;    // 0=slow, 1=medium (default), 2=fast

    // ── Render defaults ───────────────────────────────────────────────────────
    int  render_threads = 1;

    // ── Presets for easy selection ────────────────────────────────────────────
    void apply_preset_fast_ssd() {
        ring_seconds    = 20;
        ahead_seconds   = 5;
        io_chunk_frames = 2048;
        interpolation   = 1;
        block_size      = 512;
        render_threads  = 4;
        vu_response     = 2;  // Fast response for low-latency monitoring
    }
    void apply_preset_default() {
        ring_seconds    = 30;
        ahead_seconds   = 10;
        io_chunk_frames = 4096;
        interpolation   = 1;
        block_size      = 1024;
        render_threads  = 1;
        vu_response     = 1;  // Medium response
    }
    void apply_preset_slow_hdd() {
        ring_seconds    = 60;
        ahead_seconds   = 30;
        io_chunk_frames = 8192;
        interpolation   = 0;
        block_size      = 2048;
        render_threads  = 1;
        vu_response     = 0;  // Slow classic VU response
    }
    void apply_preset_low_ram() {
        ring_seconds    = 10;
        ahead_seconds   = 4;
        io_chunk_frames = 4096;
        interpolation   = 1;
        block_size      = 1024;
        render_threads  = 1;
        vu_response     = 1;  // Medium response
    }

    // ── RAM estimate in MB ────────────────────────────────────────────────────
    float ring_mb() const {
        return (float)(ring_seconds * 44100 * 2 * sizeof(float)) / (1024*1024);
    }

    // ── Persistence ───────────────────────────────────────────────────────────
    void save() const {
        nlohmann::json j;
        j["ring_seconds"]    = ring_seconds;
        j["ahead_seconds"]   = ahead_seconds;
        j["io_chunk_frames"] = io_chunk_frames;
        j["interpolation"]   = interpolation;
        j["block_size"]      = block_size;
        j["render_threads"]  = render_threads;
        j["vu_response"]     = vu_response;
        std::ofstream f(AppDirs::config("options.json"));
        if (f) f << j.dump(2);
    }
    void load() {
        std::ifstream f(AppDirs::config("options.json"));
        if (!f) return;
        try {
            nlohmann::json j; f >> j;
            ring_seconds    = j.value("ring_seconds",    ring_seconds);
            ahead_seconds   = j.value("ahead_seconds",   ahead_seconds);
            io_chunk_frames = j.value("io_chunk_frames", io_chunk_frames);
            interpolation   = j.value("interpolation",   interpolation);
            block_size      = j.value("block_size",      block_size);
            render_threads  = j.value("render_threads",  render_threads);
            vu_response     = j.value("vu_response",     vu_response);
        } catch (...) {}
    }
};

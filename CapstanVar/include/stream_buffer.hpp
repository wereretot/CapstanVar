#pragma once
// ── StreamBuffer — lock-free ring-buffered audio streamer ────────────────────
// Keeps a sliding window of decoded audio around the play position.
// Ring indexing: slot = file_pos % ring_size  (absolute, never shifts).
// Valid range tracked by [_valid_start, _valid_end) in file-space.
// IO thread fills ahead; DSP thread reads. One writer, one reader per slot.

#include "dsp_types.hpp"
#include "error_log.hpp"

#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <string>
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

class StreamBuffer {
public:
    // ── Tuneable config (set before open, applied at open time) ──────────────
    int ring_frames     = 30 * 44100;  // total ring size (default ≈10 MB stereo)
    int ahead_frames    = 10 * 44100;  // read-ahead goal
    int io_chunk_frames = 4096;        // frames per IO read

    // ── File info (read-only after open) ─────────────────────────────────────
    std::string _file_path;
    int  total_samples = 0;
    int  sample_rate   = 44100;
    int  channels      = 2;
    float norm_gain    = 1.f;

    // ─────────────────────────────────────────────────────────────────────────
    bool open(const std::string& path) {
        close();

        SF_INFO info{};
        SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
        if (!sf) {
            CV_ERR(FILE_OPEN_FAILED, path + ": " + sf_strerror(nullptr));
            return false;
        }
        if (info.frames <= 0) { sf_close(sf); return false; }

        _file_path    = path;
        total_samples = (int)info.frames;
        sample_rate   = info.samplerate;
        channels      = info.channels;

        // Quick peak scan of first 5 s for normalisation
        {
            int scan = std::min(total_samples, 5 * sample_rate);
            std::vector<float> tmp((size_t)scan * channels);
            sf_readf_float(sf, tmp.data(), scan);
            float peak = 1e-9f;
            for (float v : tmp) peak = std::max(peak, std::abs(v));
            norm_gain = 0.707f / peak;
            sf_seek(sf, 0, SEEK_SET);
        }

        ring_frames = std::max(ring_frames, ahead_frames + 16384);
        _ring.assign(ring_frames, Frame{});

        _sf          = sf;
        _valid_start = 0;
        _valid_end   = 0;

        // Synchronous prime fill so first dsp_process() call has data
        int prime_n = std::min(ahead_frames, total_samples);
        _prime(0, prime_n);
        std::fprintf(stderr,"[StreamBuffer] opened %s: %d samples, primed %d frames, valid %d..%d\n",
            path.c_str(), total_samples, prime_n, _valid_start, _valid_end);

        _running.store(true);
        _io_thread = std::thread(&StreamBuffer::_io_loop, this);
        return true;
    }

    void close() {
        _running.store(false);
        _cv.notify_all();
        if (_io_thread.joinable()) _io_thread.join();
        {
            std::lock_guard<std::mutex> g(_sf_mtx);
            if (_sf) { sf_close(_sf); _sf = nullptr; }
        }
        _ring.clear();
        total_samples = 0;
        _valid_start  = 0;
        _valid_end    = 0;
    }

    bool is_open() const { return !_ring.empty() && total_samples > 0; }

    // ── Read: Catmull-Rom interpolation at file position pos ─────────────────
    // Called from DSP thread. Returns silence on window miss (triggers refill).
    Frame read(double pos) const {
        pos = std::clamp(pos, 0.0, (double)(total_samples - 1));
        int i0   = (int)pos;
        float fr = (float)(pos - i0);

        // Need i0-1 .. i0+2 in window
        int need_lo = std::max(0, i0 - 1);
        int need_hi = std::min(total_samples - 1, i0 + 2);

        if (!_valid(need_lo) || !_valid(need_hi)) {
            _seek_hint.store(i0);   // wake IO thread
            // Fallback: linear interp with whatever we have
            Frame f0 = _slot(i0);
            Frame f1 = _slot(std::min(i0+1, total_samples-1));
            return {f0.l + (f1.l-f0.l)*fr, f0.r + (f1.r-f0.r)*fr};
        }

        // Catmull-Rom
        Frame p0 = _slot(std::max(0, i0-1));
        Frame p1 = _slot(i0);
        Frame p2 = _slot(std::min(total_samples-1, i0+1));
        Frame p3 = _slot(std::min(total_samples-1, i0+2));
        float t2 = fr*fr, t3 = t2*fr;
        return {
            0.5f*((2*p1.l)+(-p0.l+p2.l)*fr+(2*p0.l-5*p1.l+4*p2.l-p3.l)*t2+(-p0.l+3*p1.l-3*p2.l+p3.l)*t3),
            0.5f*((2*p1.r)+(-p0.r+p2.r)*fr+(2*p0.r-5*p1.r+4*p2.r-p3.r)*t2+(-p0.r+3*p1.r-3*p2.r+p3.r)*t3)
        };
    }

    // Called from DSP thread after each block to let IO thread know where we are
    void notify_position(int file_pos) {
        _play_pos.store(file_pos);
        // Evict old data behind play head (keep 2048-frame back buffer)
        int new_start = std::max(0, file_pos - 2048);
        if (new_start > _valid_start)
            _valid_start = new_start;
        _cv.notify_one();
    }

    size_t ram_bytes() const { return _ring.size() * sizeof(Frame); }
    int valid_start() const { return _valid_start; }
    int valid_end()   const { return _valid_end;   }

private:
    SNDFILE*   _sf = nullptr;
    std::mutex _sf_mtx;

    std::vector<Frame>   _ring;
    int _valid_start = 0;  // first valid file pos in ring
    int _valid_end   = 0;  // one past last valid file pos

    std::atomic<int>  _play_pos{0};
    mutable std::atomic<int> _seek_hint{-1};

    std::thread             _io_thread;
    std::atomic<bool>       _running{false};
    std::condition_variable _cv;
    std::mutex              _cv_mtx;

    // Absolute slot: file_pos maps to ring[file_pos % ring_size] always
    Frame& _slot_ref(int fp)       { return _ring[(size_t)fp % _ring.size()]; }
    Frame  _slot(int fp)     const { return _ring[(size_t)fp % _ring.size()]; }
    bool   _valid(int fp)    const {
        return fp >= _valid_start && fp < _valid_end;
    }

    // Synchronous fill from file_pos for count frames (called with _sf_mtx held)
    void _decode(int file_pos, int count) {
        if (!_sf || count <= 0 || file_pos >= total_samples) return;
        count = std::min(count, total_samples - file_pos);
        std::vector<float> raw((size_t)count * channels);
        sf_seek(_sf, file_pos, SEEK_SET);
        sf_count_t got = sf_readf_float(_sf, raw.data(), count);
        for (int i = 0; i < (int)got; ++i) {
            float l = raw[i * channels] * norm_gain;
            float r = (channels > 1) ? raw[i * channels + 1] * norm_gain : l;
            _slot_ref(file_pos + i) = {l, r};
        }
        _valid_end = std::max(_valid_end, file_pos + (int)got);
    }

    void _prime(int from, int count) {
        std::lock_guard<std::mutex> g(_sf_mtx);
        _decode(from, count);
        _valid_start = from;
    }

    void _io_loop() {
        while (_running.load()) {
            {
                std::unique_lock<std::mutex> lk(_cv_mtx);
                _cv.wait_for(lk, std::chrono::milliseconds(15));
            }
            if (!_running.load()) break;

            // Handle seek hint (window miss from DSP thread)
            int hint = _seek_hint.exchange(-1);
            if (hint >= 0) {
                std::lock_guard<std::mutex> g(_sf_mtx);
                int new_start = std::max(0, hint - 2048);
                _valid_start = new_start;
                _valid_end   = new_start;  // invalidate
                _decode(new_start, std::min(ahead_frames, total_samples - new_start));
                continue;
            }

            // Top up read-ahead from _valid_end
            int play = _play_pos.load();
            int want_end = std::min(total_samples, play + ahead_frames);

            if (_valid_end < want_end) {
                std::lock_guard<std::mutex> g(_sf_mtx);
                // Check we won't overwrite data still in back-buffer
                int chunk = std::min(io_chunk_frames, want_end - _valid_end);
                // Ring wraps — only write into slots not still needed
                int ring_sz = (int)_ring.size();
                int write_pos = _valid_end;
                // If write_pos % ring_sz would overlap _valid_start, stop
                if ((write_pos % ring_sz) != (_valid_start % ring_sz) || _valid_end == _valid_start)
                    _decode(write_pos, chunk);
            }
        }
    }
};

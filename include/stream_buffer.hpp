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
#include <cctype>     // std::tolower for case-insensitive .opus extension check (libopusfile fallback)
#include <sndfile.h>
#include <opusfile.h> // libopusfile integration for native .opus decoding (fetched via FetchContent)
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

        // ── Path-extension-first dispatch ────────────────────────────────────
        // libsndfile 1.2.x has no bundled Opus decoder. Checking the
        // extension first avoids a useless sf_open round-trip + libsndfile
        // parse attempt for .opus files (sf_open on .opus returns "File
        // contains data in an unimplemented format" per the earlier
        // ad-hoc test case). Case-insensitive suffix match (.opus, .OPUS,
        // .Opus all qualify).
        const auto ends_with_ci = [](const std::string& s, const char* suf) -> bool {
            const size_t ls = std::strlen(suf);
            if (s.size() < ls) return false;
            for (size_t i = 0; i < ls; ++i) {
                if (std::tolower((unsigned char)s[s.size() - ls + i]) != (unsigned char)suf[i])
                    return false;
            }
            return true;
        };
        if (ends_with_ci(path, ".opus")) {
            return _open_opus(path);
        }

        // ── libsndfile happy path ────────────────────────────────────────────
        SF_INFO info{};
        SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
        if (sf) {
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
            _decoder     = Decoder::SndFile;
            _valid_start = 0;
            _valid_end   = 0;
            int prime_n = std::min(ahead_frames, total_samples);
            _prime(0, prime_n);
            float ring_mb = (float)(ring_frames * 2 * sizeof(float)) / (1024.f * 1024.f);
            std::fprintf(stderr, "\033[92m[StreamBuffer] ✓ OPENED\033[0m (libsndfile) %s: %d samples, ring=%.1fMB (%d frames), ahead=%d frames, chunk=%d\n",
                path.c_str(), total_samples, ring_mb, ring_frames, ahead_frames, io_chunk_frames);
            _running.store(true);
            _io_thread = std::thread(&StreamBuffer::_io_loop, this);
            return true;
        }

        // ── libsndfile failure: opportunistic libopusfile fallback ──────────
        // Catch cases where the file extension isn't .opus but libsndfile
        // still rejects the file (mysterious misnamed files, future libsndfile
        // versions dropping a format, etc.). Capture sf_strerror so the
        // final CV_ERR after this also fails mentions both backends'
        // outcomes.
        std::string sf_err = sf_strerror(nullptr);
        if (_open_opus(path)) return true;

        // Both paths tried. Combined diagnostic + supported-format hint.
        CV_ERR(FILE_OPEN_FAILED,
            path + ": libsndfile error: " + sf_err +
            " (also tried libopusfile fallback) -- supported formats: WAV, AIFF, FLAC, OGG Vorbis, MP3, Opus"
            " (WMA not supported anywhere; WavPack/SD2 are gated under ENABLE_EXPERIMENTAL=OFF in this build)");
        return false;
    }

    void close() {
        _running.store(false);
        _cv.notify_all();
        if (_io_thread.joinable()) _io_thread.join();
        {
            std::lock_guard<std::mutex> g(_sf_mtx);
            if (_sf) { sf_close(_sf);   _sf  = nullptr; }
            if (_of) { op_free(_of);    _of  = nullptr; }
        }
        _ring.clear();
        total_samples = 0;
        _valid_start  = 0;
        _valid_end    = 0;
        _decoder      = Decoder::SndFile; // default reset; open() reassigns on the active path
    }

    bool is_open() const { return !_ring.empty() && total_samples > 0; }

    // ── Read: Catmull-Rom interpolation at file position pos ─────────────────
    // Called from DSP thread. Returns last valid sample on window miss (prevents clicks).
    Frame read(double pos) const {
        pos = std::clamp(pos, 0.0, (double)(total_samples - 1));
        int i0   = (int)pos;
        float fr = (float)(pos - i0);

        // Need i0-1 .. i0+2 in window for Catmull-Rom
        int need_lo = std::max(0, i0 - 1);
        int need_hi = std::min(total_samples - 1, i0 + 2);

        if (!_valid(need_lo) || !_valid(need_hi)) {
            _seek_hint.store(i0);   // wake IO thread
            // Fallback: return last valid sample (sample-and-hold) to avoid clicks
            // Search backward from i0 for the last valid sample
            for (int i = i0; i >= _valid_start && i < _valid_end; --i) {
                if (_valid(i)) return _slot(i);
            }
            // If nothing valid, return silence
            // Log underruns sparingly - only first one and then every 1000
            static std::atomic<int> underrun_count{0};
            int uc = ++underrun_count;
            if (uc == 1 || uc % 1000 == 1)
                std::fprintf(stderr, "\n\033[91m[StreamBuffer] ✖ UNDERRUN #%d\033[0m at pos=%d valid=%d..%d ring_sz=%zu\n",
                    uc, i0, _valid_start, _valid_end, _ring.size());
            return {0.f, 0.f};
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
    void notify_position(int file_pos, bool reversed = false) {
        _play_pos.store(file_pos);
        _reversed.store(reversed);
        // Evict old data behind play head (keep 2048-frame back buffer)
        // When reversed, we need data BEFORE play_pos, so keep buffer ahead
        int back_buffer = 2048;
        if (reversed) {
            // Playing backward: keep data AHEAD of play position
            int new_end = std::min(total_samples - 1, file_pos + back_buffer);
            if (new_end < _valid_end)
                _valid_end = new_end;
        } else {
            // Playing forward: keep data BEHIND play position
            int new_start = std::max(0, file_pos - back_buffer);
            if (new_start > _valid_start)
                _valid_start = new_start;
        }
        _cv.notify_one();
    }

    size_t ram_bytes() const { return _ring.size() * sizeof(Frame); }
    int valid_start() const { return _valid_start; }
    int valid_end()   const { return _valid_end;   }

private:
    // Decoder dispatch — exactly one backend handle is non-null after open()
    // (mutually exclusive lifetime, set by open()/close()). libsndfile handles
    // WAV/AIFF/FLAC/Vorbis/MP3 (under the ENABLE_EXTERNAL_LIBS=OFF bundled-codec
    // config); libopusfile handles .opus via the libopusfile-0.12 fallback
    // path. The rest of the class dispatches on `_decoder` rather than
    // reaching for `_sf` directly, so future code only needs to compare
    // against the enum instead of null-checking both handles.
    enum class Decoder { SndFile, OpusFile };
    Decoder     _decoder = Decoder::SndFile;
    SNDFILE*    _sf  = nullptr;
    OggOpusFile* _of = nullptr;
    std::mutex  _sf_mtx;

    std::vector<Frame>   _ring;
    int _valid_start = 0;  // first valid file pos in ring
    int _valid_end   = 0;  // one past last valid file pos

    std::atomic<int>  _play_pos{0};
    mutable std::atomic<int> _seek_hint{-1};
    std::atomic<bool> _reversed{false};  // Track playback direction

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
    // Returns number of frames actually decoded
    // Synchronous fill from file_pos for count frames (called with _sf_mtx
    // held). Returns number of frames actually decoded. Dispatches on
    // `_decoder`: libsndfile for the bundled codec path; libopusfile for
    // the .opus path. Both backends fill `raw` as interleaved float frames,
    // which we then de-interleave into the per-sample `Frame` slots.
    int _decode(int file_pos, int count) {
        if (count <= 0 || file_pos >= total_samples) return 0;
        count = std::min(count, total_samples - file_pos);
        std::vector<float> raw((size_t)count * channels);
        int got = 0;
        if (_decoder == Decoder::SndFile) {
            if (!_sf) return 0;
            sf_seek(_sf, file_pos, SEEK_SET);
            got = (int)sf_readf_float(_sf, raw.data(), count);
        } else {
            if (!_of) return 0;
            // libopusfile is forward-only at the API tier; op_pcm_seek
            // handles backwards seeks by bisecting Opus pages under the hood.
            // The seek rounds to packet boundary; the read following it is
            // strictly forward, which matches our IO-loop fill pattern
            // (always seek+read-forward) so the access mode aligns cleanly.
            // seek errors are non-fatal — op_read below will return -1 if
            // we landed somewhere unreachable.
            op_pcm_seek(_of, file_pos);
            int n = op_read_float(_of, raw.data(), count, /*li=*/nullptr);
            if (n >= 0) got = n; // n is frames per channel returned; elements in `raw` = got * channels
            // else got stays 0 -- IO loop retries on next _valid miss
        }
        for (int i = 0; i < got; ++i) {
            float l = raw[i * channels] * norm_gain;
            float r = (channels > 1) ? raw[i * channels + 1] * norm_gain : l;
            _slot_ref(file_pos + i) = {l, r};
        }
        return got;
    }

    // ── libopusfile open path ──────────────────────────────────────────────
    // Called from open() when the file extension is .opus (or libsndfile's
    // sf_open returns "unimplemented", as a last-ditch fallback). Sets the
    // file metadata (total_samples, sample_rate=48000, channels),
    // performs a peak-scan-based norm_gain calibration, primes the ring
    // buffer via _prime(), then spawns the IO thread. Posts
    // CV_ERR(FILE_OPEN_FAILED, ...) and returns false on any failure.
    // ─────────────────────────────────────────────────────────────────────
    bool _open_opus(const std::string& path) {
        int oerr = 0;
        OggOpusFile* of = op_open(path.c_str(), &oerr);
        if (!of) {
            CV_ERR(FILE_OPEN_FAILED,
                path + ": libopusfile op_open failed (code " + std::to_string(oerr) +
                ") -- file is not an Opus stream, or is corrupt");
            return false;
        }
        const OpusHead* head = op_head(of, -1);
        if (!head) {
            op_free(of);
            CV_ERR(FILE_OPEN_FAILED, path + ": libopusfile returned no OpusHead (file may be a chained stream with no header link)");
            return false;
        }
        _file_path    = path;
        channels      = head->channel_count;
        sample_rate   = 48000; // libopusfile always decodes to 48 kHz internally (see opusfile docs)
        opus_int64 pcm_total = op_pcm_total(of, -1);
        if (pcm_total <= 0 || pcm_total > (opus_int64)INT32_MAX) {
            op_free(of);
            CV_ERR(FILE_OPEN_FAILED,
                path + ": libopusfile op_pcm_total returned " + std::to_string(pcm_total) +
                " -- empty or unreasonably long Opus stream");
            return false;
        }
        total_samples = (int)pcm_total;

        // Quick peak scan of first 5 s for normalisation gain
        int scan = std::min(total_samples, 5 * sample_rate);
        std::vector<float> tmp((size_t)scan * channels);
        // op_pcm_seek handles packet-boundary alignment automatically
        op_pcm_seek(of, 0);
        int n = op_read_float(of, tmp.data(), scan, /*li=*/nullptr);
        if (n < 0) n = 0;
        int read_frames = std::min(n, scan);
        float peak = 1e-9f;
        for (int i = 0; i < read_frames * channels; ++i)
            peak = std::max(peak, std::abs(tmp[i]));
        norm_gain = 0.707f / peak;

        ring_frames = std::max(ring_frames, ahead_frames + 16384);
        _ring.assign(ring_frames, Frame{});
        _of          = of;
        _decoder     = Decoder::OpusFile;
        _valid_start = 0;
        _valid_end   = 0;

        // Synchronous prime fill so first dsp_process() call has data
        int prime_n = std::min(ahead_frames, total_samples);
        _prime(0, prime_n);
        float ring_mb = (float)(ring_frames * 2 * sizeof(float)) / (1024.f * 1024.f);
        std::fprintf(stderr, "\033[92m[StreamBuffer] ✓ OPENED\033[0m (libopusfile) %s: %d samples, ring=%.1fMB (%d frames), ahead=%d frames, chunk=%d\n",
            path.c_str(), total_samples, ring_mb, ring_frames, ahead_frames, io_chunk_frames);

        _running.store(true);
        _io_thread = std::thread(&StreamBuffer::_io_loop, this);
        return true;
    }

    void _prime(int from, int count) {
        std::lock_guard<std::mutex> g(_sf_mtx);
        int decoded = _decode(from, count);
        _valid_start = from;
        _valid_end = from + decoded;
    }

    void _io_loop() {
        int last_logged_fill_pct = -1;
        while (_running.load()) {
            {
                std::unique_lock<std::mutex> lk(_cv_mtx);
                _cv.wait_for(lk, std::chrono::milliseconds(10));
            }
            if (!_running.load()) break;
            
            // Log buffer status only when fill percentage changes significantly
            static int counter = 0;
            if (++counter % 500 == 1) {
                int used = (_valid_end - _valid_start);
                int fill_pct = (int)((float)used / _ring.size() * 100.f);
                if (fill_pct != last_logged_fill_pct) {
                    last_logged_fill_pct = fill_pct;
                    const char* status = fill_pct < 20 ? "⚠ LOW" : (fill_pct < 50 ? "• OK" : "✓ FULL");
                    std::fprintf(stderr, "\r[StreamBuffer] %s  valid=%d..%d (%d%% of %zu) %s",
                        status, _valid_start, _valid_end, fill_pct, _ring.size(),
                        _reversed.load() ? "REV" : "FWD");
                    fflush(stderr);
                }
            }

            // Handle seek hint (window miss from DSP thread) - high priority
            int hint = _seek_hint.exchange(-1);
            if (hint >= 0) {
                std::lock_guard<std::mutex> g(_sf_mtx);
                bool rev = _reversed.load();
                if (rev) {
                    // Playing backward: need data BEFORE hint position
                    int new_end = std::min(total_samples - 1, hint + 4096);
                    _valid_start = std::max(0, new_end - ahead_frames);
                    _valid_end   = new_end;
                    int decoded = _decode(_valid_start, _valid_end - _valid_start);
                    std::fprintf(stderr, "\n\033[93m[StreamBuffer] • SEEK REV to %d: valid=%d..%d (%d frames decoded)\033[0m\n",
                        hint, _valid_start, _valid_end, decoded);
                } else {
                    // Playing forward: need data AFTER hint position
                    int new_start = std::max(0, hint - 4096);
                    int decode_n = std::min(ahead_frames, total_samples - new_start);
                    _valid_start = new_start;
                    _valid_end   = new_start;
                    int decoded = _decode(new_start, decode_n);
                    _valid_end = new_start + decoded;
                    std::fprintf(stderr, "\n\033[93m[StreamBuffer] • SEEK FWD to %d: valid=%d..%d (%d frames decoded)\033[0m\n",
                        hint, _valid_start, _valid_end, decoded);
                }
                fflush(stderr);
                continue;
            }

            // Top up buffer based on playback direction
            int play = _play_pos.load();
            bool rev = _reversed.load();
            
            if (rev) {
                // Playing backward: fill BEFORE play position
                int want_start = std::max(0, play - ahead_frames);

                // Keep safety margin
                int safety_margin = io_chunk_frames * 2;
                int target_start = std::max(0, play - safety_margin - ahead_frames / 2);
                want_start = std::min(want_start, target_start);

                if (_valid_start > want_start) {
                    std::lock_guard<std::mutex> g(_sf_mtx);
                    int ring_sz = (int)_ring.size();
                    int used = (_valid_end - _valid_start);
                    int free = ring_sz - used - 64;

                    if (free > 0) {
                        int chunk = std::min({io_chunk_frames, _valid_start - want_start, free});
                        if (chunk > 0) {
                            int decode_start = std::max(0, _valid_start - chunk);
                            int decoded = _decode(decode_start, _valid_start - decode_start);
                            if (decoded > 0) _valid_start = decode_start;
                        }
                    }
                }
            } else {
                // Playing forward: fill AFTER play position (original behavior)
                int want_end = std::min(total_samples, play + ahead_frames);

                // Keep safety margin
                int safety_margin = io_chunk_frames * 2;
                int target_end = std::min(total_samples, play + safety_margin + ahead_frames / 2);
                want_end = std::max(want_end, target_end);

                if (_valid_end < want_end) {
                    std::lock_guard<std::mutex> g(_sf_mtx);
                    int ring_sz = (int)_ring.size();
                    int used = (_valid_end - _valid_start);
                    int free = ring_sz - used - 64;

                    if (free > 0) {
                        int chunk = std::min({io_chunk_frames, want_end - _valid_end, free});
                        if (chunk > 0) {
                            int decoded = _decode(_valid_end, chunk);
                            if (decoded > 0) _valid_end += decoded;
                        }
                    }
                }
            }
        }
    }
};

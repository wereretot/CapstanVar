#pragma once
#include "param_anim.hpp"
#include "engine.hpp"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <vector>
#include <optional>

// ── Quality preset ────────────────────────────────────────────────────────────
enum class SimQuality {
    Draft,        // 4096-block, no bark/print
    Standard,     // 2048-block, bark
    High,         // 1024-block, full
    Ultra,        //  512-block, full
    Pristine_2x,  // 1024-block, 2× oversample
    Extreme_4x,   // 1024-block, 4× oversample
    Archival_8x,  //  512-block, 8× oversample
};

// ── Per-render options ────────────────────────────────────────────────────────
struct RenderOptions {
    std::string path;
    std::string display_name;       // shown in queue (defaults to filename)
    int         sample_rate    = 44100;
    int         bit_depth      = 24;
    SimQuality  quality        = SimQuality::High;
    bool        reverse        = false;
    float       preroll_s      = 0.f;
    int         threads        = 1;
    bool        dither         = true;
    bool        dc_block       = true;
    bool        normalize      = true;
    bool        match_loudness = false;
    // Animation — optional. If anim.enabled and curves exist, params are
    // updated per-block during render to match the animated values.
    ParamAnim   anim;   // copied from _anim at enqueue time
};

// ── Live status (updated from render thread, read from UI thread) ─────────────
struct RenderStatus {
    // Queue position
    int   job_index   = 0;     // 0-based index of the running job
    int   total_jobs  = 0;     // total jobs including queued

    // Current job progress
    float job_progress    = 0.f; // 0.0–1.0
    float overall_progress= 0.f; // across all jobs

    // Phase label
    std::string phase;           // "Pre-roll", "Rendering", "Post-process", "Writing"

    // Timing
    float elapsed_s  = 0.f;
    float eta_s      = 0.f;
    float xrt        = 0.f;  // realtime multiple (e.g. 3.2× RT)

    // Block counters
    int   blocks_done  = 0;
    int   total_blocks = 0;
    int   samples_done = 0;
    int   total_samples_job = 0;

    // Job metadata (for display)
    int         sr_out      = 0;
    int         bit_depth   = 0;
    int         oversample  = 1;
    int         threads     = 1;
    bool        reverse     = false;
    std::string output_path;
    std::string display_name;

    // Queue listing (names of pending jobs, not including current)
    std::vector<std::string> queued_names;

    // Completion of finished jobs (appended on done)
    struct CompletedJob {
        std::string name;
        bool        ok;
        std::string message;
    };
    std::vector<CompletedJob> completed;
};

// ── Render engine ─────────────────────────────────────────────────────────────
class RenderEngine {
public:
    explicit RenderEngine(TapeEngine& src);
    ~RenderEngine();

    // Enqueue a job. Returns queue position (0 = will start immediately).
    int  enqueue(const RenderOptions& opts,
                 std::function<void(bool, std::string)> done_cb = {});

    // Legacy single-job start (wraps enqueue).
    void start(const RenderOptions& opts,
               std::function<void(float)>            progress_cb,
               std::function<void(bool, std::string)> done_cb);

    void cancel_current();    // cancels the running job; queue continues
    void cancel_all();        // cancels running job and clears queue
    int  queue_size() const;
    bool is_running() const { return _worker_running.load(); }

    // Thread-safe snapshot of live status. Call every frame from UI.
    RenderStatus get_status() const;

private:
    TapeEngine& _src;

    // ── Queue ─────────────────────────────────────────────────────────────────
    struct QueuedJob {
        RenderOptions                          opts;
        std::function<void(bool,std::string)>  done_cb;
    };
    mutable std::mutex       _queue_mtx;
    std::deque<QueuedJob>    _queue;
    std::thread              _worker;
    std::atomic<bool>        _worker_running{false};
    std::atomic<bool>        _cancel_current{false};
    std::atomic<bool>        _cancel_all{false};
    std::atomic<bool>        _shutdown{false};
    std::condition_variable  _queue_cv;

    // ── Live status (written by worker, read by UI) ───────────────────────────
    mutable std::mutex _status_mtx;
    RenderStatus       _status;
    // Completed-job list accumulated across all jobs this session
    std::vector<RenderStatus::CompletedJob> _completed;

    void _update_status(std::function<void(RenderStatus&)> fn);
    void _worker_loop();

    // ── DSP pipeline for one job ──────────────────────────────────────────────
    struct QualitySettings { int block_size, oversample; bool barkhausen, print_through; };
    static QualitySettings _quality_settings(SimQuality q);

    void _run_job(const QueuedJob& job, int job_idx, int total_jobs);

    // Oversampled saturation — proper context-preserving implementation
    static void _saturate_os(std::vector<Frame>& buf,
                              const EngineParams& p,
                              int oversample,
                              std::vector<Frame>& ctx);   // cross-block context

    // Polyphase-quality linear resampler
    static void _resample(std::vector<Frame>& data, int from_sr, int to_sr);

    // TPDF-dithered WAV writer
    static void _write_wav(const std::string& path,
                           const std::vector<Frame>& data,
                           int sr, int bit_depth, bool dither);
};

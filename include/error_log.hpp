#pragma once
// ── CapstanVar error logging system ──────────────────────────────────────────
// Centralised, thread-safe error log. All subsystems post errors here.
// The UI polls and displays them as an in-app notification panel.
//
// Error codes are grouped by subsystem prefix:
//   AUDIO_*   — audio device and DSP thread
//   FILE_*    — audio file loading
//   RENDER_*  — render engine
//   PROJECT_* — project save/load
//   PRESET_*  — preset import/export
//   ENGINE_*  — DSP engine internals

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <atomic>

enum class ErrSeverity {
    Info,     // ℹ - Informational messages
    Warning,  // ⚠ - Potential issues
    Error     // ✖ - Critical failures
};

enum class ErrCode {
    // Audio device
    AUDIO_DEVICE_OPEN_FAILED,
    AUDIO_DEVICE_PARAMS_FAILED,
    AUDIO_WRITE_FAILED,
    AUDIO_DSP_END_OF_FILE,
    AUDIO_DSP_EMPTY_ENGINE,

    // File loading
    FILE_NOT_FOUND,
    FILE_OPEN_FAILED,
    FILE_READ_FAILED,
    FILE_FORMAT_UNSUPPORTED,

    // Render
    RENDER_OUTPUT_OPEN_FAILED,
    RENDER_OUTPUT_WRITE_FAILED,
    RENDER_ENGINE_EMPTY,
    RENDER_CANCELLED,

    // Project
    PROJECT_SAVE_OPEN_FAILED,
    PROJECT_SAVE_WRITE_FAILED,
    PROJECT_LOAD_NOT_FOUND,
    PROJECT_LOAD_PARSE_FAILED,
    PROJECT_LOAD_WRONG_FORMAT,
    PROJECT_AUDIO_NOT_FOUND,

    // Preset
    PRESET_EXPORT_FAILED,
    PRESET_IMPORT_NOT_FOUND,
    PRESET_IMPORT_PARSE_FAILED,
    PRESET_IMPORT_WRONG_APP,

    // Engine / DSP
    ENGINE_TRANSPORT_NO_SPEEDS,
    ENGINE_OVERSAMPLE_ALLOC,
};

inline ErrSeverity err_code_severity(ErrCode c) {
    // Warnings: non-critical issues that don't stop operation
    switch (c) {
    case ErrCode::AUDIO_DSP_END_OF_FILE:
    case ErrCode::RENDER_CANCELLED:
    case ErrCode::PROJECT_AUDIO_NOT_FOUND:
        return ErrSeverity::Warning;
    default:
        return ErrSeverity::Error;
    }
}

inline const char* err_code_str(ErrCode c) {
    switch (c) {
    case ErrCode::AUDIO_DEVICE_OPEN_FAILED:    return "AUDIO_DEVICE_OPEN_FAILED";
    case ErrCode::AUDIO_DEVICE_PARAMS_FAILED:  return "AUDIO_DEVICE_PARAMS_FAILED";
    case ErrCode::AUDIO_WRITE_FAILED:          return "AUDIO_WRITE_FAILED";
    case ErrCode::AUDIO_DSP_END_OF_FILE:       return "AUDIO_DSP_END_OF_FILE";
    case ErrCode::AUDIO_DSP_EMPTY_ENGINE:      return "AUDIO_DSP_EMPTY_ENGINE";
    case ErrCode::FILE_NOT_FOUND:              return "FILE_NOT_FOUND";
    case ErrCode::FILE_OPEN_FAILED:            return "FILE_OPEN_FAILED";
    case ErrCode::FILE_READ_FAILED:            return "FILE_READ_FAILED";
    case ErrCode::FILE_FORMAT_UNSUPPORTED:     return "FILE_FORMAT_UNSUPPORTED";
    case ErrCode::RENDER_OUTPUT_OPEN_FAILED:   return "RENDER_OUTPUT_OPEN_FAILED";
    case ErrCode::RENDER_OUTPUT_WRITE_FAILED:  return "RENDER_OUTPUT_WRITE_FAILED";
    case ErrCode::RENDER_ENGINE_EMPTY:         return "RENDER_ENGINE_EMPTY";
    case ErrCode::RENDER_CANCELLED:            return "RENDER_CANCELLED";
    case ErrCode::PROJECT_SAVE_OPEN_FAILED:    return "PROJECT_SAVE_OPEN_FAILED";
    case ErrCode::PROJECT_SAVE_WRITE_FAILED:   return "PROJECT_SAVE_WRITE_FAILED";
    case ErrCode::PROJECT_LOAD_NOT_FOUND:      return "PROJECT_LOAD_NOT_FOUND";
    case ErrCode::PROJECT_LOAD_PARSE_FAILED:   return "PROJECT_LOAD_PARSE_FAILED";
    case ErrCode::PROJECT_LOAD_WRONG_FORMAT:   return "PROJECT_LOAD_WRONG_FORMAT";
    case ErrCode::PROJECT_AUDIO_NOT_FOUND:     return "PROJECT_AUDIO_NOT_FOUND";
    case ErrCode::PRESET_EXPORT_FAILED:        return "PRESET_EXPORT_FAILED";
    case ErrCode::PRESET_IMPORT_NOT_FOUND:     return "PRESET_IMPORT_NOT_FOUND";
    case ErrCode::PRESET_IMPORT_PARSE_FAILED:  return "PRESET_IMPORT_PARSE_FAILED";
    case ErrCode::PRESET_IMPORT_WRONG_APP:     return "PRESET_IMPORT_WRONG_APP";
    case ErrCode::ENGINE_TRANSPORT_NO_SPEEDS:  return "ENGINE_TRANSPORT_NO_SPEEDS";
    case ErrCode::ENGINE_OVERSAMPLE_ALLOC:     return "ENGINE_OVERSAMPLE_ALLOC";
    }
    return "UNKNOWN";
}

inline const char* err_severity_icon(ErrSeverity s) {
    switch (s) {
    case ErrSeverity::Info:     return "ℹ";
    case ErrSeverity::Warning:  return "⚠";
    case ErrSeverity::Error:    return "✖";
    }
    return "?";
}

// ANSI color codes for terminal output
inline const char* err_severity_color(ErrSeverity s) {
    switch (s) {
    case ErrSeverity::Info:     return "\033[94m";   // Bright Blue
    case ErrSeverity::Warning:  return "\033[93m";   // Bright Yellow
    case ErrSeverity::Error:    return "\033[91m";   // Bright Red
    }
    return "\033[0m";
}
inline const char* COLOR_RESET = "\033[0m";

struct ErrorEntry {
    ErrCode       code;
    ErrSeverity   severity;
    std::string   message;   // human-readable detail (path, system error, etc.)
    std::string   timestamp; // HH:MM:SS
    bool          dismissed = false;
};

class ErrorLog {
public:
    static ErrorLog& get() { static ErrorLog inst; return inst; }

    void post(ErrCode code, const std::string& detail = "", ErrSeverity severity = ErrSeverity::Error) {
        auto now  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm* tm = std::localtime(&now);
        char ts[12]; std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                                   tm->tm_hour, tm->tm_min, tm->tm_sec);
        ErrorEntry e;
        e.code      = code;
        e.severity  = severity;
        e.message   = detail;
        e.timestamp = ts;

        // Always log to stderr for terminal diagnostics with colors
        const char* color = err_severity_color(severity);
        std::fprintf(stderr, "%s[CapstanVar %s] %s  %s  %s%s\n",
                     color, err_severity_icon(severity), ts, err_code_str(code), detail.c_str(), COLOR_RESET);

        std::lock_guard<std::mutex> g(_mtx);
        _entries.push_back(std::move(e));
        if (_entries.size() > MAX_ENTRIES) _entries.erase(_entries.begin());
        _has_new.store(true);
    }

    // Returns true if there are undismissed errors
    bool has_new() const { return _has_new.load(); }

    // Consume the flag (call once per frame from UI)
    void clear_new_flag() { _has_new.store(false); }

    // Copy for UI display (thread-safe snapshot)
    std::vector<ErrorEntry> snapshot() {
        std::lock_guard<std::mutex> g(_mtx);
        return _entries;
    }

    void dismiss_all() {
        std::lock_guard<std::mutex> g(_mtx);
        for (auto& e : _entries) e.dismissed = true;
        _has_new.store(false);
    }

    void clear() {
        std::lock_guard<std::mutex> g(_mtx);
        _entries.clear();
        _has_new.store(false);
    }

    int undismissed_count() {
        std::lock_guard<std::mutex> g(_mtx);
        int n = 0;
        for (auto& e : _entries) if (!e.dismissed) ++n;
        return n;
    }

private:
    static constexpr size_t MAX_ENTRIES = 200;
    std::vector<ErrorEntry> _entries;
    std::mutex              _mtx;
    std::atomic<bool>       _has_new{false};
};

// Convenience macro — post to the global log (defaults to Error severity)
#define CV_ERR(code, ...)  ErrorLog::get().post(ErrCode::code, ##__VA_ARGS__, err_code_severity(ErrCode::code))

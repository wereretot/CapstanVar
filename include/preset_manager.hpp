#pragma once
#include "dsp_types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

struct Preset {
    std::string  name;
    EngineParams params;
};

class PresetManager {
public:
    PresetManager();

    // ── Built-in library ──────────────────────────────────────────────────────
    const std::vector<Preset>& builtin_presets() const { return _builtins; }
    std::optional<Preset>      find_builtin(const std::string& name) const;

    // ── Session (user-saved this run) ─────────────────────────────────────────
    void  save_session(const std::string& name, const EngineParams& p);
    const std::vector<Preset>& session_presets() const { return _session; }
    std::optional<Preset>      find_session(const std::string& name) const;

    // ── File I/O ──────────────────────────────────────────────────────────────
    bool export_preset(const std::string& path, const std::string& name,
                       const EngineParams& p) const;
    std::optional<Preset> import_preset(const std::string& path) const;

    // ── Default ───────────────────────────────────────────────────────────────
    static EngineParams default_params();

private:
    std::vector<Preset> _builtins;
    std::vector<Preset> _session;

    void _build_builtins();

    static EngineParams _params_from_json(const void* json_obj); // nlohmann forward
    static void         _params_to_json(void* json_obj, const std::string& name,
                                        const EngineParams& p);
};

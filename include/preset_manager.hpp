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

    // ── Phase 5c startup audit ─────────────────────────────────────────────────
    // Round-trip smoke test for the 4 storage presets added to
    // _build_builtins(). Iterates each storage preset, serializes via
    // ep_to_json, deserializes via ep_from_json, and verifies the
    // canonical storage-profile values survive the round-trip while
    // the wear fields stay at struct defaults (storage presets must
    // NOT reset existing wear). stderr-only output (PASS/FAIL).
    // Called once from main.cpp alongside the env-module audits.
    static bool verify_storage_preset_round_trip();

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

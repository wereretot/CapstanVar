#pragma once
// ── CapstanVar Project File (.cvproject) ─────────────────────────────────────
// Saves and restores the complete session state:
//   - audio source file path (relative preferred, absolute fallback)
//   - all EngineParams (same JSON as .cvpr preset)
//   - full animation data (all curves + keyframes)
//   - render options
//   - timeline cursor position
//   - playback position (play_head)
//
// Format: JSON, extension .cvproject
// The audio file is NOT embedded — only the path is stored.
// On load, if the relative path can't be resolved the absolute path is tried.

#include "dsp_types.hpp"
#include "param_anim.hpp"
#include "render_engine.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <filesystem>
#include <fstream>
#include <optional>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── Serialise EngineParams ────────────────────────────────────────────────────
inline json ep_to_json_proj(const EngineParams& e) {
    json j;
    j["oxide_type"] = e.oxide_type;
    #define F(x) j[#x] = e.x
    F(ips_base); F(motor_health); F(motor_drag);  F(motor_boost);
    F(wow_dep);  F(flutter_dep);  F(scrape_flutter); F(tension_load); F(dropout_rate);
    F(drive);    F(bias);         F(replay_diff);  F(asperities);   F(barkhausen);
    F(crosstalk);F(print_through);F(demagnetization); F(oxide_shedding);
    F(hiss);     F(hiss_color);   F(mains_hum);    F(cutoff_base);
    F(head_bump);F(azimuth_drift);F(sticky_shed);
    #undef F
    return j;
}
inline EngineParams ep_from_json_proj(const json& j) {
    EngineParams e;
    auto g = [&](const char* k, float& v){ if(j.contains(k)) v=j[k].get<float>(); };
    g("ips_base",e.ips_base); g("motor_health",e.motor_health);
    g("motor_drag",e.motor_drag); g("motor_boost",e.motor_boost);
    g("wow_dep",e.wow_dep); g("flutter_dep",e.flutter_dep);
    g("scrape_flutter",e.scrape_flutter); g("tension_load",e.tension_load);
    g("dropout_rate",e.dropout_rate); g("drive",e.drive);
    g("bias",e.bias); g("replay_diff",e.replay_diff);
    g("asperities",e.asperities); g("barkhausen",e.barkhausen);
    g("crosstalk",e.crosstalk); g("print_through",e.print_through);
    g("demagnetization",e.demagnetization); g("oxide_shedding",e.oxide_shedding);
    g("hiss",e.hiss); g("hiss_color",e.hiss_color);
    g("mains_hum",e.mains_hum); g("cutoff_base",e.cutoff_base);
    g("head_bump",e.head_bump); g("azimuth_drift",e.azimuth_drift);
    g("sticky_shed",e.sticky_shed);
    if (j.contains("oxide_type")) e.oxide_type = j["oxide_type"].get<std::string>();
    return e;
}

// ── Project data ──────────────────────────────────────────────────────────────
struct ProjectData {
    // Metadata
    std::string app_version   = "1.0";
    std::string project_name;

    // Audio source
    std::string audio_path_abs;   // absolute path stored
    std::string audio_path_rel;   // relative to project file (preferred on load)

    // Parameters
    EngineParams params;

    // Animation
    ParamAnim anim;

    // Render defaults
    int   render_sr         = 44100;
    int   render_bit_depth  = 24;
    int   render_quality    = 2;   // SimQuality index
    bool  render_dither     = true;
    bool  render_normalize  = true;
    float render_preroll    = 0.f;
    int   render_threads    = 1;

    // Playback state
    double play_head        = 0.0;
    double anim_cursor      = 0.0;
};

// ── Save ──────────────────────────────────────────────────────────────────────
inline bool save_project(const std::string& path, const ProjectData& d) {
    json j;
    j["__format__"]      = "CapstanVar Project";
    j["__version__"]     = 2;
    j["project_name"]    = d.project_name;
    j["audio_path_abs"]  = d.audio_path_abs;
    j["audio_path_rel"]  = d.audio_path_rel;
    j["params"]          = ep_to_json_proj(d.params);
    j["anim"]            = d.anim.to_json();
    j["render_sr"]       = d.render_sr;
    j["render_bit_depth"]= d.render_bit_depth;
    j["render_quality"]  = d.render_quality;
    j["render_dither"]   = d.render_dither;
    j["render_normalize"]= d.render_normalize;
    j["render_preroll"]  = d.render_preroll;
    j["render_threads"]  = d.render_threads;
    j["play_head"]       = d.play_head;
    j["anim_cursor"]     = d.anim_cursor;

    try {
        std::ofstream f(path);
        if (!f) return false;
        f << j.dump(2);
        return f.good();
    } catch (...) { return false; }
}

// ── Load ──────────────────────────────────────────────────────────────────────
inline std::optional<ProjectData> load_project(const std::string& path) {
    try {
        std::ifstream f(path);
        if (!f) return std::nullopt;
        json j; f >> j;
        if (j.value("__format__","") != "CapstanVar Project")
            return std::nullopt;

        ProjectData d;
        d.project_name   = j.value("project_name", "");
        d.audio_path_abs = j.value("audio_path_abs", "");
        d.audio_path_rel = j.value("audio_path_rel", "");
        if (j.contains("params")) d.params = ep_from_json_proj(j["params"]);
        if (j.contains("anim"))   d.anim.from_json(j["anim"]);
        d.render_sr         = j.value("render_sr",        44100);
        d.render_bit_depth  = j.value("render_bit_depth", 24);
        d.render_quality    = j.value("render_quality",   2);
        d.render_dither     = j.value("render_dither",    true);
        d.render_normalize  = j.value("render_normalize", true);
        d.render_preroll    = j.value("render_preroll",   0.f);
        d.render_threads    = j.value("render_threads",   1);
        d.play_head         = j.value("play_head",        0.0);
        d.anim_cursor       = j.value("anim_cursor",      0.0);

        // Resolve audio path: try relative first, then absolute
        if (!d.audio_path_rel.empty()) {
            fs::path proj_dir = fs::path(path).parent_path();
            fs::path rel_resolved = proj_dir / d.audio_path_rel;
            std::error_code ec;
            if (fs::is_regular_file(rel_resolved, ec))
                d.audio_path_abs = rel_resolved.string();
        }
        return d;
    } catch (...) { return std::nullopt; }
}

// ── Build relative path from project file to audio file ──────────────────────
inline std::string make_relative_audio_path(const std::string& project_path,
                                             const std::string& audio_path) {
    try {
        fs::path pp = fs::path(project_path).parent_path();
        fs::path ap(audio_path);
        return fs::relative(ap, pp).string();
    } catch (...) { return ""; }
}

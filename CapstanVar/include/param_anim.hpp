#pragma once
// ── Parameter Animation System ───────────────────────────────────────────────
// Blender-style keyframe animation for all EngineParams float fields.
//
// Usage (mirrors Blender):
//   - Hover a slider and press I → inserts a keyframe at current time/value
//   - Sliders with keyframes glow their accent colour (green tint when animated)
//   - A timeline panel shows all curves; keyframes are draggable
//   - During playback, curves are evaluated and override _ui_params each frame
//   - During render, the render engine samples the curve per block
//
// Data model:
//   Each animatable parameter has a Curve (sorted list of Keyframe).
//   Keyframe: {time_samples, value}. Interpolation: Catmull-Rom cubic.
//   "time" is in SAMPLES (not seconds) so it stretches/compresses correctly
//   if the file length changes.
//
// Persistence: saved as JSON alongside the preset, keyed by param id.

#include "dsp_types.hpp"
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <optional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ── Keyframe ──────────────────────────────────────────────────────────────────
struct Keyframe {
    double time;    // position in samples (double for sub-sample precision)
    float  value;
    // Handles for bezier tangent (in normalised time-value space)
    float  tan_l = 0.f;   // left tangent (value slope)
    float  tan_r = 0.f;   // right tangent
};

// ── Curve: one animated parameter ────────────────────────────────────────────
struct AnimCurve {
    std::string         param_id;   // matches the id passed to _slider()
    std::string         label;      // display name
    std::vector<Keyframe> keys;     // always kept sorted by time
    bool                enabled = true;

    bool empty() const { return keys.empty(); }

    // Insert or update a keyframe at this time. Returns index.
    int insert(double time, float value) {
        // Check if a key already exists within 512 samples (snap zone)
        for (int i = 0; i < (int)keys.size(); ++i) {
            if (std::abs(keys[i].time - time) < 512.0) {
                keys[i].value = value;
                return i;
            }
        }
        Keyframe k; k.time = time; k.value = value;
        keys.push_back(k);
        _sort();
        for (int i = 0; i < (int)keys.size(); ++i)
            if (keys[i].time == k.time) return i;
        return 0;
    }

    void remove(int idx) {
        if (idx >= 0 && idx < (int)keys.size())
            keys.erase(keys.begin() + idx);
    }

    // Evaluate curve at time t using Catmull-Rom interpolation.
    // Returns nullopt if no keys exist.
    std::optional<float> evaluate(double t) const {
        if (keys.empty()) return std::nullopt;
        if (keys.size() == 1) return keys[0].value;
        if (t <= keys.front().time) return keys.front().value;
        if (t >= keys.back().time)  return keys.back().value;

        // Find the segment
        int i = 0;
        for (int j = 0; j < (int)keys.size()-1; ++j) {
            if (keys[j].time <= t && t < keys[j+1].time) { i = j; break; }
        }

        // Catmull-Rom: uses points i-1, i, i+1, i+2
        double dt  = keys[i+1].time - keys[i].time;
        float  frac = (float)((t - keys[i].time) / dt);

        float p0 = (i > 0)                   ? keys[i-1].value : keys[i].value;
        float p1 = keys[i].value;
        float p2 = keys[i+1].value;
        float p3 = (i+2 < (int)keys.size())  ? keys[i+2].value : keys[i+1].value;

        float t2 = frac * frac, t3 = t2 * frac;
        return 0.5f * ((2.f*p1)
                     + (-p0 + p2) * frac
                     + (2.f*p0 - 5.f*p1 + 4.f*p2 - p3) * t2
                     + (-p0 + 3.f*p1 - 3.f*p2 + p3) * t3);
    }

    void _sort() {
        std::sort(keys.begin(), keys.end(),
                  [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
    }

    json to_json() const {
        json j; j["id"] = param_id; j["label"] = label; j["enabled"] = enabled;
        json ks = json::array();
        for (auto& k : keys) ks.push_back({{"t",k.time},{"v",k.value}});
        j["keys"] = ks;
        return j;
    }
    static AnimCurve from_json(const json& j) {
        AnimCurve c;
        c.param_id = j.value("id",""); c.label = j.value("label","");
        c.enabled  = j.value("enabled", true);
        for (auto& k : j["keys"])
            c.keys.push_back({k["t"].get<double>(), k["v"].get<float>()});
        c._sort();
        return c;
    }
};

// ── ParamAnim: the full animation data for one session ────────────────────────
// Holds all curves, knows about total_samples for the timeline display.
// Evaluated each UI frame to produce an override EngineParams.
struct ParamAnim {
    std::unordered_map<std::string, AnimCurve> curves;
    bool enabled = true;   // global on/off

    // Return true if param_id has any keyframes
    bool has_keys(const std::string& id) const {
        auto it = curves.find(id);
        return it != curves.end() && !it->second.empty();
    }

    // Insert keyframe for param_id at time_samples with value.
    // Creates the curve if it doesn't exist. label is for display.
    void insert_key(const std::string& id, const std::string& label,
                    double time_samples, float value)
    {
        auto& c = curves[id];
        c.param_id = id;
        c.label    = label;
        c.insert(time_samples, value);
    }

    void delete_key(const std::string& id, int key_idx) {
        auto it = curves.find(id);
        if (it != curves.end()) {
            it->second.remove(key_idx);
            if (it->second.empty()) curves.erase(it);
        }
    }

    void clear_curve(const std::string& id) { curves.erase(id); }
    void clear_all()  { curves.clear(); }

    // Apply all curves to params at time_samples.
    // Only animated params are overridden; others keep their ui values.
    void apply(EngineParams& p, double time_samples) const {
        if (!enabled) return;
        struct Map { const char* id; float EngineParams::* field; float mn; float mx; };
        static const Map table[] = {
            {"ips",     &EngineParams::ips_base,        0.5f,  30.f},
            {"mh",      &EngineParams::motor_health,    0.f,   10.f},
            {"mdrag",   &EngineParams::motor_drag,      0.f,   0.9f},
            {"mboost",  &EngineParams::motor_boost,     0.f,   0.9f},
            {"wow",     &EngineParams::wow_dep,         0.f,   30.f},
            {"flt",     &EngineParams::flutter_dep,     0.f,   10.f},
            {"scr",     &EngineParams::scrape_flutter,  0.f,   1.f},
            {"tens",    &EngineParams::tension_load,    0.f,   0.12f},
            {"drop",    &EngineParams::dropout_rate,    0.f,   1.f},
            {"drv",     &EngineParams::drive,           1.f,   20.f},
            {"bias",    &EngineParams::bias,            0.5f,  3.f},
            {"rd",      &EngineParams::replay_diff,     0.f,   1.f},
            {"asp",     &EngineParams::asperities,      0.f,   0.5f},
            {"bark",    &EngineParams::barkhausen,      0.f,   0.1f},
            {"xtk",     &EngineParams::crosstalk,       0.f,   0.5f},
            {"prt",     &EngineParams::print_through,   0.f,   0.1f},
            {"dmg",     &EngineParams::demagnetization, 0.f,   0.99f},
            {"shed",    &EngineParams::oxide_shedding,  0.f,   1.f},
            {"hiss",    &EngineParams::hiss,            0.f,   0.02f},
            {"hcol",    &EngineParams::hiss_color,      0.f,   1.f},
            {"hum",     &EngineParams::mains_hum,       0.f,   0.05f},
            {"cut",     &EngineParams::cutoff_base,     500.f, 22000.f},
            {"bump",    &EngineParams::head_bump,       0.f,   5.f},
            {"azdrift", &EngineParams::azimuth_drift,   0.f,   1.f},
            {"sticky",  &EngineParams::sticky_shed,     0.f,   1.f},
        };
        for (auto& m : table) {
            auto it = curves.find(m.id);
            if (it == curves.end() || !it->second.enabled) continue;
            if (auto v = it->second.evaluate(time_samples)) {
                // Clamp to param range — prevents Catmull-Rom overshoot from
                // pushing values outside the physically valid range.
                p.*m.field = std::max(m.mn, std::min(m.mx, *v));
            }
        }
    }

    json to_json() const {
        json j; j["enabled"] = enabled;
        json cs = json::array();
        for (auto& [id, c] : curves) cs.push_back(c.to_json());
        j["curves"] = cs;
        return j;
    }
    void from_json(const json& j) {
        curves.clear();
        enabled = j.value("enabled", true);
        if (j.contains("curves"))
            for (auto& cj : j["curves"]) {
                AnimCurve c = AnimCurve::from_json(cj);
                curves[c.param_id] = c;
            }
    }
};

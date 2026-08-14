#include "error_log.hpp"
#include <cstdio>
#include "preset_manager.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void ep_to_json(json& j, const std::string& name, const EngineParams& e) {
    j["__version__"]   = 2;          // Phase 2: extended EQ / format schema
    j["__app__"]       = "CapstanVar";
    j["__name__"]      = name;
    j["oxide_type"]    = e.oxide_type;
    j["eq_curve"]      = eq_curve_name(e.eq_curve);
    j["format_id"]     = e.format_id;
    j["format_locked"] = e.format_locked;
    j["lf_trim_db"]    = e.lf_trim_db;
    j["hf_trim_db"]    = e.hf_trim_db;
    #define F(x) j[#x] = e.x
    F(input_gain);
    F(ips_base); F(motor_health); F(motor_drag); F(motor_boost);
    F(wow_dep);  F(flutter_dep);  F(scrape_flutter); F(tension_load); F(dropout_rate);
    F(drive);    F(bias);         F(replay_diff);     F(asperities);   F(barkhausen);
    F(crosstalk);F(print_through);F(demagnetization); F(oxide_shedding);
    F(hiss);     F(hiss_color);   F(mains_hum);       F(cutoff_base);
    F(head_bump);F(azimuth_drift);F(sticky_shed);
    // Phase 5 — Environment & aging (append-only). All 6 env_* keys
    // ride the same F() macro because the macro generates `j[#x] = e.x`
    // textually and nlohmann_json accepts double + uint32 on the
    // assignment operator transparently. The get<>() tagged-type
    // requirement applies only to the LOADER side (ep_from_json),
    // not the writer. See docs/PRESET_SCHEMA.md "Phase 5 fields" and
    // docs/TAPE_PHYSICS_REFACTOR.md §X "Phase 5 schema migration".
    F(env_temperature_c);     F(env_humidity_pct);
    F(env_age_acceleration);  F(env_age_seconds);
    F(env_tape_health);       F(env_failure_modes);
    #undef F
}

static EngineParams ep_from_json(const json& j) {
    EngineParams e;
    auto get = [&](const char* k, float& v){ if(j.contains(k)) v = j[k].get<float>(); };
    get("input_gain",e.input_gain);
    get("ips_base",e.ips_base); get("motor_health",e.motor_health);
    get("motor_drag",e.motor_drag); get("motor_boost",e.motor_boost);
    get("wow_dep",e.wow_dep); get("flutter_dep",e.flutter_dep);
    get("scrape_flutter",e.scrape_flutter); get("tension_load",e.tension_load);
    get("dropout_rate",e.dropout_rate); get("drive",e.drive);
    get("bias",e.bias); get("replay_diff",e.replay_diff);
    get("asperities",e.asperities); get("barkhausen",e.barkhausen);
    get("crosstalk",e.crosstalk); get("print_through",e.print_through);
    get("demagnetization",e.demagnetization); get("oxide_shedding",e.oxide_shedding);
    get("hiss",e.hiss); get("hiss_color",e.hiss_color);
    get("mains_hum",e.mains_hum); get("cutoff_base",e.cutoff_base);
    get("head_bump",e.head_bump); get("azimuth_drift",e.azimuth_drift);
    get("sticky_shed",e.sticky_shed);
    // Phase 5 — Environment & aging (append-only). Four float-valued
    // env_* keys ride the same get() lambda as transport/magnetic/
    // electronic numerics. The 2 non-float env_* keys (env_age_seconds
    // = double, env_failure_modes = uint32) need explicit get<type>()
    // calls because auto `get` lambda above is float-typed, per
    // docs/TAPE_PHYSICS_REFACTOR.md §X F() macro gotcha note.
    get("env_temperature_c",e.env_temperature_c);
    get("env_humidity_pct",e.env_humidity_pct);
    get("env_age_acceleration",e.env_age_acceleration);
    get("env_tape_health",e.env_tape_health);
    if (j.contains("env_age_seconds"))
        e.env_age_seconds = j["env_age_seconds"].get<double>();
    if (j.contains("env_failure_modes"))
        e.env_failure_modes = j["env_failure_modes"].get<uint32_t>();
    if (j.contains("oxide_type"))    e.oxide_type   = j["oxide_type"].get<std::string>();
    if (j.contains("format_id")) {
        std::string fid = j["format_id"].get<std::string>();
        if (!fid.empty() && !tape_format_by_id(fid)) {
            // Phase 4 fix: warn + clear stale format_ids left over from
            // pre-rename JSON (e.g., "Ampex_456_30_NAB" → "Ampex_456_30").
            // Without this the dropdown would silently show "Custom (no
            // coupling)" because the stale id no longer resolves to a
            // catalog entry. Coupled oxide/eq/ips values are still loaded
            // verbatim from JSON (ep_to_json wrote them out), so audibility
            // is preserved.
            std::string nm = j.value("__name__", std::string{"(unnamed)"});
            CV_ERR(PRESET_FORMAT_UNKNOWN,
                   "imported preset \u2018" + nm + "\u2019 references unknown format_id \u2018" +
                   fid + "\u2019 \u2014 clearing");
        } else {
            e.format_id = fid;
        }
    }
    if (j.contains("format_locked") && !e.format_id.empty()) {
        e.format_locked = j["format_locked"].get<bool>();
    } else {
        // If the catalog id cleared, force detach so UI shows "Custom".
        e.format_locked = false;
    }
    if (j.contains("lf_trim_db"))    e.lf_trim_db   = j["lf_trim_db"].get<float>();
    if (j.contains("hf_trim_db"))    e.hf_trim_db   = j["hf_trim_db"].get<float>();
    if (j.contains("eq_curve"))
        e.eq_curve = eq_curve_from_name(j["eq_curve"].get<std::string>());
    // eq_curve defaults to Legacy if missing (Phase 1 / pre-Phase-2 files
    // load unchanged audibly because the LP path is preserved).
    return e;
}

// ── Macro to build a preset entry compactly ───────────────────────────────────
#define BEGIN_PRESET(NAME, IPS) { \
    EngineParams e = PresetManager::default_params(); \
    const char* _pname = NAME; \
    e.ips_base = IPS;
#define P(k,v) e.k = v;
#define END_PRESET _builtins.push_back({_pname, e}); }

// ── Format-coupling helper ────────────────────────────────────────────────────
// Phase 2/3 re-authoring: each reference-fidelity preset pulls its coupled
// parameters (ips, eq_curve, oxide, bias, fluxivity, motor_health, hiss_floor)
// from the TapeFormat catalog. The preset's free-form mechanical params
// (motor_health*dial, wow/flutter, drive, hiss, head_bump, etc.) stay
// independent so users can still tune the character without detaching the
// format. format_locked is set true so the UI dropdown shows the catalog
// label with the locked badge; touching any of the coupled knobs detaches.
//
// format_id is the canonical catalog key, e.g. "Studer_A820_15_IEC". Unknown
// ids log a stderr warning at startup so missing catalog entries are loud
// rather than silently degraded to Legacy free-form.
static void apply_format(EngineParams& e, const std::string& fid) {
    auto* f = tape_format_by_id(fid);
    if (!f) {
        if (!fid.empty())
            CV_ERR(PRESET_FORMAT_UNKNOWN,
                   "preset references unknown format_id '" + fid + "' — left free-form");
        return;
    }
    e.format_id     = f->id;
    e.format_locked = true;
    e.ips_base      = f->ips;
    e.eq_curve      = f->eq_curve;
    e.oxide_type    = f->oxide;
    e.bias          = f->bias_recommend;
}

EngineParams PresetManager::default_params() {
    return EngineParams{};  // all defaults defined in struct
}

void PresetManager::_build_builtins() {
    // ── Studio / Pro ──────────────────────────────────────────────────────────
    BEGIN_PRESET("Ampex 456 (30ips)", 30.0f)
        // NAB broadcast standard — pulls 456 oxide (MOL +6 dB, SM-grade
        // sensitivity), AES-30 playback EQ shelf, bias trim from catalog.
        // Phase 1/2/3 physics (MOL remap, HF rolloff, hysteresis) follow.
        apply_format(e, "Ampex_456_30");
        P(motor_health,0.02f) P(wow_dep,0.008f) P(flutter_dep,0.004f)
        P(scrape_flutter,0.025f) P(drive,1.05f)               // bias: catalog 1.0
        P(hiss,0.000020f) P(hiss_color,0.1f)                 // cutoff_base: dropped (eq_curve != Legacy)
        P(head_bump,0.28f) P(print_through,0.005f) P(replay_diff,0.32f)
        P(mains_hum,0.000008f) P(barkhausen,0.004f) P(asperities,0.006f)
    END_PRESET
    BEGIN_PRESET("Ampex 456 (15ips)", 15.0f)
        // NAB broadcast standard — 456 oxide, NAB-15 playback EQ.
        apply_format(e, "Ampex_456_15_NAB");
        P(motor_health,0.04f) P(wow_dep,0.018f) P(flutter_dep,0.006f)
        P(scrape_flutter,0.032f) P(drive,1.08f)
        P(hiss,0.000039f) P(hiss_color,0.12f)
        P(head_bump,0.42f) P(print_through,0.006f) P(replay_diff,0.30f)
        P(mains_hum,0.000012f) P(barkhausen,0.006f) P(asperities,0.009f)
    END_PRESET
    BEGIN_PRESET("Studer A820 (30ips)", 30.0f)
        // IEC mastering standard — SM911 oxide (low-noise balanced),
        // IEC-15 playback EQ shelf, pristine transport.
        apply_format(e, "Studer_A820_30_IEC");
        P(motor_health,0.01f) P(wow_dep,0.005f) P(flutter_dep,0.003f)
        P(scrape_flutter,0.018f) P(drive,1.02f)
        P(hiss,0.000013f) P(hiss_color,0.07f)
        P(head_bump,0.20f) P(print_through,0.004f) P(replay_diff,0.35f)
        P(mains_hum,0.000005f) P(barkhausen,0.002f) P(asperities,0.004f)
    END_PRESET
    BEGIN_PRESET("Studer A820 (15ips)", 15.0f)
        // IEC mastering standard — SM911 oxide, IEC-15 playback EQ shelf.
        apply_format(e, "Studer_A820_15_IEC");
        P(motor_health,0.025f) P(wow_dep,0.010f) P(flutter_dep,0.004f)
        P(scrape_flutter,0.025f) P(drive,1.04f)
        P(hiss,0.000025f) P(hiss_color,0.09f)
        P(head_bump,0.35f) P(print_through,0.005f) P(replay_diff,0.32f)
        P(mains_hum,0.000008f) P(barkhausen,0.004f) P(asperities,0.006f)
    END_PRESET
    BEGIN_PRESET("Otari MTR-90 (30ips)", 30.0f)
        // Japanese 1/2-inch mastering deck; pulls SM911 oxide + AES-30 EQ.
        apply_format(e, "Otari_MTR_90_30_AES");
        P(motor_health,0.012f) P(wow_dep,0.006f) P(flutter_dep,0.003f)
        P(scrape_flutter,0.022f) P(drive,1.03f) P(bias,1.0f)
        P(hiss,0.000016f) P(hiss_color,0.08f) P(cutoff_base,22000.f)
        P(head_bump,0.24f) P(print_through,0.005f) P(replay_diff,0.33f)
        P(mains_hum,0.000006f) P(barkhausen,0.003f) P(asperities,0.005f)
    END_PRESET
    BEGIN_PRESET("Otari MTR-90 (15ips)", 15.0f)
        // Otari MTR-90 at 15 ips; pulls SM911 oxide + NAB-15 EQ.
        apply_format(e, "Otari_MTR_90_15_NAB");
        P(motor_health,0.035f) P(wow_dep,0.013f) P(flutter_dep,0.005f)
        P(scrape_flutter,0.028f) P(drive,1.06f) P(bias,1.0f)
        P(hiss,0.000031f) P(hiss_color,0.10f) P(cutoff_base,20500.f)
        P(head_bump,0.38f) P(print_through,0.005f) P(replay_diff,0.31f)
        P(mains_hum,0.000010f) P(barkhausen,0.005f) P(asperities,0.007f)
    END_PRESET
    BEGIN_PRESET("MCI JH-24 (30ips)", 30.0f)
        // American MCI/Quantegy 24-track mastering deck; 456 oxide + AES-30 EQ.
        apply_format(e, "MCI_JH_24_30_AES");
        P(motor_health,0.020f) P(wow_dep,0.009f) P(flutter_dep,0.005f)
        P(scrape_flutter,0.030f) P(drive,1.07f) P(bias,1.0f)
        P(hiss,0.000020f) P(hiss_color,0.18f) P(cutoff_base,21000.f)
        P(head_bump,0.35f) P(print_through,0.006f) P(replay_diff,0.28f)
        P(mains_hum,0.000015f) P(barkhausen,0.005f) P(asperities,0.008f)
    END_PRESET
    BEGIN_PRESET("Scotch 226 (7.5ips)", 7.5f)
        // 3M Scotch 226 medium-output stock; American NAB-7.5 EQ + 456 oxide
        // (closest MOL match in the 10-stock OxygenProps table).
        apply_format(e, "Scotch_226_7_5_NAB");
        P(motor_health,0.15f) P(wow_dep,0.06f) P(flutter_dep,0.014f)
        P(scrape_flutter,0.050f) P(drive,1.22f) P(bias,0.95f)
        P(hiss,0.000079f) P(hiss_color,0.22f) P(cutoff_base,16000.f)
        P(head_bump,0.58f) P(print_through,0.007f) P(replay_diff,0.28f)
        P(mains_hum,0.000020f) P(barkhausen,0.010f) P(asperities,0.014f)
    END_PRESET
    BEGIN_PRESET("Revox B77 (7.5ips)", 7.5f)
        // Catalog uses BASF_LH oxide + IEC-7.5 EQ (Swiss consumer deck
        // convention — id label "NAB" in catalog is a vestigial name).
        apply_format(e, "Revox_B77_7_5");
        P(motor_health,0.12f) P(wow_dep,0.05f) P(flutter_dep,0.011f)
        P(scrape_flutter,0.045f) P(drive,1.18f)
        P(hiss,0.000063f) P(hiss_color,0.18f)
        P(head_bump,0.52f) P(print_through,0.006f) P(replay_diff,0.29f)
        P(mains_hum,0.000018f) P(barkhausen,0.009f) P(asperities,0.012f)
    END_PRESET
    BEGIN_PRESET("Revox B77 (3.75ips)", 3.75f)
        // Catalog uses BASF_LH oxide + NAB-3.75 EQ.
        apply_format(e, "Revox_B77_3_75_NAB");
        P(motor_health,0.30f) P(wow_dep,0.18f) P(flutter_dep,0.028f)
        P(scrape_flutter,0.070f) P(drive,1.38f)
        P(hiss,0.000158f) P(hiss_color,0.30f)
        P(head_bump,0.80f) P(print_through,0.008f) P(replay_diff,0.24f)
        P(mains_hum,0.000025f) P(barkhausen,0.014f) P(asperities,0.018f)
    END_PRESET
    // ── Consumer reel ─────────────────────────────────────────────────────────
    BEGIN_PRESET("BASF LH Super (7.5ips)", 7.5f)
        // BASF LH Super tape stock on consumer 7.5 ips deck — catalog
        // entry uses BASF_LH oxide + IEC-7.5 EQ (consumer reel convention).
        apply_format(e, "Revox_B77_7_5");
        P(motor_health,0.28f) P(wow_dep,0.10f) P(flutter_dep,0.020f)
        P(scrape_flutter,0.055f) P(drive,1.28f)
        P(hiss,0.000100f) P(hiss_color,0.26f)
        P(head_bump,0.70f) P(print_through,0.008f) P(replay_diff,0.25f)
        P(mains_hum,0.000025f) P(barkhausen,0.014f) P(asperities,0.018f)
        P(crosstalk,0.03f) P(tension_load,0.008f)
    END_PRESET
    BEGIN_PRESET("Maxell UD (7.5ips)", 7.5f)
        // Maxell UD consumer reel — Maxell_UD oxide + IEC-7.5 EQ.
        apply_format(e, "Maxell_UD_7_5");
        P(motor_health,0.24f) P(wow_dep,0.09f) P(flutter_dep,0.017f)
        P(scrape_flutter,0.048f) P(drive,1.24f)
        P(hiss,0.000089f) P(hiss_color,0.24f)
        P(head_bump,0.62f) P(print_through,0.007f) P(replay_diff,0.26f)
        P(mains_hum,0.000020f) P(barkhausen,0.012f) P(asperities,0.016f)
        P(crosstalk,0.025f) P(tension_load,0.006f)
    END_PRESET
    BEGIN_PRESET("Tascam 38 (7.5ips)", 7.5f)
        // Japanese Tascam/TEAC 1/2-inch deck; IEC-7.5 EQ + BASF_LH oxide.
        apply_format(e, "Tascam_38_7_5_IEC");
        P(motor_health,0.35f) P(wow_dep,0.14f) P(flutter_dep,0.026f)
        P(scrape_flutter,0.060f) P(drive,1.32f) P(bias,0.92f)
        P(hiss,0.000125f) P(hiss_color,0.28f) P(cutoff_base,14500.f)
        P(head_bump,0.78f) P(print_through,0.007f) P(replay_diff,0.26f)
        P(crosstalk,0.06f) P(mains_hum,0.000030f)
        P(barkhausen,0.016f) P(asperities,0.020f) P(tension_load,0.010f)
    END_PRESET
    // ── Multitrack ────────────────────────────────────────────────────────────
    BEGIN_PRESET("4-Track Portastudio (1.875ips)", 1.875f)
        P(motor_health,0.70f) P(wow_dep,0.90f) P(flutter_dep,0.090f)
        P(scrape_flutter,0.090f) P(drive,1.65f) P(bias,0.82f)
        P(hiss,0.000316f) P(hiss_color,0.55f) P(cutoff_base,10000.f)
        P(head_bump,0.95f) P(replay_diff,0.18f) P(crosstalk,0.18f)
        P(mains_hum,0.000040f) P(barkhausen,0.030f) P(asperities,0.040f)
        P(tension_load,0.020f)
    END_PRESET
    BEGIN_PRESET("8-Track Cartridge", 3.75f)
        P(motor_health,0.80f) P(wow_dep,0.75f) P(flutter_dep,0.075f)
        P(scrape_flutter,0.110f) P(drive,1.55f) P(bias,0.85f)
        P(hiss,0.000251f) P(hiss_color,0.50f) P(cutoff_base,9000.f)
        P(head_bump,1.10f) P(replay_diff,0.17f) P(crosstalk,0.22f)
        P(mains_hum,0.000035f) P(tension_load,0.028f)
        P(barkhausen,0.025f) P(asperities,0.035f)
    END_PRESET
    // ── Cassette ──────────────────────────────────────────────────────────────
    BEGIN_PRESET("Type I (Fe2O3) Normal", 1.875f)
        // Cassette Type I — Fe2O3 oxide, 3180+120µs EQ shelf.
        apply_format(e, "Cassette_Type_I");
        P(motor_health,0.50f) P(wow_dep,0.70f) P(flutter_dep,0.070f)
        P(scrape_flutter,0.065f) P(drive,1.45f)
        P(hiss,0.000199f) P(hiss_color,0.45f)
        P(head_bump,0.80f) P(replay_diff,0.20f)
        P(mains_hum,0.000030f) P(barkhausen,0.025f) P(asperities,0.032f)
        P(crosstalk,0.14f) P(tension_load,0.015f)
    END_PRESET
    BEGIN_PRESET("Type II Chrome (CrO2)", 1.875f)
        // Cassette Type II — CrO2 oxide (high-output), 3180+70µs EQ shelf.
        // Phase 3 activates hysteresis_amt=0.10 + sens_10k=1.0 here (vs
        // Fe2O3 0.85) — gently brighter HF saturation.
        apply_format(e, "Cassette_Type_II");
        P(motor_health,0.40f) P(wow_dep,0.55f) P(flutter_dep,0.058f)
        P(scrape_flutter,0.055f) P(drive,1.35f)
        P(hiss,0.000125f) P(hiss_color,0.30f)
        P(head_bump,0.60f) P(replay_diff,0.25f)
        P(mains_hum,0.000022f) P(barkhausen,0.016f) P(asperities,0.022f)
        P(crosstalk,0.08f) P(tension_load,0.010f)
    END_PRESET
    BEGIN_PRESET("Type IV Metal", 1.875f)
        // Cassette Type IV — Metal particle, 3180+70µs EQ shelf, 160 nWb/m
        // fluxivity. Phase 3 hysteresis_amt=0.20 weights the Preisach
        // blend most heavily here.
        apply_format(e, "Cassette_Type_IV");
        P(motor_health,0.28f) P(wow_dep,0.40f) P(flutter_dep,0.045f)
        P(scrape_flutter,0.042f) P(drive,1.25f)
        P(hiss,0.000079f) P(hiss_color,0.18f)
        P(head_bump,0.45f) P(replay_diff,0.30f)
        P(mains_hum,0.000015f) P(barkhausen,0.010f) P(asperities,0.014f)
        P(crosstalk,0.05f) P(tension_load,0.007f)
    END_PRESET
    BEGIN_PRESET("Dolby B (Type I)", 1.875f)
        // Dolby-B NR cassette — same Type I physics + EQ, with hiss much
        // lower because the model represents the decoded playback.
        apply_format(e, "Cassette_Type_I");
        P(motor_health,0.45f) P(wow_dep,0.60f) P(flutter_dep,0.065f)
        P(scrape_flutter,0.060f) P(drive,1.40f)
        P(hiss,0.000063f) P(hiss_color,0.18f)
        P(head_bump,0.72f) P(replay_diff,0.22f)
        P(mains_hum,0.000025f) P(barkhausen,0.020f) P(asperities,0.026f)
        P(crosstalk,0.12f)
    END_PRESET
    BEGIN_PRESET("Dolby C (Type II)", 1.875f)
        // Dolby-C NR Type II — CrO2 oxide + 70µs EQ shelf, even lower
        // hiss (more aggressive NR decoding assumption).
        apply_format(e, "Cassette_Type_II");
        P(motor_health,0.35f) P(wow_dep,0.48f) P(flutter_dep,0.052f)
        P(scrape_flutter,0.050f) P(drive,1.30f)
        P(hiss,0.000031f) P(hiss_color,0.14f)
        P(head_bump,0.55f) P(replay_diff,0.24f)
        P(mains_hum,0.000018f) P(barkhausen,0.014f) P(asperities,0.018f)
        P(crosstalk,0.06f) P(tension_load,0.008f)
    END_PRESET
    BEGIN_PRESET("Lo-Fi Bedroom (Type I)", 1.875f)
        P(motor_health,1.00f) P(wow_dep,1.20f) P(flutter_dep,0.110f)
        P(scrape_flutter,0.100f) P(drive,1.80f) P(bias,0.75f)
        P(hiss,0.000316f) P(hiss_color,0.62f) P(cutoff_base,9000.f)
        P(head_bump,1.05f) P(replay_diff,0.15f)
        P(mains_hum,0.000060f) P(barkhausen,0.038f) P(asperities,0.050f)
        P(crosstalk,0.20f) P(tension_load,0.022f) P(azimuth_drift,0.10f)
    END_PRESET
    // ── Video / Broadcast ─────────────────────────────────────────────────────
    BEGIN_PRESET("VHS Linear Audio", 1.3125f)
        P(motor_health,0.90f) P(wow_dep,1.00f) P(flutter_dep,0.085f)
        P(scrape_flutter,0.085f) P(drive,1.55f) P(bias,0.80f)
        P(hiss,0.000397f) P(hiss_color,0.60f) P(cutoff_base,8000.f)
        P(head_bump,1.00f) P(replay_diff,0.15f)
        P(mains_hum,0.000100f) P(crosstalk,0.28f) P(asperities,0.048f)
        P(tension_load,0.022f)
    END_PRESET
    BEGIN_PRESET("Betamax Audio", 1.873f)
        P(motor_health,0.70f) P(wow_dep,0.80f) P(flutter_dep,0.072f)
        P(scrape_flutter,0.072f) P(drive,1.48f) P(bias,0.82f)
        P(hiss,0.000316f) P(hiss_color,0.50f) P(cutoff_base,9500.f)
        P(head_bump,0.88f) P(replay_diff,0.17f)
        P(mains_hum,0.000080f) P(crosstalk,0.22f) P(asperities,0.042f)
        P(tension_load,0.018f)
    END_PRESET
    BEGIN_PRESET("U-Matic Low Band", 3.75f)
        P(motor_health,0.45f) P(wow_dep,0.45f) P(flutter_dep,0.048f)
        P(scrape_flutter,0.060f) P(drive,1.42f) P(bias,0.85f)
        P(hiss,0.000251f) P(hiss_color,0.45f) P(cutoff_base,10000.f)
        P(head_bump,0.82f) P(replay_diff,0.20f)
        P(mains_hum,0.000060f) P(crosstalk,0.18f) P(asperities,0.036f)
        P(tension_load,0.015f) P(print_through,0.005f)
    END_PRESET
    // ── Damaged ───────────────────────────────────────────────────────────────
    BEGIN_PRESET("Sticky Shed Syndrome", 7.5f)
        P(sticky_shed,0.55f) P(motor_health,1.80f)
        P(wow_dep,2.80f) P(flutter_dep,0.200f) P(scrape_flutter,0.200f)
        P(drive,1.70f) P(hiss,0.000500f) P(hiss_color,0.65f)
        P(cutoff_base,5500.f) P(head_bump,1.10f)
        P(dropout_rate,0.30f) P(oxide_shedding,0.40f) P(tension_load,0.040f)
    END_PRESET
    BEGIN_PRESET("Baked Tape (Post-Oven)", 7.5f)
        P(sticky_shed,0.12f) P(motor_health,0.50f)
        P(wow_dep,0.50f) P(flutter_dep,0.042f) P(scrape_flutter,0.055f)
        P(drive,1.45f) P(hiss,0.000200f) P(hiss_color,0.35f)
        P(cutoff_base,12000.f) P(head_bump,0.72f)
        P(dropout_rate,0.030f) P(oxide_shedding,0.06f)
        P(tension_load,0.012f) P(demagnetization,0.08f)
    END_PRESET
    BEGIN_PRESET("Mouldy Attic Find", 7.5f)
        P(motor_health,1.20f) P(wow_dep,1.80f) P(flutter_dep,0.145f)
        P(scrape_flutter,0.150f) P(drive,1.75f)
        P(hiss,0.000380f) P(hiss_color,0.68f) P(cutoff_base,6500.f)
        P(head_bump,1.05f) P(print_through,0.012f) P(demagnetization,0.38f)
        P(dropout_rate,0.22f) P(oxide_shedding,0.24f)
        P(asperities,0.065f) P(barkhausen,0.030f)
    END_PRESET
    BEGIN_PRESET("Dropout Disaster", 15.0f)
        P(motor_health,0.25f) P(wow_dep,0.22f) P(flutter_dep,0.018f)
        P(scrape_flutter,0.040f) P(drive,1.35f)
        P(hiss,0.000090f) P(hiss_color,0.25f) P(cutoff_base,14000.f)
        P(head_bump,0.52f) P(dropout_rate,0.70f) P(oxide_shedding,0.42f)
        P(asperities,0.055f) P(demagnetization,0.07f)
    END_PRESET
    BEGIN_PRESET("Heavily Demagnetised", 15.0f)
        P(motor_health,0.20f) P(wow_dep,0.18f) P(flutter_dep,0.015f)
        P(scrape_flutter,0.030f) P(drive,1.28f)
        P(hiss,0.000120f) P(hiss_color,0.45f) P(cutoff_base,7500.f)
        P(head_bump,0.60f) P(demagnetization,0.50f)
        P(print_through,0.008f) P(replay_diff,0.15f)
    END_PRESET
    BEGIN_PRESET("Warped & Fighting Motors", 15.0f)
        P(motor_health,2.20f) P(motor_drag,0.28f) P(motor_boost,0.28f)
        P(wow_dep,4.00f) P(flutter_dep,0.280f) P(tension_load,0.048f)
        P(drive,1.55f) P(scrape_flutter,0.140f)
        P(hiss,0.000199f) P(cutoff_base,12000.f) P(head_bump,0.75f)
        P(dropout_rate,0.07f)
    END_PRESET
    BEGIN_PRESET("Chewed Tape", 7.5f)
        P(motor_health,1.50f) P(wow_dep,2.50f) P(flutter_dep,0.240f)
        P(scrape_flutter,0.220f) P(drive,1.95f)
        P(hiss,0.000500f) P(hiss_color,0.72f) P(cutoff_base,5500.f)
        P(head_bump,1.20f) P(dropout_rate,0.55f) P(oxide_shedding,0.45f)
        P(asperities,0.100f) P(demagnetization,0.18f) P(tension_load,0.042f)
    END_PRESET
    BEGIN_PRESET("Print-Through Ghost", 15.0f)
        P(motor_health,0.06f) P(wow_dep,0.035f) P(flutter_dep,0.005f)
        P(scrape_flutter,0.022f) P(drive,1.08f)
        P(hiss,0.000035f) P(hiss_color,0.10f) P(cutoff_base,19000.f)
        P(head_bump,0.38f) P(print_through,0.016f) P(replay_diff,0.30f)
        P(demagnetization,0.04f) P(barkhausen,0.005f)
    END_PRESET
    BEGIN_PRESET("Stretched Tape", 15.0f)
        P(motor_health,0.80f) P(wow_dep,2.00f) P(flutter_dep,0.160f)
        P(scrape_flutter,0.110f) P(tension_load,0.038f)
        P(drive,1.45f) P(hiss,0.000158f) P(hiss_color,0.35f)
        P(cutoff_base,12000.f) P(head_bump,0.80f)
        P(dropout_rate,0.10f) P(oxide_shedding,0.14f) P(demagnetization,0.08f)
    END_PRESET
    BEGIN_PRESET("Heat Warped", 7.5f)
        P(motor_health,1.60f) P(wow_dep,3.50f) P(flutter_dep,0.200f)
        P(scrape_flutter,0.170f) P(tension_load,0.042f)
        P(drive,1.62f) P(hiss,0.000315f) P(hiss_color,0.55f)
        P(cutoff_base,7500.f) P(head_bump,0.95f)
        P(dropout_rate,0.18f) P(oxide_shedding,0.20f)
        P(demagnetization,0.16f) P(sticky_shed,0.18f)
    END_PRESET
    BEGIN_PRESET("Spliced Archive", 15.0f)
        P(motor_health,0.12f) P(wow_dep,0.12f) P(flutter_dep,0.010f)
        P(scrape_flutter,0.045f) P(drive,1.12f)
        P(hiss,0.000050f) P(hiss_color,0.14f) P(cutoff_base,18000.f)
        P(head_bump,0.42f) P(print_through,0.010f) P(replay_diff,0.30f)
        P(dropout_rate,0.04f) P(asperities,0.022f) P(mains_hum,0.000010f)
    END_PRESET
    BEGIN_PRESET("Soviet ORWO Copy", 15.0f)
        P(motor_health,0.40f) P(wow_dep,0.22f) P(flutter_dep,0.035f)
        P(scrape_flutter,0.075f) P(drive,1.45f) P(bias,0.88f)
        P(hiss,0.000125f) P(hiss_color,0.36f) P(cutoff_base,15000.f)
        P(head_bump,0.68f) P(print_through,0.009f) P(replay_diff,0.27f)
        P(mains_hum,0.000045f) P(barkhausen,0.022f) P(asperities,0.030f)
        P(crosstalk,0.04f)
    END_PRESET
    // ── Radio / Broadcast ─────────────────────────────────────────────────────
    BEGIN_PRESET("BBC Radiophonic (7.5ips)", 7.5f)
        // BBC Radiophonic Workshop reference: British mastering-style IEC-7.5
        // EQ + 456 oxide (closest MRL match to BBC's custom stock).
        apply_format(e, "BBC_Radiophonic_7_5_IEC");
        P(motor_health,0.10f) P(wow_dep,0.042f) P(flutter_dep,0.009f)
        P(scrape_flutter,0.042f) P(drive,1.18f) P(bias,0.98f)
        P(hiss,0.000079f) P(hiss_color,0.18f) P(cutoff_base,16000.f)
        P(head_bump,0.55f) P(print_through,0.007f) P(replay_diff,0.30f)
        P(mains_hum,0.000020f) P(barkhausen,0.008f) P(asperities,0.012f)
        P(dropout_rate,0.025f)
    END_PRESET
    BEGIN_PRESET("AM Radio Dub", 3.75f)
        P(motor_health,0.32f) P(wow_dep,0.20f) P(flutter_dep,0.025f)
        P(scrape_flutter,0.058f) P(drive,1.52f) P(bias,0.88f)
        P(hiss,0.000200f) P(hiss_color,0.50f) P(cutoff_base,5000.f)
        P(head_bump,0.80f) P(replay_diff,0.22f)
        P(mains_hum,0.000080f) P(crosstalk,0.10f) P(asperities,0.030f)
    END_PRESET
    // ── Lo-Fi / Special ───────────────────────────────────────────────────────
    BEGIN_PRESET("Ghetto Blaster", 1.875f)
        P(motor_health,1.10f) P(wow_dep,1.40f) P(flutter_dep,0.130f)
        P(scrape_flutter,0.120f) P(drive,1.72f) P(bias,1.1f)
        P(hiss,0.000398f) P(hiss_color,0.60f) P(cutoff_base,10000.f)
        P(head_bump,1.00f) P(replay_diff,0.17f)
        P(mains_hum,0.0f) P(crosstalk,0.24f) P(asperities,0.052f)
        P(tension_load,0.025f) P(azimuth_drift,0.12f)
    END_PRESET
    BEGIN_PRESET("Answering Machine", 1.2f)
        P(motor_health,1.40f) P(wow_dep,2.20f) P(flutter_dep,0.220f)
        P(scrape_flutter,0.185f) P(drive,2.10f) P(bias,0.75f)
        P(hiss,0.000631f) P(hiss_color,0.70f) P(cutoff_base,6000.f)
        P(head_bump,1.30f) P(replay_diff,0.12f)
        P(mains_hum,0.0f) P(crosstalk,0.32f) P(asperities,0.065f)
        P(azimuth_drift,0.18f) P(tension_load,0.032f)
    END_PRESET
    BEGIN_PRESET("Handheld Dictaphone", 0.9375f)
        P(motor_health,1.30f) P(wow_dep,1.90f) P(flutter_dep,0.190f)
        P(scrape_flutter,0.165f) P(drive,1.95f) P(bias,0.78f)
        P(hiss,0.000794f) P(hiss_color,0.75f) P(cutoff_base,5000.f)
        P(head_bump,1.20f) P(replay_diff,0.10f)
        P(mains_hum,0.0f) P(crosstalk,0.38f) P(asperities,0.070f)
        P(azimuth_drift,0.22f)
    END_PRESET
    BEGIN_PRESET("Toy Piano Recording", 1.875f)
        P(motor_health,3.50f) P(wow_dep,7.00f) P(flutter_dep,0.550f)
        P(scrape_flutter,0.280f) P(drive,2.40f) P(bias,0.65f)
        P(hiss,0.001000f) P(hiss_color,0.82f) P(cutoff_base,3500.f)
        P(head_bump,1.60f) P(replay_diff,0.10f)
        P(mains_hum,0.0f) P(crosstalk,0.45f) P(asperities,0.130f)
        P(dropout_rate,0.12f) P(tension_load,0.050f)
    END_PRESET
    // ── More Damaged ──────────────────────────────────────────────────────────
    BEGIN_PRESET("Tsunami Flood Tape", 7.5f)
        P(motor_health,1.60f) P(wow_dep,2.20f) P(flutter_dep,0.175f)
        P(scrape_flutter,0.200f) P(drive,1.80f)
        P(hiss,0.000600f) P(hiss_color,0.65f) P(cutoff_base,5500.f)
        P(head_bump,1.15f) P(dropout_rate,0.50f) P(oxide_shedding,0.45f)
        P(demagnetization,0.26f) P(asperities,0.095f)
        P(sticky_shed,0.35f) P(tension_load,0.042f)
    END_PRESET
    BEGIN_PRESET("Fire-Damaged Archive", 7.5f)
        P(motor_health,1.80f) P(wow_dep,3.00f) P(flutter_dep,0.240f)
        P(scrape_flutter,0.230f) P(drive,2.20f)
        P(hiss,0.000630f) P(hiss_color,0.75f) P(cutoff_base,4500.f)
        P(head_bump,1.25f) P(dropout_rate,0.60f) P(oxide_shedding,0.55f)
        P(demagnetization,0.45f) P(asperities,0.120f)
        P(sticky_shed,0.45f) P(tension_load,0.050f)
    END_PRESET
    BEGIN_PRESET("Played 1000 Times", 7.5f)
        P(motor_health,0.45f) P(wow_dep,0.40f) P(flutter_dep,0.055f)
        P(scrape_flutter,0.080f) P(drive,1.55f)
        P(hiss,0.000280f) P(hiss_color,0.40f) P(cutoff_base,8500.f)
        P(head_bump,0.78f) P(dropout_rate,0.08f) P(oxide_shedding,0.24f)
        P(demagnetization,0.32f) P(print_through,0.012f)
        P(asperities,0.055f) P(barkhausen,0.028f)
    END_PRESET
    // ── Phase 5 storage presets (4 built-in) — must align with
    // storage_profiles() in include/mod_environment.hpp. Each entry
    // sets ONLY env_temperature_c + env_humidity_pct (the two static
    // axes). env_age_acceleration / env_age_seconds / env_tape_health
    // / env_failure_modes stay at struct defaults so applying a storage
    // preset does NOT reset existing wear the user has accumulated.
    // Display names mirror storage_profiles() and are stable so the
    // UI storage combo resolution is consistent.
    BEGIN_PRESET("Controlled (climate-controlled vault)", 15.0f)
        P(env_temperature_c, 20.0f) P(env_humidity_pct, 50.0f)
    END_PRESET
    BEGIN_PRESET("Consumer Closet (typical bedroom)", 15.0f)
        P(env_temperature_c, 25.0f) P(env_humidity_pct, 60.0f)
    END_PRESET
    BEGIN_PRESET("Hot Attic (abandoned for years)", 15.0f)
        P(env_temperature_c, 35.0f) P(env_humidity_pct, 70.0f)
    END_PRESET
    BEGIN_PRESET("Cold Warehouse (unheated storage)", 15.0f)
        P(env_temperature_c, 10.0f) P(env_humidity_pct, 40.0f)
    END_PRESET
}

#undef BEGIN_PRESET
#undef P
#undef END_PRESET

// ── PresetManager methods ─────────────────────────────────────────────────────
PresetManager::PresetManager() {
 _build_builtins();
 }

std::optional<Preset> PresetManager::find_builtin(const std::string& name) const {
    for (auto& pr : _builtins)
        if (pr.name == name) return pr;
    return std::nullopt;
}

void PresetManager::save_session(const std::string& name, const EngineParams& p) {
    for (auto& s : _session) {
        if (s.name == name) { s.params = p; return; }
    }
    _session.push_back({name, p});
}

std::optional<Preset> PresetManager::find_session(const std::string& name) const {
    for (auto& s : _session)
        if (s.name == name) return s;
    return std::nullopt;
}

bool PresetManager::export_preset(const std::string& path, const std::string& name,
                                  const EngineParams& p) const
{
    try {
        json j;
        ep_to_json(j, name, p);
        std::ofstream f(path);
        f << j.dump(2);
        return f.good();
    } catch(const std::exception& e) {
    CV_ERR(PRESET_EXPORT_FAILED, path + ": " + e.what());
    return false;
}
}

std::optional<Preset> PresetManager::import_preset(const std::string& path) const {
    try {
        std::ifstream f(path);
        json j = json::parse(f);
        if (j.value("__app__", "") != "CapstanVar") {
            CV_ERR(PRESET_IMPORT_WRONG_APP, path + ": not a CapstanVar preset");
            return std::nullopt;
        }
        Preset pr;
        pr.name   = j.value("__name__", path);
        pr.params = ep_from_json(j);
        return pr;
    } catch(...) { return std::nullopt; }
}

// Phase 5c — round-trip smoke test for the 4 storage presets added
// to _build_builtins(). Iterates each storage preset, serializes via
// ep_to_json, deserializes via ep_from_json, and verifies that:
//   1. The preset exists in _build_builtins() (find_builtin succeeds).
//   2. The preset's env_temperature_c + env_humidity_pct match the
//      canonical values from storage_profiles() in mod_environment.hpp
//      (single source of truth — if storage_profiles() ever changes,
//      this audit fires so the preset is updated too).
//   3. The four WEAR fields (env_age_acceleration, env_age_seconds,
//      env_tape_health, env_failure_modes) stay at struct defaults
//      after the round-trip (storage presets must NOT reset wear).
// Cheap enough to call once at startup; self-contained; follows the
// env-module audit pattern (stderr-only PASS/FAIL).
//
// The expected values below mirror storage_profiles() entries verbatim.
// If the canonical catalog in include/mod_environment.hpp ever changes,
// this list must be updated in lockstep (the audit will FAIL otherwise).
bool PresetManager::verify_storage_preset_round_trip() {
    struct Expected {
        const char* name;
        float       t_c;
        float       rh_pct;
    };
    static const Expected kExpected[] = {
        {"Controlled (climate-controlled vault)", 20.0f, 50.0f},
        {"Consumer Closet (typical bedroom)",      25.0f, 60.0f},
        {"Hot Attic (abandoned for years)",        35.0f, 70.0f},
        {"Cold Warehouse (unheated storage)",      10.0f, 40.0f},
    };
    static constexpr int kCount = sizeof(kExpected) / sizeof(kExpected[0]);

    bool ok = true;
    int  found = 0;
    PresetManager pm;

    for (int i = 0; i < kCount; ++i) {
        const Expected& exp = kExpected[i];
        auto pr = pm.find_builtin(exp.name);
        if (!pr) {
            std::fprintf(stderr,
                         "[preset_manager] FAIL: storage preset \u201c%s\u201d "
                         "not in _build_builtins().\\n",
                         exp.name);
            std::fflush(stderr);
            ok = false;
            continue;
        }
        found++;

        // Cross-check A: preset's stored env_* in _build_builtins match
        // the canonical catalog values exactly (within float tolerance).
        const float got_t  = pr->params.env_temperature_c;
        const float got_rh = pr->params.env_humidity_pct;
        if (std::fabs(got_t  - exp.t_c)   > 1e-5f) ok = false;
        if (std::fabs(got_rh - exp.rh_pct) > 1e-5f) ok = false;

        // Cross-check B: wear fields stay at struct defaults (storage
        // presets are designed NOT to reset existing wear).
        if (pr->params.env_age_acceleration != 1.0f) ok = false;
        if (pr->params.env_age_seconds      != 0.0)  ok = false;
        if (pr->params.env_tape_health      != 1.0f) ok = false;
        if (pr->params.env_failure_modes    != 0u)   ok = false;

        // Round-trip via ep_to_json / ep_from_json without touching disk.
        json j;
        ep_to_json(j, exp.name, pr->params);
        EngineParams back = ep_from_json(j);

        // Cross-check C: round-trip preserves the env_* values exactly.
        if (std::fabs(back.env_temperature_c - exp.t_c)   > 1e-5f) ok = false;
        if (std::fabs(back.env_humidity_pct  - exp.rh_pct) > 1e-5f) ok = false;

        // Cross-check D: round-trip preserves the wear defaults.
        if (back.env_age_acceleration != 1.0f) ok = false;
        if (back.env_age_seconds      != 0.0)  ok = false;
        if (back.env_tape_health      != 1.0f) ok = false;
        if (back.env_failure_modes    != 0u)   ok = false;
    }

    if (ok) {
        std::fprintf(stderr,
                     "[preset_manager] PASS: storage_preset round-trip holds "
                     "(%d/%d presets; T_c, RH, and wear defaults preserved).\\n",
                     found, kCount);
        std::fflush(stderr);
        return true;
    }

    // FAIL diagnostic: print divergent fields per preset so the
    // implementer can spot which catalog entry drifted.
    std::fprintf(stderr,
                 "[preset_manager] FAIL: storage_preset round-trip regression "
                 "(found %d of %d expected).\\n",
                 found, kCount);
    for (int i = 0; i < kCount; ++i) {
        const Expected& exp = kExpected[i];
        auto pr = pm.find_builtin(exp.name);
        if (!pr) continue;
        std::fprintf(stderr,
                     "  [%s]  env_temperature_c    want=%.4f   got=%.4f\\n"
                     "         env_humidity_pct     want=%.4f   got=%.4f\\n"
                     "         env_age_acceleration  want=1.0    got=%.4f\\n"
                     "         env_age_seconds       want=0.0    got=%.6f\\n"
                     "         env_tape_health       want=1.0    got=%.4f\\n"
                     "         env_failure_modes     want=0u     got=%u\\n",
                     exp.name,
                     exp.t_c,    pr->params.env_temperature_c,
                     exp.rh_pct, pr->params.env_humidity_pct,
                     1.0f,       pr->params.env_age_acceleration,
                     0.0,        pr->params.env_age_seconds,       0.0,
                     1.0f,       pr->params.env_tape_health,       1.0f,
                     0u,         pr->params.env_failure_modes,     0u);
    }
    std::fflush(stderr);
    return false;
}

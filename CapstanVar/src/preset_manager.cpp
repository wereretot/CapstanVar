#include "error_log.hpp"
#include <cstdio>
#include "preset_manager.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

using json = nlohmann::json;

// ── Helpers ───────────────────────────────────────────────────────────────────
static void ep_to_json(json& j, const std::string& name, const EngineParams& e) {
    j["__version__"] = 1;
    j["__app__"]     = "CapstanVar";
    j["__name__"]    = name;
    j["oxide_type"]  = e.oxide_type;
    #define F(x) j[#x] = e.x
    F(ips_base); F(motor_health); F(motor_drag); F(motor_boost);
    F(wow_dep);  F(flutter_dep);  F(scrape_flutter); F(tension_load); F(dropout_rate);
    F(drive);    F(bias);         F(replay_diff);     F(asperities);   F(barkhausen);
    F(crosstalk);F(print_through);F(demagnetization); F(oxide_shedding);
    F(hiss);     F(hiss_color);   F(mains_hum);       F(cutoff_base);
    F(head_bump);F(azimuth_drift);F(sticky_shed);
    #undef F
}

static EngineParams ep_from_json(const json& j) {
    EngineParams e;
    auto get = [&](const char* k, float& v){ if(j.contains(k)) v = j[k].get<float>(); };
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
    if (j.contains("oxide_type")) e.oxide_type = j["oxide_type"].get<std::string>();
    return e;
}

// ── Macro to build a preset entry compactly ───────────────────────────────────
#define BEGIN_PRESET(NAME, IPS) { \
    EngineParams e = PresetManager::default_params(); \
    const char* _pname = NAME; \
    e.ips_base = IPS;
#define P(k,v) e.k = v;
#define END_PRESET _builtins.push_back({_pname, e}); }

EngineParams PresetManager::default_params() {
    return EngineParams{};  // all defaults defined in struct
}

void PresetManager::_build_builtins() {
    // ── Studio / Pro ──────────────────────────────────────────────────────────
    BEGIN_PRESET("Ampex 456 (30ips)", 30.0f)
        P(motor_health,0.02f) P(wow_dep,0.008f) P(flutter_dep,0.004f)
        P(scrape_flutter,0.025f) P(drive,1.05f) P(bias,1.0f)
        P(hiss,0.000020f) P(hiss_color,0.1f) P(cutoff_base,22000.f)
        P(head_bump,0.28f) P(print_through,0.005f) P(replay_diff,0.32f)
        P(mains_hum,0.000008f) P(barkhausen,0.004f) P(asperities,0.006f)
    END_PRESET
    BEGIN_PRESET("Ampex 456 (15ips)", 15.0f)
        P(motor_health,0.04f) P(wow_dep,0.018f) P(flutter_dep,0.006f)
        P(scrape_flutter,0.032f) P(drive,1.08f) P(bias,1.0f)
        P(hiss,0.000039f) P(hiss_color,0.12f) P(cutoff_base,20000.f)
        P(head_bump,0.42f) P(print_through,0.006f) P(replay_diff,0.30f)
        P(mains_hum,0.000012f) P(barkhausen,0.006f) P(asperities,0.009f)
    END_PRESET
    BEGIN_PRESET("Studer A820 (30ips)", 30.0f)
        P(motor_health,0.01f) P(wow_dep,0.005f) P(flutter_dep,0.003f)
        P(scrape_flutter,0.018f) P(drive,1.02f) P(bias,1.0f)
        P(hiss,0.000013f) P(hiss_color,0.07f) P(cutoff_base,22000.f)
        P(head_bump,0.20f) P(print_through,0.004f) P(replay_diff,0.35f)
        P(mains_hum,0.000005f) P(barkhausen,0.002f) P(asperities,0.004f)
    END_PRESET
    BEGIN_PRESET("Studer A820 (15ips)", 15.0f)
        P(motor_health,0.025f) P(wow_dep,0.010f) P(flutter_dep,0.004f)
        P(scrape_flutter,0.025f) P(drive,1.04f) P(bias,1.0f)
        P(hiss,0.000025f) P(hiss_color,0.09f) P(cutoff_base,21000.f)
        P(head_bump,0.35f) P(print_through,0.005f) P(replay_diff,0.32f)
        P(mains_hum,0.000008f) P(barkhausen,0.004f) P(asperities,0.006f)
    END_PRESET
    BEGIN_PRESET("Otari MTR-90 (30ips)", 30.0f)
        P(motor_health,0.012f) P(wow_dep,0.006f) P(flutter_dep,0.003f)
        P(scrape_flutter,0.022f) P(drive,1.03f) P(bias,1.0f)
        P(hiss,0.000016f) P(hiss_color,0.08f) P(cutoff_base,22000.f)
        P(head_bump,0.24f) P(print_through,0.005f) P(replay_diff,0.33f)
        P(mains_hum,0.000006f) P(barkhausen,0.003f) P(asperities,0.005f)
    END_PRESET
    BEGIN_PRESET("Otari MTR-90 (15ips)", 15.0f)
        P(motor_health,0.035f) P(wow_dep,0.013f) P(flutter_dep,0.005f)
        P(scrape_flutter,0.028f) P(drive,1.06f) P(bias,1.0f)
        P(hiss,0.000031f) P(hiss_color,0.10f) P(cutoff_base,20500.f)
        P(head_bump,0.38f) P(print_through,0.005f) P(replay_diff,0.31f)
        P(mains_hum,0.000010f) P(barkhausen,0.005f) P(asperities,0.007f)
    END_PRESET
    BEGIN_PRESET("MCI JH-24 (30ips)", 30.0f)
        P(motor_health,0.020f) P(wow_dep,0.009f) P(flutter_dep,0.005f)
        P(scrape_flutter,0.030f) P(drive,1.07f) P(bias,1.0f)
        P(hiss,0.000020f) P(hiss_color,0.18f) P(cutoff_base,21000.f)
        P(head_bump,0.35f) P(print_through,0.006f) P(replay_diff,0.28f)
        P(mains_hum,0.000015f) P(barkhausen,0.005f) P(asperities,0.008f)
    END_PRESET
    BEGIN_PRESET("Scotch 226 (7.5ips)", 7.5f)
        P(motor_health,0.15f) P(wow_dep,0.06f) P(flutter_dep,0.014f)
        P(scrape_flutter,0.050f) P(drive,1.22f) P(bias,0.95f)
        P(hiss,0.000079f) P(hiss_color,0.22f) P(cutoff_base,16000.f)
        P(head_bump,0.58f) P(print_through,0.007f) P(replay_diff,0.28f)
        P(mains_hum,0.000020f) P(barkhausen,0.010f) P(asperities,0.014f)
    END_PRESET
    BEGIN_PRESET("Revox B77 (7.5ips)", 7.5f)
        P(motor_health,0.12f) P(wow_dep,0.05f) P(flutter_dep,0.011f)
        P(scrape_flutter,0.045f) P(drive,1.18f) P(bias,0.97f)
        P(hiss,0.000063f) P(hiss_color,0.18f) P(cutoff_base,17000.f)
        P(head_bump,0.52f) P(print_through,0.006f) P(replay_diff,0.29f)
        P(mains_hum,0.000018f) P(barkhausen,0.009f) P(asperities,0.012f)
    END_PRESET
    BEGIN_PRESET("Revox B77 (3.75ips)", 3.75f)
        P(motor_health,0.30f) P(wow_dep,0.18f) P(flutter_dep,0.028f)
        P(scrape_flutter,0.070f) P(drive,1.38f) P(bias,0.90f)
        P(hiss,0.000158f) P(hiss_color,0.30f) P(cutoff_base,12000.f)
        P(head_bump,0.80f) P(print_through,0.008f) P(replay_diff,0.24f)
        P(mains_hum,0.000025f) P(barkhausen,0.014f) P(asperities,0.018f)
    END_PRESET
    // ── Consumer reel ─────────────────────────────────────────────────────────
    BEGIN_PRESET("BASF LH Super (7.5ips)", 7.5f)
        P(motor_health,0.28f) P(wow_dep,0.10f) P(flutter_dep,0.020f)
        P(scrape_flutter,0.055f) P(drive,1.28f) P(bias,0.90f)
        P(hiss,0.000100f) P(hiss_color,0.26f) P(cutoff_base,14000.f)
        P(head_bump,0.70f) P(print_through,0.008f) P(replay_diff,0.25f)
        P(mains_hum,0.000025f) P(barkhausen,0.014f) P(asperities,0.018f)
        P(crosstalk,0.03f) P(tension_load,0.008f)
    END_PRESET
    BEGIN_PRESET("Maxell UD (7.5ips)", 7.5f)
        P(motor_health,0.24f) P(wow_dep,0.09f) P(flutter_dep,0.017f)
        P(scrape_flutter,0.048f) P(drive,1.24f) P(bias,0.92f)
        P(hiss,0.000089f) P(hiss_color,0.24f) P(cutoff_base,15000.f)
        P(head_bump,0.62f) P(print_through,0.007f) P(replay_diff,0.26f)
        P(mains_hum,0.000020f) P(barkhausen,0.012f) P(asperities,0.016f)
        P(crosstalk,0.025f) P(tension_load,0.006f)
    END_PRESET
    BEGIN_PRESET("Tascam 38 (7.5ips)", 7.5f)
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
        P(motor_health,0.50f) P(wow_dep,0.70f) P(flutter_dep,0.070f)
        P(scrape_flutter,0.065f) P(drive,1.45f) P(bias,0.85f)
        P(hiss,0.000199f) P(hiss_color,0.45f) P(cutoff_base,12500.f)
        P(head_bump,0.80f) P(replay_diff,0.20f)
        P(mains_hum,0.000030f) P(barkhausen,0.025f) P(asperities,0.032f)
        P(crosstalk,0.14f) P(tension_load,0.015f)
    END_PRESET
    BEGIN_PRESET("Type II Chrome (CrO2)", 1.875f)
        P(motor_health,0.40f) P(wow_dep,0.55f) P(flutter_dep,0.058f)
        P(scrape_flutter,0.055f) P(drive,1.35f) P(bias,1.35f)
        P(hiss,0.000125f) P(hiss_color,0.30f) P(cutoff_base,15000.f)
        P(head_bump,0.60f) P(replay_diff,0.25f)
        P(mains_hum,0.000022f) P(barkhausen,0.016f) P(asperities,0.022f)
        P(crosstalk,0.08f) P(tension_load,0.010f)
        P(oxide_type,"CrO2")
    END_PRESET
    BEGIN_PRESET("Type IV Metal", 1.875f)
        P(motor_health,0.28f) P(wow_dep,0.40f) P(flutter_dep,0.045f)
        P(scrape_flutter,0.042f) P(drive,1.25f) P(bias,1.70f)
        P(hiss,0.000079f) P(hiss_color,0.18f) P(cutoff_base,18000.f)
        P(head_bump,0.45f) P(replay_diff,0.30f)
        P(mains_hum,0.000015f) P(barkhausen,0.010f) P(asperities,0.014f)
        P(crosstalk,0.05f) P(tension_load,0.007f)
        P(oxide_type,"Metal")
    END_PRESET
    BEGIN_PRESET("Dolby B (Type I)", 1.875f)
        P(motor_health,0.45f) P(wow_dep,0.60f) P(flutter_dep,0.065f)
        P(scrape_flutter,0.060f) P(drive,1.40f) P(bias,0.88f)
        P(hiss,0.000063f) P(hiss_color,0.18f) P(cutoff_base,14000.f)
        P(head_bump,0.72f) P(replay_diff,0.22f)
        P(mains_hum,0.000025f) P(barkhausen,0.020f) P(asperities,0.026f)
        P(crosstalk,0.12f)
    END_PRESET
    BEGIN_PRESET("Dolby C (Type II)", 1.875f)
        P(motor_health,0.35f) P(wow_dep,0.48f) P(flutter_dep,0.052f)
        P(scrape_flutter,0.050f) P(drive,1.30f) P(bias,1.35f)
        P(hiss,0.000031f) P(hiss_color,0.14f) P(cutoff_base,15500.f)
        P(head_bump,0.55f) P(replay_diff,0.24f)
        P(mains_hum,0.000018f) P(barkhausen,0.014f) P(asperities,0.018f)
        P(crosstalk,0.06f) P(tension_load,0.008f)
        P(oxide_type,"CrO2")
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

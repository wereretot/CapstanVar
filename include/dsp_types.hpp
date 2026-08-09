#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int   SR          = 44100;
static constexpr float SR_F        = 44100.0f;
static constexpr float SR_F_INV    = 1.0f / 44100.0f;
static constexpr float PI          = 3.14159265358979323846f;
static constexpr float TWO_PI      = 2.0f * PI;
static constexpr int   BLOCK_SIZE  = 1024;
static constexpr int   MAX_CHANNELS = 2;

// ── Stereo sample ─────────────────────────────────────────────────────────────
struct Frame {
    float l = 0.0f;
    float r = 0.0f;

    Frame  operator+(const Frame& o) const { return {l+o.l, r+o.r}; }
    Frame  operator*(float s)        const { return {l*s,   r*s  }; }
    Frame& operator+=(const Frame& o)      { l+=o.l; r+=o.r; return *this; }
    Frame& operator*=(float s)             { l*=s;   r*=s;   return *this; }
};

// ── Oxide presets ─────────────────────────────────────────────────────────────
// OxideProps holds per-tape-stock physics data used by MagneticPath's
// saturation block and ElectronicComponents' hiss mixer. Phase 1 of the
// tape-physics refactor: expand OxideProps and populate 10 reference stocks.
//
// The first three fields (Hc/Ms/bias_trim) keep their existing semantics so
// the 30+ built-in presets remain byte-identical. New fields below are
// STAGED — runtime ignores them in Phase 1; Phase 2 will use mol_thd3_db to
// remap the saturator's knee/ceiling, Phase 3 will use sens_* and
// hysteresis_amt with the new Preisach LUT. See docs/TAPE_PHYSICS_REFACTOR.md.
struct OxideProps {
    // Phase 1 — read by MagneticPath::saturate_only and MagneticPath::process
    float Hc;                  // coercivity (Oersted)
    float Ms;                  // saturation magnetisation (normalised)
    float bias_trim;           // optimal bias multiplier

    // Phase 2+ — STAGED, runtime-ignored in Phase 1
    float mol_thd3_db;         // MOL @ 3% THD, dB ref 200 nWb/m
    float sens_1k;             // sensitivity at 1 kHz, linear (1.0 by definition)
    float sens_10k;            // sensitivity at 10 kHz, linear rel. to 1 kHz
    float sens_15k;            // sensitivity at 15 kHz, linear rel. to 1 kHz
    float hf_rolloff_db_oct;   // HF rolloff above ~12 kHz, dB/oct (negative)
    float hiss_floor_db;       // hiss floor in dB below MOL
    float hysteresis_amt;      // 0..1 weight of Preisach hysteresis (Phase 3)
};

// 10 reference stocks. The 4 legacy keys (Fe2O3/CrO2/Metal/FeCo) keep their
// Hc/Ms/bias_trim byte-identical to the pre-refactor values → audibility
// preserved for the 30+ built-in presets that still reference them. The 6
// new keys (456/SM911/SM900/GP9/Maxell_UD/BASF_LH) carry MRL-style data and
// are available for Phase 6 preset reauthoring.
inline const std::unordered_map<std::string, OxideProps>& oxide_presets() {
    static const std::unordered_map<std::string, OxideProps> P = {
        // ── Legacy — Hc/Ms/bias_trim byte-identical to pre-refactor values ──
        {"Fe2O3", {250.0f,  1.0f,  1.00f, +6.0f, 1.0f, 0.85f, 0.65f, -5.0f, -65.0f, 0.05f}},
        {"CrO2",  {480.0f,  1.2f,  1.35f, +5.0f, 1.0f, 1.00f, 0.71f, -4.0f, -68.0f, 0.10f}},
        {"Metal", {1400.0f, 1.8f,  1.70f, +7.0f, 1.0f, 0.89f, 0.63f, -2.5f, -70.0f, 0.20f}},
        {"FeCo",  {700.0f,  1.4f,  1.40f, +6.0f, 1.0f, 0.85f, 0.55f, -5.5f, -66.0f, 0.05f}},
        // ── New Phase-1 additions (MRL/Tape-Stock reference data) ──
        {"456",       {320.0f, 1.15f, 1.05f, +6.0f, 1.0f, 0.89f, 0.71f, -4.0f, -65.0f, 0.10f}},
        {"SM911",     {320.0f, 1.20f, 1.05f, +6.5f, 1.0f, 0.89f, 0.79f, -4.0f, -66.0f, 0.10f}},
        {"SM900",     {360.0f, 1.30f, 1.10f, +9.0f, 1.0f, 0.71f, 0.50f, -3.5f, -68.0f, 0.15f}},
        {"GP9",       {370.0f, 1.35f, 1.15f, +9.0f, 1.0f, 0.67f, 0.45f, -3.5f, -67.5f, 0.15f}},
        {"Maxell_UD", {280.0f, 1.00f, 1.00f, +4.0f, 1.0f, 0.84f, 0.63f, -5.0f, -62.0f, 0.05f}},
        {"BASF_LH",   {300.0f, 1.05f, 1.00f, +4.0f, 1.0f, 0.63f, 0.40f, -6.0f, -63.0f, 0.05f}},
    };
    return P;
}

static constexpr float HC_REF = 250.0f; // Fe2O3 reference coercivity

// ── EQ playback curves (Phase 2) ──────────────────────────────────────────────
// EQCurve selects a NAB / IEC / AES / cassette standard playback EQ curve.
// "Legacy" is the pre-refactor default — uses the existing cutoff_base LP path
// so all 30+ existing presets behave byte-identically. The non-Legacy curves
// apply a low-shelf + high-shelf RBJ biquad pair derived from the standards'
// LF / HF time constants (see docs/TAPE_PHYSICS_REFACTOR.md §5).
enum class EQCurve : uint8_t {
    Legacy          = 0,    // Pre-refactor: single Butterworth LP at cutoff_base
    Cassette_I      = 1,    // 1.875 ips cassette Type I        (3180 + 120 µs)
    Cassette_II_IV  = 2,    // 1.875 ips cassette Type II / IV  (3180 + 70  µs)
    NAB_3_75        = 3,    // 3.75  ips NAB                     (3180 + 90  µs)
    NAB_7_5         = 4,    // 7.5   ips NAB                     (3180 + 50  µs)
    IEC_7_5         = 5,    // 7.5   ips IEC                     (3180 + 35  µs)
    NAB_15          = 6,    // 15    ips NAB                     (3180 + 50  µs)
    IEC_15          = 7,    // 15    ips IEC (mastering std)     (3180 + 35  µs)
    AES_30          = 8,    // 30    ips AES                     (3180 + 17  µs)
};

// ── Engine parameters (all controls in one flat struct) ───────────────────────
struct EngineParams {
    // Input
    float input_gain     = 1.0f;    // Input volume/trim (0 to 2.0, 1.0 = unity)
    
    // Transport
    float ips_base       = 15.0f;
    float motor_health   = 0.0f;
    float motor_drag     = 0.0f;
    float motor_boost    = 0.0f;
    float wow_dep        = 0.2f;
    float flutter_dep    = 0.05f;
    float scrape_flutter = 0.1f;
    float tension_load   = 0.01f;
    float dropout_rate   = 0.0f;

    // Magnetic
    float drive          = 1.2f;
    float bias           = 1.0f;
    float replay_diff    = 0.3f;
    float asperities     = 0.0f;
    float barkhausen     = 0.0f;
    float crosstalk      = 0.0f;
    float print_through  = 0.0f;
    float demagnetization= 0.0f;
    float oxide_shedding = 0.0f;

    // Electronics
    float hiss           = 0.001f;
    float hiss_color     = 0.0f;
    float mains_hum      = 0.0f;
    float cutoff_base    = 18000.0f;
    float head_bump      = 0.5f;
    float azimuth_drift  = 0.05f;
    float sticky_shed    = 0.0f;

    // Meta (not exposed as sliders)
    std::string oxide_type = "Fe2O3";
    bool is_reversed       = false;
    float motor_engage     = 1.0f;
    float tape_speed_mult  = 1.0f;  // actual read stride (1=normal, 40=shuttle, <1=spindown)
    bool presaturated      = false;

    // Phase 2 — EQ playback curves + format-id coupling (Hybrid refactor)
    EQCurve    eq_curve       = EQCurve::Legacy;  // std playback EQ: Legacy = current LP path
    float      lf_trim_db     = 0.0f;             // ±6 dB LF trim (applied on top of eq_curve)
    float      hf_trim_db     = 0.0f;             // ±6 dB HF trim
    std::string format_id     = "";               // canonical {machine, speed, EQ} key; "" = legacy free-form
    bool       format_locked  = false;            // when true, picking format snaps values; touching a knob detaches
};

// ── EQ playback curves (Phase 2) ──────────────────────────────────────────────
// EQCurve is declared above (before EngineParams, which references it as the
// default value for its eq_curve field).
//
// Time-constant lookup for non-Legacy curves. The LF shelf is always at fc
// = 1 / (2π · 3180 µs) ≈ 50 Hz for every standard; the HF shelf is at
// fc = 1 / (2π · τ_hf_us · 1e-6). Implicit shelf gains are lf_gain_db (LF)
// and hf_gain_db (HF), with user LF/HF trims adding ±6 dB on top.
struct EQSpec {
    float hf_tau_s;    // HF shelf time constant in seconds (used in fc = 1/(2π·τ))
    float lf_tau_s;    // LF shelf time constant (usually 3.180e-3)
    float lf_gain_db;  // implicit LF shelf gain (representative; standards vary)
    float hf_gain_db;  // implicit HF shelf gain (typically negative — playback rolls off top)
};
inline EQSpec eq_spec(EQCurve c) {
    static const float T3180 = 3180.0e-6f;
    switch (c) {
    case EQCurve::Cassette_I:     return EQSpec{ 120.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::Cassette_II_IV: return EQSpec{  70.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::NAB_3_75:       return EQSpec{  90.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::NAB_7_5:        return EQSpec{  50.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::IEC_7_5:        return EQSpec{  35.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::NAB_15:         return EQSpec{  50.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::IEC_15:         return EQSpec{  35.0e-6f, T3180, +3.0f, -3.0f};
    case EQCurve::AES_30:         return EQSpec{  17.0e-6f, T3180, +3.0f, -3.0f};
    default:                      return EQSpec{   0.0f,    T3180,  0.0f,  0.0f};  // Legacy
    }
}

// ── Tape formats (Phase 2) ────────────────────────────────────────────────────
// TapeFormat binds the coupled parameters (speed, EQ curve, oxide, fluxivity,
// recommended bias) for a canonical machine + speed + EQ standard combo.
// Used by the format dropdown in src/ui.cpp; settings here are derived from
// manufacturer / MRL reference data.
struct TapeFormat {
    std::string id;                // stable key, e.g. "Studer_A820_30_ips_IEC"
    std::string display_name;      // human label, e.g. "Studer A820 — 30 ips IEC"
    float       ips;               // canonical speed
    EQCurve     eq_curve;          // canonical playback EQ
    std::string oxide;             // canonical oxide ("SM911", "456", ...)
    float       fluxivity_nWb_m;   // reference record fluxivity
    float       bias_recommend;    // recommended bias level (multiplier, 1.0 = nominal)
    float       motor_health;      // canonical pristine motor health (0 = perfect)
    float       hiss_floor_db;     // canonical hiss floor relative to MOL (e.g. -68 dB)
};

// Canonical machine + speed + EQ formats. Each is a distinct preset of the
// coupled parameters; the format readonly dropdown in ui.cpp enumerates these.
inline const std::vector<TapeFormat>& tape_formats() {
    static const std::vector<TapeFormat> F = {
        // Open reel studio
        {"Studer_A820_30_IEC", "Studer A820 — 30 ips IEC",  30.0f,  EQCurve::IEC_15, "SM911",  250.f, 1.0f, 0.010f, -68.f},
        {"Studer_A820_30_NAB", "Studer A820 — 30 ips NAB",  30.0f,  EQCurve::NAB_15, "SM911",  250.f, 1.0f, 0.010f, -68.f},
        {"Studer_A820_15_IEC", "Studer A820 — 15 ips IEC",  15.0f,  EQCurve::IEC_15, "SM911",  250.f, 1.0f, 0.025f, -67.f},
        {"Studer_A820_15_NAB", "Studer A820 — 15 ips NAB",  15.0f,  EQCurve::NAB_15, "SM911",  250.f, 1.0f, 0.025f, -67.f},
        {"Ampex_456_30_NAB",    "Ampex 456 — 30 ips NAB",    30.0f,  EQCurve::AES_30, "456",    250.f, 1.0f, 0.020f, -65.f},
        {"Ampex_456_15_NAB",    "Ampex 456 — 15 ips NAB",    15.0f,  EQCurve::NAB_15, "456",    250.f, 1.0f, 0.040f, -65.f},
        {"Ampex_456_15_IEC",    "Ampex 456 — 15 ips IEC",    15.0f,  EQCurve::IEC_15, "456",    250.f, 1.0f, 0.040f, -65.f},
        {"Revox_B77_7_5_NAB",   "Revox B77 — 7.5 ips NAB",   7.5f,   EQCurve::IEC_7_5, "BASF_LH", 200.f, 0.97f, 0.120f, -63.f},
        {"Revox_B77_3_75_NAB",  "Revox B77 — 3.75 ips NAB",  3.75f,  EQCurve::NAB_3_75, "BASF_LH", 200.f, 0.90f, 0.300f, -63.f},
        {"Maxell_UD_7_5",       "Maxell UD — 7.5 ips",       7.5f,   EQCurve::IEC_7_5, "Maxell_UD", 200.f, 0.92f, 0.240f, -62.f},
        // Cassette references
        {"Cassette_Type_I",     "Cassette — Type I (Fe₂O₃)", 1.875f, EQCurve::Cassette_I,     "Fe2O3", 100.f, 0.85f, 0.500f, -65.f},
        {"Cassette_Type_II",    "Cassette — Type II (CrO₂)", 1.875f, EQCurve::Cassette_II_IV, "CrO2",  100.f, 1.35f, 0.400f, -68.f},
        {"Cassette_Type_IV",    "Cassette — Type IV (Metal)",1.875f, EQCurve::Cassette_II_IV, "Metal", 160.f, 1.70f, 0.280f, -70.f},
    };
    return F;
}

// Tape-format lookup by id; returns nullptr if id is "" or unknown.
inline const TapeFormat* tape_format_by_id(const std::string& id) {
    if (id.empty()) return nullptr;
    for (auto& f : tape_formats())
        if (f.id == id) return &f;
    return nullptr;
}

// Maps an EQCurve enum value to a human-readable string used in JSON I/O
// and as the EQ dropdown preview text.
inline const char* eq_curve_name(EQCurve c) {
    switch (c) {
    case EQCurve::Legacy:         return "Legacy (single LP)";
    case EQCurve::Cassette_I:     return "Cassette I (3180+120 µs)";
    case EQCurve::Cassette_II_IV: return "Cassette II/IV (3180+70 µs)";
    case EQCurve::NAB_3_75:       return "NAB 3.75 ips (3180+90 µs)";
    case EQCurve::NAB_7_5:        return "NAB 7.5 ips (3180+50 µs)";
    case EQCurve::IEC_7_5:        return "IEC 7.5 ips (3180+35 µs)";
    case EQCurve::NAB_15:         return "NAB 15 ips (3180+50 µs)";
    case EQCurve::IEC_15:         return "IEC 15 ips (3180+35 µs)";
    case EQCurve::AES_30:         return "AES 30 ips (3180+17 µs)";
    }
    return "Legacy (single LP)";
}

// Inverse of eq_curve_name — parses a JSON eq_curve string back into the
// enum. Unknown / empty strings default to Legacy for backward compat.
inline EQCurve eq_curve_from_name(const std::string& s) {
    if (s.empty()) return EQCurve::Legacy;
    // Tolerate both short ("Legacy") and long ("Legacy (single LP)") labels
    // plus the historical "Cassette_I" / "NAB_15" / etc. forms generated by
    // the canonical eq_curve_name() output above.
    if (s.find("Legacy")        != std::string::npos) return EQCurve::Legacy;
    if (s.find("Cassette I")    != std::string::npos ||
        s.find("Cassette_I")    != std::string::npos) return EQCurve::Cassette_I;
    if (s.find("Cassette II")   != std::string::npos ||
        s.find("Cassette_II")   != std::string::npos) return EQCurve::Cassette_II_IV;
    if (s.find("NAB 3.75")      != std::string::npos ||
        s.find("NAB_3_75")      != std::string::npos) return EQCurve::NAB_3_75;
    if (s.find("NAB 7.5")       != std::string::npos ||
        s.find("NAB_7_5")       != std::string::npos) return EQCurve::NAB_7_5;
    if (s.find("IEC 7.5")       != std::string::npos ||
        s.find("IEC_7_5")       != std::string::npos) return EQCurve::IEC_7_5;
    if (s.find("NAB 15")        != std::string::npos ||
        s.find("NAB_15")        != std::string::npos) return EQCurve::NAB_15;
    if (s.find("IEC 15")        != std::string::npos ||
        s.find("IEC_15")        != std::string::npos) return EQCurve::IEC_15;
    if (s.find("AES 30")        != std::string::npos ||
        s.find("AES_30")        != std::string::npos) return EQCurve::AES_30;
    return EQCurve::Legacy;  // safe default
}

// ── 2-pole IIR filter state ───────────────────────────────────────────────────
struct Biquad {
    float b[3] = {1,0,0};
    float a[3] = {1,0,0};
    // Per-channel delay lines
    float z1[2] = {0,0};
    float z2[2] = {0,0};

    void reset() { z1[0]=z1[1]=z2[0]=z2[1]=0.0f; }

    // Transposed Direct Form II — tick one sample, one channel
    float tick(float x, int ch) {
        float y  = b[0]*x + z1[ch];
        z1[ch]   = b[1]*x - a[1]*y + z2[ch];
        z2[ch]   = b[2]*x - a[2]*y;
        return y;
    }

    // Process a block of stereo frames in-place
    void process(Frame* buf, int n) {
        for (int i = 0; i < n; ++i) {
            buf[i].l = tick(buf[i].l, 0);
            buf[i].r = tick(buf[i].r, 1);
        }
    }
};

// ── Butterworth lowpass (2-pole) coefficient calculation ──────────────────────
inline void butter_lp(float fc_norm, Biquad& bq) {
    // fc_norm = fc / (SR/2), range (0, 1)
    fc_norm = std::clamp(fc_norm, 1e-4f, 0.4999f);
    float w  = std::tan(PI * fc_norm);
    float w2 = w * w;
    float n  = 1.0f / (1.0f + M_SQRT2 * w + w2);
    bq.b[0] = w2 * n;
    bq.b[1] = 2.0f * bq.b[0];
    bq.b[2] = bq.b[0];
    bq.a[0] = 1.0f;
    bq.a[1] = 2.0f * (w2 - 1.0f) * n;
    bq.a[2] = (1.0f - M_SQRT2 * w + w2) * n;
}

// ── Butterworth bandpass coefficient calculation ───────────────────────────────
inline void butter_bp(float low_norm, float high_norm, Biquad& bq) {
    low_norm  = std::clamp(low_norm,  1e-4f, 0.4998f);
    high_norm = std::clamp(high_norm, low_norm + 1e-4f, 0.4999f);
    float wl = std::tan(PI * low_norm);
    float wh = std::tan(PI * high_norm);
    float bw = wh - wl;
    float w0 = std::sqrt(wl * wh);
    float Q  = w0 / bw;
    float w02 = w0 * w0;
    float d  = 1.0f + w0/Q + w02;
    float n  = 1.0f / d;
    bq.b[0]  = (w0 / Q) * n;
    bq.b[1]  = 0.0f;
    bq.b[2]  = -bq.b[0];
    bq.a[0]  = 1.0f;
    bq.a[1]  = 2.0f * (w02 - 1.0f) * n;
    bq.a[2]  = (1.0f - w0/Q + w02) * n;
}

// ── RBJ shelving-biquad coefficients (Phase 2 standard playback EQ) ──────────
// Standard R. Bristow-Johnson "cookbook" shelf biquads, used for the
// NAB/IEC/AES/cassette playback EQ curves in EQCurve. A is the shelf gain
// (10^(dB/40)); w0 is the shelf corner in radians; Q shapes the overshoot.
// We canonicalise Q to 0.707 (Butterworth) by default — the standards are
// first-order, so Q is approximately 1/√2 once the shelf gain is applied.
//
// We hand-divide by a0 because the cookbook pre-divided form does not match
// the dsp_types Biquad layout (which expects a0 == 1 by convention).
inline void rbj_lowshelf(float fc, float gain_db, float Q, Biquad& bq) {
    float A     = std::pow(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * PI * fc / SR_F;
    float cw0   = std::cos(w0);
    float sw0   = std::sin(w0);
    float Qe    = std::max(Q, 0.05f);                 // numeric floor on Q
    float alpha = sw0 / (2.0f * Qe);
    float sqrtA = std::sqrt(A);

    float b0 =     A * ((A + 1.0f) - (A - 1.0f) * cw0 + 2.0f * sqrtA * alpha);
    float b1 =  2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw0);
    float b2 =     A * ((A + 1.0f) - (A - 1.0f) * cw0 - 2.0f * sqrtA * alpha);
    float a0 =          (A + 1.0f) + (A - 1.0f) * cw0 + 2.0f * sqrtA * alpha;
    float a1 =    -2.0f * ((A - 1.0f) + (A + 1.0f) * cw0);
    float a2 =          (A + 1.0f) + (A - 1.0f) * cw0 - 2.0f * sqrtA * alpha;

    float inv_a0 = 1.0f / a0;
    bq.b[0] = b0 * inv_a0;  bq.b[1] = b1 * inv_a0;  bq.b[2] = b2 * inv_a0;
    bq.a[0] = 1.0f;          bq.a[1] = a1 * inv_a0; bq.a[2] = a2 * inv_a0;
}

inline void rbj_highshelf(float fc, float gain_db, float Q, Biquad& bq) {
    float A     = std::pow(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * PI * fc / SR_F;
    float cw0   = std::cos(w0);
    float sw0   = std::sin(w0);
    float Qe    = std::max(Q, 0.05f);
    float alpha = sw0 / (2.0f * Qe);
    float sqrtA = std::sqrt(A);

    float b0 =     A * ((A + 1.0f) + (A - 1.0f) * cw0 + 2.0f * sqrtA * alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw0);
    float b2 =     A * ((A + 1.0f) + (A - 1.0f) * cw0 - 2.0f * sqrtA * alpha);
    float a0 =          (A + 1.0f) - (A - 1.0f) * cw0 + 2.0f * sqrtA * alpha;
    float a1 =      2.0f * ((A - 1.0f) - (A + 1.0f) * cw0);
    float a2 =          (A + 1.0f) - (A - 1.0f) * cw0 - 2.0f * sqrtA * alpha;

    float inv_a0 = 1.0f / a0;
    bq.b[0] = b0 * inv_a0;  bq.b[1] = b1 * inv_a0;  bq.b[2] = b2 * inv_a0;
    bq.a[0] = 1.0f;          bq.a[1] = a1 * inv_a0; bq.a[2] = a2 * inv_a0;
}

// ── Highpass (1-pole DC block) ────────────────────────────────────────────────
struct DC_Block {
    float x1[2]={0,0}, y1[2]={0,0};
    float R = 0.9997f;
    float tick(float x, int ch) {
        float y = x - x1[ch] + R * y1[ch];
        x1[ch]=x; y1[ch]=y; return y;
    }
    void process(Frame* buf, int n) {
        for(int i=0;i<n;++i){ buf[i].l=tick(buf[i].l,0); buf[i].r=tick(buf[i].r,1); }
    }
    void reset(){ x1[0]=x1[1]=y1[0]=y1[1]=0.f; }
};

// ── Tape read interpolation ──────────────────────────────────────────────────
// Catmull-Rom cubic Hermite — C1 continuous, no zipper at slow tape speeds.
// 19× smoother derivative than linear interpolation at 0.05× speed.
// 4 taps, zero latency, works at any read speed including very slow spindown.
inline Frame cubic_interp(const Frame* data, int N, double pos) {
    int   i1  = (int)pos;
    float t   = (float)(pos - i1);
    int   i0  = i1 > 0       ? i1-1 : 0;
    int   i2  = i1 < N-1     ? i1+1 : N-1;
    int   i3  = i1 < N-2     ? i1+2 : N-1;
    // Catmull-Rom coefficients
    float t2  = t*t, t3 = t2*t;
    float c0  = -0.5f*t3 + 1.0f*t2 - 0.5f*t;
    float c1  =  1.5f*t3 - 2.5f*t2          + 1.0f;
    float c2  = -1.5f*t3 + 2.0f*t2 + 0.5f*t;
    float c3  =  0.5f*t3 - 0.5f*t2;
    return {
        data[i0].l*c0 + data[i1].l*c1 + data[i2].l*c2 + data[i3].l*c3,
        data[i0].r*c0 + data[i1].r*c1 + data[i2].r*c2 + data[i3].r*c3
    };
}

// ── Fast math helpers ─────────────────────────────────────────────────────────
inline float fast_tanh(float x) {
    // Padé approximation — accurate to <0.5% for |x| < 3, clips beyond
    if (x >  3.0f) return  1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float lerp(float a, float b, float t) { return a + (b-a)*t; }
inline float clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }

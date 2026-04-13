#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr int   SR          = 44100;
static constexpr float SR_F        = 44100.0f;
static constexpr float SR_F_INV    = 1.0f / 44100.0f;
static constexpr float PI          = 3.14159265358979323846f;
static constexpr float TWO_PI      = 2.0f * PI;
static constexpr int   BLOCK_SIZE  = 1024;
static constexpr int   MAX_CHANNELS = 2;
using std::lerp;

// ── Stereo sample ─────────────────────────────────────────────────────────────
struct Frame {
    float l = 0.0f;
    float r = 0.0f;

    Frame() = default;
    Frame(float _l, float _r) : l(_l), r(_r) {}

    Frame  operator+(const Frame& o) const { return {l+o.l, r+o.r}; }
    Frame  operator*(float s)        const { return {l*s,   r*s  }; }
    Frame& operator+=(const Frame& o)      { l+=o.l; r+=o.r; return *this; }
    Frame& operator*=(float s)             { l*=s;   r*=s;   return *this; }
};

// ── Oxide presets ─────────────────────────────────────────────────────────────
struct OxideProps {
    float Hc;         // coercivity (Oersted)
    float Ms;         // saturation magnetisation (normalised)
    float bias_trim;  // optimal bias multiplier
};

inline const std::unordered_map<std::string, OxideProps>& oxide_presets() {
    static const std::unordered_map<std::string, OxideProps> P = {
        {"Fe2O3", {250.0f,  1.0f,  1.00f}},
        {"CrO2",  {480.0f,  1.2f,  1.35f}},
        {"Metal", {1400.0f, 1.8f,  1.70f}},
        {"FeCo",  {700.0f,  1.4f,  1.40f}},
    };
    return P;
}

static constexpr float HC_REF = 250.0f; // Fe2O3 reference coercivity

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
    float tracking_error = 0.0f; // Linked to VCR head alignment

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
};

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

inline float cv_lerp(float a, float b, float t) { return a + (b-a)*t; }
inline float clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }

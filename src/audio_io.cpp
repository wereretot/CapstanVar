#include "error_log.hpp"
#include "audio_io.hpp"
#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════════════
//  ALSA backend
// ═══════════════════════════════════════════════════════════════════════════
#if defined(NAGRA_AUDIO_ALSA)
#include <alsa/asoundlib.h>
struct AlsaBackend { snd_pcm_t* pcm = nullptr; };
bool AudioIO::_open_device() {
    auto* b = new AlsaBackend(); _backend = b;
    if (snd_pcm_open(&b->pcm,"default",SND_PCM_STREAM_PLAYBACK,0)<0)
        if (snd_pcm_open(&b->pcm,"plughw:0,0",SND_PCM_STREAM_PLAYBACK,0)<0)
            { CV_ERR(AUDIO_DEVICE_OPEN_FAILED, "ALSA: snd_pcm_open failed"); delete b; _backend=nullptr; return false; }
    snd_pcm_hw_params_t* hw; snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(b->pcm,hw);
    snd_pcm_hw_params_set_access(b->pcm,hw,SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(b->pcm,hw,SND_PCM_FORMAT_FLOAT_LE);
    snd_pcm_hw_params_set_channels(b->pcm,hw,2);
    unsigned int rate=SR;
    snd_pcm_hw_params_set_rate_near(b->pcm,hw,&rate,nullptr);
    snd_pcm_uframes_t period=BLOCK_SIZE, buf=BLOCK_SIZE*4;
    snd_pcm_hw_params_set_period_size_near(b->pcm,hw,&period,nullptr);
    snd_pcm_hw_params_set_buffer_size_near(b->pcm,hw,&buf);
    if (snd_pcm_hw_params(b->pcm,hw)<0) { CV_ERR(AUDIO_DEVICE_PARAMS_FAILED,"ALSA: hw_params failed"); _close_device(); return false; }
    snd_pcm_prepare(b->pcm);
    std::fprintf(stderr,"[AudioIO] ALSA ready %u Hz\n",rate);
    return true;
}
void AudioIO::_close_device() {
    auto* b=static_cast<AlsaBackend*>(_backend); if(!b) return;
    if(b->pcm){snd_pcm_drain(b->pcm);snd_pcm_close(b->pcm);}
    delete b; _backend=nullptr;
}
bool AudioIO::_write_block(const float* d,int frames) {
    auto* b=static_cast<AlsaBackend*>(_backend); if(!b||!b->pcm) return false;
    snd_pcm_sframes_t w=snd_pcm_writei(b->pcm,d,frames);
    if(w==-EPIPE){snd_pcm_prepare(b->pcm);snd_pcm_writei(b->pcm,d,frames);}
    else if(w<0) snd_pcm_recover(b->pcm,(int)w,0);
    return true;
}

#elif defined(NAGRA_AUDIO_PORTAUDIO)
#include <portaudio.h>
struct PaBackend { PaStream* stream=nullptr; };
static bool s_pa_init=false;
bool AudioIO::_open_device() {
    if(!s_pa_init){if(Pa_Initialize()!=paNoError){CV_ERR(AUDIO_DEVICE_OPEN_FAILED,"PortAudio: Pa_Initialize failed");return false;}s_pa_init=true;}
    PaDeviceIndex dev=Pa_GetDefaultOutputDevice(); if(dev==paNoDevice){CV_ERR(AUDIO_DEVICE_OPEN_FAILED,"PortAudio: no default output device");return false;}
    auto* b=new PaBackend(); _backend=b;
    PaStreamParameters op{}; op.device=dev; op.channelCount=2; op.sampleFormat=paFloat32;
    op.suggestedLatency=Pa_GetDeviceInfo(dev)->defaultLowOutputLatency;
    if(Pa_OpenStream(&b->stream,nullptr,&op,SR,BLOCK_SIZE,paClipOff,nullptr,nullptr)!=paNoError)
        {delete b;_backend=nullptr;return false;}
    Pa_StartStream(b->stream);
    std::fprintf(stderr,"[AudioIO] PortAudio ready\n"); return true;
}
void AudioIO::_close_device() {
    auto* b=static_cast<PaBackend*>(_backend); if(!b) return;
    if(b->stream){Pa_StopStream(b->stream);Pa_CloseStream(b->stream);}
    delete b; _backend=nullptr;
}
bool AudioIO::_write_block(const float* d,int frames) {
    auto* b=static_cast<PaBackend*>(_backend); if(!b||!b->stream) return false;
    PaError e=Pa_WriteStream(b->stream,d,frames);
    return e==paNoError||e==paOutputUnderflowed;
}

#elif defined(NAGRA_AUDIO_WINMM)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmreg.h>    // WAVE_FORMAT_IEEE_FLOAT
#include <mmsystem.h>
static constexpr int WINMM_BUFS=4;
struct WinMMBackend{HWAVEOUT hwo=nullptr;WAVEHDR hdrs[WINMM_BUFS]={};std::vector<float> bufs[WINMM_BUFS];int cur=0;};
bool AudioIO::_open_device(){auto* b=new WinMMBackend();_backend=b;WAVEFORMATEX wfx{};wfx.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;wfx.nChannels=2;wfx.nSamplesPerSec=SR;wfx.wBitsPerSample=32;wfx.nBlockAlign=8;wfx.nAvgBytesPerSec=SR*8;if(waveOutOpen(&b->hwo,WAVE_MAPPER,&wfx,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR){delete b;_backend=nullptr;return false;}for(int i=0;i<WINMM_BUFS;++i){b->bufs[i].assign(BLOCK_SIZE*2,0.f);b->hdrs[i].lpData=(LPSTR)b->bufs[i].data();b->hdrs[i].dwBufferLength=BLOCK_SIZE*2*4;waveOutPrepareHeader(b->hwo,&b->hdrs[i],sizeof(WAVEHDR));waveOutWrite(b->hwo,&b->hdrs[i],sizeof(WAVEHDR));}return true;}
void AudioIO::_close_device(){auto* b=static_cast<WinMMBackend*>(_backend);if(!b)return;if(b->hwo){waveOutReset(b->hwo);for(auto& h:b->hdrs)waveOutUnprepareHeader(b->hwo,&h,sizeof(WAVEHDR));waveOutClose(b->hwo);}delete b;_backend=nullptr;}
bool AudioIO::_write_block(const float* d,int frames){auto* b=static_cast<WinMMBackend*>(_backend);if(!b||!b->hwo)return false;WAVEHDR& h=b->hdrs[b->cur];while(!(h.dwFlags&WHDR_DONE))Sleep(1);h.dwFlags&=~WHDR_DONE;memcpy(h.lpData,d,frames*8);h.dwBufferLength=frames*8;waveOutWrite(b->hwo,&h,sizeof(WAVEHDR));b->cur=(b->cur+1)%WINMM_BUFS;return true;}

#elif defined(NAGRA_AUDIO_COREAUDIO)
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
struct CABackend {
    AudioUnit au = nullptr;
    std::vector<float> ring;
    std::atomic<int>   write_pos{0};
    std::atomic<int>   read_pos{0};
    static constexpr int RING = BLOCK_SIZE * 16;
    static OSStatus render_cb(void* ref, AudioUnitRenderActionFlags*,
                              const AudioTimeStamp*, UInt32, UInt32 nf,
                              AudioBufferList* data) {
        auto* b = static_cast<CABackend*>(ref);
        float* out = (float*)data->mBuffers[0].mData;
        int avail = (b->write_pos.load() - b->read_pos.load() + b->RING) % b->RING;
        int fill  = std::min((int)nf * 2, avail);
        int rp    = b->read_pos.load();
        for (int i = 0; i < fill; ++i) out[i] = b->ring[(rp + i) % b->RING];
        for (int i = fill; i < (int)nf * 2; ++i) out[i] = 0.f;
        b->read_pos.store((rp + fill) % b->RING);
        return noErr;
    }
};
bool AudioIO::_open_device() {
    auto* b = new CABackend(); b->ring.assign(CABackend::RING, 0.f); _backend = b;
    AudioComponentDescription desc{kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &b->au) != noErr) { delete b; _backend=nullptr; return false; }
    AURenderCallbackStruct cb{CABackend::render_cb, b};
    AudioUnitSetProperty(b->au, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    AudioStreamBasicDescription fmt{(Float64)SR, kAudioFormatLinearPCM,
        kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AudioUnitSetProperty(b->au, kAudioUnitProperty_StreamFormat,
                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt));
    AudioUnitInitialize(b->au); AudioOutputUnitStart(b->au);
    std::fprintf(stderr, "[AudioIO] CoreAudio ready\n"); return true;
}
void AudioIO::_close_device() {
    auto* b = static_cast<CABackend*>(_backend); if (!b) return;
    if (b->au) { AudioOutputUnitStop(b->au); AudioUnitUninitialize(b->au); AudioComponentInstanceDispose(b->au); }
    delete b; _backend = nullptr;
}
bool AudioIO::_write_block(const float* d, int frames) {
    auto* b = static_cast<CABackend*>(_backend); if (!b) return false;
    int wp = b->write_pos.load();
    for (int i = 0; i < frames * 2; ++i) { b->ring[(wp + i) % CABackend::RING] = d[i]; }
    b->write_pos.store((wp + frames * 2) % CABackend::RING);
    return true;
}

#else
bool  AudioIO::_open_device()                         { std::fprintf(stderr,"[AudioIO] null backend\n"); return true; }
void  AudioIO::_close_device()                        {}
bool  AudioIO::_write_block(const float*,int frames)  {
    std::this_thread::sleep_for(std::chrono::microseconds((long long)(frames*1000000LL/SR)));
    return true;
}
#endif

// ═══════════════════════════════════════════════════════════════════════════
//  AudioIO — transport state machine
// ═══════════════════════════════════════════════════════════════════════════
AudioIO::AudioIO(TapeEngine& engine) : _engine(engine) {}
AudioIO::~AudioIO() { close(); }

bool AudioIO::open() {
    if (_open_flag.load()) return true;
    if (!_open_device())
        CV_ERR(AUDIO_DEVICE_OPEN_FAILED, "Audio device unavailable — running silently");
    // Start in stopped state at play speed
    _tape_speed    = 0.f;
    _target_speed  = 0.f;
    _squeal_phase  = 0.f;
    _open_flag.store(true);
    _thread = std::thread(&AudioIO::_dsp_thread, this);
    return true;
}
void AudioIO::close() {
    if (!_open_flag.load()) return;
    _open_flag.store(false);
    if (_thread.joinable()) _thread.join();
    _close_device();
}

// ── Transport commands ────────────────────────────────────────────────────────
// _target_speed is the SIGNED speed we ramp toward.
//   0       = stopped
//  +1       = play forward at normal speed
//  -1       = play reverse at normal speed
//  +N / -N  = shuttle forward/reverse at N× speed

void AudioIO::play_forward() {
    _target_speed.store(+1.f);
    if (!_open_flag.load()) open();
}
void AudioIO::play_reverse() {
    _target_speed.store(-1.f);
    if (!_open_flag.load()) open();
}
void AudioIO::stop() {
    _target_speed.store(0.f);
}
void AudioIO::shuttle_rewind(float speed_mult) {
    _target_speed.store(-speed_mult);
    if (!_open_flag.load()) open();
}
void AudioIO::shuttle_ff(float speed_mult) {
    _target_speed.store(+speed_mult);
    if (!_open_flag.load()) open();
}
void AudioIO::shuttle_faster(bool reverse) {
    // Increase shuttle speed: 10x → 20x → 40x → 60x → 80x → 100x (max)
    float cur = std::abs(_target_speed.load());
    float next;
    if (cur < 5.f)           next = 10.f;
    else if (cur < 15.f)     next = 20.f;
    else if (cur < 30.f)     next = 40.f;
    else if (cur < 50.f)     next = 60.f;
    else if (cur < 70.f)     next = 80.f;
    else                     next = 100.f;  // max
    float sign = reverse ? -1.f : +1.f;
    _target_speed.store(sign * next);
}
void AudioIO::stop_shuttle() { stop(); }

void AudioIO::cycle_shuttle_speed() {
    float cur = std::abs(_target_speed.load());
    const float tiers[] = {40.f, 80.f, 160.f};
    float next = tiers[0];
    for (int i = 0; i < 3; ++i)
        if (std::abs(cur - tiers[i]) < 5.f) { next = tiers[(i+1)%3]; break; }
    // Preserve sign (rewind stays negative, ff stays positive)
    float sign = (_target_speed.load() >= 0.f) ? +1.f : -1.f;
    _target_speed.store(sign * next);
}

bool AudioIO::is_playing()   const {
    float ts = _target_speed.load();
    return (ts > 0.5f || ts < -0.5f) && std::abs(_tape_speed) > 0.05f;
}
bool AudioIO::is_stopped()   const { return std::abs(_tape_speed) < 0.02f; }
bool AudioIO::is_rewinding() const {
    float ts = _target_speed.load();
    return ts < -1.5f;
}
bool AudioIO::is_ffing() const {
    float ts = _target_speed.load();
    return ts > +1.5f;
}

// ── DSP thread ────────────────────────────────────────────────────────────────
// Key insight: we track a SINGLE signed _tape_speed value.
// Positive = forward, negative = reverse, magnitude = speed multiplier.
// This eliminates the independent capstan/direction bug entirely:
// a single ramp from -40 → +1 passes through zero naturally,
// and at every point the tape moves at exactly tape_speed × base_ips.
//
// Inertia model (linear ramp per block):
//   Stopping (any speed → 0):  BRAKE_RATE   (fast, like pressing stop)
//   Starting play (0 → ±1):    PLAY_SPINUP  (0.6s, realistic motor ramp)
//   Shuttle start (0 → ±N):    SHUT_UP      (0.25s, fast)
//   Shuttle wind-down to play: rate proportional to speed difference
//   Direction flip (e.g. play fwd → play rev): passes through 0 at PLAY_SPINUP rate
//   Shuttle → stop:            BRAKE_RATE   (fast, ~0.15s from 40×)

void AudioIO::_dsp_thread() {
    std::vector<Frame> frame_buf(BLOCK_SIZE);
    std::vector<float> interleaved(BLOCK_SIZE * 2);
    float squeal_phase = 0.f;

    while (_open_flag.load()) {
        float target = _target_speed.load();
        float cur    = _tape_speed;

        // ── Inertia ramp ──────────────────────────────────────────────────────
        // Choose ramp rate based on what transition is happening.
        float rate;
        float diff = target - cur;

        if (std::abs(target) < 0.01f) {
            // Braking to stop — fast regardless of current speed
            rate = BRAKE_RATE;
        } else if (std::abs(target) <= 1.1f) {
            // Target is play speed (±1)
            if (std::abs(cur) <= 1.1f) {
                // Play ↔ play or play → reverse: normal capstan rate
                rate = PLAY_SPINUP;
            } else {
                // Shuttle → play: first brake to near-play-speed, then normal ramp
                // Use a rate proportional to how far we still need to travel,
                // floored at PLAY_SPINUP so it doesn't stall at high speeds.
                rate = std::max(PLAY_SPINUP, BRAKE_RATE * (std::abs(cur) - 1.f) / 39.f);
            }
        } else {
            // Target is shuttle speed (|target| > 1)
            if (std::abs(cur) < 0.05f) {
                // Spinning up from stop
                rate = SHUT_UP;
            } else if (std::signbit(target) == std::signbit(cur) &&
                       std::abs(target) > std::abs(cur)) {
                // Accelerating in same direction
                rate = SHUT_UP;
            } else {
                // Decelerating or changing direction — brake
                rate = BRAKE_RATE;
            }
        }

        // ── IPS-based braking inertia ─────────────────────────────────────────
        // Higher tape speeds = more momentum = longer to stop
        // Scale brake time by current speed: at 1x (play speed) use normal rate,
        // at 40x shuttle use ~3x longer braking distance
        float ips_mult = std::max(1.f, std::abs(cur) / 15.f);
        rate /= std::clamp(ips_mult, 1.f, 4.f);  // Cap at 4x slower braking

        // Apply ramp — move cur toward target at rate per sample, over one block
        float step = rate * BLOCK_SIZE;
        if (std::abs(diff) <= step)
            cur = target;
        else
            cur += std::copysign(step, diff);

        _tape_speed = cur;
        _current_speed_mult.store(std::abs(cur));
        // Signed speed for reel animation: positive=fwd, negative=rev
        // When reversing, is_reversed flips at zero crossing, so use
        // engine direction × magnitude for a smooth signed value.
        {
            float sign = _engine.is_reversed ? -1.f : 1.f;
            _signed_tape_speed.store(sign * std::abs(cur));
        }

        // ── Update engine direction flag ─────────────────────────────────────
        bool want_rev = (cur < -0.001f);
        if (want_rev != _engine.is_reversed) {
            float tgt = _target_speed.load();
            bool is_shuttle = (std::abs(tgt) > 1.5f);
            // Shuttle: flip direction immediately (squeal covers the glitch).
            // Play mode: wait for near-zero to avoid a jarring audio artifact.
            if (is_shuttle || std::abs(cur) < 0.05f)
                _engine.set_reverse(want_rev, false);
        }

        // ── Set engine transport parameters ────────────────────────────────
        // tape_speed_mult: full signed speed magnitude — carries shuttle speed
        //   and the smooth inertia ramp (0→1 during spinup, 0→40 for shuttle).
        //   This is multiplied into the read stride in dsp_process.
        // motor_engage: kept at 1.0 while tape is moving — the inertia ramp
        //   is now entirely in _tape_speed, so motor_engage is just on/off.
        {
            std::lock_guard<std::mutex> g(_engine.lock);
            _engine.params.tape_speed_mult = std::abs(cur);
            _engine.params.motor_engage    = 1.0f;  // always on when moving
        }

        // ── Near stopped — silence ────────────────────────────────────────────
        if (std::abs(cur) < 0.002f) {
            _engine.is_playing.store(false);
            std::fill(interleaved.begin(), interleaved.end(), 0.f);
            // If the audio backend init failed earlier (_backend == nullptr),
            // _write_block returns false every iteration. Without this
            // sleep_for fallback the loop would spin at 100% CPU pinning a
            // core and starving the UI thread. Pace ourselves to one block's
            // worth of wall-clock audio so responsive normalisation kicks
            // back in as soon as the backend recovers.
            if (!_write_block(interleaved.data(), BLOCK_SIZE))
                std::this_thread::sleep_for(std::chrono::microseconds((long long)(BLOCK_SIZE * 1000000LL / SR)));
            continue;
        }

        _engine.is_playing.store(true);

        // ── Run DSP ───────────────────────────────────────────────────────────
        bool ok = _engine.dsp_process(frame_buf.data(), BLOCK_SIZE);
        if (!ok) {
            // Distinguish end-of-file (normal) from empty engine (error)
            // In streaming mode, audio_data is empty but stream.is_open() is true
            if (_engine.audio_data.empty() && !_engine.stream.is_open())
                CV_ERR(AUDIO_DSP_EMPTY_ENGINE, "dsp_process: no audio loaded");
            else
                CV_ERR(AUDIO_DSP_END_OF_FILE, "end of tape reached");
            _target_speed.store(0.f);
            _engine.is_playing.store(false);
            std::fill(interleaved.begin(), interleaved.end(), 0.f);
            // Same sleep_for fallback as the stopped path: on a backend-less
            // machine the loop otherwise chews through EOF cycling at 100%
            // CPU and the rash of CV_ERR/EOF notifications hides the real
            // root cause. The block-duration sleep keeps the loop bounded
            // and lets the UI thread pre-empt normally.
            if (!_write_block(interleaved.data(), BLOCK_SIZE))
                std::this_thread::sleep_for(std::chrono::microseconds((long long)(BLOCK_SIZE * 1000000LL / SR)));
            continue;
        }

        // ── Shuttle squeal blend ──────────────────────────────────────────────
        float abs_speed = std::abs(cur);
        if (abs_speed > 2.f) {
            float squeal_mix = std::clamp((abs_speed - 2.f) / 38.f, 0.f, 0.9f);
            float base  = _engine.params.ips_base;
            float prog  = (_engine.total_samples > 0)
                          ? (float)(_engine.play_head / _engine.total_samples) : 0.5f;
            float rload = (cur < 0.f) ? (1.f+(1.f-prog)*1.5f) : (1.f+prog*1.5f);
            float hz    = std::clamp(1800.f*(base/15.f)*rload*std::sqrt(abs_speed/40.f),
                                     150.f, 16000.f);
            float pinc  = TWO_PI * hz / SR_F;
            float amp   = (0.18f*std::sin(PI*prog)+0.04f) * squeal_mix;
            for (int i = 0; i < BLOCK_SIZE; ++i) {
                float sq = (std::sin(squeal_phase)
                          + std::sin(squeal_phase*3.f)*0.15f
                          + std::sin(squeal_phase*5.f)*0.07f) * amp;
                frame_buf[i].l = frame_buf[i].l*(1.f-squeal_mix) + sq*(cur<0?1.f:0.92f);
                frame_buf[i].r = frame_buf[i].r*(1.f-squeal_mix) + sq*(cur<0?0.92f:1.f);
                squeal_phase = std::fmod(squeal_phase + pinc, TWO_PI);
            }
        }

        for (int i = 0; i < BLOCK_SIZE; ++i) {
            interleaved[i*2]   = frame_buf[i].l;
            interleaved[i*2+1] = frame_buf[i].r;
        }

        // ── VU Metering — compute peak levels with decay ─────────────────────
        float peak_l = 0.f, peak_r = 0.f;
        for (int i = 0; i < BLOCK_SIZE; ++i) {
            float al = std::abs(frame_buf[i].l);
            float ar = std::abs(frame_buf[i].r);
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
        }
        // VU response speed: 0=slow (classic VU), 1=medium, 2=fast (PPM)
        // Decay rates and hold times vary by setting
        int response = _vu_response.load();
        float decay_rate;
        int hold_blocks;
        switch (response) {
            case 0:  // Slow - classic VU: 300ms attack, 1.5s decay
                decay_rate = 0.003f;
                hold_blocks = (SR / BLOCK_SIZE) / 3;  // ~333ms hold
                break;
            case 2:  // Fast - PPM style: 50ms attack, 200ms decay
                decay_rate = 0.03f;
                hold_blocks = (SR / BLOCK_SIZE) / 20;  // ~50ms hold
                break;
            default: // Medium - standard: 100ms attack, 500ms decay
                decay_rate = 0.01f;
                hold_blocks = (SR / BLOCK_SIZE) / 10;  // ~100ms hold
                break;
        }
        _level_peak_l = std::max(_level_peak_l, peak_l);
        _level_peak_r = std::max(_level_peak_r, peak_r);
        if (++_level_decay_cnt >= hold_blocks) {
            _level_decay_cnt = 0;
            _level_peak_l = std::max(0.f, _level_peak_l - decay_rate);
            _level_peak_r = std::max(0.f, _level_peak_r - decay_rate);
        }
        _level_left.store(_level_peak_l);
        _level_right.store(_level_peak_r);

        // Sleep fallback applies on the success path too: the backend can
        // be opened but degrade mid-track (e.g. USB headset yanked, ALSA
        // device hot-unplugged). _write_block returning false in that case
        // shouldn't pin a core; pacing at one block duration keeps the
        // engine honest until the user gets the failure surfaced via the
        // notification panel rather than via a pegged CPU.
        if (!_write_block(interleaved.data(), BLOCK_SIZE))
            std::this_thread::sleep_for(std::chrono::microseconds((long long)(BLOCK_SIZE * 1000000LL / SR)));
    }

    _engine.is_playing.store(false);
}

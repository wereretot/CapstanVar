#include "ui.hpp"
#include <imgui.h>
#include <imgui-SFML.h>
#include <SFML/Graphics.hpp>
#include <SFML/Window/Event.hpp>
#include <cmath>
#include <filesystem>
namespace fs = std::filesystem;
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include "key_bindings.hpp"

#ifdef NAGRA_HAS_TFD
#include <tinyfiledialogs.h>
#endif

// ── Colour helpers ────────────────────────────────────────────────────────────
[[maybe_unused]] static ImVec4 dim(ImVec4 c, float f = 0.3f) { return {c.x*f,c.y*f,c.z*f,c.w}; }
static ImVec4 blend(ImVec4 a, ImVec4 b, float t) {
    return {a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, 1.f};
}

// ── CapstanApp ──────────────────────────────────────────────────────────────────
CapstanApp::CapstanApp()
    : _window(sf::VideoMode(1280, 900),
              "CAPSTANVAR  ·  ANALOG TAPE SIMULATOR  ·  MODULAR DSP",
              sf::Style::Default),
      _engine(), _audio(_engine), _presets(), _renderer(_engine)
{
    _window.setFramerateLimit(60);
    _init_imgui();
    _apply_imgui_theme();
    _render_opts.sample_rate = 44100;
    _render_opts.bit_depth   = 24;
    _render_opts.dither      = true;
    _render_opts.dc_block    = true;
    _render_opts.normalize   = true;

    AppDirs::init();
    _perf.load();
    FileDialog::recents.load(AppDirs::recents_file());

    // Set window icon from embedded RGBA data
    _window.setIcon(AppIcon::WIDTH, AppIcon::HEIGHT, AppIcon::PIXELS);

    // Sync VU meter response setting to audio engine
    _audio.set_vu_response(_perf.vu_response);

    auto pr = _presets.find_builtin("Ampex 456 (30ips)");
    if (pr) _apply_preset(pr->params, pr->name);
    _audio.open();   // start DSP thread now; transport starts in Stopped/braking state
}

CapstanApp::~CapstanApp() {
    _audio.close();
    _renderer.cancel_all();
    _shutdown_imgui();
}

void CapstanApp::run() {
    sf::Clock clock;
    while (_window.isOpen()) {
        _process_events();
        ImGui::SFML::Update(_window, clock.restart());
        _draw_frame();
        _window.clear(sf::Color(10, 10, 11));
        ImGui::SFML::Render(_window);
        _window.display();
    }
}

// ── ImGui init ────────────────────────────────────────────────────────────────
void CapstanApp::_init_imgui() {
    (void)ImGui::SFML::Init(_window);
    // Rebuild font atlas at 15px — ImGui-SFML default is 13px (too small)
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    io.Fonts->AddFontDefault();  // adds Proggy Clean
    // Scale up: rebuild at 15px by using FontGlobalScale
    io.FontGlobalScale = 15.f / 13.f;  // ~1.154 — scales all text up uniformly
    (void)ImGui::SFML::UpdateFontTexture();
}

void CapstanApp::_apply_imgui_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = 0.f; s.ChildRounding   = 3.f;
    s.FrameRounding   = 3.f; s.GrabRounding    = 3.f;
    s.PopupRounding   = 3.f; s.TabRounding     = 2.f;
    s.WindowBorderSize= 0.f; s.ChildBorderSize = 1.f;
    s.FramePadding    = {8,5}; s.ItemSpacing = {6,4}; s.WindowPadding = {10,8};
    s.GrabMinSize     = 8.f;
    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]          = Col::bg;
    c[ImGuiCol_ChildBg]           = Col::bg2;
    c[ImGuiCol_PopupBg]           = Col::bg3;
    c[ImGuiCol_Border]            = Col::border;
    c[ImGuiCol_FrameBg]           = Col::bg3;
    c[ImGuiCol_FrameBgHovered]    = Col::bg4;
    c[ImGuiCol_FrameBgActive]     = Col::bg4;
    c[ImGuiCol_TitleBg]           = Col::bg2;
    c[ImGuiCol_TitleBgActive]     = Col::bg3;
    c[ImGuiCol_ScrollbarBg]       = Col::bg2;
    c[ImGuiCol_ScrollbarGrab]     = Col::grey;
    c[ImGuiCol_SliderGrab]        = Col::amber;
    c[ImGuiCol_SliderGrabActive]  = Col::amber;
    c[ImGuiCol_Button]            = Col::bg3;
    c[ImGuiCol_ButtonHovered]     = Col::bg4;
    c[ImGuiCol_ButtonActive]      = Col::border;
    c[ImGuiCol_Header]            = Col::bg4;
    c[ImGuiCol_HeaderHovered]     = Col::border;
    c[ImGuiCol_HeaderActive]      = Col::border;
    c[ImGuiCol_Tab]               = Col::bg2;
    c[ImGuiCol_TabHovered]        = Col::bg4;
    c[ImGuiCol_TabActive]         = Col::bg4;
    c[ImGuiCol_TabUnfocusedActive]= Col::bg3;
    c[ImGuiCol_CheckMark]         = Col::amber;
    c[ImGuiCol_Text]              = Col::white;
    c[ImGuiCol_TextDisabled]      = Col::grey;
    c[ImGuiCol_Separator]         = Col::border;
}

void CapstanApp::_shutdown_imgui() { ImGui::SFML::Shutdown(); }

// ── Events ────────────────────────────────────────────────────────────────────
void CapstanApp::_process_events() {
    sf::Event ev;
    while (_window.pollEvent(ev)) {
        ImGui::SFML::ProcessEvent(_window, ev);
        if (ev.type == sf::Event::Closed) {
            _audio.stop();
            if (_project_dirty) _show_close_confirm = true;
            else                _window.close();
        }

        // Keyboard shortcuts — only when ImGui is not consuming keyboard
        if (ev.type == sf::Event::KeyPressed)
        {
            // Only suppress transport shortcuts when user is typing in a text box
            bool typing = ImGui::GetIO().WantTextInput;
            bool playing   = _engine.is_playing.load();
            bool rewinding = _audio.is_rewinding();
            bool ffing     = _audio.is_ffing();

            if (!typing) switch (ev.key.code) {
            // Space — play/stop (toggle)
            case Keys::PLAY_TOGGLE:
                if (rewinding || ffing) { _audio.stop(); }
                else if (playing)       { _stop_transport(); }
                else                    { _start_forward(); }
                break;

            // Enter — play forward
            case Keys::PLAY_FWD_ALT:
                if (!playing || _engine.is_reversed) _start_forward();
                break;

            // Backspace / R — play reverse (alternative to Left arrow)
            case Keys::PLAY_REV_ALT:
            case Keys::PLAY_REV_ALT2:
                if (!playing || !_engine.is_reversed) _start_reverse();
                break;

            // Down arrow / Escape — stop (also cancels shuttle operations)
            case Keys::STOP:
            case Keys::STOP_ALT:
                if (rewinding || ffing || playing) _stop_transport();
                break;

            // Left arrow — play reverse; Shift+Left — rewind shuttle
            // Pressing Left during rewind increases shuttle speed
            case Keys::PLAY_REVERSE:
                if (ev.key.shift) {
                    // Shift+Left = rewind shuttle (toggle)
                    if (rewinding) _audio.stop();
                    else           { _stop_transport(); _toggle_rewind(); }
                } else {
                    // Left = play reverse or increase rewind speed
                    if (ffing) {
                        _stop_transport(); _start_reverse();
                    } else if (rewinding) {
                        _audio.shuttle_faster(true);  // increase rewind speed
                    } else if (playing && !_engine.is_reversed) {
                        _start_reverse();
                    } else if (!playing) {
                        _start_reverse();
                    }
                }
                break;

            // Right arrow — play forward; Shift+Right — fast forward shuttle
            // Pressing Right during FF increases shuttle speed
            case Keys::PLAY_FORWARD:
                if (ev.key.shift) {
                    // Shift+Right = fast forward shuttle (toggle)
                    if (ffing) _audio.stop();
                    else       { _stop_transport(); _toggle_ff(); }
                } else {
                    // Right = play forward or increase FF speed
                    if (rewinding) {
                        _stop_transport(); _start_forward();
                    } else if (ffing) {
                        _audio.shuttle_faster(false);  // increase FF speed
                    } else if (playing && _engine.is_reversed) {
                        _start_forward();
                    } else if (!playing) {
                        _start_forward();
                    }
                }
                break;

            // Up arrow — play forward (alternative to Right)
            case Keys::PLAY_FORWARD_ALT:
                if (rewinding || ffing) { _stop_transport(); _start_forward(); }
                else if (playing && _engine.is_reversed) _start_forward();
                else if (!playing) _start_forward();
                break;

            // O — open audio file
            case Keys::OPEN_AUDIO:
                if (ev.key.control && !ev.key.shift) _open_load_audio();
                break;

            default: break;
            } // end if(!typing) switch

            // Ctrl+O always works regardless of text focus
            if (ev.key.control && ev.key.code == Keys::OPEN_AUDIO && !ev.key.shift)
                _open_load_audio();
            // Ctrl+S = save project; Ctrl+Shift+S = save as
            if (ev.key.control && ev.key.code == Keys::SAVE_PROJECT) {
                if (ev.key.shift || _project_path.empty()) _open_save_project();
                else _save_project(_project_path);
            }
            // Ctrl+Shift+O = open project
            if (ev.key.control && ev.key.shift && ev.key.code == Keys::OPEN_PROJECT)
                _open_load_project();
        }
    }
}

// ── File dialog management ────────────────────────────────────────────────────
void CapstanApp::_open_load_audio() {
    _fd_load_audio.open_load("Load Tape File",
        {".wav",".flac",".aif",".aiff",".ogg",".mp3"});
    _fd_pending = FDPending::LoadAudio;
}
void CapstanApp::_open_load_preset() {
    _fd_load_preset.open_load("Import Preset", {".cvpr",".json"},
                          AppDirs::presets());
    _fd_pending = FDPending::LoadPreset;
}
void CapstanApp::_open_save_preset(const std::string& default_name) {
    _fd_save_preset.open_save("Export Preset", default_name+".cvpr",
                          AppDirs::presets());
    _fd_pending = FDPending::SavePreset;
}
void CapstanApp::_open_save_render() {
    _fd_save_render.open_save("Save Rendered File", "output.wav");
    _fd_pending = FDPending::SaveRender;
}

void CapstanApp::_draw_file_dialogs() {
    switch (_fd_pending) {
    case FDPending::LoadAudio: {
        if (_fd_load_audio.draw()) {
            std::string path = _fd_load_audio.result();
            if (!path.empty()) {
                std::error_code _ec;
                if (!fs::is_regular_file(path, _ec)) {
                    CV_ERR(FILE_NOT_FOUND, path);
                } else {
                    // Check if user is trying to open a project file as audio
                    if (path.size() >= 10 && path.substr(path.size() - 10) == ".cvproject") {
                        CV_ERR(FILE_OPEN_FAILED, path + ": This is a project file. Use File → Open Project (" + std::string(Keys::UI::OPEN_PROJECT) + ") instead.");
                        _fd_pending = FDPending::None;
                        return;
                    }
                    // Check if user is trying to open a preset file as audio
                    if ((path.size() >= 5 && path.substr(path.size() - 5) == ".cvpr") ||
                        (path.size() >= 5 && path.substr(path.size() - 5) == ".json")) {
                        CV_ERR(FILE_OPEN_FAILED, path + ": This is a preset file. Use File → Import Preset instead.");
                        _fd_pending = FDPending::None;
                        return;
                    }
                    
                    _audio.stop();
                    _loaded_file = path;
                    // Apply current perf settings to stream buffer BEFORE opening
                    // (StreamBuffer::open() uses these values to allocate the ring buffer)
                    _engine.stream.ring_frames   = _perf.ring_seconds  * 44100;
                    _engine.stream.ahead_frames  = _perf.ahead_seconds * 44100;
                    _engine.stream.io_chunk_frames = _perf.io_chunk_frames;
                    std::fprintf(stderr, "[UI] Applying perf options: ring=%ds (%d frames), ahead=%ds (%d frames), chunk=%d frames\n",
                        _perf.ring_seconds, _engine.stream.ring_frames,
                        _perf.ahead_seconds, _engine.stream.ahead_frames,
                        _perf.io_chunk_frames);
                    if (!_engine.load_file(path)) {
                        CV_ERR(FILE_OPEN_FAILED, path);
                        _loaded_file.clear();
                    } else {
                        _reel.reset();
                        _project_dirty = true;
                        _update_window_title();
                        _anim.clear_all();
                        _anim_cursor = 0.0;
                        _timeline.view_start = 0.0;
                        double new_ts = (double)_engine.total_samples;
                        _timeline.view_end = (new_ts > 0) ? new_ts : 44100.0;
                        _timeline.prev_cursor_pos = 0.0;
                    }
                }
            }
            _fd_pending = FDPending::None;
        }
        break;
    }
    case FDPending::LoadPreset:
        if (_fd_load_preset.draw()) {
            std::string path = _fd_load_preset.result();
            if (!path.empty()) {
                auto pr = _presets.import_preset(path);
                if (pr) { _presets.save_session(pr->name,pr->params); _apply_preset(pr->params,pr->name); }
            }
            _fd_pending = FDPending::None;
        }
        break;
    case FDPending::SavePreset: {
        if (_fd_save_preset.draw()) {
            std::string path = _fd_save_preset.result();
            if (!path.empty()) {
                if (path.size()<5||path.substr(path.size()-5)!=".cvpr") path+=".cvpr";
                auto& b = _presets.builtin_presets();
                std::string name=(_preset_idx>=0&&_preset_idx<(int)b.size())?b[_preset_idx].name:"Custom";
                _presets.export_preset(path,name,_ui_params);
            }
            _fd_pending = FDPending::None;
        }
        break;
    }
    case FDPending::SaveRender:
        if (_fd_save_render.draw()) {
            std::string path = _fd_save_render.result();
            if (!path.empty()) {
                if (path.size()<4||path.substr(path.size()-4)!=".wav") path+=".wav";
                _render_opts.path         = path;
                _render_opts.display_name = path.substr(path.find_last_of("/\\")+1);
                _render_opts.anim         = _anim;  // snapshot animation at enqueue time
                _show_render_dialog       = false;
                _renderer.enqueue(_render_opts);
            }
            _fd_pending = FDPending::None;
        }
        break;
    case FDPending::LoadProject:
        if (_fd_load_project.draw()) {
            std::string path = _fd_load_project.result();
            if (!path.empty()) _load_project(path);
            _fd_pending = FDPending::None;
        }
        break;
    case FDPending::SaveProject:
        if (_fd_save_project.draw()) {
            std::string path = _fd_save_project.result();
            if (!path.empty()) {
                if (path.size()<11 ||
                    path.substr(path.size()-10) != ".cvproject")
                    path += ".cvproject";
                _save_project(path);
                if (_pending_close_after_save) {
                    _pending_close_after_save = false;
                    _window.close();
                }
                if (_pending_new_after_save) {
                    _pending_new_after_save = false;
                    _new_project();
                }
            }
            _fd_pending = FDPending::None;
        }
        break;
    case FDPending::None: break;
    }
}

// ── Main frame ────────────────────────────────────────────────────────────────
// Layout (top to bottom, all fixed heights except the tab area):
//
//  ┌─────────────────────────────────────────────┐
//  │  Header strip: title | reel | status         │  ~90px
//  ├─────────────────────────────────────────────┤
//  │  Preset bar                                  │  ~30px
//  ├─────────────────────────────────────────────┤
//  │                                             │
//  │  Tabs + sliders  (fills remaining space)    │  flexible
//  │                                             │
//  ├─────────────────────────────────────────────┤
//  │  Transport controls (always visible)         │  ~120px
//  └─────────────────────────────────────────────┘
//
// Key trick: draw the transport CHILD last but with a RESERVED negative height
// in the tabs child so both are always fully visible.

static constexpr float TRANSPORT_H = 144.f;  // row1(60) + row2(30) + footer(28) + padding
static constexpr float HEADER_H    = 96.f;
static constexpr float PRESETBAR_H = 38.f;

void CapstanApp::_draw_frame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ── Header ────────────────────────────────────────────────────────────────
    _draw_header();

    ImGui::Separator();

    // ── Preset bar ────────────────────────────────────────────────────────────
    _draw_preset_bar();

    ImGui::Separator();

    // Compute display params: base + curves (for slider visual feedback).
    // Also push animated values to engine every frame during playback.
    _display_params = _ui_params;
    if (_anim.enabled && !_anim.curves.empty() && _engine.is_playing.load()) {
        _anim.apply(_display_params, _engine.play_head);
        std::lock_guard<std::mutex> g(_engine.lock);
        EngineParams& ep = _engine.params;
        ep.ips_base=_display_params.ips_base; ep.motor_health=_display_params.motor_health;
        ep.motor_drag=_display_params.motor_drag; ep.motor_boost=_display_params.motor_boost;
        ep.wow_dep=_display_params.wow_dep; ep.flutter_dep=_display_params.flutter_dep;
        ep.scrape_flutter=_display_params.scrape_flutter; ep.tension_load=_display_params.tension_load;
        ep.dropout_rate=_display_params.dropout_rate; ep.drive=_display_params.drive;
        ep.bias=_display_params.bias; ep.replay_diff=_display_params.replay_diff;
        ep.asperities=_display_params.asperities; ep.barkhausen=_display_params.barkhausen;
        ep.crosstalk=_display_params.crosstalk; ep.print_through=_display_params.print_through;
        ep.demagnetization=_display_params.demagnetization; ep.oxide_shedding=_display_params.oxide_shedding;
        ep.hiss=_display_params.hiss; ep.hiss_color=_display_params.hiss_color;
        ep.mains_hum=_display_params.mains_hum; ep.cutoff_base=_display_params.cutoff_base;
        ep.head_bump=_display_params.head_bump; ep.azimuth_drift=_display_params.azimuth_drift;
        ep.sticky_shed=_display_params.sticky_shed; ep.oxide_type=_display_params.oxide_type;
        ep.eq_curve=_display_params.eq_curve; ep.lf_trim_db=_display_params.lf_trim_db;
        ep.hf_trim_db=_display_params.hf_trim_db; ep.format_id=_display_params.format_id;
        ep.format_locked=_display_params.format_locked;
    }

    // ── Tabs — fixed height that leaves exactly TRANSPORT_H + margins at bottom
    float avail = io.DisplaySize.y - HEADER_H - PRESETBAR_H
                  - TRANSPORT_H - 28.f; // separators + window padding
    avail = std::max(avail, 80.f);
    ImGui::BeginChild("##tabs_area", {0, avail}, false);
    _draw_tabs();
    ImGui::EndChild();

    // ── Transport controls — always at bottom ─────────────────────────────────
    ImGui::Separator();
    _draw_transport_controls();

    ImGui::End();

    _draw_file_dialogs();
    _draw_close_confirm();
    if (_show_render_dialog) _draw_render_dialog();
    if (_show_save_dialog)   _draw_save_dialog();
    _draw_timeline();
    _draw_error_log();
    if (_show_options) _draw_options();
    _draw_notifications();  // Inline notifications below header
    ErrorLog::get().clear_new_flag();
}

// ── Compact header ────────────────────────────────────────────────────────────
void CapstanApp::_draw_header() {
    float total_w = ImGui::GetContentRegionAvail().x;

    // ── Left: machine status ──────────────────────────────────────────────────
    ImGui::BeginChild("##hdr_status", {260, HEADER_H}, true);
    {
        auto& t    = _engine.transport;
        float inst = t.last_instant_speed;
        float dev  = (inst-1.f)*100.f;
        float conf = t.last_conflict;
        bool  pl   = _engine.is_playing.load();
        bool  rwd  = _audio.is_rewinding();
        bool  ff   = _audio.is_ffing();

        const char* state; ImVec4 sc;
        if      (rwd)                                    { state="REWINDING"; sc=Col::cyan;   }
        else if (ff)                                     { state="FAST FWD";  sc=Col::green;  }
        else if (pl&&(conf>.2f||std::abs(dev)>8))        { state="UNSTABLE";  sc=Col::red;    }
        else if (pl&&_engine.is_reversed)                 { state="REVERSE";   sc=Col::orange; }
        else if (pl)                                      { state="RUNNING";   sc=Col::amber;  }
        else                                              { state="STOPPED";   sc=Col::grey;   }

        ImGui::PushStyleColor(ImGuiCol_Text, sc);
        ImGui::Text("%-10s", state); ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
        ImGui::Text("%+.1f%%  %.2fC", dev, conf); ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, pl ? Col::amber : Col::grey);
        ImGui::Text("%s %06.3f IPS",
                    pl?(_engine.is_reversed?"<":">"): " ",
                    pl?_ui_params.ips_base*inst:0.f);
        ImGui::PopStyleColor();

        if (_engine.total_samples > 0) {
            float el  = (float)(_engine.play_head / SR);
            float rem = (float)((_engine.total_samples-_engine.play_head)/SR);
            ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
            ImGui::Text("%02d:%05.2f  -%02d:%05.2f",
                        (int)(el/60), std::fmod(el,60.f),
                        (int)(rem/60),std::fmod(rem,60.f));

            float prog = (float)(_engine.play_head/_engine.total_samples);
            // Inline tape-position bar
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float bar_w = ImGui::GetContentRegionAvail().x;
            ImGui::GetWindowDrawList()->AddRectFilled(
                p0, {p0.x+bar_w, p0.y+5}, IM_COL32(40,40,50,255));
            ImGui::GetWindowDrawList()->AddRectFilled(
                p0, {p0.x+bar_w*prog, p0.y+5},
                IM_COL32(200,140,0,230));
            ImGui::Dummy({bar_w, 6});
            ImGui::PopStyleColor();
        }

        // Loaded file name
        if (!_loaded_file.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
            std::string fn = _loaded_file;
            auto p = fn.find_last_of("/\\");
            if (p!=std::string::npos) fn=fn.substr(p+1);
            ImGui::TextUnformatted(fn.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ── Centre: title + reel animation + VU meters ────────────────────────────
    float side_w  = 260.f + 10.f + 200.f + 10.f;
    float mid_w   = std::max(120.f, total_w - side_w);
    ImGui::BeginChild("##hdr_mid", {mid_w, HEADER_H}, false);
    {
        // ── Left VU Meter ─────────────────────────────────────────────────────
        float meter_radius = HEADER_H * 0.42f;
        float level_l = _audio.get_level_left();
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        ImVec2 vu_l_center = {c0.x + meter_radius + 8.f, c0.y + meter_radius + 4.f};
        _draw_vu_meter(vu_l_center, meter_radius, level_l, "L");
        ImGui::SetCursorScreenPos({c0.x + meter_radius * 2.f + 16.f, c0.y});

        // Reel visualisation using draw list
        ImVec2 reel_pos = ImGui::GetCursorScreenPos();
        // Reel animation — ground truth is play_head delta per frame.
        // Captures inertia, speed, direction, wow, flutter, all effects.
        // Update reel rotation speed based on current IPS setting
        _reel.set_ips(_display_params.ips_base);
        _reel.draw(
            _engine.play_head,
            _engine.total_samples,
            _engine.is_reversed,
            std::abs(_audio.signed_tape_speed()) > 0.002f,
            _audio.signed_tape_speed(),
            reel_pos,
            {mid_w - (meter_radius * 2.f + 16.f) * 2.f, HEADER_H}
        );

        // ── Right VU Meter ────────────────────────────────────────────────────
        float level_r = _audio.get_level_right();
        float right_vu_x = reel_pos.x + mid_w - (meter_radius * 2.f + 16.f) - meter_radius - 8.f;
        ImVec2 vu_r_center = {right_vu_x, vu_l_center.y};
        _draw_vu_meter(vu_r_center, meter_radius, level_r, "R");

        // Title (centred between reels)
        ImGui::SetCursorScreenPos({reel_pos.x, reel_pos.y+2});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
        ImGui::SetWindowFontScale(1.3f);
        float tw = ImGui::CalcTextSize("CAPSTANVAR").x;
        ImGui::SetCursorPosX((mid_w - tw) * 0.5f);
        ImGui::Text("CAPSTANVAR");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
        float sw = ImGui::CalcTextSize("ANALOG TAPE SIMULATOR").x;
        ImGui::SetCursorPosX((mid_w - sw) * 0.5f);
        ImGui::Text("ANALOG TAPE SIMULATOR");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // ── Right: oxide / saturation / bias state ────────────────────────────────
    ImGui::BeginChild("##hdr_right", {0, HEADER_H}, true);
    {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
        ImGui::Text("OXIDE: %s", _ui_params.oxide_type.c_str()); ImGui::PopStyleColor();

        float sat=_ui_params.drive/8.f;
        const char* ss; ImVec4 sv;
        if      (sat<.15f){ ss="CLEAN";     sv=Col::grey;   }
        else if (sat<.30f){ ss="WARM";      sv=Col::amber;  }
        else if (sat<.55f){ ss="DRIVEN";    sv=Col::orange; }
        else               { ss="SATURATED"; sv=Col::red;    }
        ImGui::PushStyleColor(ImGuiCol_Text, sv);
        ImGui::Text("SAT : %s", ss); ImGui::PopStyleColor();

        float bv=_ui_params.bias;
        const char* bs; ImVec4 bvc;
        if      (std::abs(bv-1.f)<.08f){ bs="NOMINAL"; bvc=Col::grey;   }
        else if (bv<1.f)                { bs="UNDER";   bvc=Col::orange; }
        else if (bv<1.5f)               { bs="OVER";    bvc=Col::amber;  }
        else                            { bs="HIGH";    bvc=Col::red;    }
        ImGui::PushStyleColor(ImGuiCol_Text, bvc);
        ImGui::Text("BIAS: %-8s %.2f", bs, bv); ImGui::PopStyleColor();

        // Render status compact
        auto st = _renderer.get_status();
        if (_renderer.is_running()) {
            char lb[48];
            std::snprintf(lb,sizeof(lb),"%.0f%% %s %.1fx",
                          st.job_progress*100.f, st.phase.c_str(), st.xrt);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col::purple_dim);
            ImGui::ProgressBar(st.job_progress,{-1,0},lb);
            ImGui::PopStyleColor();
        } else if (!st.completed.empty()) {
            auto& last=st.completed.back();
            ImGui::PushStyleColor(ImGuiCol_Text, last.ok?Col::green:Col::red);
            ImGui::Text("%s %s", last.ok?"OK":"!!", last.name.substr(0,14).c_str());
            ImGui::PopStyleColor();
        }

        // Dropout counter
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
        ImGui::Text("DROP: #%d  DEG: %s",
                    _engine.transport.dropout_count,
                    _ui_params.demagnetization<.05f?"CLEAR":
                    _ui_params.demagnetization<.25f?"MINOR":"DEGRADED");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

// ── Preset bar ────────────────────────────────────────────────────────────────
void CapstanApp::_draw_preset_bar() {
    // Left side: Project buttons
    {
        bool dirty = _project_dirty;
        std::string proj_label = "NEW";
        if (!_project_path.empty()) {
            proj_label = fs::path(_project_path).stem().string();
            if (proj_label.size() > 14) proj_label = proj_label.substr(0,12)+"..";
            if (dirty) proj_label = "* " + proj_label;
        }
        if (_col_button(proj_label.c_str(), dirty?Col::orange_dim:Col::bg3,
                         dirty?Col::orange:Col::grey_lt, 130)) {
            if (_project_path.empty()) _open_save_project();
            else _save_project(_project_path);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(("Save project  (" + std::string(Keys::UI::SAVE_PROJECT) + ")\nRight-click: Save As").c_str());
        // Right-click for Save As
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            _open_save_project();
        }
    }
    ImGui::SameLine();
    if (_col_button("Open Project", Col::bg3, Col::cyan, 115)) _open_load_project();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(("Open project  (" + std::string(Keys::UI::OPEN_PROJECT) + ")").c_str());
    ImGui::SameLine(0, 4);
    if (_col_button("New", Col::bg3, Col::grey_lt, 44)) {
        if (_project_dirty) _show_new_confirm = true;
        else                _new_project();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New project  (clears all state)");
    ImGui::SameLine(0, 12);
    if (_col_button("LOAD", Col::green_dim, Col::green, 50)) _open_load_audio();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(("Load audio file  (" + std::string(Keys::UI::OPEN_AUDIO) + ")").c_str());
    ImGui::SameLine();
    
    // Centre spacer - push preset controls to right side
    float win_w = ImGui::GetWindowWidth();
    float left_w = 130.f + 115.f + 44.f + 12.f + 50.f + 30.f;  // project buttons + LOAD + spacing
    float right_w = 50.f + 240.f + 45.f + 80.f + 50.f + 70.f + 70.f + 72.f;  // PRESET+dropdown+OXIDE+dropdown+buttons
    float spacer = win_w - left_w - right_w - 20.f;
    if (spacer > 10.f) ImGui::Dummy({spacer, 1});
    ImGui::SameLine();
    
    // Right side: Preset selector and controls
    auto& builtins = _presets.builtin_presets();
    auto& sessions = _presets.session_presets();
    ImGui::Text("PRESET:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(240);
    const char* preview = (_preset_idx>=0&&_preset_idx<(int)builtins.size())
                          ? builtins[_preset_idx].name.c_str() : "──";
    if (ImGui::BeginCombo("##preset", preview)) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
        ImGui::Selectable("── BUILT-IN ──", false, ImGuiSelectableFlags_Disabled);
        ImGui::PopStyleColor();
        for (int i=0;i<(int)builtins.size();++i) {
            bool sel=(i==_preset_idx);
            if (ImGui::Selectable(builtins[i].name.c_str(), sel)) {
                _preset_idx=i; _apply_preset(builtins[i].params, builtins[i].name);
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        if (!sessions.empty()) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, Col::cyan);
            ImGui::Selectable("── SESSION ──",false,ImGuiSelectableFlags_Disabled);
            ImGui::PopStyleColor();
            for (auto& s:sessions)
                if (ImGui::Selectable(("[+] "+s.name).c_str(),false))
                    _apply_preset(s.params,s.name);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::Text("OXIDE:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    const char* oxides[]={"Fe2O3","CrO2","Metal","FeCo"};
    int ox=0; for(int i=0;i<4;++i) if(_ui_params.oxide_type==oxides[i]){ox=i;break;}
    if (ImGui::BeginCombo("##oxide",oxides[ox])) {
        for(int i=0;i<4;++i)
            if(ImGui::Selectable(oxides[i],ox==i))
                {_ui_params.oxide_type=oxides[i]; _sync_params();}
        ImGui::EndCombo();
    }
    ImGui::SameLine(0,16);
    if (_col_button("IMPORT",Col::bg3,Col::cyan,70))  _open_load_preset();
    ImGui::SameLine();
    if (_col_button("EXPORT",Col::bg3,Col::amber,70)) {
        auto& b=_presets.builtin_presets();
        std::string name=(_preset_idx>=0&&_preset_idx<(int)b.size())?b[_preset_idx].name:"Custom";
        _open_save_preset(name);
    }
    ImGui::SameLine();
    if (_col_button("SAVE AS",Col::bg3,Col::purple,72)) {
        _show_save_dialog=true; _save_name_buf[0]='\0';
    }
}

// ── Tabs ──────────────────────────────────────────────────────────────────────
void CapstanApp::_draw_tabs() {
    if (!ImGui::BeginTabBar("##tabs")) return;
    if (ImGui::BeginTabItem("  Transport Mechanics  ")) { _draw_transport_tab(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("  Magnetic Flux  "))       { _draw_magnetic_tab();  ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("  Electronics & Wear  "))  { _draw_electronics_tab(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

void CapstanApp::_draw_transport_tab() {
    ImGui::BeginChild("##ts",{0,0},false); bool c=false;
    c|=_aslider("ips",   "CAPSTAN SPEED (IPS)",       _ui_params.ips_base,       0.5f, 30.f,  Col::amber,"Tape speed in inches/second. ↑↓ keys cycle speeds.");
    c|=_aslider("mh",    "VOLTAGE DRIFT / JITTER",     _ui_params.motor_health,   0.f,  10.f,  Col::amber,"PSU aging: slow irregular speed surges.");
    c|=_aslider("mdrag", "TORQUE LOAD",                _ui_params.motor_drag,     0.f,  0.9f,  Col::amber,"Friction on capstan/reels.");
    c|=_aslider("mboost","MOTOR BOOST",                _ui_params.motor_boost,    0.f,  0.9f,  Col::amber,"Overdrive motor. Combined with drag = conflict.");
    c|=_aslider("wow",   "WOW INTENSITY",              _ui_params.wow_dep,        0.f,  30.f,  Col::amber,"Slow pitch variation from capstan eccentricity.");
    c|=_aslider("flt",   "FLUTTER INTENSITY",          _ui_params.flutter_dep,    0.f,  10.f,  Col::amber,"Fast speed variation from mechanical resonance.");
    c|=_aslider("scr",   "SCRAPE FLUTTER (3kHz)",      _ui_params.scrape_flutter, 0.f,  1.f,   Col::amber,"Rapid modulation from tape sticking on heads.");
    c|=_aslider("tens",  "REEL TENSION DYNAMICS",      _ui_params.tension_load,   0.f,  0.12f, Col::amber,"Tension variation from supply/takeup reels.");
    c|=_aslider("drop",  "OXIDE DROPOUT RATE",         _ui_params.dropout_rate,   0.f,  1.f,   Col::amber,"Random signal gaps from missing oxide.");
    if (c) _sync_params();
    ImGui::EndChild();
}
void CapstanApp::_draw_magnetic_tab() {
    ImGui::BeginChild("##ms",{0,0},false); bool c=false;
    
    // ── Input ──────────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::green);
    ImGui::TextUnformatted("INPUT"); ImGui::PopStyleColor();
    ImGui::Separator();
    c|=_aslider("ingain","INPUT VOLUME",               _ui_params.input_gain,     0.f,  2.f,   Col::green,"Input gain/trim. Reduce for hot sources to prevent clipping.");
    ImGui::Spacing();
    
    c|=_aslider("drv",  "HEAD SATURATION",             _ui_params.drive,          1.f,  20.f,  Col::cyan,"Drive into coating. Higher = warmth then clip.");
    c|=_aslider("bias", "AC BIAS TUNING",              _ui_params.bias,           0.5f, 3.f,   Col::cyan,"Under=bright/distorted. Over=dark/clean.");
    c|=_aslider("rd",   "REPLAY DIFFERENTIATION",      _ui_params.replay_diff,    0.f,  1.f,   Col::cyan,"Head reads flux rate-of-change (+6dB/oct HF).");
    c|=_aslider("asp",  "ASPERITIES (MOD NOISE)",      _ui_params.asperities,     0.f,  0.5f,  Col::cyan,"Signal-correlated grain noise.");
    c|=_aslider("bark", "BARKHAUSEN GRAIN",            _ui_params.barkhausen,     0.f,  0.1f,  Col::cyan,"Discrete domain-switching pulses.");
    c|=_aslider("xtk",  "STEREO CROSSTALK",            _ui_params.crosstalk,      0.f,  0.5f,  Col::cyan,"Inter-track magnetic bleed.");
    c|=_aslider("prt",  "PRINT-THROUGH (GHOST)",       _ui_params.print_through,  0.f,  0.1f,  Col::cyan,"Layer-to-layer imprinting: pre/post echo.");
    c|=_aslider("dmg",  "DEMAGNETISATION (HF LOSS)",   _ui_params.demagnetization,0.f,  0.99f, Col::cyan,"Progressive HF rolloff over time.");
    c|=_aslider("shed", "OXIDE SHEDDING",              _ui_params.oxide_shedding, 0.f,  1.f,   Col::cyan,"Binder degradation: random oxide dropout.");
    if (c) _sync_params();
    ImGui::EndChild();
}
void CapstanApp::_draw_electronics_tab() {
    ImGui::BeginChild("##es",{0,0},false); bool c=false;

    // ── FORMAT (Phase 2) ──────────────────────────────────────────────────────────
    // Picking a canonical format snaps ips, EQ curve, oxide, and bias to its
    // MRL-derived defaults and locks those fields (format_locked = true).
    // Touching any of the coupled knobs detaches (sets format_locked = false)
    // and the label flips to 'Custom'.
    ImGui::PushStyleColor(ImGuiCol_Text, Col::purple);
    ImGui::TextUnformatted("FORMAT");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Text("Tape Format:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300);
    const char* fmt_preview = "── Custom (no coupling) ──";
    if (!_ui_params.format_id.empty()) {
        if (auto* f = tape_format_by_id(_ui_params.format_id)) fmt_preview = f->display_name.c_str();
    }
    if (ImGui::BeginCombo("##format_id", fmt_preview)) {
        if (ImGui::Selectable("── Custom (no coupling) ──", _ui_params.format_id.empty())) {
            _ui_params.format_id = "";
            _ui_params.format_locked = false;
            c = true;
        }
        for (auto& tf : tape_formats()) {
            bool sel = (_ui_params.format_id == tf.id);
            if (ImGui::Selectable(tf.display_name.c_str(), sel)) {
                _ui_params.format_id     = tf.id;
                _ui_params.format_locked = true;
                _ui_params.ips_base      = tf.ips;
                _ui_params.eq_curve      = tf.eq_curve;
                _ui_params.oxide_type    = tf.oxide;
                _ui_params.bias          = tf.bias_recommend;
                c = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (_ui_params.format_locked) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::cyan);
        ImGui::TextUnformatted("  [locked: eq + oxide + ips + bias coupled]");
        ImGui::PopStyleColor();
    } else if (!_ui_params.format_id.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::orange);
        ImGui::TextUnformatted("  [overridden — Custom]");
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();

    // ── EQ CURVE ─────────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::purple);
    ImGui::TextUnformatted("EQUALISATION");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Text("EQ Curve:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo("##eq_curve", eq_curve_name(_ui_params.eq_curve))) {
        for (int i = 0; i <= (int)EQCurve::AES_30; ++i) {
            auto curve = (EQCurve)i;
            bool selected = (_ui_params.eq_curve == curve);
            if (ImGui::Selectable(eq_curve_name(curve), selected)) {
                _ui_params.eq_curve = curve;
                if (!_ui_params.format_id.empty())
                    _ui_params.format_locked = false;
                c = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    // When Eq is Legacy: show the cutoff_base LP slider (audible unchanged path).
    // When Eq is non-Legacy: show LF / HF trim sliders that add on top of the
    // implicit ±3 dB shelf gains baked into the RBJ curve.
    if (_ui_params.eq_curve == EQCurve::Legacy) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
        ImGui::TextUnformatted("(Legacy single-LP path — cutoff_base drives bandwidth)");
        ImGui::PopStyleColor();
        c|=_aslider("cut",  "AZIMUTH CUTOFF (Hz)",      _ui_params.cutoff_base, 500.f,22000.f,Col::purple,"Head gap bandwidth. Legacy single LP path.");
    } else {
        EQSpec spec = eq_spec(_ui_params.eq_curve);
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
        ImGui::Text("Standard: %.0f Hz LF shelf, %.2f kHz HF shelf",
                    1.f / (TWO_PI * spec.lf_tau_s),
                    1.f / (TWO_PI * spec.hf_tau_s) * 1e-3f);
        ImGui::PopStyleColor();
        c|=_aslider("lftrim","LF TRIM (dB)",            _ui_params.lf_trim_db, -6.f, 6.f, Col::purple,
                    "Additional LF shelf gain on top of the standard curve's LF shelf gain.");
        c|=_aslider("hftrim","HF TRIM (dB)",            _ui_params.hf_trim_db, -6.f, 6.f, Col::purple,
                    "Additional HF shelf gain on top of the standard curve's HF shelf gain.");
    }
    ImGui::Spacing();

    c|=_aslider("hiss",   "NOISE FLOOR",               _ui_params.hiss,          0.f,  0.02f,  Col::purple,"Broadband white noise from preamp/oxide.");
    c|=_aslider("hcol",   "HISS COLOUR (PINK TILT)",   _ui_params.hiss_color,    0.f,  1.f,    Col::purple,"1/f noise colouring from preamp transistors.");
    c|=_aslider("hum",    "60Hz MAINS HUM",            _ui_params.mains_hum,     0.f,  0.05f,  Col::purple,"AC supply at 60/120/180/240 Hz.");
    c|=_aslider("bump",   "HEAD BUMP (LF EQ)",         _ui_params.head_bump,     0.f,  5.f,    Col::purple,"Head resonance boosting 50-200 Hz.");
    c|=_aslider("azdrift","AZIMUTH PHASE DRIFT",       _ui_params.azimuth_drift, 0.f,  1.f,    Col::purple,"Head angle error: HF phase diff between channels.");
    c|=_aslider("sticky", "STICKY SHED INTENSITY",     _ui_params.sticky_shed,   0.f,  1.f,    Col::purple,"Binder absorption: squeal, drag, HF loss.");
    if (c) _sync_params();
    ImGui::EndChild();
}

// ── Transport controls — always-visible bottom strip ─────────────────────────
void CapstanApp::_draw_transport_controls() {
    bool playing   = _engine.is_playing.load();
    bool rewinding = _audio.is_rewinding();
    bool ffing     = _audio.is_ffing();
    bool reversed  = _engine.is_reversed;

    // Button sizing — tall, wide, tape-machine style
    const float BTN_H  = 60.f;
    const float BTN_W  = 115.f;
    const float SH_W   = 95.f;   // shuttle buttons
    const float STOP_W = 72.f;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {6.f, 10.f});

    // ── Row 1: main transport ─────────────────────────────────────────────────
    // REWIND  |  PLAY  |  STOP  |  REVERSE  |  FF  |  [render progress]
    {
        // REWIND
        if (rewinding) {
            float speed = _audio.shuttle_speed();
            char lb[24]; std::snprintf(lb,sizeof(lb),"<< %.0fX", speed);
            ImVec4 flash = (std::fmod(ImGui::GetTime()*3.f,1.f)>.5f) ? Col::cyan : Col::cyan_dim;
            if (_transport_btn(lb, Col::cyan_dim, flash, SH_W, BTN_H))
                _audio.stop();
        } else {
            if (_transport_btn("<<\nREWIND", Col::bg4, Col::cyan, SH_W, BTN_H))
                { _stop_transport(); _toggle_rewind(); }
        }
        ImGui::SameLine(0,4);

        // PLAY
        {
            bool active = playing && !reversed;
            ImVec4 bg   = active ? blend(Col::amber_dim,Col::amber,.5f) : Col::bg4;
            ImVec4 fg   = active ? Col::amber : Col::white;
            const char* lbl = active ? ">\nPLAYING" : ">\nPLAY";
            if (_transport_btn(lbl, bg, fg, BTN_W, BTN_H)) _start_forward();
        }
        ImGui::SameLine(0,4);

        // STOP
        {
            bool active = !playing && !rewinding && !ffing;
            ImVec4 bg   = active ? blend(Col::red_dim,Col::red,.4f) : Col::bg4;
            if (_transport_btn("[ ]\nSTOP", bg, active?Col::red:Col::white, STOP_W, BTN_H))
                _stop_transport();
        }
        ImGui::SameLine(0,4);

        // REVERSE
        {
            bool active = playing && reversed;
            ImVec4 bg   = active ? blend(Col::orange_dim,Col::orange,.5f) : Col::bg4;
            ImVec4 fg   = active ? Col::orange : Col::white;
            const char* lbl = active ? "<\nREVERSING" : "<\nREVERSE";
            if (_transport_btn(lbl, bg, fg, BTN_W, BTN_H)) _start_reverse();
        }
        ImGui::SameLine(0,4);

        // FF
        if (ffing) {
            float speed = _audio.shuttle_speed();
            char lb[24]; std::snprintf(lb,sizeof(lb),">> %.0fX", speed);
            ImVec4 flash = (std::fmod(ImGui::GetTime()*3.f,1.f)>.5f) ? Col::green : Col::green_dim;
            if (_transport_btn(lb, Col::green_dim, flash, SH_W, BTN_H))
                _audio.stop();
        } else {
            if (_transport_btn(">>\nFF", Col::bg4, Col::green, SH_W, BTN_H))
                { _stop_transport(); _toggle_ff(); }
        }
        ImGui::SameLine(0,12);

        // Render progress (when running)
        {
            auto st = _renderer.get_status();
            if (_renderer.is_running()) {
                char lb[64];
                std::snprintf(lb,sizeof(lb),"%s  %.0f%%  %.1fx",
                              st.phase.c_str(), st.job_progress*100.f, st.xrt);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col::purple_dim);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg3);
                ImGui::ProgressBar(st.job_progress,{220,BTN_H},lb);
                ImGui::PopStyleColor(2);
            }
        }
    }

    ImGui::PopStyleVar(2);

    // ── Row 2: tool buttons + shortcut bar ────────────────────────────────────
    // Thin separator line
    ImGui::Spacing();
    {
        const float row_h = 26.f;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {6.f, 3.f});

        // TIMELINE button
        {
            bool on = _timeline.open;
            ImGui::PushStyleColor(ImGuiCol_Button,
                on ? ImVec4(.0f,.28f,.12f,.9f) : ImVec4(.12f,.12f,.16f,.9f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                on ? ImVec4(.0f,.38f,.18f,1.f) : ImVec4(.18f,.18f,.24f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                on ? ImVec4(.35f,1.f,.55f,1.f) : ImVec4(.6f,.6f,.68f,1.f));
            ImGui::SetNextItemWidth(90.f);
            if (ImGui::Button(on ? "TIMELINE ##tl" : "TIMELINE ##tl", {90.f, row_h}))
                _timeline.open = !_timeline.open;
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine(0, 4);

        // RENDER button
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.18f,.08f,.28f,.9f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.28f,.12f,.42f,1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(.75f,.45f,1.f,1.f));
        if (ImGui::Button("RENDER ##rnd", {80.f, row_h}))
            _show_render_dialog = !_show_render_dialog;
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, 4);

        // OPTIONS button — opens side panel at same position as RENDER
        {
            bool on = _show_options;
            ImGui::PushStyleColor(ImGuiCol_Button,
                on ? ImVec4(.06f,.18f,.32f,.95f) : ImVec4(.10f,.10f,.18f,.90f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.10f,.25f,.44f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                on ? ImVec4(.4f,.8f,1.f,1.f) : ImVec4(.55f,.60f,.75f,1.f));
            if (ImGui::Button("OPTIONS##row2", {84.f, row_h}))
                _show_options = !_show_options;
            ImGui::PopStyleColor(3);
        }

        ImGui::PopStyleVar(2);
    }

    // ── Shortcut footer ───────────────────────────────────────────────────────
    {
        auto* dl    = ImGui::GetWindowDrawList();
        float  ww   = ImGui::GetWindowWidth();
        ImVec2 wpos = ImGui::GetWindowPos();
        float  cy   = ImGui::GetCursorScreenPos().y + 2.f;
        ImFont* font  = ImGui::GetFont();
        float   fs    = ImGui::GetFontSize() * 0.90f;
        float   pad_x = 5.f, pad_y = 1.f;
        float   kh    = font->CalcTextSizeA(fs,FLT_MAX,0.f,"X").y + pad_y*2.f;
        float   bar_h = kh + 6.f;

        dl->AddRectFilled({wpos.x, cy - 2.f}, {wpos.x + ww, cy + bar_h},
                          IM_COL32(14, 14, 22, 255));
        dl->AddLine({wpos.x, cy - 2.f}, {wpos.x + ww, cy - 2.f},
                    IM_COL32(50, 50, 68, 255), 1.f);

        struct KV { const char* key; const char* desc; };
        static const KV shortcuts[] = {
            {Keys::UI::PLAY_TOGGLE,     "play/stop"},
            {Keys::UI::STOP,            "stop"},
            {Keys::UI::PLAY_REV_ALT,    "reverse"},
            {Keys::UI::SHUTTLE_REV,     "rwd"},
            {Keys::UI::SHUTTLE_FWD,     "ff"},
            {Keys::UI::OPEN_AUDIO,      "load"},
            {Keys::UI::SAVE_PROJECT,    "save"},
            {Keys::UI::SAVE_PROJECT_AS, "save as"},
            {Keys::UI::OPEN_PROJECT,    "open proj"},
        };

        float x = wpos.x + 8.f, y = cy + 2.f;
        for (auto& s : shortcuts) {
            ImVec2 ksz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, s.key);
            ImVec2 dsz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, s.desc);
            float  kw  = ksz.x + pad_x * 2.f;
            if (x + kw + 4.f + dsz.x + 14.f > wpos.x + ww - 8.f) break;
            dl->AddRectFilled({x, y+pad_y-1.f},{x+kw, y+kh},IM_COL32(36,36,54,255),3.f);
            dl->AddRect({x, y+pad_y-1.f},{x+kw, y+kh},IM_COL32(85,85,115,200),3.f,0,1.f);
            dl->AddText(font,fs,{x+pad_x,y+pad_y},IM_COL32(210,215,240,255),s.key);
            x += kw + 4.f;
            dl->AddText(font,fs,{x,y+pad_y},IM_COL32(125,123,140,255),s.desc);
            x += dsz.x + 14.f;
        }
        ImGui::SetCursorScreenPos({wpos.x, cy});
        ImGui::Dummy({ww, bar_h + 2.f});
    }
}

// ── Transport button helper ───────────────────────────────────────────────────
bool CapstanApp::_transport_btn(const char* label, const ImVec4& bg,
                               const ImVec4& fg, float w, float h)
{
    ImVec4 hov = {std::min(bg.x*1.5f+.05f,1.f),
                  std::min(bg.y*1.5f+.05f,1.f),
                  std::min(bg.z*1.5f+.05f,1.f), 1.f};
    ImVec4 act = {bg.x*.6f, bg.y*.6f, bg.z*.6f, 1.f};

    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Invisible button for hit-test — ID uses a hash of label+size, safe length
    char idbuf[64];
    std::snprintf(idbuf, sizeof(idbuf), "##tb%.0f%.0f", w, h);
    // Mix in first 8 chars of label so same-size buttons get different IDs
    for (int i = 0; i < 8 && label[i]; ++i)
        idbuf[4 + i] = (label[i] == '\n') ? '_' : label[i];

    ImGui::PushStyleColor(ImGuiCol_Button,        bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
    bool pressed = ImGui::Button(idbuf, {w, h});
    ImGui::PopStyleColor(3);

    // Overlay text using draw list — no SetWindowFontScale (corrupts layout)
    // Symbol line rendered at 1.5× by passing explicit size to AddText
    auto* dl = ImGui::GetWindowDrawList();
    ImU32 fg_col  = ImGui::ColorConvertFloat4ToU32(fg);
    ImU32 dim_col = IM_COL32((int)(fg.x*180),(int)(fg.y*180),(int)(fg.z*180),210);
    ImFont* font  = ImGui::GetFont();
    float   base  = ImGui::GetFontSize();   // snapshot BEFORE any scale call
    float   big   = base * 2.0f;
    float   small = base * 0.88f;

    std::string lbl(label);
    auto nl = lbl.find('\n');

    if (nl == std::string::npos) {
        // Single line centred
        ImVec2 ts = font->CalcTextSizeA(base, FLT_MAX, 0.f, lbl.c_str());
        dl->AddText(font, base, {pos.x+(w-ts.x)*.5f, pos.y+(h-ts.y)*.5f}, fg_col, lbl.c_str());
    } else {
        std::string sym  = lbl.substr(0, nl);
        std::string name = lbl.substr(nl+1);

        ImVec2 ts_sym  = font->CalcTextSizeA(big,   FLT_MAX, 0.f, sym.c_str());
        ImVec2 ts_name = font->CalcTextSizeA(small, FLT_MAX, 0.f, name.c_str());

        float gap     = 3.f;
        float total_h = ts_sym.y + gap + ts_name.y;
        float y0      = pos.y + (h - total_h) * 0.5f;

        dl->AddText(font, big,
                    {pos.x + (w - ts_sym.x)  * 0.5f, y0},
                    fg_col, sym.c_str());
        dl->AddText(font, small,
                    {pos.x + (w - ts_name.x) * 0.5f, y0 + ts_sym.y + gap},
                    dim_col, name.c_str());
    }

    return pressed;
}

// ── 180° Squared VU Meter ─────────────────────────────────────────────────────
// Draws a semicircular VU meter with squared edges
// Parameters:
//   center: center point of the semicircle (bottom-center of the arc)
//   radius: outer radius of the meter
//   level: audio level 0.0-1.0 (linear, not dB)
//   label: channel label ("L" or "R")
void CapstanApp::_draw_vu_meter(const ImVec2& center, float radius, float level, const char* label) {
    auto* dl = ImGui::GetWindowDrawList();

    // Clamp level and convert to angle (0 to PI for 180 degrees)
    float clamped = std::clamp(level, 0.f, 1.f);
    // Apply logarithmic scaling for more realistic VU response (-60dB to 0dB)
    float log_level = (clamped > 0.001f) ? (1.f + std::log10(clamped) / 3.f) : 0.f;
    log_level = std::clamp(log_level, 0.f, 1.f);
    // float angle = log_level * PI;  // 0 to 180 degrees in radians (unused, kept for reference)

    // Scale thickness proportionally to radius (thicker for larger meters)
    const float thickness = radius * 0.22f;
    const float inner_r = radius - thickness;

    // Background arc (full 180 degrees) - dark
    const int segments = 32;
    const float start_angle = PI;  // Start from left (180 degrees)

    // Draw background arc (squared/rectangular style)
    for (int i = 0; i < segments; ++i) {
        float a0 = start_angle - (float)i * PI / segments;
        float a1 = start_angle - (float)(i + 1) * PI / segments;

        // Outer points
        ImVec2 o0(center.x + std::cos(a0) * radius, center.y - std::sin(a0) * radius);
        ImVec2 o1(center.x + std::cos(a1) * radius, center.y - std::sin(a1) * radius);
        // Inner points
        ImVec2 i0(center.x + std::cos(a0) * inner_r, center.y - std::sin(a0) * inner_r);
        ImVec2 i1(center.x + std::cos(a1) * inner_r, center.y - std::sin(a1) * inner_r);

        dl->AddQuad(o0, o1, i1, i0, IM_COL32(40, 40, 50, 200));
    }

    // Draw filled portion (active level) with color gradient
    int filled_segments = (int)(segments * log_level);
    if (filled_segments > 0) {
        for (int i = 0; i < filled_segments; ++i) {
            float a0 = start_angle - (float)i * PI / segments;
            float a1 = start_angle - (float)(i + 1) * PI / segments;

            ImVec2 o0(center.x + std::cos(a0) * radius, center.y - std::sin(a0) * radius);
            ImVec2 o1(center.x + std::cos(a1) * radius, center.y - std::sin(a1) * radius);
            ImVec2 i0(center.x + std::cos(a0) * inner_r, center.y - std::sin(a0) * inner_r);
            ImVec2 i1(center.x + std::cos(a1) * inner_r, center.y - std::sin(a1) * inner_r);

            // Color gradient: green -> yellow -> red
            float t = (float)i / segments;
            ImU32 col;
            if (t < 0.5f) {
                // Green to yellow
                float s = t * 2.f;
                col = IM_COL32((int)(100 + 155 * s), 255, 50, 255);
            } else if (t < 0.75f) {
                // Yellow to orange
                float s = (t - 0.5f) * 4.f;
                col = IM_COL32(255, (int)(255 - 100 * s), 50, 255);
            } else {
                // Orange to red
                float s = (t - 0.75f) * 4.f;
                col = IM_COL32(255, (int)(155 - 105 * s), 50, 255);
            }

            dl->AddQuadFilled(o0, o1, i1, i0, col);
        }
    }

    // Draw needle/hand pointer (gauge style - triangle shaped)
    float needle_angle = start_angle - log_level * PI;  // Point to current level
    float needle_len = radius * 0.85f;
    float needle_x = std::cos(needle_angle) * needle_len;
    float needle_y = -std::sin(needle_angle) * needle_len;
    ImVec2 needle_tip(center.x + needle_x, center.y + needle_y);
    
    // Calculate perpendicular direction for triangle width
    float perp_angle = needle_angle - PI / 2.f;
    float tri_half_width = radius * 0.08f;  // Triangle width at base
    float perp_x = std::cos(perp_angle) * tri_half_width;
    float perp_y = -std::sin(perp_angle) * tri_half_width;
    
    // Triangle base points (at center pivot edge)
    float base_dist = inner_r * 0.35f;
    float base_x = std::cos(needle_angle) * base_dist;
    float base_y = -std::sin(needle_angle) * base_dist;
    ImVec2 base_center(center.x + base_x, center.y + base_y);
    ImVec2 base_left(base_center.x + perp_x, base_center.y + perp_y);
    ImVec2 base_right(base_center.x - perp_x, base_center.y - perp_y);
    
    // Needle shadow (slightly offset triangle)
    dl->AddTriangleFilled(
        {needle_tip.x + 1.5f, needle_tip.y + 1.5f},
        {base_left.x + 1.5f, base_left.y + 1.5f},
        {base_right.x + 1.5f, base_right.y + 1.5f},
        IM_COL32(0, 0, 0, 80));
    
    // Needle triangle (red with gradient effect via outline)
    dl->AddTriangleFilled(needle_tip, base_left, base_right, IM_COL32(255, 90, 90, 255));
    dl->AddTriangle(needle_tip, base_left, base_right, IM_COL32(200, 40, 40, 255), 1.5f);
    
    // Needle tip circle (smaller, integrated with triangle)
    dl->AddCircleFilled(needle_tip, 2.5f, IM_COL32(255, 120, 120, 255));

    // Draw center pivot circle (scaled proportionally) - on top of needle base
    dl->AddCircleFilled(center, inner_r * 0.55f, IM_COL32(60, 60, 70, 255));
    dl->AddCircle(center, inner_r * 0.55f, IM_COL32(100, 100, 120, 255), 0, 3.f);
    
    // Center pivot highlight
    dl->AddCircleFilled(center, inner_r * 0.25f, IM_COL32(80, 80, 95, 255));

    // Draw channel label (larger font for bigger meters)
    ImFont* font = ImGui::GetFont();
    float fs = radius * 0.5f;  // Scale font with meter size
    ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, label);
    dl->AddText(font, fs, {center.x - ts.x * 0.5f, center.y - ts.y * 0.5f},
                IM_COL32(220, 220, 230, 255), label);
}

// ── Render dialog ─────────────────────────────────────────────────────────────
void CapstanApp::_draw_render_dialog() {
    // Pin to right side of screen, same level as main window content
    {
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        float w = std::min(780.f, vp.x * 0.55f);
        float h = vp.y - 32.f;  // full height minus OS taskbar margin
        ImGui::SetNextWindowSize({w, h}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({vp.x - w, 0.f}, ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha(0.97f);
    if (!ImGui::Begin("RENDER / QUEUE", &_show_render_dialog,
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize)) {
        ImGui::End(); return; }

    float lpw = 400.f;
    ImGui::BeginChild("##render_settings",{lpw,0},false);
    ImGui::PushStyleColor(ImGuiCol_Text, Col::purple);
    ImGui::Text("NEW RENDER JOB"); ImGui::PopStyleColor(); ImGui::Separator();

    int sr=_render_opts.sample_rate;
    ImGui::Text("Sample Rate:");
    if(ImGui::RadioButton("44.1k",sr==44100)){_render_opts.sample_rate=44100;} ImGui::SameLine();
    if(ImGui::RadioButton("48k",  sr==48000)){_render_opts.sample_rate=48000;} ImGui::SameLine();
    if(ImGui::RadioButton("88.2k",sr==88200)){_render_opts.sample_rate=88200;} ImGui::SameLine();
    if(ImGui::RadioButton("96k",  sr==96000)){_render_opts.sample_rate=96000;}
    int bd=_render_opts.bit_depth;
    ImGui::Text("Bit Depth:");
    if(ImGui::RadioButton("16", bd==16)){_render_opts.bit_depth=16;} ImGui::SameLine();
    if(ImGui::RadioButton("24", bd==24)){_render_opts.bit_depth=24;} ImGui::SameLine();
    if(ImGui::RadioButton("32f",bd==32)){_render_opts.bit_depth=32;}
    ImGui::Separator();
    ImGui::Text("Quality:");
    const char* ql[]={"Draft","Standard","High","Ultra","2xOS","4xOS","8xOS"};
    const char* qd[]={"4096-blk no bark","2048-blk bark","1024-blk full","512-blk full","1024 2× OS","1024 4× OS","512 8× OS"};
    int qi=(int)_render_opts.quality;
    for(int i=0;i<7;++i){
        if(i%4)ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,(qi==i)?Col::amber:Col::grey_lt);
        if(ImGui::RadioButton(ql[i],qi==i))_render_opts.quality=(SimQuality)i;
        ImGui::PopStyleColor();
        if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",qd[i]);
    }
    ImGui::Separator();
    bool fwd=!_render_opts.reverse;
    if(ImGui::RadioButton("Forward",fwd)){_render_opts.reverse=false;} ImGui::SameLine();
    if(ImGui::RadioButton("Reverse",!fwd)){_render_opts.reverse=true;}
    ImGui::Separator();
    ImGui::Checkbox("TPDF Dither",&_render_opts.dither); ImGui::SameLine();
    ImGui::Checkbox("DC Block",&_render_opts.dc_block); ImGui::SameLine();
    ImGui::Checkbox("Normalize",&_render_opts.normalize);
    ImGui::Checkbox("RMS Match",&_render_opts.match_loudness);
    ImGui::Separator();
    ImGui::Text("Pre-roll:");
    const float pre[]={0,.5f,1,2,3,5};
    for(int i=0;i<6;++i){
        if(i)ImGui::SameLine();
        char b[10];std::snprintf(b,10,i==0?"None":"%.1fs",pre[i]);
        if(ImGui::RadioButton(b,std::abs(_render_opts.preroll_s-pre[i])<.01f))
            _render_opts.preroll_s=pre[i];
    }
    ImGui::Separator();
    ImGui::Text("Threads:");
    for(int t:{1,2,4,8}){
        if(t>1)ImGui::SameLine();
        char b[4];std::snprintf(b,4,"%d",t);
        if(ImGui::RadioButton(b,_render_opts.threads==t))_render_opts.threads=t;
    }
    ImGui::Separator();
    if(_col_button("CLOSE",Col::bg3,Col::grey_lt,90))_show_render_dialog=false;
    ImGui::SameLine();
    if(_col_button("ADD TO QUEUE",Col::purple_dim,Col::purple,140))_open_save_render();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##render_queue",{0,0},false);
    _draw_render_queue();
    ImGui::EndChild();
    ImGui::End();
}

void CapstanApp::_draw_render_queue() {
    auto st = _renderer.get_status();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
    ImGui::Text("RENDER QUEUE & STATUS"); ImGui::PopStyleColor(); ImGui::Separator();

    if (_renderer.is_running()) {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::cyan);
        ImGui::Text("RUNNING: %s", st.display_name.c_str()); ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
        ImGui::Text("Phase    : %s", st.phase.c_str());
        ImGui::Text("Quality  : %dx OS  Threads: %d  %s",
                    st.oversample, st.threads, st.reverse?"REVERSE":"FORWARD");
        ImGui::Text("Output   : %d Hz / %d-bit", st.sr_out, st.bit_depth);
        ImGui::Text("Progress : %d/%d blk  %dk/%dk samp",
                    st.blocks_done,st.total_blocks,
                    st.samples_done/1000,st.total_samples_job/1000);
        ImGui::Text("Elapsed  : %.1fs  ETA: %.1fs  %.2fx RT",
                    st.elapsed_s, st.eta_s, st.xrt);
        ImGui::PopStyleColor();
        char pb[64]; std::snprintf(pb,sizeof(pb),"%.1f%%  %s  %.1fx",
                                   st.job_progress*100.f,st.phase.c_str(),st.xrt);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col::purple);
        ImGui::ProgressBar(st.job_progress,{-1,0},pb); ImGui::PopStyleColor();
        if(st.total_jobs>1){
            char ob[64];std::snprintf(ob,sizeof(ob),"Overall: %d/%d",st.job_index+1,st.total_jobs);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col::purple_dim);
            ImGui::ProgressBar(st.overall_progress,{-1,0},ob); ImGui::PopStyleColor();
        }
        ImGui::Separator();
        if(_col_button("CANCEL JOB",Col::red_dim,Col::red,110))_renderer.cancel_current();
        ImGui::SameLine();
        if(_col_button("CANCEL ALL",Col::red_dim,Col::orange,110))_renderer.cancel_all();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
        ImGui::TextUnformatted("No render in progress."); ImGui::PopStyleColor();
    }

    if (!st.queued_names.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
        ImGui::Text("QUEUED (%d)",(int)st.queued_names.size()); ImGui::PopStyleColor();
        for(int i=0;i<(int)st.queued_names.size();++i){
            ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
            ImGui::Text("  %d. %s",i+1,st.queued_names[i].c_str()); ImGui::PopStyleColor();
        }
    }

    if (!st.completed.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
        ImGui::TextUnformatted("COMPLETED THIS SESSION:"); ImGui::PopStyleColor();
        ImGui::BeginChild("##completed",{0,140},true);
        for(auto it=st.completed.rbegin();it!=st.completed.rend();++it){
            ImGui::PushStyleColor(ImGuiCol_Text, it->ok?Col::green:Col::red);
            ImGui::Text("%s  %s",it->ok?"[OK]":"[!!]",it->name.c_str());
            ImGui::PopStyleColor();
            if(!it->message.empty()&&ImGui::IsItemHovered())
                ImGui::SetTooltip("%s",it->message.c_str());
        }
        ImGui::EndChild();
    }
}

// ── Save preset dialog ────────────────────────────────────────────────────────
void CapstanApp::_draw_save_dialog() {
    ImGui::SetNextWindowSize({380,140},ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver,{0.5f,0.5f});
    if (!ImGui::Begin("Save Preset As",&_show_save_dialog,
                      ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End(); return;
    }
    ImGui::Text("Preset name:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##savename",_save_name_buf,sizeof(_save_name_buf));
    ImGui::Separator();
    if(_col_button("SAVE",Col::purple_dim,Col::purple,80)){
        std::string name(_save_name_buf);
        if(!name.empty()){_presets.save_session(name,_ui_params);_show_save_dialog=false;}
    }
    ImGui::SameLine();
    if(_col_button("CANCEL",Col::bg3,Col::grey_lt,80))_show_save_dialog=false;
    ImGui::End();
}

// ── Widget helpers ────────────────────────────────────────────────────────────
// Custom drawn parameter row:
//   [ LABEL            ][=====|              ][ VALUE ]
//   Left: label text   Middle: fill-bar       Right: numeric, click-editable
//
// The bar is drawn on the ImDrawList for full colour control.
// An invisible ImGui::SliderFloat on top provides drag + scroll interaction.
// Animation support: when has_keys, bar glows green and a keyframe dot appears.

static void _draw_param_row(
    ImDrawList* dl,
    ImVec2 row_pos, float row_w, float row_h,
    const char* label, const char* val_str,
    float norm,
    const ImVec4& accent,
    bool has_keys, bool is_animated,
    bool hovered_slider)
{
    // Two-line layout within row_h:
    //  y0 → label (left) + value (right)
    //  y1 → thin track bar spanning full width (with margins)
    //  y2 → 2px gap (separator)
    ImFont* font  = ImGui::GetFont();
    float   fs    = ImGui::GetFontSize();
    float   fs_lbl = fs * 0.92f;   // label font size
    float   pad   = 6.f;

    float text_y  = row_pos.y + 2.f;
    float track_y = row_pos.y + row_h - 9.f;  // track near bottom of row
    float track_x0 = row_pos.x + pad;
    float track_x1 = row_pos.x + row_w - pad;
    float track_w  = track_x1 - track_x0;
    float clamp_norm = std::clamp(norm, 0.f, 1.f);

    // ── Label ─────────────────────────────────────────────────────────────────
    ImU32 lbl_col = has_keys
        ? IM_COL32(100, 255, 140, 220)
        : (hovered_slider ? IM_COL32(200,200,220,230) : IM_COL32(160,158,180,190));

    // Reserve space for value on the right so label doesn't overlap it
    ImVec2 vsz   = font->CalcTextSizeA(fs_lbl, FLT_MAX, 0.f, val_str);
    float  val_x = row_pos.x + row_w - vsz.x - pad;

    // Clip label so it never runs into the value
    dl->PushClipRect(row_pos, {val_x - 4.f, row_pos.y + row_h}, true);
    // Keyframe dot inline with label
    float text_draw_x = row_pos.x + pad;
    if (has_keys) {
        float dot_cy = text_y + fs_lbl * 0.5f;
        ImU32 dot_c  = is_animated ? IM_COL32(60,255,110,255) : IM_COL32(60,200,90,180);
        dl->AddCircleFilled({text_draw_x + 3.f, dot_cy}, 3.f, dot_c);
        text_draw_x += 10.f;
    }
    dl->AddText(font, fs_lbl, {text_draw_x, text_y}, lbl_col, label);
    dl->PopClipRect();

    // ── Value (right-aligned, accent colour) ──────────────────────────────────
    ImU32 val_col = has_keys
        ? IM_COL32(80, 255, 130, 255)
        : ImGui::ColorConvertFloat4ToU32(
            hovered_slider ? accent : ImVec4(accent.x*.85f,accent.y*.85f,accent.z*.85f,0.85f));
    dl->AddText(font, fs_lbl, {val_x, text_y}, val_col, val_str);

    // ── Track: thin line ──────────────────────────────────────────────────────
    float ty = track_y + 1.5f;   // centre of 3px track

    // Full track — very dim
    ImU32 track_bg = has_keys
        ? IM_COL32(15, 50, 22, 200)
        : IM_COL32(40, 40, 55, 180);
    dl->AddLine({track_x0, ty}, {track_x1, ty}, track_bg, 3.f);

    // Filled portion — accent-tinted, moderate brightness
    float fill_x = track_x0 + track_w * clamp_norm;
    if (fill_x > track_x0 + 1.f) {
        ImU32 fill_c;
        if (has_keys) {
            fill_c = is_animated ? IM_COL32(50,210,95,230) : IM_COL32(40,170,75,200);
        } else {
            // Accent at ~55% brightness so it's clearly visible but not glaring
            fill_c = IM_COL32(
                (int)(accent.x * 155 + 20),
                (int)(accent.y * 100 + 10),
                (int)(accent.z * 40),
                210);
        }
        dl->AddLine({track_x0, ty}, {fill_x, ty}, fill_c, 3.f);
    }

    // ── Position dot ──────────────────────────────────────────────────────────
    {
        float dot_r  = hovered_slider ? 5.f : 4.f;
        ImU32 dot_c  = has_keys
            ? (is_animated ? IM_COL32(80,255,120,255) : IM_COL32(60,210,90,220))
            : ImGui::ColorConvertFloat4ToU32(
                hovered_slider ? accent
                               : ImVec4(accent.x,accent.y*.85f,accent.z*.4f,0.9f));
        // Subtle glow ring when hovered
        if (hovered_slider)
            dl->AddCircleFilled({fill_x, ty}, dot_r + 3.f,
                IM_COL32((int)(accent.x*80),(int)(accent.y*60),(int)(accent.z*20),60));
        dl->AddCircleFilled({fill_x, ty}, dot_r, dot_c);
    }

    // ── Row separator — barely visible ────────────────────────────────────────
    dl->AddLine(
        {row_pos.x, row_pos.y + row_h - 1.f},
        {row_pos.x + row_w, row_pos.y + row_h - 1.f},
        IM_COL32(28, 28, 40, 180));
}

bool CapstanApp::_slider(const char* id, const char* label,
                       float& value, float mn, float mx,
                       const ImVec4& accent, const char* tooltip)
{
    const float ROW_H = 32.f;
    float w = ImGui::GetContentRegionAvail().x;
    float bar_w = w;
    float norm  = (mx > mn) ? (value - mn) / (mx - mn) : 0.f;

    ImVec2 row_pos = ImGui::GetCursorScreenPos();
    // Invisible slider on the bar region
    ImGui::SetCursorScreenPos({row_pos.x, row_pos.y});
    ImGui::SetNextItemWidth(bar_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         {0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       {0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0,0,0,0});
    bool changed = ImGui::SliderFloat(("##sl_"+std::string(id)).c_str(),
                                      &value, mn, mx, "");
    bool hov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    if (tooltip && hov) ImGui::SetTooltip("%s", tooltip);
    // Reset cursor to row start for drawing
    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy({w, ROW_H});

    char vbuf[24]; std::snprintf(vbuf, sizeof(vbuf), "%.4g", (double)value);
    _draw_param_row(ImGui::GetWindowDrawList(), row_pos, w, ROW_H,
                    label, vbuf, norm, accent, false, false, hov);
    return changed;
}

bool CapstanApp::_aslider(const char* id, const char* label,
                           float& value, float mn, float mx,
                           const ImVec4& accent, const char* tooltip)
{
    bool has_keys   = _anim.has_keys(id);
    bool is_playing = _engine.is_playing.load();
    bool is_animated = has_keys && _anim.enabled && is_playing;

    // Evaluate animated display value
    float show_val = value;
    if (is_animated) {
        auto it = _anim.curves.find(id);
        if (it != _anim.curves.end())
            if (auto v = it->second.evaluate(_engine.play_head)) show_val = *v;
    }

    const float ROW_H = 32.f;
    float w     = ImGui::GetContentRegionAvail().x;
    float bar_w = w;
    float norm  = (mx > mn) ? (show_val - mn) / (mx - mn) : 0.f;

    ImVec2 row_pos = ImGui::GetCursorScreenPos();

    // Invisible slider
    ImGui::SetCursorScreenPos({row_pos.x, row_pos.y});
    ImGui::SetNextItemWidth(bar_w);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         {0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       {0,0,0,0});
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {0,0,0,0});
    bool changed = ImGui::SliderFloat(("##asl_"+std::string(id)).c_str(),
                                      &show_val, mn, mx, "");
    if (changed) value = show_val;
    bool hov = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    if (tooltip && hov) ImGui::SetTooltip("%s  [I = keyframe]", tooltip);

    // I key = insert keyframe
    if (hov && ImGui::IsKeyPressed(ImGuiKey_I)) {
        double t = is_playing ? _engine.play_head : _anim_cursor;
        _anim.insert_key(id, label, t, value);
        _timeline.open = true;
    }

    // Reset cursor for visual draw
    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy({w, ROW_H});

    char vbuf[24]; std::snprintf(vbuf, sizeof(vbuf), "%.4g", (double)show_val);
    _draw_param_row(ImGui::GetWindowDrawList(), row_pos, w, ROW_H,
                    label, vbuf, norm, accent, has_keys, is_animated, hov);
    return changed;
}


void CapstanApp::_draw_options() {
    // Same position as render dialog — they share the right panel slot
    {
        ImVec2 vp = ImGui::GetMainViewport()->Size;
        float w = std::min(520.f, vp.x * 0.38f);
        float h = vp.y - 32.f;
        ImGui::SetNextWindowSize({w, h}, ImGuiCond_Always);
        ImGui::SetNextWindowPos({vp.x - w, 0.f}, ImGuiCond_Always);
    }
    ImGui::SetNextWindowBgAlpha(0.97f);
    if (!ImGui::Begin("Performance Options", &_show_options,
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
                      | ImGuiWindowFlags_NoResize)) {
        ImGui::End(); return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
    ImGui::TextWrapped(
        "These settings control how much audio is buffered in RAM. "
        "Larger buffers tolerate slower drives. Changes take effect on next file load.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ── Quick presets ─────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
    ImGui::TextUnformatted("QUICK PRESETS"); ImGui::PopStyleColor();
    ImGui::Separator();

    auto preset_btn = [&](const char* lbl, const char* tip, auto fn) {
        if (_col_button(lbl, Col::bg3, Col::cyan, 110)) { fn(); _perf.save(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, 6);
    };
    preset_btn("Fast SSD",    "Low latency, minimal buffer, multi-core render",
               [&]{ _perf.apply_preset_fast_ssd(); });
    preset_btn("Default",     "Balanced settings for most systems",
               [&]{ _perf.apply_preset_default(); });
    preset_btn("Slow HDD",    "Large buffers, sequential reads, avoids stutters on spinning drives",
               [&]{ _perf.apply_preset_slow_hdd(); });
    preset_btn("Low RAM",     "Minimal ring buffer, uses less memory at cost of seek latency",
               [&]{ _perf.apply_preset_low_ram(); });
    ImGui::NewLine();
    ImGui::Spacing();

    // ── Stream buffer ─────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
    ImGui::TextUnformatted("STREAM BUFFER"); ImGui::PopStyleColor();
    ImGui::Separator();

    bool changed = false;

    ImGui::Text("Ring buffer size:  %.1f MB  (%d s)", _perf.ring_mb(), _perf.ring_seconds);
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderInt("##ring", &_perf.ring_seconds, 5, 120, "%d s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Total audio held in RAM. One minute stereo 44.1kHz ≈ 21 MB.");

    ImGui::Spacing();
    ImGui::Text("Read-ahead:  %d s", _perf.ahead_seconds);
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderInt("##ahead", &_perf.ahead_seconds, 2, 60, "%d s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "How far ahead the IO thread reads. Increase if you hear gaps on slow drives.");

    ImGui::Spacing();
    ImGui::Text("IO chunk:  %d frames", _perf.io_chunk_frames);
    ImGui::SetNextItemWidth(-1);
    {
        int chunk_kb = _perf.io_chunk_frames;
        if (ImGui::SliderInt("##chunk", &chunk_kb, 512, 32768, "%d frames")) {
            _perf.io_chunk_frames = chunk_kb; changed = true;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Frames per disk read. Larger helps spinning drives. Smaller reduces IO latency.");

    ImGui::Spacing();

    // ── DSP ───────────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::cyan);
    ImGui::TextUnformatted("DSP"); ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::Text("Interpolation:");
    ImGui::SameLine();
    const char* interp_names[] = {"Linear (fast)", "Catmull-Rom (default)", "Sinc 6-tap (quality)"};
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##interp", interp_names[_perf.interpolation])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(interp_names[i], _perf.interpolation == i)) {
                _perf.interpolation = i; changed = true;
            }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    ImGui::Text("Render threads:  %d", _perf.render_threads);
    ImGui::SetNextItemWidth(-1);
    changed |= ImGui::SliderInt("##rthreads", &_perf.render_threads, 1, 8, "%d");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Threads used for offline render. Match your CPU core count for best speed.");

    ImGui::Spacing();

    // ── VU Meter ──────────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::green);
    ImGui::TextUnformatted("VU METER"); ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::Text("Response speed:");
    ImGui::SameLine();
    const char* vu_names[] = {"Slow (Classic VU)", "Medium (Default)", "Fast (PPM)"};
    const char* vu_tips[]  = {
        "300ms attack, 1.5s decay - traditional analog VU meter behavior",
        "100ms attack, 500ms decay - balanced response for most material",
        "50ms attack, 200ms decay - fast PPM-style metering, shows transients"
    };
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##vuresp", vu_names[_perf.vu_response])) {
        for (int i = 0; i < 3; ++i)
            if (ImGui::Selectable(vu_names[i], _perf.vu_response == i)) {
                _perf.vu_response = i;
                _audio.set_vu_response(i);
                _perf.save();
            }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", vu_tips[_perf.vu_response]);

    ImGui::Spacing();

    if (changed) _perf.save();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.5f,.5f,.6f,1.f));
    ImGui::TextUnformatted("Reload audio file to apply buffer changes.");
    ImGui::PopStyleColor();

    ImGui::End();
}

void CapstanApp::_draw_error_log() {
    if (!_show_error_log) return;

    ImGui::SetNextWindowSize({680, 420}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    if (!ImGui::Begin("Error Log", &_show_error_log, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End(); return;
    }

    auto entries = ErrorLog::get().snapshot();

    // Toolbar
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.5f,.5f,.6f,1.f));
    ImGui::Text("%d entries", (int)entries.size());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Dismiss All")) ErrorLog::get().dismiss_all();
    ImGui::SameLine();
    if (ImGui::Button("Clear"))       ErrorLog::get().clear();
    ImGui::Separator();

    ImGui::BeginChild("##errlist", {0,0}, false);
    for (int i = (int)entries.size()-1; i >= 0; --i) {
        auto& e = entries[i];
        ImGui::PushID(i);

        // Timestamp
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.4f,.4f,.5f,1.f));
        ImGui::TextUnformatted(e.timestamp.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);

        // Error code badge
        ImU32 badge_bg  = e.dismissed ? IM_COL32(35,35,45,200) : IM_COL32(80,15,15,240);
        ImU32 badge_fg  = e.dismissed ? IM_COL32(100,100,120,200) : IM_COL32(255,100,100,255);
        auto* dl = ImGui::GetWindowDrawList();
        ImVec2 bp = ImGui::GetCursorScreenPos();
        float  fs = ImGui::GetFontSize() * 0.88f;
        ImFont* font = ImGui::GetFont();
        const char* code_str = err_code_str(e.code);
        ImVec2 csz = font->CalcTextSizeA(fs, FLT_MAX, 0.f, code_str);
        float bpad = 4.f;
        dl->AddRectFilled({bp.x, bp.y+1}, {bp.x+csz.x+bpad*2, bp.y+csz.y+3},
                          badge_bg, 3.f);
        dl->AddRect({bp.x, bp.y+1}, {bp.x+csz.x+bpad*2, bp.y+csz.y+3},
                    badge_fg, 3.f, 0, 1.f);
        dl->AddText(font, fs, {bp.x+bpad, bp.y+2}, badge_fg, code_str);
        ImGui::Dummy({csz.x+bpad*2+4.f, csz.y+4.f});
        ImGui::SameLine(0, 6);

        // Detail message
        ImGui::PushStyleColor(ImGuiCol_Text,
            e.dismissed ? ImVec4(.4f,.4f,.5f,1.f) : ImVec4(.82f,.82f,.88f,1.f));
        ImGui::TextUnformatted(e.message.empty() ? "(no detail)" : e.message.c_str());
        ImGui::PopStyleColor();

        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ── Inline notifications (below header) ───────────────────────────────────────
void CapstanApp::_draw_notifications() {
    auto entries = ErrorLog::get().snapshot();
    
    // Find first undismissed entry
    for (auto& e : entries) {
        if (e.dismissed) continue;
        
        // Show notifications bar
        ImGuiViewport* vp = ImGui::GetMainViewport();
        float pad = 8.f;
        float height = 0.f;
        
        // Calculate total height needed
        for (auto& entry : entries) {
            if (!entry.dismissed) height += 28.f;
        }
        if (height == 0.f) return;  // No undismissed entries
        
        height += pad * 2.f;
        
        ImGui::SetNextWindowPos({vp->Pos.x, vp->Pos.y + HEADER_H});
        ImGui::SetNextWindowSize({vp->Size.x, height});
        ImGui::SetNextWindowBgAlpha(0.95f);
        
        if (!ImGui::Begin("##notifications", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus)) {
            ImGui::End(); return;
        }
        
        ImGui::SetCursorPosY(pad);
        
        for (auto& entry : entries) {
            if (entry.dismissed) continue;
            
            // Severity colors and icons
            ImU32 icon_col, text_col, bg_col;
            const char* icon = err_severity_icon(entry.severity);
            
            switch (entry.severity) {
            case ErrSeverity::Info:
                icon_col = IM_COL32(100,180,255,255);
                text_col = IM_COL32(200,220,255,255);
                bg_col   = IM_COL32(30,50,80,200);
                break;
            case ErrSeverity::Warning:
                icon_col = IM_COL32(255,200,50,255);
                text_col = IM_COL32(255,230,150,255);
                bg_col   = IM_COL32(80,60,20,200);
                break;
            case ErrSeverity::Error:
                icon_col = IM_COL32(255,80,80,255);
                text_col = IM_COL32(255,180,180,255);
                bg_col   = IM_COL32(80,20,20,200);
                break;
            default:
                // Neutral grey for any unrecognised severity (e.g., a future
                // enum value). Mirrors the "?" / reset fallback used by
                // err_severity_icon() and err_severity_color().
                icon_col = IM_COL32(160,160,170,255);
                text_col = IM_COL32(200,200,210,255);
                bg_col   = IM_COL32(45,45,55,200);
                break;
            }
            
            // Background
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = {vp->Pos.x + vp->Size.x - pad*2, p_min.y + 24.f};
            ImGui::GetWindowDrawList()->AddRectFilled(p_min, p_max, bg_col, 4.f);
            
            // Icon
            ImVec2 icon_pos = {p_min.x + 10.f, p_min.y + 4.f};
            ImGui::GetWindowDrawList()->AddText(nullptr, 18.f, icon_pos, icon_col, icon);
            
            // Message
            char msg[512];
            std::snprintf(msg, sizeof(msg), "%s  %s", err_code_str(entry.code),
                         entry.message.empty() ? "" : entry.message.c_str());
            ImVec2 text_pos = {p_min.x + 32.f, p_min.y + 5.f};
            ImGui::GetWindowDrawList()->AddText(nullptr, 15.f, text_pos, text_col, msg);
            
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 28.f);
        }
        
        ImGui::End();
        return;  // Only show one notification bar
    }
}
void CapstanApp::_draw_timeline() {
    // Keep anim cursor in sync with playback
    if (_engine.is_playing.load())
        _anim_cursor = _engine.play_head;

    double prev_cursor = _anim_cursor;
    _timeline.draw(_anim, _engine.play_head,
                   (double)_engine.total_samples, _anim_cursor);

    // If cursor was moved by user click and we're not playing, seek engine
    if (!_engine.is_playing.load() && _anim_cursor != prev_cursor
        && _engine.total_samples > 0) {
        std::lock_guard<std::mutex> g(_engine.lock);
        _engine.play_head = std::clamp(_anim_cursor,
                                        0.0, (double)(_engine.total_samples-1));
    }
}

bool CapstanApp::_col_button(const char* label, const ImVec4& bg_col,
                            const ImVec4& fg_col, float width)
{
    ImVec4 hov={std::min(bg_col.x*1.4f,1.f),std::min(bg_col.y*1.4f,1.f),
                std::min(bg_col.z*1.4f,1.f),1.f};
    ImVec4 act={bg_col.x*.7f,bg_col.y*.7f,bg_col.z*.7f,1.f};
    ImGui::PushStyleColor(ImGuiCol_Button,        bg_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hov);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  act);
    ImGui::PushStyleColor(ImGuiCol_Text,          fg_col);
    bool p=(width>0)?ImGui::Button(label,{width,0}):ImGui::Button(label);
    ImGui::PopStyleColor(4);
    return p;
}

std::string CapstanApp::_vu_bar(float norm, int width) const {
    norm=std::clamp(norm,0.f,1.f);
    int f=(int)(norm*width);
    return std::string(f,'#')+std::string(width-f,'.');
}

void CapstanApp::_sync_params() {
    // Copy only user-controllable DSP fields — never touch transport internals.
    std::lock_guard<std::mutex> g(_engine.lock);
    EngineParams& ep = _engine.params;
    ep.input_gain=_ui_params.input_gain;
    ep.ips_base=_ui_params.ips_base; ep.motor_health=_ui_params.motor_health;
    ep.motor_drag=_ui_params.motor_drag; ep.motor_boost=_ui_params.motor_boost;
    ep.wow_dep=_ui_params.wow_dep; ep.flutter_dep=_ui_params.flutter_dep;
    ep.scrape_flutter=_ui_params.scrape_flutter; ep.tension_load=_ui_params.tension_load;
    ep.dropout_rate=_ui_params.dropout_rate; ep.drive=_ui_params.drive;
    ep.bias=_ui_params.bias; ep.replay_diff=_ui_params.replay_diff;
    ep.asperities=_ui_params.asperities; ep.barkhausen=_ui_params.barkhausen;
    ep.crosstalk=_ui_params.crosstalk; ep.print_through=_ui_params.print_through;
    ep.demagnetization=_ui_params.demagnetization; ep.oxide_shedding=_ui_params.oxide_shedding;
    ep.hiss=_ui_params.hiss; ep.hiss_color=_ui_params.hiss_color;
    ep.mains_hum=_ui_params.mains_hum; ep.cutoff_base=_ui_params.cutoff_base;
    ep.head_bump=_ui_params.head_bump; ep.azimuth_drift=_ui_params.azimuth_drift;
    ep.sticky_shed=_ui_params.sticky_shed; ep.oxide_type=_ui_params.oxide_type;
    // Phase 2 — EQ curves + format-id coupling
    ep.eq_curve      = _ui_params.eq_curve;
    ep.lf_trim_db    = _ui_params.lf_trim_db;
    ep.hf_trim_db    = _ui_params.hf_trim_db;
    ep.format_id     = _ui_params.format_id;
    ep.format_locked = _ui_params.format_locked;
    // Apply curves on top if playing
    if (_anim.enabled && !_anim.curves.empty() && _engine.is_playing.load())
        _anim.apply(ep, _engine.play_head);
    _project_dirty = true;
}

void CapstanApp::_apply_preset(const EngineParams& p, const std::string&) {
    _ui_params = p;
    _sync_params();
    _engine.trigger_fade_in(512);
}


void CapstanApp::_open_load_project() {
    _fd_load_project.open_load("Open Project", {".cvproject"},
                           AppDirs::projects());
    _fd_pending = FDPending::LoadProject;
}
void CapstanApp::_open_save_project() {
    std::string def = _project_path.empty()
        ? "untitled.cvproject"
        : fs::path(_project_path).filename().string();
    _fd_save_project.open_save("Save Project As", def,
                               AppDirs::projects());
    _fd_pending = FDPending::SaveProject;
}

ProjectData CapstanApp::_collect_project_data(const std::string& path) const {
    ProjectData d;
    d.project_name   = _project_path.empty()
                       ? "Untitled"
                       : fs::path(_project_path).stem().string();
    d.audio_path_abs = _loaded_file;
    d.audio_path_rel = path.empty() ? "" : make_relative_audio_path(path, _loaded_file);
    d.params         = _ui_params;
    d.anim           = _anim;
    d.render_sr      = _render_opts.sample_rate;
    d.render_bit_depth = _render_opts.bit_depth;
    d.render_quality   = (int)_render_opts.quality;
    d.render_dither    = _render_opts.dither;
    d.render_normalize = _render_opts.normalize;
    d.render_preroll   = _render_opts.preroll_s;
    d.render_threads   = _render_opts.threads;
    d.play_head        = _engine.play_head;
    d.anim_cursor      = _anim_cursor;
    return d;
}

void CapstanApp::_save_project(const std::string& path) {
    ProjectData d = _collect_project_data(path);
    if (save_project(path, d)) {
        _project_path  = path;
        _project_dirty = false;
        FileDialog::recents.push(path);
        _update_window_title();
    }
}

void CapstanApp::_load_project(const std::string& path) {
    auto d = load_project(path);
    if (!d) return;  // silent fail — bad format

    _project_path = path;
    _project_dirty = false;

    // Apply parameters
    _apply_preset(d->params, d->project_name);

    // Animation
    _anim = d->anim;
    _anim_cursor = d->anim_cursor;

    // Render options
    _render_opts.sample_rate = d->render_sr;
    _render_opts.bit_depth   = d->render_bit_depth;
    _render_opts.quality     = (SimQuality)d->render_quality;
    _render_opts.dither      = d->render_dither;
    _render_opts.normalize   = d->render_normalize;
    _render_opts.preroll_s   = d->render_preroll;
    _render_opts.threads     = d->render_threads;

    // Load audio if path is valid
    if (!d->audio_path_abs.empty()) {
        std::error_code ec;
        if (fs::is_regular_file(d->audio_path_abs, ec)) {
            _audio.stop();
            _loaded_file = d->audio_path_abs;
            // Apply current perf settings to stream buffer BEFORE opening
            _engine.stream.ring_frames   = _perf.ring_seconds  * 44100;
            _engine.stream.ahead_frames  = _perf.ahead_seconds * 44100;
            _engine.stream.io_chunk_frames = _perf.io_chunk_frames;
            _engine.load_file(_loaded_file);
            _reel.reset();
            // Seek to saved position
            {
                std::lock_guard<std::mutex> g(_engine.lock);
                _engine.play_head = d->play_head;
            }
        }
    }

    FileDialog::recents.push(path);
    _update_window_title();
}
void CapstanApp::_update_window_title() {
    std::string title = "CAPSTANVAR";
    if (!_project_path.empty()) {
        title += "  —  " + fs::path(_project_path).stem().string();
        if (_project_dirty) title += " *";
    } else if (_project_dirty) {
        title += "  —  Untitled *";
    }
    title += "  ·  ANALOG TAPE SIMULATOR";
    _window.setTitle(title);
}

void CapstanApp::_new_project() {
    _audio.stop();
    _engine.audio_data.clear();
    _engine.total_samples = 0;
    _engine.play_head     = 0.0;
    _loaded_file.clear();
    _reel.reset();

    // Reset to default preset
    auto pr = _presets.find_builtin("Ampex 456 (30ips)");
    if (pr) _apply_preset(pr->params, pr->name);

    // Clear animation
    _anim = ParamAnim{};
    _anim_cursor = 0.0;
    _timeline.view_start      = 0.0;
    _timeline.view_end        = 44100.0;
    _timeline.prev_cursor_pos = -1.0;

    // Clear project identity
    _project_path.clear();
    _project_dirty = false;
    _show_new_confirm = false;
    _update_window_title();
}

void CapstanApp::_draw_close_confirm() {
    // ── Unsaved changes on CLOSE ──────────────────────────────────────────────
    if (_show_close_confirm) {
        ImGui::OpenPopup("Unsaved Changes##close");
        _show_close_confirm = false;
    }
    ImGui::SetNextWindowSize({380, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Unsaved Changes##close",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.85f,.2f,1.f});
        ImGui::TextWrapped("You have unsaved changes in \"%s\".",
            _project_path.empty() ? "Untitled"
                                  : fs::path(_project_path).stem().string().c_str());
        ImGui::PopStyleColor();
        ImGui::TextUnformatted("Save before closing?");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        {.15f,.35f,.15f,.9f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.2f,.5f,.2f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {.4f,1.f,.4f,1.f});
        if (ImGui::Button("Save & Close", {120, 0})) {
            if (_project_path.empty()) {
                // Need a path — open Save As, then close will happen after save
                ImGui::CloseCurrentPopup();
                _fd_save_project.open_save("Save Project As",
                    "untitled.cvproject", AppDirs::projects());
                // Mark that after save we should close
                _pending_close_after_save = true;
            } else {
                _save_project(_project_path);
                ImGui::CloseCurrentPopup();
                _window.close();
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_Button,        {.35f,.1f,.1f,.9f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.5f,.15f,.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {1.f,.4f,.4f,1.f});
        if (ImGui::Button("Discard & Close", {130, 0})) {
            ImGui::CloseCurrentPopup();
            _project_dirty = false;
            _window.close();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8);
        if (ImGui::Button("Cancel", {70, 0}))
            ImGui::CloseCurrentPopup();

        ImGui::Spacing();
        ImGui::EndPopup();
    }

    // ── Unsaved changes on NEW ────────────────────────────────────────────────
    if (_show_new_confirm) {
        ImGui::OpenPopup("Unsaved Changes##new");
        _show_new_confirm = false;
    }
    ImGui::SetNextWindowSize({360, 0}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Always, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("Unsaved Changes##new",
                               nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.85f,.2f,1.f});
        ImGui::TextWrapped("You have unsaved changes in \"%s\".",
            _project_path.empty() ? "Untitled"
                                  : fs::path(_project_path).stem().string().c_str());
        ImGui::PopStyleColor();
        ImGui::TextUnformatted("Save before creating a new project?");
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        {.15f,.35f,.15f,.9f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.2f,.5f,.2f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {.4f,1.f,.4f,1.f});
        if (ImGui::Button("Save & New", {110, 0})) {
            if (_project_path.empty()) {
                ImGui::CloseCurrentPopup();
                _fd_save_project.open_save("Save Project As", "untitled.cvproject",
                           AppDirs::projects());
                _pending_new_after_save = true;
            } else {
                _save_project(_project_path);
                ImGui::CloseCurrentPopup();
                _new_project();
            }
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_Button,        {.35f,.1f,.1f,.9f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.5f,.15f,.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {1.f,.4f,.4f,1.f});
        if (ImGui::Button("Discard & New", {120, 0})) {
            ImGui::CloseCurrentPopup();
            _new_project();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 8);
        if (ImGui::Button("Cancel", {70, 0}))
            ImGui::CloseCurrentPopup();

        ImGui::Spacing();
        ImGui::EndPopup();
    }
}

void CapstanApp::_start_forward() {
    // Check if audio is loaded (either via audio_data or StreamBuffer)
    if (_engine.audio_data.empty() && !_engine.stream.is_open()) return;
    _audio.play_forward();
}
void CapstanApp::_start_reverse() {
    if (_engine.audio_data.empty() && !_engine.stream.is_open()) return;
    _audio.play_reverse();
}
void CapstanApp::_stop_transport() {
    _audio.stop();
}
void CapstanApp::_toggle_rewind() {
    if (_engine.audio_data.empty() && !_engine.stream.is_open()) return;
    // Only stop if already shuttling at significant speed (> 5x)
    if (_audio.is_rewinding() && std::abs(_audio.shuttle_speed()) > 5.f) { 
        _audio.stop(); 
        return; 
    }
    _audio.shuttle_rewind(10.f);  // Start at 10x
}
void CapstanApp::_toggle_ff() {
    if (_engine.audio_data.empty() && !_engine.stream.is_open()) return;
    // Only stop if already shuttling at significant speed (> 5x)
    if (_audio.is_ffing() && std::abs(_audio.shuttle_speed()) > 5.f) { 
        _audio.stop(); 
        return; 
    }
    _audio.shuttle_ff(10.f);  // Start at 10x
}

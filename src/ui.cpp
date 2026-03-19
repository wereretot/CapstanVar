#include "ui.hpp"
#include <imgui.h>
#include <imgui-SFML.h>
#include <SFML/Graphics.hpp>
#include <SFML/Window/Event.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>

#ifdef NAGRA_HAS_TFD
#include <tinyfiledialogs.h>
#endif

// ── Colour helpers ────────────────────────────────────────────────────────────
static ImVec4 dim(ImVec4 c, float f = 0.3f) { return {c.x*f,c.y*f,c.z*f,c.w}; }
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

    const char* home = std::getenv("HOME");
    std::string rp   = home ? std::string(home)+"/.capstanvar_recents" : ".capstanvar_recents";
    FileDialog::recents.load(rp);

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
    ImGuiIO& io = ImGui::GetIO();
    // NavEnableKeyboard disabled: we handle all transport keys ourselves
    // io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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
            _audio.stop(); _window.close();
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
            case sf::Keyboard::Space:
                if (rewinding || ffing) { _audio.stop(); }
                else if (playing)       { _stop_transport(); }
                else                    { _start_forward(); }
                break;

            // Enter — play forward
            case sf::Keyboard::Enter:
                if (!playing || _engine.is_reversed) _start_forward();
                break;

            // Backspace / R — play reverse
            case sf::Keyboard::BackSpace:
            case sf::Keyboard::R:
                if (!playing || !_engine.is_reversed) _start_reverse();
                break;

            // S / Escape — stop
            case sf::Keyboard::S:
            case sf::Keyboard::Escape:
                _stop_transport();
                break;

            // Left arrow — rewind shuttle; Shift+Left — play in reverse
            case sf::Keyboard::Left:
                if (ev.key.shift) {
                    // Shift+Left = play in reverse direction
                    if (!playing || !_engine.is_reversed) _start_reverse();
                } else {
                    if (rewinding) _audio.stop();
                    else           { _stop_transport(); _toggle_rewind(); }
                }
                break;

            // Right arrow — fast forward; Shift+Right — play forward
            case sf::Keyboard::Right:
                if (ev.key.shift) {
                    // Shift+Right = play forward (mirror of Shift+Left)
                    if (!playing || _engine.is_reversed) _start_forward();
                } else {
                    if (ffing) _audio.stop();
                    else       { _stop_transport(); _toggle_ff(); }
                }
                break;

            // Up/Down arrows — intentionally unbound (reserved for UI scroll)

            // O — open file
            case sf::Keyboard::O:
                if (ev.key.control) _open_load_audio();
                break;

            default: break;
            } // end if(!typing) switch

            // Ctrl+O always works regardless of text focus
            if (ev.key.control && ev.key.code == sf::Keyboard::O)
                _open_load_audio();
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
    _fd_load_preset.open_load("Import Preset", {".cvpr",".json"});
    _fd_pending = FDPending::LoadPreset;
}
void CapstanApp::_open_save_preset(const std::string& default_name) {
    _fd_save_preset.open_save("Export Preset", default_name+".cvpr");
    _fd_pending = FDPending::SavePreset;
}
void CapstanApp::_open_save_render() {
    _fd_save_render.open_save("Save Rendered File", "output.wav");
    _fd_pending = FDPending::SaveRender;
}

void CapstanApp::_draw_file_dialogs() {
    switch (_fd_pending) {
    case FDPending::LoadAudio:
        if (_fd_load_audio.draw()) {
            std::string path = _fd_load_audio.result();
            if (!path.empty()) {
                _audio.stop();
                _loaded_file = path;
                _engine.load_file(path);
                _reel.reset();   // prevent delta jump on first play frame
            }
            _fd_pending = FDPending::None;
        }
        break;
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
                _show_render_dialog       = false;
                _renderer.enqueue(_render_opts);
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

static constexpr float TRANSPORT_H = 118.f;  // reserved height for transport strip
static constexpr float HEADER_H    = 88.f;
static constexpr float PRESETBAR_H = 34.f;

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
    if (_show_render_dialog) _draw_render_dialog();
    if (_show_save_dialog)   _draw_save_dialog();
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

    // ── Centre: title + reel animation ────────────────────────────────────────
    float side_w  = 260.f + 10.f + 200.f + 10.f;
    float mid_w   = std::max(120.f, total_w - side_w);
    ImGui::BeginChild("##hdr_mid", {mid_w, HEADER_H}, false);
    {
        // Reel visualisation using draw list
        ImVec2 c0 = ImGui::GetCursorScreenPos();
        bool   pl   = _engine.is_playing.load();

        // Reel animation — ground truth is play_head delta per frame.
        // Captures inertia, speed, direction, wow, flutter, all effects.
        _reel.draw(
            _engine.play_head,
            _engine.total_samples,
            _engine.is_reversed,
            std::abs(_audio.signed_tape_speed()) > 0.002f,
            _audio.signed_tape_speed(),
            c0,
            {mid_w, HEADER_H}
        );

        // Title (centred between reels)
        ImGui::SetCursorScreenPos({c0.x, c0.y+2});
        ImGui::PushStyleColor(ImGuiCol_Text, Col::amber);
        ImGui::SetWindowFontScale(1.4f);
        float tw = ImGui::CalcTextSize("CAPSTANVAR").x;
        ImGui::SetCursorPosX((mid_w-tw)*0.5f);
        ImGui::Text("CAPSTANVAR");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
        float sw = ImGui::CalcTextSize("ANALOG TAPE SIMULATOR").x;
        ImGui::SetCursorPosX((mid_w-sw)*0.5f);
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
    if (_col_button("LOAD", Col::green_dim, Col::green, 50)) _open_load_audio();
    ImGui::SameLine();

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
    ImGui::SameLine(0,16);
    if (_col_button("RENDER",Col::purple_dim,Col::purple,70))
        _show_render_dialog=true;
    ImGui::SameLine();
    // Keyboard hint
    ImGui::PushStyleColor(ImGuiCol_Text,Col::grey);
    ImGui::TextUnformatted("SPC=play  ←→=rwd/ff  Shift+←=reverse  Shift+→=forward  S=stop  R=rev");
    ImGui::PopStyleColor();
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
    c|=_slider("ips",   "CAPSTAN SPEED (IPS)",       _ui_params.ips_base,       0.5f, 30.f,  Col::amber,"Tape speed in inches/second. ↑↓ keys cycle speeds.");
    c|=_slider("mh",    "VOLTAGE DRIFT / JITTER",     _ui_params.motor_health,   0.f,  10.f,  Col::amber,"PSU aging: slow irregular speed surges.");
    c|=_slider("mdrag", "TORQUE LOAD",                _ui_params.motor_drag,     0.f,  0.9f,  Col::amber,"Friction on capstan/reels.");
    c|=_slider("mboost","MOTOR BOOST",                _ui_params.motor_boost,    0.f,  0.9f,  Col::amber,"Overdrive motor. Combined with drag = conflict.");
    c|=_slider("wow",   "WOW INTENSITY",              _ui_params.wow_dep,        0.f,  30.f,  Col::amber,"Slow pitch variation from capstan eccentricity.");
    c|=_slider("flt",   "FLUTTER INTENSITY",          _ui_params.flutter_dep,    0.f,  10.f,  Col::amber,"Fast speed variation from mechanical resonance.");
    c|=_slider("scr",   "SCRAPE FLUTTER (3kHz)",      _ui_params.scrape_flutter, 0.f,  1.f,   Col::amber,"Rapid modulation from tape sticking on heads.");
    c|=_slider("tens",  "REEL TENSION DYNAMICS",      _ui_params.tension_load,   0.f,  0.12f, Col::amber,"Tension variation from supply/takeup reels.");
    c|=_slider("drop",  "OXIDE DROPOUT RATE",         _ui_params.dropout_rate,   0.f,  1.f,   Col::amber,"Random signal gaps from missing oxide.");
    if (c) _sync_params();
    ImGui::EndChild();
}
void CapstanApp::_draw_magnetic_tab() {
    ImGui::BeginChild("##ms",{0,0},false); bool c=false;
    c|=_slider("drv",  "HEAD SATURATION",             _ui_params.drive,          1.f,  20.f,  Col::cyan,"Drive into coating. Higher = warmth then clip.");
    c|=_slider("bias", "AC BIAS TUNING",              _ui_params.bias,           0.5f, 3.f,   Col::cyan,"Under=bright/distorted. Over=dark/clean.");
    c|=_slider("rd",   "REPLAY DIFFERENTIATION",      _ui_params.replay_diff,    0.f,  1.f,   Col::cyan,"Head reads flux rate-of-change (+6dB/oct HF).");
    c|=_slider("asp",  "ASPERITIES (MOD NOISE)",      _ui_params.asperities,     0.f,  0.5f,  Col::cyan,"Signal-correlated grain noise.");
    c|=_slider("bark", "BARKHAUSEN GRAIN",            _ui_params.barkhausen,     0.f,  0.1f,  Col::cyan,"Discrete domain-switching pulses.");
    c|=_slider("xtk",  "STEREO CROSSTALK",            _ui_params.crosstalk,      0.f,  0.5f,  Col::cyan,"Inter-track magnetic bleed.");
    c|=_slider("prt",  "PRINT-THROUGH (GHOST)",       _ui_params.print_through,  0.f,  0.1f,  Col::cyan,"Layer-to-layer imprinting: pre/post echo.");
    c|=_slider("dmg",  "DEMAGNETISATION (HF LOSS)",   _ui_params.demagnetization,0.f,  0.99f, Col::cyan,"Progressive HF rolloff over time.");
    c|=_slider("shed", "OXIDE SHEDDING",              _ui_params.oxide_shedding, 0.f,  1.f,   Col::cyan,"Binder degradation: random oxide dropout.");
    if (c) _sync_params();
    ImGui::EndChild();
}
void CapstanApp::_draw_electronics_tab() {
    ImGui::BeginChild("##es",{0,0},false); bool c=false;
    c|=_slider("hiss",   "NOISE FLOOR",               _ui_params.hiss,          0.f,  0.02f,  Col::purple,"Broadband white noise from preamp/oxide.");
    c|=_slider("hcol",   "HISS COLOUR (PINK TILT)",   _ui_params.hiss_color,    0.f,  1.f,    Col::purple,"1/f noise colouring from preamp transistors.");
    c|=_slider("hum",    "60Hz MAINS HUM",            _ui_params.mains_hum,     0.f,  0.05f,  Col::purple,"AC supply at 60/120/180/240 Hz.");
    c|=_slider("cut",    "AZIMUTH CUTOFF (Hz)",       _ui_params.cutoff_base,   500.f,22000.f,Col::purple,"Head gap bandwidth.");
    c|=_slider("bump",   "HEAD BUMP (LF EQ)",         _ui_params.head_bump,     0.f,  5.f,    Col::purple,"Head resonance boosting 50-200 Hz.");
    c|=_slider("azdrift","AZIMUTH PHASE DRIFT",       _ui_params.azimuth_drift, 0.f,  1.f,    Col::purple,"Head angle error: HF phase diff between channels.");
    c|=_slider("sticky", "STICKY SHED INTENSITY",     _ui_params.sticky_shed,   0.f,  1.f,    Col::purple,"Binder absorption: squeal, drag, HF loss.");
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
    const float BTN_H  = 54.f;
    const float BTN_W  = 115.f;
    const float SH_W   = 95.f;   // shuttle buttons
    const float STOP_W = 72.f;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  {6.f, 10.f});

    // ── Row 1: main transport ─────────────────────────────────────────────────
    // REWIND  |  PLAY  |  STOP  |  REVERSE  |  FF  |  [render progress]
    {
        static const float rwd_speeds[]={40.f,80.f,160.f};
        static int rwd_tier=0, ff_tier=0;

        // REWIND
        if (rewinding) {
            char lb[24]; std::snprintf(lb,sizeof(lb),"<< %.0fx",rwd_speeds[rwd_tier]);
            ImVec4 flash = (std::fmod(ImGui::GetTime()*3.f,1.f)>.5f) ? Col::cyan : Col::cyan_dim;
            if (_transport_btn(lb, Col::cyan_dim, flash, SH_W, BTN_H))
                { rwd_tier=(rwd_tier+1)%3; _audio.cycle_shuttle_speed(); }
        } else {
            rwd_tier=0;
            if (_transport_btn("|<\nREWIND", Col::bg4, Col::cyan, SH_W, BTN_H))
                { _stop_transport(); _toggle_rewind(); }
        }
        ImGui::SameLine(0,4);

        // PLAY
        {
            bool active = playing && !reversed;
            ImVec4 bg   = active ? blend(Col::amber_dim,Col::amber,.5f) : Col::bg4;
            ImVec4 fg   = active ? Col::amber : Col::white;
            const char* lbl = active ? "|>\nPLAYING" : "|>\nPLAY";
            if (_transport_btn(lbl, bg, fg, BTN_W, BTN_H)) _start_forward();
        }
        ImGui::SameLine(0,4);

        // STOP
        {
            bool active = !playing && !rewinding && !ffing;
            ImVec4 bg   = active ? blend(Col::red_dim,Col::red,.4f) : Col::bg4;
            if (_transport_btn("--\nSTOP", bg, active?Col::red:Col::white, STOP_W, BTN_H))
                _stop_transport();
        }
        ImGui::SameLine(0,4);

        // REVERSE
        {
            bool active = playing && reversed;
            ImVec4 bg   = active ? blend(Col::orange_dim,Col::orange,.5f) : Col::bg4;
            ImVec4 fg   = active ? Col::orange : Col::white;
            const char* lbl = active ? "<|\nREVERSING" : "<|\nREVERSE";
            if (_transport_btn(lbl, bg, fg, BTN_W, BTN_H)) _start_reverse();
        }
        ImGui::SameLine(0,4);

        // FF
        if (ffing) {
            char lb[24]; std::snprintf(lb,sizeof(lb),"%.0fx\n>>",rwd_speeds[ff_tier]);
            ImVec4 flash = (std::fmod(ImGui::GetTime()*3.f,1.f)>.5f) ? Col::green : Col::green_dim;
            if (_transport_btn(lb, Col::green_dim, flash, SH_W, BTN_H))
                { ff_tier=(ff_tier+1)%3; _audio.cycle_shuttle_speed(); }
        } else {
            ff_tier=0;
            if (_transport_btn("FF\n>|", Col::bg4, Col::green, SH_W, BTN_H))
                { _stop_transport(); _toggle_ff(); }
        }
        ImGui::SameLine(0,12);

        // Render progress / button (right side)
        {
            auto st = _renderer.get_status();
            if (_renderer.is_running()) {
                char lb[64];
                std::snprintf(lb,sizeof(lb),"%s\n%.0f%% %.1fx",
                              st.phase.c_str(), st.job_progress*100.f, st.xrt);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Col::purple_dim);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, Col::bg3);
                ImGui::ProgressBar(st.job_progress,{200,BTN_H},lb);
                ImGui::PopStyleColor(2);
            } else {
                if (_transport_btn("(o)\nRENDER", Col::purple_dim, Col::purple, 100, BTN_H))
                    _show_render_dialog=true;
            }
        }
    }

    // ── Row 2: keyboard hints ─────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Text, Col::grey);
    ImGui::Text("← RWD   SPACE play/stop   S stop   R rev   → FF   ↑↓ IPS   Ctrl+O load");
    ImGui::PopStyleColor();

    ImGui::PopStyleVar(2);
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
    float   big   = base * 1.55f;
    float   small = base * 0.85f;

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

// ── Render dialog ─────────────────────────────────────────────────────────────
void CapstanApp::_draw_render_dialog() {
    ImGui::SetNextWindowSize({780,720}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_FirstUseEver, {0.5f,0.5f});
    if (!ImGui::Begin("RENDER / QUEUE", &_show_render_dialog)) { ImGui::End(); return; }

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
bool CapstanApp::_slider(const char* id, const char* label,
                       float& value, float mn, float mx,
                       const ImVec4& accent, const char* tooltip)
{
    ImGui::PushStyleColor(ImGuiCol_FrameBg,         dim(accent));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, accent);
    ImGui::SetNextItemWidth(std::max(50.f, ImGui::GetContentRegionAvail().x - 200.f));
    bool changed = ImGui::SliderFloat(("##sl_"+std::string(id)).c_str(), &value, mn, mx, "");
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::grey_lt);
    ImGui::TextUnformatted(label); ImGui::PopStyleColor();
    ImGui::SameLine(std::max(50.f, ImGui::GetWindowWidth()-80.f));
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::Text("%.4g",(double)value); ImGui::PopStyleColor();
    ImGui::PopStyleColor(3);
    ImGui::Separator();
    return changed;
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
    std::lock_guard<std::mutex> g(_engine.lock);
    _engine.params=_ui_params;
}

void CapstanApp::_apply_preset(const EngineParams& p, const std::string&) {
    _ui_params=p;
    std::lock_guard<std::mutex> g(_engine.lock);
    _engine.params=p;
    _engine.trigger_fade_in(512);
}

void CapstanApp::_start_forward() {
    if (_engine.audio_data.empty()) return;
    _audio.play_forward();
}
void CapstanApp::_start_reverse() {
    if (_engine.audio_data.empty()) return;
    _audio.play_reverse();
}
void CapstanApp::_stop_transport() {
    _audio.stop();
}
void CapstanApp::_toggle_rewind() {
    if (_engine.audio_data.empty()) return;
    if (_audio.is_rewinding()) { _audio.stop(); return; }
    _audio.shuttle_rewind(40.f);
}
void CapstanApp::_toggle_ff() {
    if (_engine.audio_data.empty()) return;
    if (_audio.is_ffing()) { _audio.stop(); return; }
    _audio.shuttle_ff(40.f);
}

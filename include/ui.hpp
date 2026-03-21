#pragma once
#include <imgui.h>
#include "engine.hpp"
#include "audio_io.hpp"
#include "preset_manager.hpp"
#include "render_engine.hpp"
#include "file_dialog.hpp"
#include "reel_widget.hpp"
#include "param_anim.hpp"
#include "project.hpp"
#include "app_dirs.hpp"
#include "app_icon.hpp"
#include "error_log.hpp"
#include "perf_options.hpp"
#include "timeline_widget.hpp"
#include <SFML/Graphics.hpp>
#include <string>
#include <functional>

namespace Col {
    inline constexpr ImVec4 bg         {0.039f, 0.039f, 0.043f, 1.f};
    inline constexpr ImVec4 bg2        {0.067f, 0.067f, 0.078f, 1.f};
    inline constexpr ImVec4 bg3        {0.094f, 0.094f, 0.110f, 1.f};
    inline constexpr ImVec4 bg4        {0.118f, 0.118f, 0.141f, 1.f};
    inline constexpr ImVec4 border     {0.165f, 0.165f, 0.208f, 1.f};
    inline constexpr ImVec4 amber      {1.000f, 0.690f, 0.000f, 1.f};
    inline constexpr ImVec4 amber_dim  {0.478f, 0.333f, 0.000f, 1.f};
    inline constexpr ImVec4 cyan       {0.000f, 0.831f, 1.000f, 1.f};
    inline constexpr ImVec4 cyan_dim   {0.000f, 0.333f, 0.400f, 1.f};
    inline constexpr ImVec4 green      {0.224f, 1.000f, 0.078f, 1.f};
    inline constexpr ImVec4 green_dim  {0.102f, 0.361f, 0.035f, 1.f};
    inline constexpr ImVec4 red        {1.000f, 0.200f, 0.200f, 1.f};
    inline constexpr ImVec4 red_dim    {0.361f, 0.067f, 0.067f, 1.f};
    inline constexpr ImVec4 orange     {1.000f, 0.420f, 0.000f, 1.f};
    inline constexpr ImVec4 orange_dim {0.361f, 0.157f, 0.000f, 1.f};
    inline constexpr ImVec4 purple     {0.706f, 0.310f, 1.000f, 1.f};
    inline constexpr ImVec4 purple_dim {0.239f, 0.102f, 0.400f, 1.f};
    inline constexpr ImVec4 grey       {0.267f, 0.267f, 0.333f, 1.f};
    inline constexpr ImVec4 grey_lt    {0.533f, 0.533f, 0.600f, 1.f};
    inline constexpr ImVec4 white      {0.910f, 0.910f, 0.941f, 1.f};
}

class CapstanApp {
public:
    CapstanApp();
    ~CapstanApp();
    void run();

private:
    sf::RenderWindow _window;
    TapeEngine       _engine;
    AudioIO          _audio;
    PresetManager    _presets;
    RenderEngine     _renderer;

    EngineParams _ui_params;      // user base values (never overwritten by curves)
    EngineParams _display_params; // = _ui_params with curves applied; shown in sliders
    int          _preset_idx = 0;
    std::string  _loaded_file;
    ReelWidget      _reel;      // isolated reel animation state
    ParamAnim       _anim;      // keyframe animation data
    TimelineWidget  _timeline;  // timeline panel
    double          _anim_cursor = 0.0;  // timeline edit cursor (samples)
    std::string     _project_path;
    bool            _project_dirty     = false;

    // File dialogs
    FileDialog  _fd_load_audio, _fd_load_preset, _fd_save_preset, _fd_save_render;
    FileDialog  _fd_load_project, _fd_save_project;
    std::function<void(const std::string&)> _fd_callback;
    enum class FDPending { None, LoadAudio, LoadPreset, SavePreset, SaveRender,
                           LoadProject, SaveProject };
    FDPending _fd_pending = FDPending::None;

    // Render dialog
    bool          _show_render_dialog  = false;
    bool          _show_error_log      = false;
    bool          _show_options        = false;
    PerfOptions   _perf;
    bool          _show_close_confirm     = false;
    bool          _show_new_confirm       = false;
    bool          _pending_close_after_save = false;
    bool          _pending_new_after_save   = false;
    RenderOptions _render_opts;

    // Save preset dialog
    bool _show_save_dialog = false;
    char _save_name_buf[128] = {};

    // Init
    void _init_imgui();
    void _apply_imgui_theme();
    void _shutdown_imgui();

    // Per-frame
    void _process_events();
    void _draw_frame();
    void _draw_file_dialogs();

    // Panels
    void _draw_header();
    void _draw_preset_bar();
    void _draw_tabs();
    void _draw_transport_tab();
    void _draw_magnetic_tab();
    void _draw_electronics_tab();
    void _draw_transport_controls();   // always-visible bottom strip
    void _draw_render_dialog();
    void _draw_render_queue();
    void _draw_save_dialog();
    void _draw_timeline();

    // Helpers
    void _sync_params();
    void _apply_preset(const EngineParams& p, const std::string& name);
    void _start_forward();
    void _start_reverse();
    void _stop_transport();
    void _toggle_rewind();
    void _toggle_ff();
    void _open_load_audio();
    void _open_load_preset();
    void _open_save_preset(const std::string& default_name);
    void _open_save_render();
    void _open_load_project();
    void _open_save_project();
    void _save_project(const std::string& path);
    void _load_project(const std::string& path);
    void _new_project();
    void _update_window_title();
    void _draw_close_confirm();
    void _draw_error_log();
    void _draw_options();
    ProjectData _collect_project_data(const std::string& path) const;

    // Tape-machine style tall button (multi-line label, coloured)
    bool _transport_btn(const char* label, const ImVec4& bg, const ImVec4& fg,
                        float w, float h);
    // Small utility button
    bool _col_button(const char* label, const ImVec4& bg, const ImVec4& fg,
                     float width = 0.f);
    bool _slider(const char* id, const char* label, float& value,
                 float mn, float mx, const ImVec4& accent,
                 const char* tooltip = nullptr);
    // Animated slider — same as _slider but highlights when keyframed
    // and inserts a keyframe when I is pressed while hovered.
    bool _aslider(const char* id, const char* label, float& value,
                  float mn, float mx, const ImVec4& accent,
                  const char* tooltip = nullptr);
    std::string _vu_bar(float norm, int width = 20) const;
};

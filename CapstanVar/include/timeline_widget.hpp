#pragma once
// ── Timeline widget ───────────────────────────────────────────────────────────
// Blender-style dopesheet / curve view for ParamAnim.
// Drawn as a separate ImGui window (call draw() each frame when open).
//
// Left panel:  curve list with enable toggle + delete buttons
// Right panel: timeline canvas with draggable keyframe diamonds,
//              time cursor, zoom/pan via mouse wheel + middle-drag

#include <imgui.h>
#include "param_anim.hpp"
#include <cmath>
#include <algorithm>
#include <string>

struct TimelineWidget {
    bool open = false;

    // View state
    double view_start        = 0.0;
    double view_end          = 44100.0;
    float  row_h             = 28.f;
    double prev_cursor_pos   = -1.0; // set to -1 to force view reset on next draw

    // Drag state
    struct Drag { std::string id; int key; double t0; float v0; float row_y = 0.f; };
    std::optional<Drag> dragging;
    bool time_cursor_dragging = false;

    void draw(ParamAnim& anim, double play_head, double total_samples,
              double& out_time_cursor)
    {
        if (!open) return;
        if (total_samples < 1) total_samples = 44100;

        // Auto-fit view on first open or after a file-load reset (prev_cursor_pos==-1)
        if (view_end <= view_start + 1 || prev_cursor_pos < 0.0) {
            view_start      = 0;
            view_end        = total_samples > 0 ? total_samples : 44100.0;
            prev_cursor_pos = 0.0;  // clear reset flag
        }

        ImGui::SetNextWindowSize({900, 420}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            {ImGui::GetMainViewport()->Pos.x + 60,
             ImGui::GetMainViewport()->Pos.y + ImGui::GetMainViewport()->Size.y - 460},
            ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("ANIMATION TIMELINE", &open,
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
                          | ImGuiWindowFlags_NoTitleBar))
        { ImGui::End(); return; }

        // ── Manual drag handle (NoTitleBar removes default, this replaces it) ──
        {
            ImVec2 wpos = ImGui::GetWindowPos();
            float  ww   = ImGui::GetWindowWidth();
            const float hh = 18.f;
            auto* hdl_dl = ImGui::GetWindowDrawList();
            ImVec2 hp0 = ImGui::GetCursorScreenPos();
            hdl_dl->AddRectFilled(hp0, {hp0.x + ww, hp0.y + hh}, IM_COL32(28,35,45,255));
            hdl_dl->AddLine({hp0.x, hp0.y + hh - 1.f},
                {hp0.x + ww, hp0.y + hh - 1.f}, IM_COL32(50,70,55,200));
            hdl_dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.88f,
                {hp0.x + 8.f, hp0.y + 2.f}, IM_COL32(100,200,120,220),
                ":: ANIMATION TIMELINE  (drag here to move)");
            ImGui::SetCursorScreenPos({hp0.x + ww - 22.f, hp0.y + 1.f});
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.5f,.1f,.1f,.8f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(.8f,.35f,.35f,1.f));
            if (ImGui::Button("x##tlclose", {20.f, hh - 2.f})) open = false;
            ImGui::PopStyleColor(3);
            ImGui::SetCursorScreenPos(hp0);
            ImGui::InvisibleButton("##tl_grip", {ww - 24.f, hh});
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                ImVec2 md = ImGui::GetIO().MouseDelta;
                ImGui::SetWindowPos({wpos.x + md.x, wpos.y + md.y});
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
            ImGui::SetCursorScreenPos({hp0.x, hp0.y + hh + 2.f});
        }

        // ── Toolbar ───────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, {.5f,1.f,.5f,1.f});
        bool was_en = anim.enabled;
        ImGui::Checkbox("Animate", &anim.enabled);
        ImGui::PopStyleColor();
        if (anim.enabled != was_en && !anim.enabled)
            ImGui::SetTooltip("Curves are evaluated but UI sliders reflect base values");

        ImGui::SameLine(0, 16);
        // Key badge helper drawn inline on the toolbar
        auto key_hint = [&](const char* k, const char* desc) {
            auto* dl = ImGui::GetWindowDrawList();
            ImFont* font = ImGui::GetFont();
            float   fs   = ImGui::GetFontSize();
            float   px   = 4.f, py = 1.f;
            ImVec2  ksz  = font->CalcTextSizeA(fs, FLT_MAX, 0.f, k);
            float   kw   = ksz.x + px * 2.f;
            float   kh   = ksz.y + py * 2.f;
            ImVec2  cur  = ImGui::GetCursorScreenPos();
            // Badge box
            dl->AddRectFilled({cur.x, cur.y + py - 1.f},
                              {cur.x + kw, cur.y + kh},
                              IM_COL32(38,38,52,255), 3.f);
            dl->AddRect({cur.x, cur.y + py - 1.f},
                        {cur.x + kw, cur.y + kh},
                        IM_COL32(90,90,120,200), 3.f, 0, 1.f);
            dl->AddText(font, fs, {cur.x + px, cur.y + py},
                        IM_COL32(210,215,240,255), k);
            // Advance cursor past badge
            ImGui::Dummy({kw, kh});
            ImGui::SameLine(0, 3);
            // Description
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.5f,.5f,.62f,1.f));
            ImGui::TextUnformatted(desc);
            ImGui::PopStyleColor();
            ImGui::SameLine(0, 14);
        };
        key_hint("I",      "insert keyframe");
        key_hint("Drag",   "move key (time+value)");
        key_hint("Del",    "delete selected");
        key_hint("Scroll", "zoom");
        key_hint("MMB",    "pan");

        // Fit and Clear All as styled buttons
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.1f,.2f,.35f,.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.15f,.3f,.5f,1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(.5f,.75f,1.f,1.f));
        if (ImGui::SmallButton("Fit")) { view_start=0; view_end=total_samples; }
        ImGui::PopStyleColor(3);
        ImGui::SameLine(0, 4);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(.3f,.08f,.08f,.8f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.45f,.12f,.12f,1.f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.f,.45f,.45f,1.f));
        if (ImGui::SmallButton("Clear All")) anim.clear_all();
        ImGui::PopStyleColor(3);
        ImGui::Separator();

        // ── Layout: left list + right canvas ─────────────────────────────────
        float list_w  = 260.f;
        float avail_h = ImGui::GetContentRegionAvail().y;
        float avail_w = ImGui::GetContentRegionAvail().x;
        float canvas_w = avail_w - list_w - 6;

        // ── Left: curve list ──────────────────────────────────────────────────
        ImGui::BeginChild("##tl_list", {list_w, avail_h}, true);
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.85f,0.f,1.f});
        ImGui::TextUnformatted("ANIMATED PARAMS"); ImGui::PopStyleColor();
        ImGui::Separator();

        if (anim.curves.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, {.4f,.4f,.5f,1.f});
            ImGui::TextWrapped("No keyframes yet.\nHover a slider and\npress I to add one.");
            ImGui::PopStyleColor();
        }

        // Sort curve list by label for stable display
        std::vector<std::string> ids;
        for (auto& [id,_] : anim.curves) ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [&](auto& a, auto& b){
            return anim.curves.at(a).label < anim.curves.at(b).label; });

        std::string to_delete;
        for (auto& id : ids) {
            if (anim.curves.find(id) == anim.curves.end()) continue;
            auto& c = anim.curves.at(id);
            ImGui::PushID(id.c_str());

            // Layout: [x] [●] label (key_count)
            // x and ● have fixed widths, label gets whatever remains — never clips buttons.
            ImGui::PushStyleColor(ImGuiCol_Button,  {.3f,.05f,.05f,.7f});
            ImGui::PushStyleColor(ImGuiCol_Text,    {.9f,.3f,.3f,1.f});
            if (ImGui::SmallButton("x")) to_delete = id;
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0, 3);

            ImGui::PushStyleColor(ImGuiCol_Button,
                c.enabled ? ImVec4(.1f,.5f,.1f,.8f) : ImVec4(.15f,.15f,.2f,.8f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                c.enabled ? ImVec4(.4f,1.f,.4f,1.f) : ImVec4(.4f,.4f,.5f,1.f));
            if (ImGui::SmallButton(c.enabled ? "o" : "-"))
                c.enabled = !c.enabled;
            ImGui::PopStyleColor(2);
            ImGui::SameLine(0, 4);

            // Label truncated to fit — use SetNextItemWidth so ImGui handles layout
            float col_w = ImGui::GetContentRegionAvail().x;
            // Build a string truncated to fit the column
            std::string full = c.label + " (" + std::to_string((int)c.keys.size()) + ")";
            // Truncate by character estimate (avg ~7px per char)
            int max_chars = std::max(4, (int)(col_w / 7.f));
            std::string display = (int)full.size() > max_chars
                                  ? full.substr(0, max_chars-2) + ".."
                                  : full;
            ImGui::PushStyleColor(ImGuiCol_Text, {.8f,.8f,.9f,1.f});
            ImGui::TextUnformatted(display.c_str());
            ImGui::PopStyleColor();

            ImGui::PopID();
        }
        if (!to_delete.empty()) anim.clear_curve(to_delete);
        ImGui::EndChild();

        ImGui::SameLine();

        // ── Right: canvas ─────────────────────────────────────────────────────
        ImGui::BeginChild("##tl_canvas", {canvas_w, avail_h}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImVec2 cpos = ImGui::GetCursorScreenPos();
        ImVec2 csz  = ImGui::GetContentRegionAvail();
        auto*  dl   = ImGui::GetWindowDrawList();
        ImGuiIO& io = ImGui::GetIO();

        // Background
        dl->AddRectFilled(cpos, {cpos.x+csz.x, cpos.y+csz.y}, IM_COL32(20,20,28,255));

        // Invisible button covers the whole canvas — this tells ImGui the
        // mouse is consumed here, preventing window-drag when clicking keyframes.
        ImGui::SetCursorScreenPos(cpos);
        ImGui::InvisibleButton("##canvas_capture", csz,
            ImGuiButtonFlags_MouseButtonLeft |
            ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered  = ImGui::IsItemHovered();
        bool canvas_active = ImGui::IsItemActive();
        (void)canvas_active;
        // Reset cursor so subsequent draw-list calls use the correct origin
        ImGui::SetCursorScreenPos(cpos);

        // Zoom/pan (mouse wheel = zoom around cursor, MMB = pan)
        if (hovered) {
            double span = view_end - view_start;
            if (span < 1.0) span = 44100.0;
            if (span < 1.0) span = 44100.0;
            if (io.MouseWheel != 0 && !io.KeyCtrl) {
                float mx_frac = (io.MousePos.x - cpos.x) / csz.x;
                double t_at_mouse = view_start + mx_frac * span;
                double zoom = std::pow(0.85, (double)io.MouseWheel);
                double new_span = std::max(1000.0, span * zoom);
                view_start = t_at_mouse - mx_frac * new_span;
                view_end   = view_start + new_span;
            }
            if (!dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                double dt = -(double)(io.MouseDelta.x / csz.x) * (view_end-view_start);
                view_start += dt; view_end += dt;
            }
            view_start = std::max(0.0, view_start);
        }

        // Coordinate helpers
        double view_span = view_end - view_start;
        if (view_span < 1.0) { view_span = 44100.0; view_end = view_start + view_span; }
        auto t_to_x = [&](double t) -> float {
            return cpos.x + (float)((t - view_start) / view_span) * csz.x;
        };
        auto x_to_t = [&](float x) -> double {
            return view_start + (double)((x - cpos.x) / csz.x) * view_span;
        };
        auto v_to_y = [&](float v, float mn, float mx, float row_top) -> float {
            float norm = (mx > mn) ? (v - mn) / (mx - mn) : 0.5f;
            return row_top + row_h * (1.f - std::clamp(norm, 0.f, 1.f));
        };
        auto y_to_v = [&](float y, float mn, float mx, float row_top) -> float {
            float norm = 1.f - (y - row_top) / row_h;
            return mn + std::clamp(norm, 0.f, 1.f) * (mx - mn);
        };

        // Time tick marks
        {
            double sr = 44100.0;
            double span = view_end - view_start;
            // Round to nice intervals (1, 2, 5 × 10^n seconds)
            double secs_span = span / sr;
            double tick_secs = std::pow(10.0, std::floor(std::log10(secs_span/6)));
            if (tick_secs * 6 < secs_span) tick_secs *= 2;
            if (tick_secs * 6 < secs_span) tick_secs *= 2.5;
            double tick_samp = tick_secs * sr;

            double t0 = std::floor(view_start / tick_samp) * tick_samp;
            for (double t = t0; t < view_end; t += tick_samp) {
                float x = t_to_x(t);
                dl->AddLine({x,cpos.y}, {x,cpos.y+csz.y}, IM_COL32(45,45,60,200));
                double secs = t / sr;
                char tb[16];
                if (secs < 60.0)       std::snprintf(tb,sizeof(tb),"%.1fs",secs);
                else                   std::snprintf(tb,sizeof(tb),"%d:%04.1f",(int)(secs/60),std::fmod(secs,60.0));
                dl->AddText({x+2,cpos.y+1}, IM_COL32(80,80,100,200), tb);
            }
        }

        // ── Draw each curve row ───────────────────────────────────────────────
        float row_y = cpos.y + 16.f;
        static std::string selected_id; static int selected_ki = -1;
        bool any_key_near = false;  // set true if mouse is over any keyframe

        // Build parallel param-range lookup (same order as EngineParams)
        struct PRange { const char* id; float mn, mx; };
        static const PRange ranges[] = {
            {"ips_base",0.5f,30.f},   {"motor_health",0.f,10.f}, {"motor_drag",0.f,0.9f},
            {"motor_boost",0.f,0.9f}, {"wow_dep",0.f,30.f},      {"flutter_dep",0.f,10.f},
            {"scrape_flutter",0.f,1.f},{"tension_load",0.f,.12f}, {"dropout_rate",0.f,1.f},
            {"drive",1.f,20.f},       {"bias",.5f,3.f},           {"replay_diff",0.f,1.f},
            {"asperities",0.f,.5f},   {"barkhausen",0.f,.1f},     {"crosstalk",0.f,.5f},
            {"print_through",0.f,.1f},{"demagnetization",0.f,.99f},{"oxide_shedding",0.f,1.f},
            {"hiss",0.f,.02f},        {"hiss_color",0.f,1.f},     {"mains_hum",0.f,.05f},
            {"cutoff_base",500.f,22000.f},{"head_bump",0.f,5.f},  {"azimuth_drift",0.f,1.f},
            {"sticky_shed",0.f,1.f},
        };
        auto get_range = [&](const std::string& id, float& mn, float& mx) {
            for (auto& r : ranges)
                if (id == r.id) { mn=r.mn; mx=r.mx; return; }
            mn=0.f; mx=1.f;
        };

        for (auto& id : ids) {
            if (anim.curves.find(id) == anim.curves.end())
                { row_y += row_h; continue; }
            auto& c = anim.curves.at(id);
            if (!c.enabled) { row_y += row_h; continue; }

            float mn, mx;
            get_range(id, mn, mx);

            // Row background
            bool row_sel = (selected_id == id);
            dl->AddRectFilled({cpos.x, row_y}, {cpos.x+csz.x, row_y+row_h},
                row_sel ? IM_COL32(30,40,55,200) : IM_COL32(25,25,35,180));

            // Draw curve line between keys
            for (int ki = 0; ki+1 < (int)c.keys.size(); ++ki) {
                int steps = 30;
                for (int s = 0; s < steps; ++s) {
                    double t0 = c.keys[ki].time + (c.keys[ki+1].time-c.keys[ki].time)*(s/(double)steps);
                    double t1 = c.keys[ki].time + (c.keys[ki+1].time-c.keys[ki].time)*((s+1)/(double)steps);
                    auto v0 = c.evaluate(t0); auto v1 = c.evaluate(t1);
                    if (!v0||!v1) continue;
                    // Clamp to param range so drawn curve matches what engine hears
                    float cv0 = std::max(mn, std::min(mx, *v0));
                    float cv1 = std::max(mn, std::min(mx, *v1));
                    float x0=t_to_x(t0), x1=t_to_x(t1);
                    float y0=v_to_y(cv0,mn,mx,row_y), y1=v_to_y(cv1,mn,mx,row_y);
                    dl->AddLine({x0,y0},{x1,y1}, IM_COL32(80,200,120,200), 1.5f);
                }
            }

            // Draw keyframe diamonds
            for (int ki = 0; ki < (int)c.keys.size(); ++ki) {
                auto& k = c.keys[ki];
                float kx = t_to_x(k.time);
                float ky = v_to_y(k.value, mn, mx, row_y);
                bool ksel = (selected_id==id && selected_ki==ki);
                float ks = ksel ? 7.f : 5.f;
                ImU32 kc = ksel ? IM_COL32(255,220,80,255) : IM_COL32(120,220,140,230);
                // Diamond
                dl->AddQuadFilled({kx,ky-ks},{kx+ks,ky},{kx,ky+ks},{kx-ks,ky}, kc);
                dl->AddQuad({kx,ky-ks},{kx+ks,ky},{kx,ky+ks},{kx-ks,ky},
                             IM_COL32(255,255,255,120), 1.f);

                // Hit test — generous radius for usability
                bool near = std::abs(io.MousePos.x-kx)<9.f && std::abs(io.MousePos.y-ky)<9.f;
                if (near) any_key_near = true;
                if (hovered && near && !dragging) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    ImGui::SetTooltip("%.4g  (%.3fs)  drag to move",
                        std::clamp(k.value, mn, mx), k.time/44100.0);
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        selected_id = id; selected_ki = ki;
                        // Start drag immediately; kill cursor drag so it cannot interfere
                        time_cursor_dragging = false;
                        dragging = {id, ki, k.time, k.value};
                    }
                }
                // Delete selected key with Del
                if (ksel && !dragging && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                    anim.delete_key(id, ki);
                    selected_ki = -1; break;
                }
            }
            row_y += row_h;
        }

        // Handle drag — moves both time (X) and value (Y)
        if (dragging) {
            auto it = anim.curves.find(dragging->id);
            if (it != anim.curves.end() && dragging->key < (int)it->second.keys.size()) {
                float mn, mx;
                get_range(dragging->id, mn, mx);

                // Find which row this param is in so we can map Y to value
                float drag_row_y = cpos.y + 16.f;
                for (auto& rid : ids) {
                    if (rid == dragging->id) break;
                    if (anim.curves.count(rid) && anim.curves.at(rid).enabled)
                        drag_row_y += row_h;
                }

                double new_t = std::clamp(x_to_t(io.MousePos.x), 0.0, total_samples);
                float  new_v = y_to_v(io.MousePos.y, mn, mx, drag_row_y);

                auto& key = it->second.keys[dragging->key];
                key.time  = new_t;
                key.value = std::clamp(new_v, mn, mx);

                // Show live tooltip while dragging
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                ImGui::SetTooltip("%.4g  (%.3fs)", key.value, key.time/44100.0);

                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    // Sort keys by time on release (not mid-drag to keep index stable)
                    it->second._sort();
                    // Re-select: find the key we just placed by time proximity
                    double released_t = key.time;
                    selected_ki = 0;
                    for (int i = 0; i < (int)it->second.keys.size(); ++i) {
                        if (std::abs(it->second.keys[i].time - released_t) <
                            std::abs(it->second.keys[selected_ki].time - released_t))
                            selected_ki = i;
                    }
                    dragging.reset();
                }
            } else { dragging.reset(); }
        }

        // ── Time cursor (red vertical line + time label) ─────────────────────
        float cur_x = t_to_x(out_time_cursor);
        bool cursor_near = hovered && std::abs(io.MousePos.x - cur_x) < 10.f;
        ImU32 cur_col = cursor_near ? IM_COL32(255,130,130,255) : IM_COL32(255,80,80,220);
        dl->AddLine({cur_x, cpos.y}, {cur_x, cpos.y+csz.y}, cur_col, 2.f);

        // Time label above cursor
        {
            double secs = out_time_cursor / 44100.0;
            char tl[24];
            if (secs < 60.0) std::snprintf(tl,sizeof(tl),"%.3fs", secs);
            else             std::snprintf(tl,sizeof(tl),"%d:%06.3f",
                                           (int)(secs/60), std::fmod(secs,60.0));
            float lw = ImGui::GetFont()->CalcTextSizeA(
                           ImGui::GetFontSize(), FLT_MAX, 0.f, tl).x + 4.f;
            float lx = std::clamp(cur_x - lw*0.5f, cpos.x, cpos.x+csz.x-lw);
            dl->AddRectFilled({lx-2,cpos.y+1},{lx+lw+1,cpos.y+16},
                              IM_COL32(60,20,20,220), 2.f);
            dl->AddText({lx+1,cpos.y+2}, IM_COL32(255,160,160,255), tl);
        }

        // Drag cursor or click to position it
        if (cursor_near && !dragging) {
            if (ImGui::IsMouseClicked(0)) time_cursor_dragging = true;
        }
        if (time_cursor_dragging && !dragging) {
            out_time_cursor = std::clamp(x_to_t(io.MousePos.x), 0.0, total_samples);
            if (!ImGui::IsMouseDown(0)) time_cursor_dragging = false;
        } else if (dragging) {
            time_cursor_dragging = false;
        }
        // Clicking anywhere on canvas positions cursor — but NOT if a keyframe is nearby
        if (hovered && ImGui::IsMouseClicked(0) && !dragging
            && !time_cursor_dragging && !cursor_near && !any_key_near) {
            out_time_cursor = std::clamp(x_to_t(io.MousePos.x), 0.0, total_samples);
            time_cursor_dragging = true;
        }

        // Play head indicator (dim yellow)
        float ph_x = t_to_x(play_head);
        dl->AddLine({ph_x,cpos.y},{ph_x,cpos.y+csz.y}, IM_COL32(200,180,60,120), 1.f);

        // End-of-file marker
        float eof_x = t_to_x(total_samples);
        dl->AddLine({eof_x,cpos.y},{eof_x,cpos.y+csz.y}, IM_COL32(100,60,60,200), 1.5f);

        ImGui::EndChild();
        ImGui::End();
    }
};

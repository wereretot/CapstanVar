#pragma once
// ── Reel widget — standalone, isolated from all transport/button code ─────────
// Rotation driven by play_head sample delta — captures ALL transport effects
// (inertia, wow, flutter, motor health, shuttle speed) automatically.
// Scale: 1 rotation per ~2 seconds at 15 IPS, regardless of file length.

#include <imgui.h>
#include <cmath>
#include <algorithm>

struct ReelWidget {
    float  angle    = 0.f;
    double prev_pos = -1.0;   // -1 = uninitialised

    // Call when a new file is loaded to prevent a large delta on first frame.
    void reset(double start_pos = 0.0) {
        prev_pos = start_pos;
        angle    = 0.f;
    }

    // Samples per reel rotation at 1× speed (15 IPS).
    // 2 seconds × 44100 = 88200. Adjust for visual preference.
    static constexpr float SAMPS_PER_ROT = 88200.f;   // ~2s per rotation at 15 IPS

    void draw(double play_head,
              int    total_samples,
              bool   /*is_reversed*/,
              bool   is_moving,
              float  tape_speed,   // signed AudioIO speed (+ fwd / - rev)
              ImVec2 pos,
              ImVec2 size)
    {
        // ── Rotation delta ────────────────────────────────────────────────────
        // Use actual play_head movement in samples — this is the ground truth.
        // All effects (wow, flutter, inertia, shuttle) are captured automatically.
        float delta_samps = 0.f;

        if (is_moving) {
            if (prev_pos >= 0.0) {
                delta_samps = (float)(play_head - prev_pos);

                // During shuttle, DSP may not update play_head every UI frame.
                // If no play_head movement but shuttle is active, estimate from speed.
                if (std::abs(delta_samps) < 1.f && std::abs(tape_speed) > 1.5f) {
                    // Estimate: speed × samples_per_play_frame
                    // ~735 samples per frame at 60fps × shuttle_mult
                    delta_samps = tape_speed * (44100.f / 60.f);
                }
            } else {
                // First frame after start — use speed estimate
                delta_samps = tape_speed * (44100.f / 60.f);
            }
        }

        // Advance angle: fixed rad/sample regardless of file length
        angle += delta_samps * (2.f * 3.14159f / SAMPS_PER_ROT);
        prev_pos = play_head;

        // ── Progress for fill level ───────────────────────────────────────────
        float prog = (total_samples > 0)
            ? (float)std::clamp(play_head / (double)total_samples, 0.0, 1.0)
            : 0.f;

        // ── Layout ────────────────────────────────────────────────────────────
        // Geometry: ensure reels never visually overlap regardless of window size.
        // Gap between reel edges = size.x*0.5 - 2*R >= 20px minimum.
        float max_R = (size.x * 0.5f - 20.f) * 0.5f;   // largest R that maintains 20px gap
        float R     = std::min({size.y * 0.40f, size.x * 0.18f, max_R});
        float r     = R * 0.28f;
        float cx_s  = pos.x + size.x * 0.25f;
        float cx_t  = pos.x + size.x * 0.75f;
        float cy    = pos.y + size.y * 0.5f;

        auto* dl = ImGui::GetWindowDrawList();

        auto draw_reel = [&](float cx, float fill) {
            float r_tape = r + (R - r) * std::sqrt(std::clamp(fill, 0.f, 1.f));

            dl->AddCircle({cx, cy}, R, IM_COL32(70,70,80,200), 48, 1.5f);
            dl->AddCircleFilled({cx, cy}, r_tape, IM_COL32(40,30,15,220));
            dl->AddCircle({cx, cy}, r_tape, IM_COL32(150,110,35,200), 48, 1.f);
            dl->AddCircleFilled({cx, cy}, r, IM_COL32(55,55,65,255));
            dl->AddCircle({cx, cy}, r, IM_COL32(90,90,105,200), 16, 1.f);

            // Both reels rotate in the same direction (tape moves one way)
            for (int i = 0; i < 3; ++i) {
                float a  = angle + i * (3.14159f * 2.f / 3.f);
                float sx = cx + std::cos(a) * r * 0.9f;
                float sy = cy + std::sin(a) * r * 0.9f;
                float ex = cx + std::cos(a) * r_tape * 0.82f;
                float ey = cy + std::sin(a) * r_tape * 0.82f;
                dl->AddLine({sx, sy}, {ex, ey}, IM_COL32(120,90,40,200), 1.5f);
            }

            // Hub screws
            for (int i = 0; i < 3; ++i) {
                float a  = i * (3.14159f * 2.f / 3.f) + 0.5f;
                float sr = r * 0.55f;
                dl->AddCircleFilled(
                    {cx + std::cos(a)*sr, cy + std::sin(a)*sr},
                    1.5f, IM_COL32(100,100,115,220));
            }
        };

        draw_reel(cx_s, 1.f - prog);   // supply: full at start, empties as tape plays
        draw_reel(cx_t, prog);          // takeup: empty at start, fills as tape plays

        // Tape path
        float tape_y_top = cy - 1.5f, tape_y_bot = cy + 1.5f;
        float x_left = cx_s + R + 2.f, x_right = cx_t - R - 2.f;
        if (x_right > x_left)
            dl->AddRectFilled({x_left, tape_y_top}, {x_right, tape_y_bot},
                              IM_COL32(60,45,20,200));
    }
};

#pragma once
// ── CapstanVar Key Bindings Configuration ────────────────────────────────────
// Centralised key binding definitions for easy maintenance and UI display.
// All transport and file operation shortcuts are defined here.

#include <SFML/Window/Keyboard.hpp>

namespace Keys {
    // ── Transport controls ────────────────────────────────────────────────────
    inline constexpr sf::Keyboard::Key PLAY_FORWARD = sf::Keyboard::Right;
    inline constexpr sf::Keyboard::Key PLAY_FORWARD_ALT = sf::Keyboard::Up;
    inline constexpr sf::Keyboard::Key PLAY_REVERSE = sf::Keyboard::Left;
    inline constexpr sf::Keyboard::Key STOP         = sf::Keyboard::Down;
    inline constexpr sf::Keyboard::Key STOP_ALT     = sf::Keyboard::Escape;
    inline constexpr sf::Keyboard::Key PLAY_TOGGLE  = sf::Keyboard::Space;
    inline constexpr sf::Keyboard::Key PLAY_FWD_ALT = sf::Keyboard::Enter;
    inline constexpr sf::Keyboard::Key PLAY_REV_ALT = sf::Keyboard::BackSpace;
    inline constexpr sf::Keyboard::Key PLAY_REV_ALT2 = sf::Keyboard::R;
    
    // Shuttle controls (with Shift modifier)
    inline constexpr sf::Keyboard::Key SHUTTLE_REV  = sf::Keyboard::Left;   // + Shift
    inline constexpr sf::Keyboard::Key SHUTTLE_FWD  = sf::Keyboard::Right;  // + Shift
    
    // ── File operations ──────────────────────────────────────────────────────
    inline constexpr sf::Keyboard::Key OPEN_AUDIO   = sf::Keyboard::O;
    inline constexpr sf::Keyboard::Key SAVE_PROJECT = sf::Keyboard::S;
    inline constexpr sf::Keyboard::Key OPEN_PROJECT = sf::Keyboard::O;  // + Ctrl+Shift
    
    // ── Display strings for UI ───────────────────────────────────────────────
    namespace UI {
        inline constexpr const char* PLAY_FORWARD   = "Right";
        inline constexpr const char* PLAY_REVERSE   = "Left";
        inline constexpr const char* STOP           = "Down";
        inline constexpr const char* STOP_ALT       = "Esc";
        inline constexpr const char* PLAY_TOGGLE    = "Space";
        inline constexpr const char* SHUTTLE_REV    = "Shift+Left";
        inline constexpr const char* SHUTTLE_FWD    = "Shift+Right";
        inline constexpr const char* OPEN_AUDIO     = "Ctrl+O";
        inline constexpr const char* SAVE_PROJECT   = "Ctrl+S";
        inline constexpr const char* SAVE_PROJECT_AS = "Ctrl+Sh+S";
        inline constexpr const char* OPEN_PROJECT   = "Ctrl+Sh+O";
        inline constexpr const char* PLAY_REV_ALT   = "R";
    }
}

#pragma once
// ── CapstanVar application directory management ───────────────────────────────
// All persistent data lives under one root:
//
//   Linux/macOS : ~/.local/share/CapstanVar/
//   Windows     : %APPDATA%/CapstanVar
//
// Sub-directories created on first run:
//   projects/    — .cvproject files
//   presets/     — user .cvpr files
//   renders/     — default render output location
//   config/      — recents, favourites, window state
//
// Usage:
//   AppDirs::init();          // call once at startup
//   AppDirs::projects()       // returns full path string
//   AppDirs::config("recents.txt")  // returns full config file path

#include <string>
#include <filesystem>
#include <cstdlib>
#include <cstdio>

namespace fs = std::filesystem;

class AppDirs {
public:
    // Call once at startup — creates all directories if they don't exist.
    static void init() {
        _ensure(root());
        _ensure(projects());
        _ensure(presets());
        _ensure(renders());
        _ensure(config_dir());
        _migrate_dotfiles();
    }

    // ── Directory paths ───────────────────────────────────────────────────────
    static std::string root() {
        static std::string p = _compute_root();
        return p;
    }
    static std::string projects()   { return root() + sep() + "projects"; }
    static std::string presets()    { return root() + sep() + "presets";  }
    static std::string renders()    { return root() + sep() + "renders";  }
    static std::string config_dir() { return root() + sep() + "config";   }

    // ── Config file paths ─────────────────────────────────────────────────────
    static std::string config(const std::string& filename) {
        return config_dir() + sep() + filename;
    }
    static std::string recents_file()   { return config("recents.txt");   }
    static std::string favorites_file() { return config("favorites.txt"); }

private:
    static char sep() {
#ifdef _WIN32
        return '\\';
#else
        return '/';
#endif
    }

    static std::string _compute_root() {
#ifdef _WIN32
        const char* appdata = std::getenv("APPDATA");
        if (appdata) return std::string(appdata) + "\\CapstanVar";
        return "CapstanVar";
#else
        // Prefer XDG_DATA_HOME, fall back to ~/.local/share
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg && xdg[0] != '\0')
            return std::string(xdg) + "/CapstanVar";
        const char* home = std::getenv("HOME");
        if (home)
            return std::string(home) + "/.local/share/CapstanVar";
        return "CapstanVar";
#endif
    }

    static void _ensure(const std::string& path) {
        std::error_code ec;
        fs::create_directories(path, ec);
        if (ec)
            std::fprintf(stderr, "[AppDirs] could not create %s: %s\n",
                         path.c_str(), ec.message().c_str());
    }

    // Move any old HOME dotfiles into the new config dir (one-time migration).
    static void _migrate_dotfiles() {
        const char* home = std::getenv("HOME");
        if (!home) return;
        std::string h(home);

        struct { std::string old_path; std::string new_name; } migrations[] = {
            { h + "/.capstanvar_recents",   "recents.txt"   },
            { h + "/.capstanvar_favorites", "favorites.txt" },
            { h + "/.nagrav_recents",       "recents.txt"   },
            { h + "/.nagrav_favorites",     "favorites.txt" },
        };

        for (auto& m : migrations) {
            std::error_code ec;
            if (!fs::exists(m.old_path, ec)) continue;
            std::string dest = config_dir() + sep() + m.new_name;
            // Only migrate if destination doesn't already exist
            if (!fs::exists(dest, ec)) {
                fs::rename(m.old_path, dest, ec);
                if (!ec)
                    std::fprintf(stderr, "[AppDirs] migrated %s → %s\n",
                                 m.old_path.c_str(), dest.c_str());
            } else {
                // Destination exists — delete the old file so it's not confusing
                fs::remove(m.old_path, ec);
            }
        }
    }
};

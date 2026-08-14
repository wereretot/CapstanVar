#pragma once
// ── Self-contained ImGui File Browser ────────────────────────────────────────
// Features: path bar, breadcrumbs, search filter, sortable columns
//           (name / size / modified date), extension filter toggle,
//           places sidebar, recent files, directory navigation.

#include <imgui.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include "app_dirs.hpp"
#include <cctype>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;

// ── Recent files ──────────────────────────────────────────────────────────────
class RecentFiles {
public:
    static constexpr int MAX = 12;
    void load(const std::string& path) {
        _path = path; _items.clear();
        std::ifstream f(path); std::string line;
        while (std::getline(f, line) && (int)_items.size() < MAX)
            if (!line.empty()) _items.push_back(line);
    }
    void save() {
        if (_path.empty()) return;
        std::ofstream f(_path);
        for (auto& s : _items) f << s << "\n";
    }
    void push(const std::string& p) {
        _items.erase(std::remove(_items.begin(),_items.end(),p),_items.end());
        _items.insert(_items.begin(), p);
        if ((int)_items.size() > MAX) _items.resize(MAX);
        save();
    }
    const std::vector<std::string>& items() const { return _items; }
private:
    std::vector<std::string> _items;
    std::string _path;
};

// ── File dialog ───────────────────────────────────────────────────────────────
class FileDialog {
public:
    enum class Mode { Load, Save };
    static inline RecentFiles recents;

    void open_load(const char* title,
                   std::vector<std::string> extensions = {},
                   const std::string& start_dir = "")
    {
        _title      = title;
        _mode       = Mode::Load;
        _extensions = std::move(extensions);
        _open       = true;
        _result.clear();
        _filename_buf[0] = '\0';
        _search_buf[0]   = '\0';
        _error.clear();
        _show_all    = false;
        _sort_col    = SortCol::Name;
        _sort_asc    = true;
        _load_favorites();
        _navigate(start_dir.empty() ? _home() : start_dir);
    }

    void open_save(const char* title,
                   const std::string& default_name = "",
                   const std::string& start_dir = "")
    {
        _title      = title;
        _mode       = Mode::Save;
        _extensions.clear();
        _open        = true;
        _result.clear();
        _error.clear();
        _show_all    = true;
        _search_buf[0] = '\0';
        _sort_col    = SortCol::Name;
        _sort_asc    = true;
        _load_favorites();
        std::strncpy(_filename_buf, default_name.c_str(), sizeof(_filename_buf)-1);
        _navigate(start_dir.empty() ? _home() : start_dir);
    }

    bool draw() {
        if (!_open) return false;

        ImGui::SetNextWindowSize({860, 580}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImGui::GetMainViewport()->GetCenter(),
            ImGuiCond_FirstUseEver, {0.5f, 0.5f});

        bool dismissed = false;
        if (!ImGui::Begin(_title.c_str(), &_open, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            if (!_open) dismissed = true;
            return dismissed;
        }

        // ── Path bar + Up button ─────────────────────────────────────────────
        {
            static char path_edit[1024];
            if (!_editing_path)
                std::strncpy(path_edit, _current_dir.string().c_str(), sizeof(path_edit)-1);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 56);
            ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.85f,0.f,1.f});
            if (ImGui::InputText("##pathbar", path_edit, sizeof(path_edit),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            { _navigate(path_edit); _editing_path = false; }
            _editing_path = ImGui::IsItemActive();
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("^ Up", {50,0})) _go_up();
        }

        // ── Breadcrumbs ──────────────────────────────────────────────────────
        {
            fs::path acc; bool first = true;
            // `const auto&` (not `auto&`): std::filesystem::path::iterator
            // yields iterator-proxy values per C++20 P0532R9. libstdc++
            // currently permits `auto&` binding to the proxy as an
            // extension; Apple libc++ enforces the standard literally
            // and refuses. The loop body only reads `part`, so const-ref
            // (which can bind to a temporary proxy) is the minimal-change
            // cross-platform-compatible fix.
            for (const auto& part : _current_dir) {
                std::string s = part.string();
                if (s.empty()) continue;
                if (!first) { ImGui::SameLine(0,2); ImGui::TextUnformatted("/"); ImGui::SameLine(0,2); }
                first = false; acc /= part;
                fs::path nav = acc;
                ImGui::PushStyleColor(ImGuiCol_Button,        {0,0,0,0});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.2f,.2f,.3f,1});
                ImGui::PushStyleColor(ImGuiCol_Text,          {.5f,.75f,1.f,1.f});
                if (ImGui::SmallButton(s.c_str())) _navigate(nav.string());
                ImGui::PopStyleColor(3);
            }
        }

        // ── Search bar ───────────────────────────────────────────────────────
        {
            ImGui::PushStyleColor(ImGuiCol_Text, {.8f,.8f,.9f,1.f});
            ImGui::TextUnformatted("Search:");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            bool search_changed = ImGui::InputText("##search", _search_buf, sizeof(_search_buf));
            if (search_changed) { /* filter applied live in draw loop */ }

            ImGui::SameLine(0, 16);
            // Extension filter toggle
            if (_mode == Mode::Load && !_extensions.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    _show_all ? ImVec4(.15f,.4f,.15f,.8f) : ImVec4(.1f,.1f,.15f,.7f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    _show_all ? ImVec4(.4f,1.f,.4f,1.f) : ImVec4(.5f,.5f,.6f,1.f));
                if (ImGui::Button(_show_all ? "All files" : "Filtered", {90, 0})) {
                    _show_all = !_show_all; _refresh();
                }
                ImGui::PopStyleColor(2);
            }
        }

        if (!_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.3f,.3f,1.f});
            ImGui::Text("Error: %s", _error.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        // ── Two-column layout ────────────────────────────────────────────────
        float lw = 160.f;
        float bottom_reserve = ImGui::GetFrameHeightWithSpacing() * 2.f + 12.f;
        ImGui::BeginChild("##places", {lw, -bottom_reserve}, true,
                          ImGuiWindowFlags_HorizontalScrollbar);

        // Recents
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.7f,0.f,1.f});
        ImGui::TextUnformatted("RECENT"); ImGui::PopStyleColor();
        ImGui::Separator();

        std::vector<std::string> recent_copy = recents.items();
        std::string pending_recent;
        for (int i = 0; i < (int)recent_copy.size(); ++i) {
            const std::string& r = recent_copy[i];
            fs::path rp(r);
            std::string lbl = rp.filename().string();
            if (lbl.empty()) lbl = r;
            if (lbl.size() > 20) lbl = lbl.substr(0,18) + "..";
            ImGui::PushID(i);
            bool clicked = ImGui::Selectable(lbl.c_str());
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.c_str());
            if (clicked) { pending_recent = r; break; }
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, {.5f,.75f,1.f,1.f});
        ImGui::TextUnformatted("PLACES"); ImGui::PopStyleColor();
        ImGui::Separator();
        auto place = [&](const char* lbl, const std::string& path) {
            std::error_code ec;
            if (fs::is_directory(path, ec))
                if (ImGui::Selectable(lbl)) _navigate(path);
        };
        std::string home = _home();
        place("~ Home",      home);
        place("  Desktop",   home+"/Desktop");
        place("  Documents", home+"/Documents");
        place("  Music",     home+"/Music");
        place("  Downloads", home+"/Downloads");
        place("/ Root",      "/");

        // ── FAVORITES ────────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, {1.f,.65f,.2f,1.f});
        ImGui::TextUnformatted("FAVORITES"); ImGui::PopStyleColor();
        ImGui::SameLine();
        // Add current dir as favorite
        ImGui::PushStyleColor(ImGuiCol_Button,        {.2f,.35f,.1f,.8f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.3f,.5f,.15f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {.5f,1.f,.3f,1.f});
        if (ImGui::SmallButton("[+]")) {
            std::string cur = _current_dir.string();
            if (std::find(_favorites.begin(),_favorites.end(),cur)==_favorites.end()) {
                _favorites.push_back(cur);
                _save_favorites();
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bookmark current directory");
        ImGui::Separator();
        int fav_to_remove = -1;
        for (int fi = 0; fi < (int)_favorites.size(); ++fi) {
            fs::path fp(_favorites[fi]);
            std::string flbl = fp.filename().string();
            if (flbl.empty()) flbl = _favorites[fi];
            if (flbl.size() > 15) flbl = flbl.substr(0,13) + "..";
            ImGui::PushID(1000+fi);
            // Remove button
            ImGui::PushStyleColor(ImGuiCol_Button,  {.3f,.05f,.05f,.7f});
            ImGui::PushStyleColor(ImGuiCol_Text,    {.8f,.3f,.3f,1.f});
            if (ImGui::SmallButton("x")) fav_to_remove = fi;
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            std::error_code ec;
            bool exists = fs::is_directory(fp, ec);
            ImGui::PushStyleColor(ImGuiCol_Text,
                exists ? ImVec4(1.f,.65f,.2f,1.f) : ImVec4(.4f,.4f,.4f,1.f));
            if (ImGui::Selectable(flbl.c_str()) && exists) _navigate(_favorites[fi]);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", _favorites[fi].c_str());
            ImGui::PopID();
        }
        if (fav_to_remove >= 0) {
            _favorites.erase(_favorites.begin()+fav_to_remove);
            _save_favorites();
        }
        ImGui::EndChild();

        // Handle pending recent click
        if (!pending_recent.empty()) {
            fs::path rp(pending_recent); std::error_code ec;
            if (fs::is_directory(rp, ec)) {
                _navigate(pending_recent);
            } else if (fs::is_regular_file(rp, ec)) {
                _navigate(rp.parent_path().string());
                std::strncpy(_filename_buf, rp.filename().string().c_str(), sizeof(_filename_buf)-1);
                if (_mode == Mode::Load) {
                    _result = pending_recent; recents.push(pending_recent);
                    _open = false; ImGui::End(); return true;
                }
            }
        }

        ImGui::SameLine();

        // ── File list with sortable header ───────────────────────────────────
        ImGui::BeginChild("##files", {0, -bottom_reserve}, true);

        // Build filtered + search-filtered view
        std::string search_lower = _lower(std::string(_search_buf));

        // Column header with sort buttons
        auto sort_btn = [&](const char* lbl, SortCol col, float w) {
            bool active = (_sort_col == col);
            ImGui::PushStyleColor(ImGuiCol_Text,
                active ? ImVec4(1.f,.85f,.0f,1.f) : ImVec4(.55f,.55f,.65f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Button,        {0,0,0,0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.2f,.2f,.3f,.8f});
            char hdr[32];
            if (active)
                std::snprintf(hdr, sizeof(hdr), "%s %s", lbl, _sort_asc ? "v" : "^");
            else
                std::snprintf(hdr, sizeof(hdr), "%s", lbl);
            if (ImGui::Button(hdr, {w, 0})) {
                if (_sort_col == col) _sort_asc = !_sort_asc;
                else { _sort_col = col; _sort_asc = (col == SortCol::Name); }
                _sort_entries();
            }
            ImGui::PopStyleColor(3);
        };

        sort_btn("Name",     SortCol::Name,     260);  ImGui::SameLine();
        sort_btn("Size",     SortCol::Size,       80);  ImGui::SameLine();
        sort_btn("Modified", SortCol::Modified,  160);
        ImGui::Separator();

        std::string nav_target, confirm_path;

        for (auto& e : _entries) {
            // Search filter
            if (!search_lower.empty()) {
                if (_lower(e.name).find(search_lower) == std::string::npos)
                    continue;
            }

            bool selected = (e.name == std::string(_filename_buf));
            ImGui::PushStyleColor(ImGuiCol_Text,
                e.is_dir ? ImVec4(.4f,.8f,1.f,1.f) : ImVec4(.91f,.91f,.94f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Header,        {.2f,.3f,.4f,1.f});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, {.25f,.35f,.45f,1.f});

            // Name column
            char name_lbl[280];
            if (e.is_dir) std::snprintf(name_lbl, sizeof(name_lbl), "[DIR] %s", e.name.c_str());
            else          std::snprintf(name_lbl, sizeof(name_lbl), "      %s",  e.name.c_str());

            ImGui::PushID(e.name.c_str());
            bool clicked = ImGui::Selectable(name_lbl, selected,
                               ImGuiSelectableFlags_AllowDoubleClick, {260, 0});
            ImGui::PopID();
            ImGui::PopStyleColor(3);

            // Size column
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, {.5f,.5f,.6f,1.f});
            if (!e.is_dir) {
                char sz[16]; _fmt_size(e.size, sz);
                ImGui::TextUnformatted(sz);
            } else {
                ImGui::TextUnformatted("     ");
            }
            ImGui::SameLine(0, 8);

            // Modified date column
            char date_str[32] = "--";
            if (e.mtime != 0) {
                std::time_t t = (std::time_t)e.mtime;
                std::tm* tm = std::localtime(&t);
                if (tm) std::strftime(date_str, sizeof(date_str), "%Y-%m-%d  %H:%M", tm);
            }
            ImGui::TextUnformatted(date_str);
            ImGui::PopStyleColor();

            if (clicked) {
                if (e.is_dir) {
                    if (ImGui::IsMouseDoubleClicked(0)) nav_target = e.path.string();
                } else {
                    std::strncpy(_filename_buf, e.name.c_str(), sizeof(_filename_buf)-1);
                    if (ImGui::IsMouseDoubleClicked(0) && _mode == Mode::Load)
                        confirm_path = (fs::path(_current_dir) / e.name).string();
                }
            }
        }

        ImGui::EndChild();

        if (!nav_target.empty())  _navigate(nav_target);
        if (!confirm_path.empty()) {
            _result = confirm_path; recents.push(_result);
            _open = false; ImGui::End(); return true;
        }

        // ── Bottom bar ───────────────────────────────────────────────────────
        float btn_w = 90.f;
        ImGui::SetNextItemWidth(std::max(50.f,
            ImGui::GetContentRegionAvail().x - btn_w*2 - 12));
        // Editable in both modes — Load allows typing a filename directly
        ImGui::InputText("##fname", _filename_buf, sizeof(_filename_buf));

        ImGui::SameLine();
        const char* ok_lbl = (_mode == Mode::Load) ? "Open" : "Save";
        ImGui::PushStyleColor(ImGuiCol_Button,        {.3f,.1f,.5f,.8f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {.4f,.15f,.65f,1.f});
        ImGui::PushStyleColor(ImGuiCol_Text,          {.8f,.5f,1.f,1.f});
        if (ImGui::Button(ok_lbl, {btn_w, 0})) {
            if (_filename_buf[0]) {
                _result = (fs::path(_current_dir) / _filename_buf).string();
                if (_mode == Mode::Load) recents.push(_result);
                _open = false; dismissed = true;
            }
        }
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {btn_w, 0})) {
            _result.clear(); _open = false; dismissed = true;
        }

        ImGui::End();
        return dismissed;
    }

    bool        is_open() const { return _open; }
    std::string result()  const { return _result; }

private:
    enum class SortCol { Name, Size, Modified };

    struct Entry {
        std::string name;
        fs::path    path;
        bool        is_dir  = false;
        uintmax_t   size    = 0;
        int64_t     mtime   = 0;   // unix timestamp
    };

    std::string              _title;
    Mode                     _mode     = Mode::Load;
    fs::path                 _current_dir;
    std::vector<std::string> _extensions;
    std::vector<Entry>       _entries;
    char                     _filename_buf[512] = {};
    char                     _search_buf[128]   = {};
    bool                     _open        = false;
    bool                     _show_all    = false;
    bool                     _editing_path= false;
    SortCol                  _sort_col    = SortCol::Name;
    bool                     _sort_asc    = true;
    std::string              _result;
    std::string              _error;
    std::vector<std::string> _favorites;   // user-bookmarked directories
    std::string              _fav_path;    // persistence file path

    static std::string _home() {
        const char* h = std::getenv("HOME"); return h ? h : "/";
    }

    void _navigate(const std::string& path) {
        fs::path p(path); std::error_code ec;
        if (!fs::is_directory(p, ec)) { _error = "Not a directory: " + path; return; }
        _current_dir = fs::canonical(p, ec);
        if (ec) _current_dir = p;
        _error.clear();
        _refresh();
    }

    static std::string _lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return s;
    }

    bool _ext_matches(const fs::path& p) const {
        if (_show_all || _extensions.empty()) return true;
        std::string ext = _lower(p.extension().string());
        for (auto& e : _extensions) if (_lower(e) == ext) return true;
        return false;
    }

    void _refresh() {
        _entries.clear(); _error.clear();
        std::error_code ec;
        auto it = fs::directory_iterator(_current_dir, ec);
        if (ec) { _error = "Cannot read directory: " + ec.message(); return; }

        for (; it != fs::end(it); it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            std::error_code ec2;
            std::string name = it->path().filename().string();
            if (name.empty() || name[0] == '.') continue;

            bool is_dir = it->is_directory(ec2); if (ec2) { ec2.clear(); is_dir=false; }
            if (!is_dir && !_ext_matches(it->path())) continue;

            uintmax_t sz = 0;
            int64_t mtime = 0;
            if (!is_dir) {
                sz = fs::file_size(it->path(), ec2); ec2.clear();
            }
            // Modification time — works for both files and dirs
            auto ftime = fs::last_write_time(it->path(), ec2);
            if (!ec2) {
                // Convert file_time_type to unix timestamp
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now()
                          + std::chrono::system_clock::now());
                mtime = std::chrono::duration_cast<std::chrono::seconds>(
                    sctp.time_since_epoch()).count();
            }

            _entries.push_back({name, it->path(), is_dir, sz, mtime});
        }
        _sort_entries();
    }

    void _sort_entries() {
        std::stable_sort(_entries.begin(), _entries.end(),
            [&](const Entry& a, const Entry& b) {
                // Directories always first
                if (a.is_dir != b.is_dir) return (int)a.is_dir > (int)b.is_dir;
                int cmp = 0;
                switch (_sort_col) {
                case SortCol::Name: {
                    std::string al = _lower(a.name), bl = _lower(b.name);
                    cmp = al < bl ? -1 : al > bl ? 1 : 0;
                    break; }
                case SortCol::Size:
                    cmp = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
                case SortCol::Modified:
                    cmp = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
                }
                return _sort_asc ? cmp < 0 : cmp > 0;
            });
    }

    void _load_favorites() {
        if (_fav_path.empty())
            _fav_path = AppDirs::favorites_file();
        _favorites.clear();
        std::ifstream f(_fav_path); std::string line;
        while (std::getline(f, line))
            if (!line.empty()) _favorites.push_back(line);
    }
    void _save_favorites() {
        if (_fav_path.empty()) return;
        std::ofstream f(_fav_path);
        for (auto& s : _favorites) f << s << "\n";
    }

    void _go_up() {
        auto parent = _current_dir.parent_path();
        if (parent != _current_dir) _navigate(parent.string());
    }

    static void _fmt_size(uintmax_t sz, char* buf) {
        if      (sz < 1024ULL)           std::snprintf(buf,16,"%llu B",(unsigned long long)sz);
        else if (sz < 1024ULL*1024)      std::snprintf(buf,16,"%.1f KB",sz/1024.0);
        else if (sz < 1024ULL*1024*1024) std::snprintf(buf,16,"%.1f MB",sz/(1024.0*1024));
        else                             std::snprintf(buf,16,"%.2f GB",sz/(1024.0*1024*1024));
    }
};

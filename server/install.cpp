// `ccbus install-hooks`: merges ccbus hook entries into Claude Code's settings.json.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "commands.hpp"
#include "util.hpp"

namespace ccbus {

namespace {

const char* kEvents[] = {"SessionStart", "UserPromptSubmit", "PostToolUse",
                         "Stop",         "Notification",     "SessionEnd"};

bool is_ccbus_hook(const json& h) {
    if (!h.is_object()) return false;
    std::string cmd = h.value("command", "");
    return cmd.find("ccbus") != std::string::npos && cmd.size() >= 5 &&
           cmd.compare(cmd.size() - 5, 5, " hook") == 0;
}

// Removes every ccbus hook from settings["hooks"], pruning empty groups/events.
void strip(json& settings) {
    if (!settings.contains("hooks") || !settings["hooks"].is_object()) return;
    json& hooks = settings["hooks"];
    for (auto it = hooks.begin(); it != hooks.end();) {
        json& groups = it.value();
        if (groups.is_array()) {
            for (auto g = groups.begin(); g != groups.end();) {
                if (g->is_object() && (*g)["hooks"].is_array()) {
                    json& list = (*g)["hooks"];
                    for (auto h = list.begin(); h != list.end();)
                        h = is_ccbus_hook(*h) ? list.erase(h) : h + 1;
                    if (list.empty()) {
                        g = groups.erase(g);
                        continue;
                    }
                }
                ++g;
            }
        }
        it = groups.is_array() && groups.empty() ? hooks.erase(it) : std::next(it);
    }
    if (hooks.empty()) settings.erase("hooks");
}

}  // namespace

int cmd_install_hooks(const std::string& settings_path, bool uninstall) {
    namespace fs = std::filesystem;
    std::string path = settings_path.empty() ? home_dir() + "/.claude/settings.json" : settings_path;

    json settings = json::object();
    if (fs::exists(path)) {
        std::ifstream f(path);
        std::stringstream ss;
        ss << f.rdbuf();
        settings = json::parse(ss.str(), nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            std::cerr << "ccbus: " << path << " is not a JSON object; not touching it\n";
            return 1;
        }
        std::error_code ec;
        fs::copy_file(path, path + ".bak-ccbus", fs::copy_options::overwrite_existing, ec);
    }

    strip(settings);
    if (!uninstall) {
        std::string command = self_exe() + " hook";
        for (const char* ev : kEvents) {
            json group = {{"hooks", json::array({{{"type", "command"}, {"command", command}, {"timeout", 10}}})}};
            if (std::string(ev) == "PostToolUse") group["matcher"] = "*";
            settings["hooks"][ev].push_back(group);
        }
    }

    fs::create_directories(fs::path(path).parent_path());
    std::string tmp = path + ".tmp-ccbus";
    {
        std::ofstream out(tmp);
        out << settings.dump(2) << "\n";
        if (!out) {
            std::cerr << "ccbus: failed to write " << tmp << "\n";
            return 1;
        }
    }
    fs::rename(tmp, path);
    std::cout << (uninstall ? "Removed ccbus hooks from " : "Installed ccbus hooks into ") << path
              << "\n";
    if (fs::exists(path + ".bak-ccbus")) std::cout << "Backup: " << path << ".bak-ccbus\n";
    return 0;
}

}  // namespace ccbus

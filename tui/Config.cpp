#include "Config.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "../server/util.hpp"

namespace tui {

namespace {

json read_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return json::object();
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false, true);
    return j.is_object() ? j : json::object();
}

std::string config_dir() {
    const char* x = getenv("XDG_CONFIG_HOME");
    return (x && *x ? std::string(x) : ccbus::home_dir() + "/.config") + "/ccbus";
}

}  // namespace

Config load_config() {
    Config c;
    c.path = config_dir() + "/config.json";
    json j = read_json(c.path);
    for (const json& h : j.value("hosts", json::array())) {
        if (!h.is_object()) continue;
        HostConfig hc{h.value("name", ""), h.value("ssh", ""), h.value("command", "")};
        if (hc.name.empty()) hc.name = hc.ssh.empty() ? "local" : hc.ssh;
        c.hosts.push_back(hc);
    }
    if (c.hosts.empty()) c.hosts.push_back({"local", "", ""});
    c.notify = j.value("notify", true);
    return c;
}

State::State() : path_(ccbus::state_dir() + "/tui.json"), j_(read_json(path_)) {}

int64_t State::notified(const std::string& host) const {
    auto it = j_.find("notified");
    if (it == j_.end() || !it->is_object()) return 0;
    return it->value(host, int64_t{0});
}

void State::set_notified(const std::string& host, int64_t seq) {
    if (notified(host) == seq) return;
    j_["notified"][host] = seq;
    save();
}

std::vector<std::string> State::recent_dirs(const std::string& host) const {
    std::vector<std::string> out;
    auto it = j_.find("recent_dirs");
    if (it == j_.end() || !it->is_object()) return out;
    for (const json& d : it->value(host, json::array()))
        if (d.is_string()) out.push_back(d);
    return out;
}

void State::add_recent_dir(const std::string& host, const std::string& dir) {
    std::vector<std::string> dirs = recent_dirs(host);
    std::erase(dirs, dir);
    dirs.insert(dirs.begin(), dir);
    if (dirs.size() > 15) dirs.resize(15);
    j_["recent_dirs"][host] = dirs;
    save();
}

void State::save() const {
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return;
        f << j_.dump(2) << "\n";
    }
    std::rename(tmp.c_str(), path_.c_str());
}

}  // namespace tui

#pragma once
// ~/.config/ccbus/config.json (hosts, options) and ~/.local/state/ccbus/tui.json
// (what has been notified, recent directories).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "Host.hpp"

namespace tui {

struct Config {
    std::vector<HostConfig> hosts;
    bool notify = true;
    std::string path;  // where it was (or would be) loaded from
};

Config load_config();

class State {
public:
    State();
    int64_t notified(const std::string& host) const;
    void set_notified(const std::string& host, int64_t seq);
    std::vector<std::string> recent_dirs(const std::string& host) const;
    void add_recent_dir(const std::string& host, const std::string& dir);

private:
    void save() const;
    std::string path_;
    json j_;
};

}  // namespace tui

#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace ccbus {

using json = nlohmann::json;

constexpr int kProtoVersion = 1;
constexpr const char* kVersion = "0.1.0";

// Serialize without throwing on invalid UTF-8 (pane captures can contain anything).
inline std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string home_dir();
std::string state_dir();  // ~/.local/state/ccbus, created on demand
std::string db_path();    // $CCBUS_DB or state_dir()/ccbus.db
std::string expand_tilde(const std::string& path);
std::string basename_of(const std::string& path);
std::string self_exe();
int64_t now_ms();

// Cut to at most max_bytes without splitting a UTF-8 sequence; appends "…" if cut.
std::string truncate_utf8(const std::string& s, size_t max_bytes);
// Session names double as tmux session names: [A-Za-z0-9_-]{1,64}.
bool valid_name(const std::string& name);
std::string shell_quote(const std::string& s);

}  // namespace ccbus

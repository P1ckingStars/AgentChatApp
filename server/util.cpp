#include "util.hpp"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <filesystem>

#include <pwd.h>
#include <unistd.h>

namespace ccbus {

std::string home_dir() {
    if (const char* h = std::getenv("HOME"); h && *h) return h;
    if (const passwd* pw = getpwuid(getuid())) return pw->pw_dir;
    return "/tmp";
}

std::string state_dir() {
    std::string base;
    if (const char* x = std::getenv("XDG_STATE_HOME"); x && *x)
        base = x;
    else
        base = home_dir() + "/.local/state";
    std::string dir = base + "/ccbus";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string db_path() {
    if (const char* p = std::getenv("CCBUS_DB"); p && *p) return p;
    return state_dir() + "/ccbus.db";
}

std::string expand_tilde(const std::string& path) {
    if (path == "~") return home_dir();
    if (path.rfind("~/", 0) == 0) return home_dir() + path.substr(1);
    return path;
}

std::string basename_of(const std::string& path) {
    std::string s = path;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    auto i = s.find_last_of('/');
    return i == std::string::npos ? s : s.substr(i + 1);
}

std::string self_exe() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "ccbus";
    return std::string(buf, static_cast<size_t>(n));
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n) + "…";
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string shell_quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) {
        if (c == '\'')
            r += "'\\''";
        else
            r += c;
    }
    return r + "'";
}

}  // namespace ccbus

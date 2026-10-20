#pragma once
// One connection to `ccbus serve --stdio`, run locally or through ssh. Reader threads
// only post lines to the UI thread; everything else happens on the UI thread.

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

#include <nlohmann/json.hpp>

namespace tui {

using json = nlohmann::json;
using Task = std::function<void()>;
using PostFn = std::function<void(Task)>;

struct HostConfig {
    std::string name;
    std::string ssh;      // empty: this machine
    std::string command;  // override for the serve command
    bool local() const { return ssh.empty(); }
};

class Host {
public:
    using Ok = std::function<void(const json&)>;
    using Fail = std::function<void(const std::string&)>;

    Host(HostConfig cfg, PostFn post);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    std::function<void(bool connected)> on_connection;
    std::function<void(const std::vector<json>& sessions, int64_t seq)> on_snapshot;
    std::function<void(const std::string& kind, const json& session, int64_t seq)> on_session;
    std::function<void(const std::string& key)> on_removed;
    std::function<void(const json& frame)> on_transcript;
    std::function<void(const std::string& op, const std::string& error)> on_error;

    void tick(int64_t now);  // reconnects, pings, detects a dead link
    // Without `fail`, errors go to on_error.
    void request(const std::string& op, json params = json::object(), Ok ok = {}, Fail fail = {});
    void watch(const std::string& key);
    void unwatch();

    const HostConfig& config() const { return cfg_; }
    const std::string& name() const { return cfg_.name; }
    bool connected() const { return connected_; }
    const std::string& last_error() const { return error_; }
    int64_t retry_at() const { return retry_at_; }
    // argv that shows the pane in the current terminal.
    std::vector<std::string> attach_argv(const std::string& pane) const;

private:
    void launch();
    void handle_line(const std::string& line);
    void closed(int gen);
    void write_line(const std::string& s);

    HostConfig cfg_;
    PostFn post_;
    pid_t pid_ = -1;
    int in_fd_ = -1;
    int gen_ = 0;
    bool running_ = false;
    bool connected_ = false;
    std::string error_;
    std::string stderr_last_;
    int64_t retry_at_ = 0;
    int64_t backoff_ = 1000;
    int64_t last_rx_ = 0;
    int64_t last_ping_ = 0;
    int next_id_ = 1;
    std::string watch_key_;
    struct Pending {
        std::string op;
        Ok ok;
        Fail fail;
    };
    std::map<int, Pending> pending_;
};

}  // namespace tui

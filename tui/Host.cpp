#include "Host.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../server/util.hpp"

namespace tui {

namespace {

constexpr int kProto = 1;
constexpr int64_t kPingMs = 15000;
constexpr int64_t kDeadMs = 45000;
constexpr int64_t kMaxBackoffMs = 30000;

std::string local_ccbus() {
    std::string self = ccbus::self_exe();
    std::string dir = self.substr(0, self.rfind('/') + 1);
    std::string sibling = dir + "ccbus";
    return !dir.empty() && access(sibling.c_str(), X_OK) == 0 ? sibling : "ccbus";
}

// Reads fd until EOF, posting each complete line.
void pump(int fd, PostFn post, std::function<void(std::string)> on_line, Task on_eof) {
    std::thread([fd, post, on_line, on_eof] {
        std::string buf;
        char chunk[65536];
        while (true) {
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            buf.append(chunk, static_cast<size_t>(n));
            size_t start = 0, nl;
            while ((nl = buf.find('\n', start)) != std::string::npos) {
                std::string line = buf.substr(start, nl - start);
                start = nl + 1;
                if (!line.empty()) post([on_line, line] { on_line(line); });
            }
            buf.erase(0, start);
        }
        close(fd);
        if (on_eof) post(on_eof);
    }).detach();
}

}  // namespace

Host::Host(HostConfig cfg, PostFn post) : cfg_(std::move(cfg)), post_(std::move(post)) {}

Host::~Host() {
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        waitpid(pid_, nullptr, 0);
    }
    if (in_fd_ >= 0) close(in_fd_);
}

std::vector<std::string> Host::attach_argv(const std::string& pane) const {
    std::vector<std::string> tmux = {"tmux"};
    if (const char* sock = getenv("CCBUS_TMUX_SOCKET"); sock && *sock && cfg_.local()) {
        tmux.push_back("-L");
        tmux.push_back(sock);
    }
    tmux.insert(tmux.end(), {"attach-session", "-t", pane});
    if (cfg_.local()) return tmux;
    std::vector<std::string> argv = {"ssh", "-t", cfg_.ssh};
    argv.insert(argv.end(), tmux.begin(), tmux.end());
    return argv;
}

void Host::launch() {
    std::vector<std::string> argv;
    if (cfg_.local()) {
        if (cfg_.command.empty())
            argv = {local_ccbus(), "serve", "--stdio"};
        else
            argv = {"/bin/sh", "-c", cfg_.command};
    } else {
        // BatchMode: we can't show a password prompt, so keys or an agent must work.
        argv = {"ssh", "-T", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=15", "-o",
                "ServerAliveCountMax=3", cfg_.ssh,
                cfg_.command.empty() ? "~/.local/bin/ccbus serve --stdio" : cfg_.command};
    }

    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) {
        error_ = std::string("pipe: ") + strerror(errno);
        retry_at_ = ccbus::now_ms() + backoff_;
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();  // keep terminal signals (Ctrl-C while attached) away from ssh
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(a.data());
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        dprintf(2, "cannot run %s: %s\n", cargv[0], strerror(errno));
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (pid < 0) {
        close(in[1]);
        close(out[0]);
        close(err[0]);
        error_ = std::string("fork: ") + strerror(errno);
        retry_at_ = ccbus::now_ms() + backoff_;
        return;
    }

    pid_ = pid;
    in_fd_ = in[1];
    running_ = true;
    int gen = ++gen_;
    stderr_last_.clear();
    last_rx_ = ccbus::now_ms();
    last_ping_ = last_rx_;

    pump(out[0], post_, [this, gen](std::string l) { if (gen == gen_) handle_line(l); }, [this, gen] { closed(gen); });
    pump(err[0], post_, [this, gen](std::string l) { if (gen == gen_) stderr_last_ = l; }, {});

    request("hello", {{"proto", kProto}}, [this](const json& hello) {
        if (hello.value("proto", 0) != kProto) {
            error_ = "protocol mismatch (server " + std::to_string(hello.value("proto", 0)) + ", client " +
                     std::to_string(kProto) + "): update ccbus on " + cfg_.name;
            if (pid_ > 0) kill(pid_, SIGTERM);
            return;
        }
        request("list_sessions", json::object(), [this](const json& r) {
            std::vector<json> sessions;
            for (const json& s : r.value("sessions", json::array())) sessions.push_back(s);
            int64_t seq = r.value("seq", int64_t{0});
            connected_ = true;
            backoff_ = 1000;
            error_.clear();
            if (on_snapshot) on_snapshot(sessions, seq);
            request("subscribe", {{"since_seq", seq}});
            if (!watch_key_.empty()) request("watch", {{"key", watch_key_}});
            if (on_connection) on_connection(true);
        });
    });
}

void Host::closed(int gen) {
    if (gen != gen_ || !running_) return;
    running_ = false;
    if (pid_ > 0) {
        int status = 0;
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGTERM);
            waitpid(pid_, &status, 0);
        }
    }
    pid_ = -1;
    if (in_fd_ >= 0) close(in_fd_);
    in_fd_ = -1;
    if (error_.empty()) error_ = stderr_last_.empty() ? "disconnected" : stderr_last_;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [id, p] : pending)
        if (p.fail) p.fail("connection lost");
    bool was = connected_;
    connected_ = false;
    retry_at_ = ccbus::now_ms() + backoff_;
    backoff_ = std::min(backoff_ * 2, kMaxBackoffMs);
    if (on_connection && was) on_connection(false);
}

void Host::tick(int64_t now) {
    if (!running_) {
        if (now >= retry_at_) launch();
        return;
    }
    if (now - last_rx_ > kDeadMs) {
        error_ = "connection timed out";
        if (pid_ > 0) kill(pid_, SIGTERM);
        return;
    }
    if (connected_ && now - last_ping_ > kPingMs) {
        last_ping_ = now;
        request("ping");
    }
}

void Host::write_line(const std::string& s) {
    if (in_fd_ < 0) return;
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = write(in_fd_, s.data() + off, s.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;  // EPIPE: the reader thread will report the close
        off += static_cast<size_t>(n);
    }
}

void Host::request(const std::string& op, json params, Ok ok, Fail fail) {
    if (!running_) {
        std::string why = "not connected to " + cfg_.name;
        if (fail) fail(why);
        else if (on_error) on_error(op, why);
        return;
    }
    int id = next_id_++;
    params["id"] = id;
    params["op"] = op;
    pending_[id] = {op, std::move(ok), std::move(fail)};
    write_line(ccbus::dump(params) + "\n");
}

void Host::watch(const std::string& key) {
    watch_key_ = key;
    if (connected_) request("watch", {{"key", key}});
}

void Host::unwatch() {
    if (watch_key_.empty()) return;
    watch_key_.clear();
    if (connected_) request("unwatch");
}

void Host::handle_line(const std::string& line) {
    last_rx_ = ccbus::now_ms();
    json o = json::parse(line, nullptr, false);
    if (o.is_discarded() || !o.is_object()) return;

    if (o.contains("re")) {
        if (!o["re"].is_number_integer()) return;
        auto it = pending_.find(o["re"].get<int>());
        if (it == pending_.end()) return;
        Pending p = std::move(it->second);
        pending_.erase(it);
        if (o.value("ok", false)) {
            if (p.ok) p.ok(o);
        } else {
            std::string err = o.value("error", "request failed");
            if (p.fail) p.fail(err);
            else if (on_error) on_error(p.op, err);
        }
        return;
    }
    std::string ev = o.value("ev", "");
    if (ev == "session" && on_session)
        on_session(o.value("kind", ""), o.value("session", json::object()), o.value("seq", int64_t{0}));
    else if (ev == "removed" && on_removed)
        on_removed(o.value("key", ""));
    else if (ev == "transcript" && on_transcript)
        on_transcript(o);
}

}  // namespace tui

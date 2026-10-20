#include "tmux.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.hpp"

namespace ccbus {

namespace {

void write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += static_cast<size_t>(n);
    }
}

}  // namespace

RunResult run(const std::vector<std::string>& argv_in, const std::string* input) {
    // CCBUS_TMUX_SOCKET=name puts ccbus sessions on their own tmux server (tmux -L name).
    std::vector<std::string> argv = argv_in;
    const char* sock = std::getenv("CCBUS_TMUX_SOCKET");
    if (sock && *sock && !argv.empty() && argv[0] == "tmux") argv.insert(argv.begin() + 1, {"-L", sock});

    RunResult r;
    int in[2], out[2], err[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC)) {
        r.err = "pipe failed";
        return r;
    }
    pid_t pid = fork();
    if (pid < 0) {
        r.err = "fork failed";
        return r;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(err[1]);
    if (input) write_all(in[1], *input);
    close(in[1]);

    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    std::string* sinks[2] = {&r.out, &r.err};
    int open_fds = 2;
    char buf[8192];
    while (open_fds > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0) {
                sinks[i]->append(buf, static_cast<size_t>(n));
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (r.status == 127 && r.err.empty()) r.err = argv[0] + ": command not found";
    return r;
}

namespace tmux {

std::vector<std::string> claude_argv(const std::vector<std::string>& claude_args) {
    const char* sh = std::getenv("SHELL");
    std::string shell = sh && *sh ? sh : "/bin/sh";
    const char* cc = std::getenv("CCBUS_CLAUDE");
    std::string cmd = cc && *cc ? cc : "claude";
    for (const auto& a : claude_args) cmd += " " + shell_quote(a);
    return {shell, "-ic", "exec " + cmd};
}

RunResult new_session(const std::string& name, const std::string& cwd,
                      const std::vector<std::string>& claude_args) {
    std::vector<std::string> argv = {"tmux", "new-session", "-d", "-P", "-F", "#{pane_id}",
                                     "-s", "cc-" + name, "-c", cwd,
                                     "-e", "CCBUS_NAME=" + name, "-x", "200", "-y", "50"};
    for (auto& a : claude_argv(claude_args)) argv.push_back(std::move(a));
    RunResult r = run(argv);
    while (!r.out.empty() && (r.out.back() == '\n' || r.out.back() == '\r')) r.out.pop_back();
    // Keep the pane (marked dead) when claude exits, so it can be inspected and respawned.
    if (r.ok()) run({"tmux", "set-option", "-w", "-t", r.out, "remain-on-exit", "on"});
    return r;
}

RunResult respawn(const std::string& pane, const std::string& cwd,
                  const std::vector<std::string>& claude_args) {
    std::vector<std::string> argv = {"tmux", "respawn-pane", "-k", "-t", pane, "-c", cwd};
    for (auto& a : claude_argv(claude_args)) argv.push_back(std::move(a));
    return run(argv);
}

RunResult send_text(const std::string& pane, const std::string& text) {
    if (!text.empty()) {
        std::string buffer = "ccbus-" + std::to_string(getpid());
        RunResult r = run({"tmux", "load-buffer", "-b", buffer, "-"}, &text);
        if (!r.ok()) return r;
        // -p: bracketed paste, so embedded newlines don't submit early.
        r = run({"tmux", "paste-buffer", "-p", "-d", "-b", buffer, "-t", pane});
        if (!r.ok()) return r;
        // Let the TUI finish ingesting the paste before Enter arrives.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return run({"tmux", "send-keys", "-t", pane, "Enter"});
}

RunResult send_keys(const std::string& pane, const std::vector<std::string>& keys) {
    std::vector<std::string> argv = {"tmux", "send-keys", "-t", pane};
    argv.insert(argv.end(), keys.begin(), keys.end());
    return run(argv);
}

RunResult capture(const std::string& pane, bool ansi) {
    if (ansi) return run({"tmux", "capture-pane", "-p", "-J", "-e", "-t", pane});
    return run({"tmux", "capture-pane", "-p", "-J", "-t", pane});
}

RunResult kill_pane(const std::string& pane) { return run({"tmux", "kill-pane", "-t", pane}); }

bool list_panes(std::map<std::string, bool>& panes) {
    panes.clear();
    RunResult r = run({"tmux", "list-panes", "-a", "-F", "#{pane_id} #{pane_dead}"});
    if (!r.ok()) {
        // "no server running on ..." / "error connecting to ..." => no panes at all.
        return r.err.find("no server running") != std::string::npos ||
               r.err.find("error connecting") != std::string::npos;
    }
    size_t pos = 0;
    while (pos < r.out.size()) {
        size_t nl = r.out.find('\n', pos);
        if (nl == std::string::npos) nl = r.out.size();
        std::string line = r.out.substr(pos, nl - pos);
        auto sp = line.find(' ');
        if (sp != std::string::npos) panes[line.substr(0, sp)] = line.substr(sp + 1) == "0";
        pos = nl + 1;
    }
    return true;
}

}  // namespace tmux
}  // namespace ccbus

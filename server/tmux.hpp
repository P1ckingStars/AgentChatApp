#pragma once

#include <map>
#include <string>
#include <vector>

namespace ccbus {

struct RunResult {
    int status = -1;  // exit code, or -1 if it couldn't be run
    std::string out;
    std::string err;
    bool ok() const { return status == 0; }
};

// Runs argv[0] from PATH with stdin fed from `input` (or /dev/null) and captures output.
// Never lets the child touch our own stdin/stdout, which carry the protocol.
RunResult run(const std::vector<std::string>& argv, const std::string* input = nullptr);

namespace tmux {

// Claude command line run via an interactive shell so ~/.zshrc PATH applies.
std::vector<std::string> claude_argv(const std::vector<std::string>& claude_args);

// Creates detached tmux session "cc-<name>" running claude; on success out = pane id.
RunResult new_session(const std::string& name, const std::string& cwd,
                      const std::vector<std::string>& claude_args);
RunResult respawn(const std::string& pane, const std::string& cwd,
                  const std::vector<std::string>& claude_args);
RunResult send_text(const std::string& pane, const std::string& text);
RunResult send_keys(const std::string& pane, const std::vector<std::string>& keys);
// ansi: keep colours and attributes as SGR escape sequences (capture-pane -e).
RunResult capture(const std::string& pane, bool ansi = false);
RunResult kill_pane(const std::string& pane);
// pane id -> alive (false = dead pane kept by remain-on-exit). Returns false if tmux
// failed for a reason other than "no server running" (then there are simply no panes).
bool list_panes(std::map<std::string, bool>& panes);

}  // namespace tmux
}  // namespace ccbus

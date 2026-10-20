#include <cstring>
#include <iostream>

#include <unistd.h>

#include "commands.hpp"
#include "db.hpp"
#include "tmux.hpp"

namespace ccbus {

int cmd_new(const std::string& name, const std::string& cwd, const std::vector<std::string>& args) {
    if (!valid_name(name)) {
        std::cerr << "ccbus: name must match [A-Za-z0-9_-]{1,64}\n";
        return 2;
    }
    std::string dir = expand_tilde(cwd);
    RunResult r = tmux::new_session(name, dir, args);
    if (!r.ok()) {
        std::cerr << "ccbus: tmux: " << r.err;
        return 1;
    }
    Db db(db_path());
    db.register_started(name, dir, r.out, args);
    std::cout << "Started cc-" << name << " (" << r.out << "). Attach: tmux attach -t cc-" << name << "\n";
    return 0;
}

int cmd_ls() {
    Db db(db_path());
    for (const json& s : db.sessions()) {
        std::cout << (s["unread"].get<bool>() ? "* " : "  ") << s["key"].get<std::string>() << "\t"
                  << s["state"].get<std::string>() << "\t" << s["pane"].get<std::string>() << "\t"
                  << s["cwd"].get<std::string>() << "\t" << s["title"].get<std::string>() << "\n";
    }
    return 0;
}

}  // namespace ccbus

namespace {

void usage() {
    std::cerr << "usage:\n"
                 "  ccbus install-hooks [--settings PATH] [--uninstall]\n"
                 "  ccbus new NAME [DIR] [-- CLAUDE_ARGS...]   start claude in tmux session cc-NAME\n"
                 "  ccbus ls                                  list known sessions\n"
                 "  ccbus serve --stdio                       protocol server for ccbus-qt\n"
                 "  ccbus hook                                (called by Claude Code hooks)\n"
                 "  ccbus version\n";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace ccbus;
    if (argc < 2) {
        usage();
        return 2;
    }
    std::string cmd = argv[1];
    try {
        if (cmd == "hook") return cmd_hook();
        if (cmd == "serve") {
            if (argc < 3 || std::strcmp(argv[2], "--stdio") != 0) {
                usage();
                return 2;
            }
            return cmd_serve();
        }
        if (cmd == "install-hooks") {
            std::string settings;
            bool uninstall = false;
            for (int i = 2; i < argc; ++i) {
                if (!std::strcmp(argv[i], "--uninstall"))
                    uninstall = true;
                else if (!std::strcmp(argv[i], "--settings") && i + 1 < argc)
                    settings = argv[++i];
                else {
                    usage();
                    return 2;
                }
            }
            return cmd_install_hooks(settings, uninstall);
        }
        if (cmd == "new") {
            if (argc < 3) {
                usage();
                return 2;
            }
            std::string dir = ".";
            std::vector<std::string> args;
            int i = 3;
            if (i < argc && std::strcmp(argv[i], "--") != 0) dir = argv[i++];
            if (i < argc && !std::strcmp(argv[i], "--"))
                for (++i; i < argc; ++i) args.emplace_back(argv[i]);
            if (dir == ".") {
                char buf[4096];
                if (getcwd(buf, sizeof buf)) dir = buf;
            }
            return cmd_new(argv[2], dir, args);
        }
        if (cmd == "ls") return cmd_ls();
        if (cmd == "version") {
            std::cout << "ccbus " << kVersion << " (protocol " << kProtoVersion << ")\n";
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "ccbus: " << e.what() << "\n";
        return 1;
    }
    usage();
    return 2;
}

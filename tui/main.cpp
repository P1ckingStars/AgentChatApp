// ccbus-tui: a chat-app view of Claude Code sessions, in the terminal.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "../server/util.hpp"
#include "App.hpp"

namespace {

void usage() {
    std::cout << "usage: ccbus-tui [-H NAME=SSH_DEST]... [--no-notify]\n"
                 "\n"
                 "Hosts come from ~/.config/ccbus/config.json:\n"
                 "  {\"hosts\": [{\"name\": \"local\"}, {\"name\": \"box\", \"ssh\": \"me@box\"}]}\n"
                 "-H adds a host for this run (NAME=local for this machine).\n";
}

}  // namespace

int main(int argc, char** argv) {
    tui::Config cfg = tui::load_config();
    std::vector<tui::HostConfig> extra;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage();
            return 0;
        }
        if (a == "--version") {
            std::cout << "ccbus-tui " << ccbus::kVersion << "\n";
            return 0;
        }
        if (a == "--no-notify") {
            cfg.notify = false;
            continue;
        }
        if ((a == "-H" || a == "--host") && i + 1 < argc) {
            std::string v = argv[++i];
            size_t eq = v.find('=');
            std::string name = eq == std::string::npos ? v : v.substr(0, eq);
            std::string ssh = eq == std::string::npos ? v : v.substr(eq + 1);
            if (ssh == "local") ssh.clear();
            extra.push_back({name, ssh, ""});
            continue;
        }
        std::cerr << "ccbus-tui: unknown argument " << a << "\n";
        usage();
        return 2;
    }
    if (!extra.empty()) cfg.hosts = extra;

    signal(SIGPIPE, SIG_IGN);

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(false);
    int rc = 0;
    {
        tui::App app(cfg, screen);
        auto root = ftxui::Renderer([&] { return app.view(); }) |
                    ftxui::CatchEvent([&](const ftxui::Event& e) { return app.on_event(e); });

        std::atomic<bool> running{true};
        std::thread ticker([&] {
            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!running) break;
                screen.Post([&] { app.tick(); });
                screen.PostEvent(ftxui::Event::Custom);
            }
        });

        tui::App::enable_modes(true);
        screen.Loop(root);
        tui::App::enable_modes(false);
        running = false;
        ticker.join();
    }
    std::cout << "\x1b]2;\x07" << std::flush;
    return rc;
}

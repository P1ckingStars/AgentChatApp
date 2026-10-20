#pragma once
// Desktop notifications through org.freedesktop.Notifications (dunst, mako, ...).
// A private thread owns the D-Bus connection; calls from the UI thread are queued.

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "Host.hpp"

namespace tui {

class Notifier {
public:
    explicit Notifier(PostFn post);
    ~Notifier();
    Notifier(const Notifier&) = delete;
    Notifier& operator=(const Notifier&) = delete;

    // `tag` identifies the session: a new notification for the same tag replaces the old one.
    void notify(const std::string& tag, const std::string& summary, const std::string& body, bool critical);
    void close(const std::string& tag);

    // Runs on the UI thread when the user clicks a notification.
    std::function<void(const std::string& tag)> on_activated;

private:
    struct Req {
        bool close;
        std::string tag, summary, body;
        bool critical;
    };
    void run();
    void enqueue(Req r);

    PostFn post_;
    std::mutex mu_;
    std::deque<Req> queue_;
    int wake_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tui

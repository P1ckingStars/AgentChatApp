#include "Notifier.hpp"

#include <map>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#ifdef CCBUS_HAVE_SDBUS
#include <systemd/sd-bus.h>
#endif

namespace tui {

Notifier::Notifier(PostFn post) : post_(std::move(post)) {
#ifdef CCBUS_HAVE_SDBUS
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ >= 0) thread_ = std::thread([this] { run(); });
#endif
}

Notifier::~Notifier() {
    stop_ = true;
    if (wake_fd_ >= 0) {
        uint64_t one = 1;
        (void)!write(wake_fd_, &one, sizeof one);
    }
    if (thread_.joinable()) thread_.join();
    if (wake_fd_ >= 0) ::close(wake_fd_);
}

void Notifier::enqueue(Req r) {
    if (wake_fd_ < 0) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(r));
    }
    uint64_t one = 1;
    (void)!write(wake_fd_, &one, sizeof one);
}

void Notifier::notify(const std::string& tag, const std::string& summary, const std::string& body, bool critical) {
    enqueue({false, tag, summary, body, critical});
}

void Notifier::close(const std::string& tag) { enqueue({true, tag, {}, {}, false}); }

#ifdef CCBUS_HAVE_SDBUS

namespace {

constexpr const char* kDest = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";

struct State {
    Notifier* self;
    PostFn post;
    std::function<void(const std::string&)>* on_activated;
    std::map<std::string, uint32_t> ids;
    std::map<uint32_t, std::string> tags;
};

int on_action(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* st = static_cast<State*>(userdata);
    uint32_t id = 0;
    const char* action = nullptr;
    if (sd_bus_message_read(m, "us", &id, &action) < 0) return 0;
    auto it = st->tags.find(id);
    if (it == st->tags.end()) return 0;
    std::string tag = it->second;
    auto* cb = st->on_activated;
    st->post([cb, tag] { if (*cb) (*cb)(tag); });
    return 0;
}

int on_closed(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto* st = static_cast<State*>(userdata);
    uint32_t id = 0, reason = 0;
    if (sd_bus_message_read(m, "uu", &id, &reason) < 0) return 0;
    auto it = st->tags.find(id);
    if (it == st->tags.end()) return 0;
    st->ids.erase(it->second);
    st->tags.erase(it);
    return 0;
}

void send_notify(sd_bus* bus, State& st, const std::string& tag, const std::string& summary,
                 const std::string& body, bool critical) {
    sd_bus_message* m = nullptr;
    if (sd_bus_message_new_method_call(bus, &m, kDest, kPath, kDest, "Notify") < 0) return;
    uint32_t replaces = st.ids.count(tag) ? st.ids[tag] : 0;
    sd_bus_message_append(m, "susss", "ccbus", replaces, "", summary.c_str(), body.c_str());
    sd_bus_message_append(m, "as", 2, "default", "Open");
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_append(m, "{sv}", "urgency", "y", static_cast<uint8_t>(critical ? 2 : 1));
    sd_bus_message_close_container(m);
    sd_bus_message_append(m, "i", -1);

    sd_bus_message* reply = nullptr;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_call(bus, m, 2'000'000, &err, &reply) >= 0) {
        uint32_t id = 0;
        if (sd_bus_message_read(reply, "u", &id) >= 0 && id) {
            if (replaces && replaces != id) st.tags.erase(replaces);
            st.ids[tag] = id;
            st.tags[id] = tag;
        }
    }
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
    sd_bus_message_unref(m);
}

void send_close(sd_bus* bus, State& st, const std::string& tag) {
    auto it = st.ids.find(tag);
    if (it == st.ids.end()) return;
    uint32_t id = it->second;
    st.ids.erase(it);
    st.tags.erase(id);
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_call_method(bus, kDest, kPath, kDest, "CloseNotification", &err, nullptr, "u", id);
    sd_bus_error_free(&err);
}

}  // namespace

void Notifier::run() {
    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0) return;  // no session bus: notifications silently off
    State st{this, post_, &on_activated, {}, {}};
    sd_bus_match_signal(bus, nullptr, nullptr, kPath, kDest, "ActionInvoked", on_action, &st);
    sd_bus_match_signal(bus, nullptr, nullptr, kPath, kDest, "NotificationClosed", on_closed, &st);

    while (!stop_) {
        while (sd_bus_process(bus, nullptr) > 0) {
        }
        std::deque<Req> reqs;
        {
            std::lock_guard<std::mutex> lock(mu_);
            reqs.swap(queue_);
        }
        for (const Req& r : reqs) {
            if (r.close)
                send_close(bus, st, r.tag);
            else
                send_notify(bus, st, r.tag, r.summary, r.body, r.critical);
        }
        if (!reqs.empty()) continue;  // replies may have queued signals

        pollfd fds[2] = {{sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0},
                         {wake_fd_, POLLIN, 0}};
        uint64_t usec = 0;
        int timeout = -1;
        if (sd_bus_get_timeout(bus, &usec) > 0 && usec != UINT64_MAX) timeout = 1000;
        poll(fds, 2, timeout);
        if (fds[1].revents & POLLIN) {
            uint64_t v;
            (void)!read(wake_fd_, &v, sizeof v);
        }
    }
    sd_bus_flush_close_unref(bus);
}

#else

void Notifier::run() {}

#endif

}  // namespace tui

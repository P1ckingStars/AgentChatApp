// `ccbus serve --stdio`: newline-delimited JSON over stdin/stdout.
// The Qt client runs this locally or as `ssh host ccbus serve --stdio`.
//
// Requests:  {"id":N, "op":"...", ...params}
// Responses: {"re":N, "ok":true, ...} | {"re":N, "ok":false, "error":"..."}
// Events:    {"ev":"session", "kind", "seq", "session"} | {"ev":"removed", "key", "seq"}
//            {"ev":"transcript", "key", "reset", "truncated", "msgs":[...]}

#include <cerrno>
#include <csignal>
#include <iostream>
#include <map>

#include <poll.h>
#include <unistd.h>

#include "commands.hpp"
#include "db.hpp"
#include "tmux.hpp"
#include "transcript.hpp"

namespace ccbus {

namespace {

constexpr int kTickMs = 400;
constexpr int64_t kLivenessMs = 5000;
constexpr int64_t kInitialTail = 2 * 1024 * 1024;
constexpr size_t kReadChunk = 4 * 1024 * 1024;
constexpr size_t kMsgsPerFrame = 200;

struct RequestError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string req_str(const json& req, const char* key) {
    auto it = req.find(key);
    if (it == req.end() || !it->is_string() || it->get<std::string>().empty())
        throw RequestError(std::string("missing '") + key + "'");
    return it->get<std::string>();
}

std::vector<std::string> str_list(const json& req, const char* key) {
    std::vector<std::string> out;
    auto it = req.find(key);
    if (it != req.end() && it->is_array())
        for (const auto& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

void check(const RunResult& r, const char* what) {
    if (r.ok()) return;
    std::string msg = r.err.empty() ? r.out : r.err;
    while (!msg.empty() && msg.back() == '\n') msg.pop_back();
    throw RequestError(std::string(what) + ": " + (msg.empty() ? "failed" : msg));
}

class Server {
public:
    Server() : db_(db_path()) {}

    int run() {
        std::string inbuf;
        char buf[65536];
        while (true) {
            pollfd p{0, POLLIN, 0};
            int rc = poll(&p, 1, kTickMs);
            if (rc < 0 && errno != EINTR) return 1;
            if (rc > 0) {
                ssize_t n = read(0, buf, sizeof buf);
                if (n == 0) return 0;  // client went away
                if (n < 0 && errno != EINTR && errno != EAGAIN) return 1;
                if (n > 0) inbuf.append(buf, static_cast<size_t>(n));
                size_t nl;
                while ((nl = inbuf.find('\n')) != std::string::npos) {
                    std::string line = inbuf.substr(0, nl);
                    inbuf.erase(0, nl + 1);
                    if (!line.empty()) handle_line(line);
                }
            }
            tick();
        }
    }

private:
    void send(const json& j) {
        std::string s = dump(j) + "\n";
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = write(1, s.data() + off, s.size() - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                std::exit(0);  // stdout closed: nobody is listening any more
            }
            off += static_cast<size_t>(n);
        }
    }

    void handle_line(const std::string& line) {
        json req = json::parse(line, nullptr, false);
        if (req.is_discarded() || !req.is_object()) return;
        json id = req.value("id", json());
        try {
            json res = handle(req);
            res["re"] = id;
            res["ok"] = true;
            send(res);
        } catch (const std::exception& e) {
            send({{"re", id}, {"ok", false}, {"error", e.what()}});
        }
    }

    json session_or_throw(const std::string& key) {
        auto s = db_.session(key);
        if (!s) throw RequestError("no such session: " + key);
        return *s;
    }

    std::string pane_of(const json& s) {
        std::string pane = s["pane"];
        if (pane.empty()) throw RequestError("session is not running in tmux");
        return pane;
    }

    json handle(const json& req) {
        std::string op = req.value("op", "");
        if (op == "hello") {
            char host[256] = {};
            gethostname(host, sizeof host - 1);
            return {{"proto", kProtoVersion}, {"version", kVersion}, {"host", host}};
        }
        if (op == "ping") return json::object();
        if (op == "list_sessions") {
            int64_t seq = db_.max_seq();
            return {{"sessions", db_.sessions()}, {"seq", seq}};
        }
        if (op == "subscribe") {
            int64_t since = req.value("since_seq", int64_t{0});
            last_seq_ = std::min(since, db_.max_seq());
            return json::object();
        }
        if (op == "watch") {
            watch_key_ = req_str(req, "key");
            watch_started_ = false;  // forces a reset frame
            tick_transcript();
            return json::object();
        }
        if (op == "unwatch") {
            watch_key_.clear();
            return json::object();
        }
        if (op == "send") {
            json s = session_or_throw(req_str(req, "key"));
            check(tmux::send_text(pane_of(s), req.value("text", "")), "send");
            return json::object();
        }
        if (op == "keys") {
            json s = session_or_throw(req_str(req, "key"));
            auto keys = str_list(req, "keys");
            if (keys.empty()) throw RequestError("missing 'keys'");
            check(tmux::send_keys(pane_of(s), keys), "send-keys");
            return json::object();
        }
        if (op == "capture") {
            json s = session_or_throw(req_str(req, "key"));
            RunResult r = tmux::capture(pane_of(s), req.value("ansi", false));
            check(r, "capture-pane");
            return {{"text", r.out}};
        }
        if (op == "new") return op_new(req);
        if (op == "resume") return op_resume(req);
        if (op == "kill") {
            std::string key = req_str(req, "key");
            json s = session_or_throw(key);
            check(tmux::kill_pane(pane_of(s)), "kill-pane");
            mark_exited(key, "killed");
            return json::object();
        }
        if (op == "remove") {
            std::string key = req_str(req, "key");
            Tx tx(db_);
            db_.prepare("DELETE FROM sessions WHERE key=?").bind(key).run();
            if (db_.changes() > 0) db_.add_event(key, "removed");
            tx.commit();
            return json::object();
        }
        if (op == "mark_read") {
            std::string key = req_str(req, "key");
            Tx tx(db_);
            db_.prepare("UPDATE sessions SET read_seq=attention_seq WHERE key=? AND read_seq<attention_seq")
                .bind(key)
                .run();
            if (db_.changes() > 0) db_.add_event(key, "read");
            tx.commit();
            return json::object();
        }
        throw RequestError("unknown op: " + op);
    }

    json op_new(const json& req) {
        std::string name = req_str(req, "name");
        if (!valid_name(name)) throw RequestError("name must match [A-Za-z0-9_-]{1,64}");
        std::string cwd = expand_tilde(req.value("cwd", "~"));
        if (access(cwd.c_str(), X_OK) != 0) throw RequestError("no such directory: " + cwd);
        auto args = str_list(req, "args");
        RunResult r = tmux::new_session(name, cwd, args);
        check(r, "tmux new-session");
        db_.register_started(name, cwd, r.out, args);
        return {{"key", name}, {"pane", r.out}};
    }

    json op_resume(const json& req) {
        std::string key = req_str(req, "key");
        json s = session_or_throw(key);
        std::string cs = s["claude_session"];
        if (cs.empty()) throw RequestError("no claude session id recorded to resume");
        std::string cwd = s["cwd"];
        std::string pane = s["pane"];

        // Original launch flags, minus any earlier resume/continue, plus --resume <id>.
        std::vector<std::string> args;
        const json& orig = s["args"];
        for (size_t i = 0; i < orig.size(); ++i) {
            std::string a = orig[i];
            if (a == "--resume" || a == "-r") {
                ++i;
                continue;
            }
            if (a == "--continue" || a == "-c") continue;
            args.push_back(a);
        }
        json stored = args;
        args.push_back("--resume");
        args.push_back(cs);

        std::map<std::string, bool> panes;
        if (!pane.empty() && tmux::list_panes(panes) && panes.count(pane)) {
            // The pane outlived claude (remain-on-exit): restart claude in place.
            check(tmux::respawn(pane, cwd, args), "tmux respawn-pane");
            db_.register_started(key, cwd, pane, stored);
            return {{"key", key}, {"pane", pane}};
        }
        std::string name = valid_name(key) ? key : std::string(s["name"]);
        if (!valid_name(name)) name = "resumed-" + std::to_string(now_ms() % 100000);
        RunResult r = tmux::new_session(name, cwd, args);
        check(r, "tmux new-session");
        if (name != key) {
            Tx tx(db_);
            db_.prepare("DELETE FROM sessions WHERE key=?").bind(key).run();
            db_.add_event(key, "removed");
            tx.commit();
        }
        db_.register_started(name, cwd, r.out, stored);
        return {{"key", name}, {"pane", r.out}};
    }

    void mark_exited(const std::string& key, const std::string& why) {
        Tx tx(db_);
        db_.prepare("UPDATE sessions SET state='exited', detail=?, updated=? WHERE key=? AND state<>'exited'")
            .bind(why, now_ms(), key)
            .run();
        if (db_.changes() > 0) db_.add_event(key, "gone");
        tx.commit();
    }

    void tick() {
        try {
            int64_t now = now_ms();
            if (now - last_liveness_ >= kLivenessMs) {
                last_liveness_ = now;
                check_liveness();
            }
            tick_events();
            tick_transcript();
        } catch (const std::exception& e) {
            std::cerr << "ccbus serve: " << e.what() << "\n";
        }
    }

    void tick_events() {
        if (last_seq_ < 0) return;
        for (const Event& e : db_.events_since(last_seq_, 500)) {
            last_seq_ = e.seq;
            if (e.kind == "removed") {
                send({{"ev", "removed"}, {"key", e.key}, {"seq", e.seq}});
            } else if (auto s = db_.session(e.key)) {
                send({{"ev", "session"}, {"kind", e.kind}, {"seq", e.seq}, {"session", *s}});
            }
        }
    }

    void tick_transcript() {
        if (watch_key_.empty()) return;
        auto s = db_.session(watch_key_);
        if (!s) return;
        std::string path = (*s)["transcript"];
        bool reset = false, truncated = false;
        if (path != watch_path_ || !watch_started_) {
            watch_path_ = path;
            watch_started_ = true;
            watch_offset_ = path.empty() ? 0 : initial_offset(path, kInitialTail, truncated);
            reset = true;
        }
        std::vector<json> msgs;
        if (!path.empty()) read_transcript(path, watch_offset_, msgs, kReadChunk);
        if (!reset && msgs.empty()) return;

        size_t i = 0;
        do {
            json frame = {{"ev", "transcript"}, {"key", watch_key_}, {"reset", reset},
                          {"truncated", truncated}, {"msgs", json::array()}};
            for (size_t n = 0; i < msgs.size() && n < kMsgsPerFrame; ++i, ++n)
                frame["msgs"].push_back(std::move(msgs[i]));
            send(frame);
            reset = truncated = false;
        } while (i < msgs.size());
    }

    void check_liveness() {
        std::map<std::string, bool> panes;
        if (!tmux::list_panes(panes)) return;
        for (const json& s : db_.sessions()) {
            std::string pane = s["pane"];
            std::string state = s["state"];
            if (pane.empty() || state == "exited") continue;
            auto it = panes.find(pane);
            if (it == panes.end())
                mark_exited(s["key"], "tmux pane closed");
            else if (!it->second)
                mark_exited(s["key"], "claude exited");
        }
    }

    Db db_;
    int64_t last_seq_ = -1;  // -1 until the client subscribes
    int64_t last_liveness_ = 0;
    std::string watch_key_;
    std::string watch_path_;
    bool watch_started_ = false;
    int64_t watch_offset_ = 0;
};

}  // namespace

int cmd_serve() {
    std::signal(SIGPIPE, SIG_IGN);
    try {
        Server server;
        return server.run();
    } catch (const std::exception& e) {
        std::cerr << "ccbus serve: " << e.what() << "\n";
        return 1;
    }
}

}  // namespace ccbus

#include "App.hpp"

#include <algorithm>
#include <csignal>
#include <ctime>
#include <iostream>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "../server/util.hpp"

namespace tui {

using namespace ftxui;

namespace {

// ---- palette (terminal colours, so the user's theme applies) ----------------
const Color kBlocked = Color::Red;
const Color kUnread = Color::Yellow;
const Color kRunning = Color::Blue;
const Color kAccent = Color::Blue;
const Color kUser = Color::Magenta;
const Color kOk = Color::Green;
const Color kBarBg = Color::Palette256(235);
const Color kChipBg = Color::Palette256(238);
const Color kSelBg = Color::Palette256(236);

const char* kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};

int64_t now_ms() { return ccbus::now_ms(); }

std::string glyph_prev(const std::string& s, size_t& cur) {
    size_t p = cur;
    while (p > 0) {
        --p;
        if ((static_cast<unsigned char>(s[p]) & 0xC0) != 0x80) break;
    }
    std::string g = s.substr(p, cur - p);
    cur = p;
    return g;
}

size_t glyph_next(const std::string& s, size_t cur) {
    if (cur >= s.size()) return cur;
    ++cur;
    while (cur < s.size() && (static_cast<unsigned char>(s[cur]) & 0xC0) == 0x80) ++cur;
    return cur;
}

std::string state_label(const Session& s) {
    std::string st = s.state();
    if (st == "blocked") return "needs permission";
    if (st == "waiting") return s.unread() ? "waiting for you" : "waiting";
    if (st == "running") return "working";
    if (st == "starting") return "starting";
    if (st == "idle") return "idle";
    if (st == "exited") return "exited";
    return st;
}

int rank(const Session& s) {
    std::string st = s.state();
    if (st == "blocked") return 0;
    if (s.unread()) return 1;
    if (st == "running" || st == "starting") return 2;
    if (st == "exited") return 4;
    return 3;
}

int group_of(const Session& s) {
    int r = rank(s);
    return r <= 1 ? 0 : r == 2 ? 1 : 2;
}

const char* kGroupNames[] = {"NEEDS YOU", "RUNNING", "IDLE"};

std::string age(int64_t ms) {
    if (ms <= 0) return "";
    int64_t s = (now_ms() - ms) / 1000;
    if (s < 60) return "now";
    if (s < 3600) return std::to_string(s / 60) + "m";
    if (s < 86400) return std::to_string(s / 3600) + "h";
    return std::to_string(s / 86400) + "d";
}

std::string clock_of(const std::string& iso) {
    int Y, M, D, h, m, sec;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return "";
    std::tm tm{};
    tm.tm_year = Y - 1900;
    tm.tm_mon = M - 1;
    tm.tm_mday = D;
    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = sec;
    time_t t = timegm(&tm);
    std::tm local{}, today{};
    localtime_r(&t, &local);
    time_t now = time(nullptr);
    localtime_r(&now, &today);
    char buf[32];
    if (local.tm_yday == today.tm_yday && local.tm_year == today.tm_year)
        std::strftime(buf, sizeof buf, "%H:%M", &local);
    else
        std::strftime(buf, sizeof buf, "%b %d %H:%M", &local);
    return buf;
}

std::string first_line(const std::string& s, int* more = nullptr) {
    size_t nl = s.find('\n');
    if (more) {
        int n = 0;
        for (char c : s) n += c == '\n';
        if (!s.empty() && s.back() == '\n') --n;
        *more = n;
    }
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string one_line(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\t' || c == '\r') c = ' ';
    return s;
}

// Previews are one line of plain text: drop markdown markers.
std::string plain(std::string s) {
    s = one_line(std::move(s));
    std::erase(s, '`');
    for (size_t p; (p = s.find("**")) != std::string::npos;) s.erase(p, 2);
    return s;
}

std::string short_cwd(const std::string& cwd, bool local) {
    std::string out = cwd;
    std::string home = ccbus::home_dir();
    if (local && out.rfind(home, 0) == 0) out = "~" + out.substr(home.size());
    else if (out.rfind("/home/", 0) == 0) {
        size_t p = out.find('/', 6);
        out = "~" + (p == std::string::npos ? std::string() : out.substr(p));
    }
    return out;
}

std::string suggest_name(const std::string& dir) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    std::string base = d.substr(d.rfind('/') + 1);
    if (base == "~" || base.empty()) base = "home";
    for (char& c : base)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '-';
    return base.substr(0, 64);
}

std::vector<std::string> split_args(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool any = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            any = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            any = true;
        } else if (c == ' ' || c == '\t') {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
        } else {
            cur += c;
        }
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

// Runs a short command without a terminal; returns exit status and stderr.
int run_quiet(const std::vector<std::string>& argv, std::string* err = nullptr) {
    int ep[2];
    if (pipe2(ep, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(nul, 0);
        dup2(nul, 1);
        dup2(ep[1], 2);
        signal(SIGPIPE, SIG_DFL);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        dprintf(2, "cannot run %s", a[0]);
        _exit(127);
    }
    close(ep[1]);
    std::string e;
    char buf[1024];
    ssize_t n;
    while ((n = read(ep[0], buf, sizeof buf)) > 0) e.append(buf, static_cast<size_t>(n));
    close(ep[0]);
    int st = 0;
    if (pid > 0) waitpid(pid, &st, 0);
    while (!e.empty() && (e.back() == '\n' || e.back() == ' ')) e.pop_back();
    if (err) *err = e;
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// Runs a command on the real terminal (the TUI is suspended) and waits for it.
int run_foreground(const std::vector<std::string>& argv) {
    pid_t pid = fork();
    if (pid == 0) {
        unsetenv("TMUX");
        unsetenv("TMUX_PANE");
        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        std::vector<char*> a;
        for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    int st = 0;
    if (pid > 0) {
        // Ctrl-C belongs to the attached program while it runs.
        auto old = signal(SIGINT, SIG_IGN);
        waitpid(pid, &st, 0);
        signal(SIGINT, old);
    }
    return pid > 0 && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// tmux key name for an event, or "" if we don't forward it.
std::string tmux_key(const Event& e) {
    if (e == Event::Return) return "Enter";
    if (e == Event::Escape) return "Escape";
    if (e == Event::Backspace) return "BSpace";
    if (e == Event::Delete) return "DC";
    if (e == Event::Tab) return "Tab";
    if (e == Event::TabReverse) return "BTab";
    if (e == Event::ArrowUp) return "Up";
    if (e == Event::ArrowDown) return "Down";
    if (e == Event::ArrowLeft) return "Left";
    if (e == Event::ArrowRight) return "Right";
    if (e == Event::Home) return "Home";
    if (e == Event::End) return "End";
    if (e == Event::PageUp) return "PPage";
    if (e == Event::PageDown) return "NPage";
    if (e.is_character()) {
        std::string c = e.character();
        if (c == " ") return "Space";
        if (c == ";") return "\\;";
        return c;
    }
    const std::string& in = e.input();
    if (in.size() == 1 && in[0] >= 1 && in[0] <= 26) return std::string("C-") + static_cast<char>('a' + in[0] - 1);
    return "";
}

Element chip(const std::string& key, const std::string& label) {
    return hbox({text(" " + key + " ") | bgcolor(kChipBg) | color(Color::White), text(" " + label + "  ") | dim});
}

Element pane(Element title, Element content, bool focus, std::optional<Color> c = std::nullopt) {
    Color border = c ? *c : focus ? kAccent : Color::GrayDark;
    return window(std::move(title), std::move(content) | color(Color::Default), ROUNDED) | color(border);
}

// A blank margin keeps the dialog's border from merging with the panes behind it.
Element modal(Element e) {
    return vcenter(hcenter(hbox({text(" "), vbox({text(" "), std::move(e), text(" ")}), text(" ")}) | clear_under));
}

Element title_text(const std::string& s, Color c, bool b = true) {
    Element e = text(" " + s + " ") | color(c);
    return b ? e | bold : e;
}

}  // namespace

// ---- TextEdit ------------------------------------------------------------------

void TextEdit::insert(const std::string& t) {
    s.insert(cur, t);
    cur += t.size();
}

bool TextEdit::handle(const Event& e) {
    if (e.is_character()) {
        insert(e.character());
        return true;
    }
    if (e == Event::Backspace) {
        if (cur > 0) {
            size_t end = cur;
            glyph_prev(s, cur);
            s.erase(cur, end - cur);
        }
        return true;
    }
    if (e == Event::Delete) {
        size_t n = glyph_next(s, cur);
        s.erase(cur, n - cur);
        return true;
    }
    if (e == Event::ArrowLeft) {
        if (cur > 0) glyph_prev(s, cur);
        return true;
    }
    if (e == Event::ArrowRight) {
        cur = glyph_next(s, cur);
        return true;
    }
    if (e == Event::Home || e == Event::CtrlA) {
        size_t nl = cur == 0 ? std::string::npos : s.rfind('\n', cur - 1);
        cur = nl == std::string::npos ? 0 : nl + 1;
        return true;
    }
    if (e == Event::End || e == Event::CtrlE) {
        size_t nl = s.find('\n', cur);
        cur = nl == std::string::npos ? s.size() : nl;
        return true;
    }
    if (e == Event::CtrlW || e == Event::Special("\x1b\x7f")) {
        size_t p = cur;
        while (p > 0 && s[p - 1] == ' ') --p;
        while (p > 0 && s[p - 1] != ' ' && s[p - 1] != '\n') --p;
        s.erase(p, cur - p);
        cur = p;
        return true;
    }
    if (e == Event::CtrlU) {
        s.erase(0, cur);
        cur = 0;
        return true;
    }
    if (e == Event::CtrlK) {
        s.erase(cur);
        return true;
    }
    if (e == Event::ArrowLeftCtrl || e == Event::Special("\x1b" "b")) {
        while (cur > 0 && s[cur - 1] == ' ') --cur;
        while (cur > 0 && s[cur - 1] != ' ') --cur;
        return true;
    }
    if (e == Event::ArrowRightCtrl || e == Event::Special("\x1b" "f")) {
        while (cur < s.size() && s[cur] == ' ') ++cur;
        while (cur < s.size() && s[cur] != ' ') ++cur;
        return true;
    }
    return false;
}

std::vector<Line> TextEdit::lines(bool show_cursor, const Style& st) const {
    std::vector<Line> out(1);
    Style inv = st;
    inv.inverse = true;
    size_t i = 0;
    while (i <= s.size()) {
        bool at = show_cursor && i == cur;
        if (i == s.size()) {
            if (at) out.back().push_back({" ", inv});
            break;
        }
        if (s[i] == '\n') {
            if (at) out.back().push_back({" ", inv});
            out.emplace_back();
            ++i;
            continue;
        }
        size_t n = glyph_next(s, i);
        std::string g = s.substr(i, n - i);
        if (at)
            out.back().push_back({g, inv});
        else if (!out.back().empty() && !out.back().back().st.inverse)
            out.back().back().text += g;
        else
            out.back().push_back({g, st});
        i = n;
    }
    return out;
}

// ---- App: setup ------------------------------------------------------------

App::App(Config cfg, ScreenInteractive& screen) : screen_(screen), cfg_(std::move(cfg)) {
    PostFn post = [this](Task t) {
        screen_.Post(std::move(t));
        screen_.PostEvent(Event::Custom);
    };
    if (cfg_.notify) {
        notifier_ = std::make_unique<Notifier>(post);
        notifier_->on_activated = [this](const std::string& tag) {
            if (find(tag)) select(tag);
            mode_ = Mode::List;
            // Bring this TUI's tmux window to the front.
            if (const char* p = getenv("TMUX_PANE"); p && *p && getenv("TMUX"))
                run_quiet({"tmux", "switch-client", "-t", p});
        };
    }
    for (const HostConfig& hc : cfg_.hosts) {
        auto h = std::make_unique<Host>(hc, post);
        Host* hp = h.get();
        h->on_snapshot = [this, hp](const std::vector<json>& rows, int64_t seq) { on_snapshot(hp, rows, seq); };
        h->on_session = [this, hp](const std::string& kind, const json& row, int64_t seq) {
            on_session(hp, kind, row, seq);
        };
        h->on_removed = [this, hp](const std::string& key) {
            std::string id = Session::make_id(hp->name(), key);
            std::erase_if(sessions_, [&](const Session& s) { return s.id() == id; });
            if (notifier_) notifier_->close(id);
            if (sel_ == id) {
                sel_.clear();
                move(0);
            }
        };
        h->on_transcript = [this, hp](const json& f) { on_transcript(hp, f); };
        h->on_error = [this, hp](const std::string& op, const std::string& err) {
            if (op != "ping") flash(hp->name() + ": " + op + ": " + err, true);
        };
        h->on_connection = [this](bool) { chat_dirty_ = true; };
        hosts_.push_back(std::move(h));
    }
    last_key_ = now_ms();
    tick();
}

App::~App() {
    hosts_.clear();
    notifier_.reset();
}

void App::enable_modes(bool on) {
    // 2004: bracketed paste, 1004: focus in/out reports.
    std::cout << (on ? "\x1b[?2004h\x1b[?1004h" : "\x1b[?2004l\x1b[?1004l") << std::flush;
}

Host* App::host(const std::string& name) const {
    for (const auto& h : hosts_)
        if (h->name() == name) return h.get();
    return nullptr;
}

Session* App::find(const std::string& id) {
    for (Session& s : sessions_)
        if (s.id() == id) return &s;
    return nullptr;
}

const Session* App::selected() const {
    for (const Session& s : sessions_)
        if (s.id() == sel_) return &s;
    return nullptr;
}

std::vector<const Session*> App::visible() const {
    std::vector<const Session*> out;
    std::string f = filter_.s;
    std::transform(f.begin(), f.end(), f.begin(), ::tolower);
    for (const Session& s : sessions_) {
        if (!f.empty()) {
            std::string hay = s.name() + " " + s.str("title") + " " + s.str("cwd") + " " + s.host;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(f) == std::string::npos) continue;
        }
        out.push_back(&s);
    }
    std::stable_sort(out.begin(), out.end(), [this](const Session* a, const Session* b) {
        int ga = group(*a), gb = group(*b);
        if (ga != gb) return ga < gb;
        return a->updated() > b->updated();
    });
    return out;
}

// Reading the selected session clears "unread"; keep it under NEEDS YOU until the cursor
// leaves, so it doesn't jump away. Real state changes (starting -> idle...) still regroup.
int App::group(const Session& s) const {
    if (sel_group_ == 0 && s.id() == sel_ && s.state() == "waiting") return 0;
    return group_of(s);
}

void App::upsert(const std::string& h, const json& row) {
    std::string id = Session::make_id(h, row.value("key", ""));
    if (Session* s = find(id)) {
        s->d = row;
        return;
    }
    sessions_.push_back({h, row});
}

// ---- host callbacks -----------------------------------------------------------

void App::on_snapshot(Host* h, const std::vector<json>& rows, int64_t seq) {
    std::erase_if(sessions_, [&](const Session& s) { return s.host == h->name(); });
    for (const json& r : rows) upsert(h->name(), r);

    // Tell the user about anything that needed them while we were away.
    int64_t seen = state_.notified(h->name());
    if (seen > seq) seen = 0;  // the server's database was reset
    std::vector<const Session*> missed;
    for (const Session& s : sessions_)
        if (s.host == h->name() && s.attention() && s.d.value("attention_seq", int64_t{0}) > seen &&
            !(looking() && s.id() == sel_))
            missed.push_back(&s);
    if (missed.size() == 1)
        notify_for(*missed[0], missed[0]->state() == "blocked");
    else if (missed.size() > 1 && notifier_)
        notifier_->notify("summary:" + h->name(), std::to_string(missed.size()) + " Claude sessions need you",
                          "on " + h->name(), false);
    state_.set_notified(h->name(), seq);

    if (!pending_sel_.empty() && find(pending_sel_)) {
        select(pending_sel_);
        pending_sel_.clear();
    }
    if (!selected()) move(0);
    maybe_mark_read();
}

void App::on_session(Host* h, const std::string& kind, const json& row, int64_t seq) {
    upsert(h->name(), row);
    std::string id = Session::make_id(h->name(), row.value("key", ""));
    const Session* s = find(id);
    if (!s) return;

    if ((kind == "stop" || kind == "blocked") && seq > state_.notified(h->name())) {
        if (looking() && id == sel_)
            maybe_mark_read();
        else
            notify_for(*s, kind == "blocked");
    } else if (kind == "read" || kind == "prompt" || kind == "resumed" || kind == "gone" || kind == "end") {
        if (notifier_) notifier_->close(id);
    }
    state_.set_notified(h->name(), std::max(seq, state_.notified(h->name())));

    if (id == pending_sel_) {
        select(id);
        pending_sel_.clear();
    }
    if (sel_.empty()) move(0);
    if (id == sel_) chat_dirty_ = true;
}

void App::on_transcript(Host* h, const json& f) {
    if (Session::make_id(h->name(), f.value("key", "")) != sel_) return;
    if (f.value("reset", false)) {
        msgs_.clear();
        offsets_.clear();
        truncated_ = f.value("truncated", false);
    }
    loaded_ = true;
    for (const json& m : f.value("msgs", json::array())) {
        int64_t off = m.value("offset", int64_t{-1});
        if (!offsets_.insert(off).second) continue;
        if (m.value("kind", "") == "title") continue;
        msgs_.push_back(m);
    }
    chat_dirty_ = true;
}

// ---- behaviour ----------------------------------------------------------------

void App::select(const std::string& id) {
    if (id == sel_) return;
    const Session* old = selected();
    std::string old_host = old ? old->host : std::string();
    sel_ = id;
    const Session* s = selected();
    sel_group_ = s ? group_of(*s) : -1;
    if (Host* oh = host(old_host); oh && (!s || s->host != old_host)) oh->unwatch();

    msgs_.clear();
    offsets_.clear();
    truncated_ = false;
    loaded_ = false;
    chat_dirty_ = true;
    scroll_ = 0;
    screen_lines_.clear();
    last_capture_ = 0;
    peek_ = false;
    if (s)
        if (Host* h = host(s->host)) h->watch(s->key());
    maybe_mark_read();
}

void App::move(int delta) {
    auto v = visible();
    if (v.empty()) {
        if (!sel_.empty()) select("");
        return;
    }
    int idx = -1;
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i]->id() == sel_) idx = static_cast<int>(i);
    if (idx < 0) {
        select(v[0]->id());
        return;
    }
    int n = std::clamp(idx + delta, 0, static_cast<int>(v.size()) - 1);
    select(v[n]->id());
}

bool App::looking() const {
    if (focus_known_) return focused_;
    return now_ms() - last_key_ < 120000;
}

void App::maybe_mark_read() {
    const Session* s = selected();
    if (!s || !s->unread() || !looking()) return;
    int64_t seq = s->d.value("attention_seq", int64_t{0});
    auto it = marked_.find(s->id());
    if (it != marked_.end() && it->second >= seq) return;
    marked_[s->id()] = seq;
    if (Host* h = host(s->host)) h->request("mark_read", {{"key", s->key()}});
    if (notifier_) notifier_->close(s->id());
}

void App::notify_for(const Session& s, bool blocked) {
    if (!notifier_) return;
    std::string summary = s.name() + (blocked ? " needs permission" : " is waiting for you");
    std::string body = s.str("detail");
    if (body.empty()) body = s.str("title");
    if (hosts_.size() > 1) body = "[" + s.host + "] " + body;
    notifier_->notify(s.id(), summary, body, blocked);
}

void App::flash(const std::string& msg, bool error) {
    status_ = msg;
    status_error_ = error;
    status_until_ = now_ms() + (error ? 8000 : 4000);
}

bool App::screen_visible() const {
    const Session* s = selected();
    if (!s || s->pane().empty() || s->state() == "exited") return false;
    Host* h = host(s->host);
    if (!h || !h->connected()) return false;
    return s->state() == "blocked" || s->state() == "starting" || peek_ || mode_ == Mode::Interact;
}

void App::capture() {
    const Session* s = selected();
    Host* h = s ? host(s->host) : nullptr;
    if (!h) return;
    capture_busy_ = true;
    last_capture_ = now_ms();
    std::string id = s->id();
    h->request(
        "capture", {{"key", s->key()}, {"ansi", true}},
        [this, id](const json& r) {
            capture_busy_ = false;
            if (id != sel_) return;
            screen_lines_.clear();
            std::string t = r.value("text", "");
            size_t pos = 0;
            while (pos < t.size()) {
                size_t nl = t.find('\n', pos);
                screen_lines_.push_back(parse_ansi(t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)));
                pos = nl == std::string::npos ? t.size() : nl + 1;
            }
            auto blank = [](const Line& l) {
                for (const Span& sp : l)
                    if (sp.text.find_first_not_of(' ') != std::string::npos || sp.st.has_bg || sp.st.inverse)
                        return false;
                return true;
            };
            while (!screen_lines_.empty() && blank(screen_lines_.back())) screen_lines_.pop_back();
        },
        [this, id](const std::string& err) {
            capture_busy_ = false;
            if (id != sel_) return;
            screen_lines_ = {Line{{"capture failed: " + err, Style::color(kBlocked)}}};
        });
}

void App::send_reply() {
    const Session* s = selected();
    if (!s) return;
    std::string text = input_.s;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return;
    if (s->pane().empty()) return flash("this session isn't running in tmux, so it can't take input", true);
    if (s->state() == "exited") return flash("session has exited; press r to resume it", true);
    Host* h = host(s->host);
    if (!h || !h->connected()) return flash("host " + s->host + " is offline", true);
    std::string saved = input_.s;
    input_.clear();
    scroll_ = 0;
    h->request("send", {{"key", s->key()}, {"text", text}}, {}, [this, saved](const std::string& err) {
        if (input_.s.empty()) input_.set(saved);
        flash("send failed: " + err, true);
    });
}

void App::send_keys(const std::vector<std::string>& keys) {
    const Session* s = selected();
    Host* h = s ? host(s->host) : nullptr;
    if (!h || s->pane().empty()) return;
    h->request("keys", {{"key", s->key()}, {"keys", keys}});
    last_capture_ = now_ms() - 1300;  // refresh the screen shortly
}

void App::attach() {
    const Session* s = selected();
    if (!s) return;
    if (s->pane().empty()) return flash("not running in tmux", true);
    Host* h = host(s->host);
    if (!h) return;
    std::vector<std::string> argv = h->attach_argv(s->pane());
    std::string name = s->name();

    if (getenv("TMUX")) {
        std::string err;
        if (h->config().local() && !getenv("CCBUS_TMUX_SOCKET")) {
            // Same tmux server: move this client to the session. prefix+L comes back.
            if (run_quiet({"tmux", "switch-client", "-t", s->pane()}, &err) != 0)
                return flash("tmux: " + err, true);
            flash("switched to " + name + " — prefix L to come back");
            return;
        }
        // Another server or another machine: open it in a new window here, TUI keeps running.
        std::string cmd = "env -u TMUX";
        for (const std::string& a : argv) cmd += " " + ccbus::shell_quote(a);
        if (run_quiet({"tmux", "new-window", "-n", name, cmd}, &err) != 0) return flash("tmux: " + err, true);
        return;
    }
    // Plain terminal: hand it over until the user detaches (prefix d).
    screen_.WithRestoredIO([&] {
        enable_modes(false);
        int rc = run_foreground(argv);
        if (rc != 0) flash("attach exited with status " + std::to_string(rc), true);
    })();
    enable_modes(true);
}

void App::resume() {
    const Session* s = selected();
    if (!s) return;
    if (s->state() != "exited") return flash(s->name() + " is still running", true);
    if (s->str("claude_session").empty()) return flash("no claude session id recorded; can't resume", true);
    Host* h = host(s->host);
    if (!h) return;
    std::string hn = s->host;
    h->request("resume", {{"key", s->key()}}, [this, hn](const json& r) {
        std::string id = Session::make_id(hn, r.value("key", ""));
        if (find(id)) select(id);
        else pending_sel_ = id;
        flash("resuming…");
    });
}

void App::kill_selected() {
    const Session* s = selected();
    if (!s) return;
    if (s->pane().empty() || s->state() == "exited") return flash("nothing to kill", true);
    std::string hn = s->host, key = s->key();
    confirm_text_ = "kill " + s->name() + "? this ends claude and closes its tmux pane";
    confirm_action_ = [this, hn, key] {
        if (Host* h = host(hn)) h->request("kill", {{"key", key}});
    };
    mode_ = Mode::Confirm;
}

void App::remove_selected() {
    const Session* s = selected();
    if (!s) return;
    std::string hn = s->host, key = s->key();
    bool alive = s->state() != "exited" && !s->pane().empty();
    confirm_text_ = "remove " + s->name() + " from the list?" + (alive ? " it keeps running in tmux" : "");
    confirm_action_ = [this, hn, key] {
        if (Host* h = host(hn)) h->request("remove", {{"key", key}});
    };
    mode_ = Mode::Confirm;
}

void App::open_form() {
    form_ = Form();
    const Session* s = selected();
    if (s)
        for (size_t i = 0; i < hosts_.size(); ++i)
            if (hosts_[i]->name() == s->host) form_.host = i;
    form_.recent = state_.recent_dirs(hosts_[form_.host]->name());
    form_.dir.set(form_.recent.empty() ? "~/" : form_.recent[0]);
    form_.name.set(suggest_name(form_.dir.s));
    form_.field = 1;
    mode_ = Mode::Form;
}

void App::submit_form() {
    Host* h = hosts_[form_.host].get();
    std::string dir = form_.dir.s, name = form_.name.s;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (dir.empty()) return flash("directory is required", true);
    if (!ccbus::valid_name(name)) return flash("name must be letters, digits, - or _ (max 64)", true);
    if (!h->connected()) return flash(h->name() + " is offline", true);
    std::vector<std::string> args = split_args(form_.flags.s);
    std::string hn = h->name();
    h->request(
        "new", {{"name", name}, {"cwd", dir}, {"args", args}},
        [this, hn, dir](const json& r) {
            state_.add_recent_dir(hn, dir);
            std::string id = Session::make_id(hn, r.value("key", ""));
            if (find(id)) select(id);
            else pending_sel_ = id;
            if (mode_ == Mode::Form) mode_ = Mode::List;
            flash("started " + r.value("key", std::string()));
        },
        [this](const std::string& err) { flash(err, true); });
}

void App::on_paste(const std::string& text) {
    if (mode_ == Mode::Form) {
        TextEdit* f = form_.field == 1 ? &form_.dir : form_.field == 2 ? &form_.name : form_.field == 3 ? &form_.flags : nullptr;
        if (f) f->insert(one_line(text));
        if (form_.field == 2) form_.name_edited = true;
        return;
    }
    if (mode_ == Mode::Filter) {
        filter_.insert(one_line(text));
        return;
    }
    if (mode_ == Mode::List && selected()) mode_ = Mode::Reply;
    if (mode_ == Mode::Reply) input_.insert(text);
}

void App::update_title() {
    int n = 0;
    for (const Session& s : sessions_) n += s.attention();
    if (n == title_count_) return;
    title_count_ = n;
    std::string t = n ? "(" + std::to_string(n) + ") ccbus" : "ccbus";
    std::cout << "\x1b]2;" << t << "\x07" << std::flush;
}

void App::tick() {
    ++ticks_;
    int64_t now = now_ms();
    for (auto& h : hosts_) h->tick(now);
    if (screen_visible() && !capture_busy_ && now - last_capture_ >= (mode_ == Mode::Interact ? 500 : 1500))
        capture();
    if (status_until_ && now > status_until_) {
        status_.clear();
        status_until_ = 0;
    }
    if (ticks_ % 5 == 0) update_title();
}

// ---- keys -------------------------------------------------------------------

bool App::on_event(const Event& e) {
    if (e == Event::Custom) return false;
    if (e.is_mouse()) return false;
    const std::string& in = e.input();
    if (in == "\x1b[200~") {
        pasting_ = true;
        paste_buf_.clear();
        return true;
    }
    if (in == "\x1b[201~") {
        pasting_ = false;
        on_paste(paste_buf_);
        return true;
    }
    if (pasting_) {
        if (e == Event::Return) paste_buf_ += '\n';
        else if (e == Event::Tab) paste_buf_ += "    ";
        else if (e.is_character()) paste_buf_ += e.character();
        return true;
    }
    if (in == "\x1b[I") {
        focus_known_ = true;
        focused_ = true;
        maybe_mark_read();
        return true;
    }
    if (in == "\x1b[O") {
        focus_known_ = true;
        focused_ = false;
        return true;
    }
    last_key_ = now_ms();
    if (!focus_known_) maybe_mark_read();

    switch (mode_) {
        case Mode::List: return key_list(e);
        case Mode::Reply: return key_reply(e);
        case Mode::Interact: return key_interact(e);
        case Mode::Form: return key_form(e);
        case Mode::Filter:
            if (e == Event::Return) {
                mode_ = Mode::List;
                return true;
            }
            if (e == Event::Escape) {
                filter_.clear();
                mode_ = Mode::List;
                move(0);
                return true;
            }
            if (e == Event::ArrowDown || e == Event::ArrowUp) {
                move(e == Event::ArrowDown ? 1 : -1);
                return true;
            }
            if (filter_.handle(e)) {
                bool still = false;
                for (const Session* s : visible()) still |= s->id() == sel_;
                if (!still) {
                    auto v = visible();
                    if (!v.empty()) select(v[0]->id());
                }
                return true;
            }
            return true;
        case Mode::Confirm:
            if (e == Event::Character('y') || e == Event::Character('Y')) {
                if (confirm_action_) confirm_action_();
            }
            confirm_action_ = nullptr;
            mode_ = Mode::List;
            return true;
        case Mode::Help:
            mode_ = Mode::List;
            return true;
    }
    return false;
}

bool App::scroll_keys(const Event& e) {
    int page = std::max(1, chat_page_ - 2);
    if (e == Event::PageUp || e == Event::CtrlB) scroll_ += page;
    else if (e == Event::PageDown || e == Event::CtrlF) scroll_ = std::max(0, scroll_ - page);
    else if (e == Event::CtrlU) scroll_ += page / 2;
    else if (e == Event::CtrlD) scroll_ = std::max(0, scroll_ - page / 2);
    else return false;
    return true;
}

bool App::key_list(const Event& e) {
    const Session* s = selected();
    if (e == Event::Character('j') || e == Event::ArrowDown) return move(1), true;
    if (e == Event::Character('k') || e == Event::ArrowUp) return move(-1), true;
    if (e == Event::Character('g') || e == Event::Home) return move(-100000), true;
    if (e == Event::Character('G') || e == Event::End) return move(100000), true;
    if (scroll_keys(e)) return true;
    if (e == Event::Return || e == Event::Character('i') || e == Event::Character('l') || e == Event::ArrowRight) {
        if (s) mode_ = Mode::Reply;
        return true;
    }
    if (e == Event::Character('q') || e == Event::CtrlC) {
        screen_.Exit();
        return true;
    }
    if (e == Event::Character('?')) return mode_ = Mode::Help, true;
    if (e == Event::Character('/')) return mode_ = Mode::Filter, true;
    if (e == Event::Escape) {
        if (!filter_.s.empty()) {
            filter_.clear();
            move(0);
        }
        return true;
    }
    if (e == Event::Character('n')) return open_form(), true;
    if (!s) return false;
    if (e == Event::Character('a')) return attach(), true;
    if (e == Event::Character('p')) {
        peek_ = !peek_;
        last_capture_ = 0;
        return true;
    }
    if (e == Event::Character('s')) {
        if (s->pane().empty() || s->state() == "exited") return flash("nothing to interact with", true), true;
        mode_ = Mode::Interact;
        last_capture_ = 0;
        return true;
    }
    if (e == Event::Character('r')) return resume(), true;
    if (e == Event::Character('x')) return kill_selected(), true;
    if (e == Event::Character('D')) return remove_selected(), true;
    if (e.is_character() && screen_visible()) {
        std::string c = e.character();
        if (c.size() == 1 && c[0] >= '1' && c[0] <= '9') return send_keys({c}), true;
    }
    return false;
}

bool App::key_reply(const Event& e) {
    if (e == Event::Escape) return mode_ = Mode::List, true;
    // A permission prompt is waiting and nothing is typed yet: digits answer it.
    if (input_.s.empty() && e.is_character() && screen_visible()) {
        const Session* s = selected();
        std::string c = e.character();
        if (s && s->state() == "blocked" && c.size() == 1 && c[0] >= '1' && c[0] <= '9') return send_keys({c}), true;
    }
    if (e == Event::CtrlC) {
        if (input_.s.empty()) mode_ = Mode::List;
        input_.clear();
        return true;
    }
    const std::string& in = e.input();
    bool newline = in == "\x1b\r" || in == "\x1b\n" || in == "\x1b[13;2u" || in == "\x1b[13;3u" || in == "\x1b[27;2;13~";
    if (e == Event::Return && input_.cur > 0 && input_.s[input_.cur - 1] == '\\') {
        // Same convention as Claude Code: a trailing backslash continues the message.
        input_.s.erase(input_.cur - 1, 1);
        --input_.cur;
        newline = true;
    }
    if (newline) return input_.insert("\n"), true;
    if (e == Event::Return) return send_reply(), true;
    if (e == Event::ArrowUp && input_.s.empty()) return mode_ = Mode::List, move(-1), true;
    if (e == Event::ArrowDown && input_.s.empty()) return mode_ = Mode::List, move(1), true;
    if (scroll_keys(e)) return true;
    input_.handle(e);
    return true;
}

bool App::key_interact(const Event& e) {
    if (e.input() == "\x1d") return mode_ = Mode::List, true;  // Ctrl-]
    std::string k = tmux_key(e);
    if (!k.empty()) send_keys({k});
    return true;
}

bool App::key_form(const Event& e) {
    if (e == Event::Escape) return mode_ = Mode::List, true;
    if (e == Event::Return) return submit_form(), true;
    if (e == Event::Tab) return form_.field = form_.field % 3 + 1, true;
    if (e == Event::TabReverse) return form_.field = form_.field == 1 ? 3 : form_.field - 1, true;
    auto reload_recent = [this] {
        form_.recent = state_.recent_dirs(hosts_[form_.host]->name());
        form_.pick = -1;
    };
    if (e == Event::ArrowLeftCtrl || e == Event::Special("\x1b[1;5D") || (form_.field == 0 && e == Event::ArrowLeft)) {
        form_.host = (form_.host + hosts_.size() - 1) % hosts_.size();
        reload_recent();
        return true;
    }
    if (e == Event::ArrowRightCtrl || e == Event::Special("\x1b[1;5C") || (form_.field == 0 && e == Event::ArrowRight)) {
        form_.host = (form_.host + 1) % hosts_.size();
        reload_recent();
        return true;
    }
    if (form_.field == 1 && (e == Event::ArrowDown || e == Event::ArrowUp)) {
        int n = static_cast<int>(std::min<size_t>(form_.recent.size(), 6));
        if (n == 0) return true;
        form_.pick = e == Event::ArrowDown ? std::min(form_.pick + 1, n - 1) : std::max(form_.pick - 1, 0);
        form_.dir.set(form_.recent[static_cast<size_t>(form_.pick)]);
        if (!form_.name_edited) form_.name.set(suggest_name(form_.dir.s));
        return true;
    }
    if (e == Event::ArrowDown) return form_.field = form_.field % 3 + 1, true;
    if (e == Event::ArrowUp) return form_.field = form_.field == 1 ? 3 : form_.field - 1, true;

    TextEdit* f = form_.field == 1 ? &form_.dir : form_.field == 2 ? &form_.name : &form_.flags;
    if (f->handle(e)) {
        if (form_.field == 1 && !form_.name_edited) form_.name.set(suggest_name(form_.dir.s));
        if (form_.field == 2) form_.name_edited = true;
    }
    return true;
}

// ---- rendering --------------------------------------------------------------

Element App::top_bar(int w) const {
    int n = 0, need = 0;
    for (const Session& s : sessions_) {
        ++n;
        need += s.attention();
    }
    Elements left = {text(" ccbus ") | bold, text("│ ") | dim, text(std::to_string(n) + (n == 1 ? " session" : " sessions"))};
    if (need) left.push_back(hbox({text(" · ") | dim, text(std::to_string(need) + " need you") | color(kBlocked) | bold}));

    Elements right;
    for (const auto& h : hosts_) {
        if (h->connected()) {
            right.push_back(text("● ") | color(kOk));
            right.push_back(text(h->name() + "  "));
        } else {
            int64_t wait = (h->retry_at() - now_ms() + 999) / 1000;
            std::string why = h->last_error().empty() ? "connecting" : h->last_error();
            if (wait > 0) why += " · retry " + std::to_string(wait) + "s";
            right.push_back(text("○ ") | color(kBlocked));
            right.push_back(text(h->name()) | dim);
            right.push_back(text(" " + truncate(why, std::max(10, w / 3)) + "  ") | dim);
        }
    }
    return hbox({hbox(std::move(left)), filler(), hbox(std::move(right))}) | bgcolor(kBarBg);
}

Element App::status_bar(int w) const {
    Elements es;
    if (!status_.empty()) {
        es.push_back(text(" " + truncate(status_, w - 2) + " ") | color(status_error_ ? kBlocked : kOk) | bold);
        return hbox(std::move(es)) | bgcolor(kBarBg);
    }
    switch (mode_) {
        case Mode::List: {
            // Most useful first: whatever doesn't fit is dropped from the end.
            es = {text(" ")};
            if (screen_visible()) es.push_back(chip("1-9", "answer")), es.push_back(chip("s", "interact"));
            es.insert(es.end(), {chip("j k", "move"), chip("enter", "reply"), chip("?", "help"), chip("n", "new"),
                                 chip("a", "attach"), chip("p", "screen"), chip("r", "resume"), chip("x", "kill"),
                                 chip("/", "filter"), chip("q", "quit")});
            break;
        }
        case Mode::Reply:
            es = {text(" "), chip("enter", "send"), chip("alt+enter", "newline"), chip("esc", "back to list"),
                  chip("pgup pgdn", "scroll")};
            break;
        case Mode::Interact:
            es = {text(" keys go straight to claude ") | color(kBlocked) | bold, text("│ ") | dim,
                  chip("ctrl+]", "back to list")};
            break;
        case Mode::Filter: {
            Line l = filter_.lines(true)[0];
            es = {text(" filter: ") | color(kAccent) | bold, render(l), text("   "), chip("enter", "keep"),
                  chip("esc", "clear")};
            break;
        }
        case Mode::Confirm:
            es = {text(" " + confirm_text_ + " ") | color(kBlocked) | bold, chip("y", "yes"), chip("any key", "no")};
            break;
        case Mode::Form:
            es = {text(" new session ") | color(kAccent) | bold, text("│ ") | dim, chip("tab", "next"),
                  chip("ctrl+← →", "host"), chip("↑↓", "recent dirs"), chip("enter", "create"), chip("esc", "cancel")};
            break;
        case Mode::Help:
            es = {text(" any key to close") | dim};
            break;
    }
    Elements fit;
    int used = 0;
    for (Element& e : es) {
        e->ComputeRequirement();
        used += e->requirement().min_x;
        if (used > w) break;
        fit.push_back(std::move(e));
    }
    return hbox(std::move(fit)) | bgcolor(kBarBg);
}

Element App::list_pane(int w, int h, bool narrow) {
    auto v = visible();
    bool multi = hosts_.size() > 1;
    int inner_w = w - 2;
    std::vector<Element> rows;
    int sel_top = -1, sel_bottom = -1;

    auto glyph = [&](const Session& s) -> Element {
        std::string st = s.state();
        if (st == "blocked") return text("!") | color(kBlocked) | bold;
        if (st == "running" || st == "starting") return text(kSpinner[ticks_ % 10]) | color(kRunning);
        if (st == "exited") return text("✕") | dim;
        if (s.unread()) return text("●") | color(kUnread);
        return text("○") | dim;
    };
    auto preview = [&](const Session& s) -> Element {
        std::string st = s.state();
        std::string d = plain(s.str("detail"));
        if (st == "exited") return text(d.empty() || d == "other" ? "exited · r to resume" : "exited · " + d) | dim;
        if (st == "starting") return text("starting…") | dim;
        if (d.empty()) d = one_line(s.str("title"));
        if (d.empty()) d = state_label(s);
        int pw = inner_w - (narrow ? 0 : 5);
        if (narrow) pw = std::max(8, inner_w - 3 - 16 - 9);
        Element e = text(truncate(d, pw));
        if (st == "blocked") return e | color(kBlocked);
        if (!s.unread()) return e | dim;
        return e;
    };

    int last_group = -1;
    for (const Session* s : v) {
        bool sel = s->id() == sel_;
        std::string right = (multi ? s->host + " · " : std::string()) + age(s->updated());
        Element marker = sel ? text("▌") | color(kAccent) : text(" ");
        Element name = text(truncate(s->name(), narrow ? 16 : inner_w - 6 - width(right)));
        if (s->attention() || sel) name = name | bold;

        if (narrow) {
            Element row = hbox({marker, glyph(*s), text(" "), name | size(WIDTH, EQUAL, 16), text(" "),
                                text(age(s->updated())) | dim | size(WIDTH, EQUAL, 5), text(" "), preview(*s), filler()});
            if (sel) {
                row = row | bgcolor(kSelBg);
                sel_top = sel_bottom = static_cast<int>(rows.size());
            }
            rows.push_back(row);
            continue;
        }
        int g = group(*s);
        if (g != last_group) {
            if (last_group != -1) rows.push_back(text(""));
            rows.push_back(text("  " + std::string(kGroupNames[g])) | dim);
            last_group = g;
        }
        Element l1 = hbox({marker, text(" "), glyph(*s), text("  "), name, filler(), text(right) | dim, text(" ")});
        Element l2 = hbox({sel ? text("▌") | color(kAccent) : text(" "), text("    "), preview(*s), filler()});
        if (sel) {
            l1 = l1 | bgcolor(kSelBg);
            l2 = l2 | bgcolor(kSelBg);
            sel_top = static_cast<int>(rows.size());
            sel_bottom = sel_top + 1;
        }
        rows.push_back(l1);
        rows.push_back(l2);
    }
    if (v.empty())
        rows.push_back(text(filter_.s.empty() ? "  no sessions yet — press n" : "  nothing matches the filter") | dim);

    int inner_h = std::max(1, h - 2);
    if (sel_top >= 0) {
        if (sel_top < list_scroll_) list_scroll_ = std::max(0, sel_top - (narrow ? 0 : 1));
        if (sel_bottom >= list_scroll_ + inner_h) list_scroll_ = sel_bottom - inner_h + 1;
    }
    list_scroll_ = std::clamp(list_scroll_, 0, std::max(0, static_cast<int>(rows.size()) - inner_h));
    Elements shown;
    for (int i = list_scroll_; i < static_cast<int>(rows.size()) && i < list_scroll_ + inner_h; ++i)
        shown.push_back(rows[static_cast<size_t>(i)]);

    std::string title = filter_.s.empty() ? "Sessions" : "Sessions · /" + filter_.s;
    bool focus = mode_ == Mode::List || mode_ == Mode::Filter;
    return pane(title_text(title, focus ? kAccent : Color::Default, focus), vbox(std::move(shown)) | flex, focus) |
           size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
}

const std::vector<Line>& App::chat_lines(int w) {
    if (!chat_dirty_ && w == chat_w_) return chat_;
    size_t old_n = chat_.size();
    bool same_width = w == chat_w_;
    chat_.clear();
    chat_w_ = w;
    chat_dirty_ = false;

    const Session* s = selected();
    std::string cwd = s ? s->str("cwd") : std::string();
    if (truncated_) chat_.push_back({{"  … earlier messages not loaded", Style().d()}});

    std::string prev;
    auto claude_header = [&](const json& m) {
        if (!chat_.empty()) chat_.push_back({});
        Style h;
        h.bold = true;
        chat_.push_back({{"claude", h}, {"  " + clock_of(m.value("ts", "")), Style().d()}});
    };
    for (const json& m : msgs_) {
        std::string kind = m.value("kind", "");
        std::string txt = m.value("text", "");
        bool in_turn = prev == "assistant" || prev == "tool_use" || prev == "tool_result";
        if (kind == "user") {
            if (!chat_.empty()) chat_.push_back({});
            chat_.push_back({{"you", Style::color(kUser).b()}, {"  " + clock_of(m.value("ts", "")), Style().d()}});
            size_t pos = 0;
            while (pos <= txt.size()) {
                size_t nl = txt.find('\n', pos);
                std::string l = txt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                pos = nl == std::string::npos ? txt.size() + 1 : nl + 1;
                Style st;
                st.bold = false;
                for (Line& x : wrap({{l, st}}, w, 2)) chat_.push_back(std::move(x));
            }
        } else if (kind == "assistant") {
            if (!in_turn) claude_header(m);
            else if (prev != "assistant") chat_.push_back({});
            for (Line& x : markdown(txt, w, 2)) chat_.push_back(std::move(x));
        } else if (kind == "tool_use") {
            if (!in_turn) claude_header(m);
            else if (prev == "assistant") chat_.push_back({});
            std::string arg = one_line(first_line(txt));
            if (!cwd.empty() && arg.rfind(cwd + "/", 0) == 0) arg = arg.substr(cwd.size() + 1);
            std::string tool = m.value("tool", "");
            chat_.push_back(truncate(Line{{"  ⚙ ", Style().d()}, {tool + "  ", Style().d()}, {arg, Style()}}, w));
        } else if (kind == "tool_result") {
            int more = 0;
            std::string l = one_line(first_line(txt, &more));
            bool err = m.value("error", false);
            Line line{{"    ↳ ", Style().d()}};
            if (err) line.push_back({l.empty() ? "error" : l, Style::color(kBlocked)});
            else line.push_back({l.empty() ? "(no output)" : l, Style().d()});
            if (more > 0) line.push_back({"  +" + std::to_string(more) + " lines", Style().d()});
            chat_.push_back(truncate(line, w));
        } else if (kind == "command") {
            chat_.push_back({});
            std::string c = "── " + txt + " ──";
            int pad = std::max(0, (w - width(c)) / 2);
            chat_.push_back({{std::string(static_cast<size_t>(pad), ' ') + c, Style().d()}});
        } else if (kind == "system") {
            chat_.push_back({});
            Style st;
            st.dim = true;
            st.italic = true;
            for (Line& x : wrap({{"※ " + one_line(txt), st}}, w, 2, 2)) chat_.push_back(std::move(x));
        } else {
            continue;
        }
        prev = kind;
    }
    // Keep the view still while scrolled up and new lines arrive.
    if (same_width && scroll_ > 0 && chat_.size() > old_n) scroll_ += static_cast<int>(chat_.size() - old_n);
    chat_n_ = msgs_.size();
    return chat_;
}

Element App::chat_pane(int w, int h) {
    const Session* s = selected();
    int inner_h = std::max(1, h - 2);
    int cw = std::max(10, w - 4);
    chat_page_ = inner_h;
    if (!s) {
        Element msg = vbox({filler(), text(sessions_.empty() ? "No sessions yet. Press n to start one." : "Select a session.") | dim | hcenter, filler()});
        return pane(text(""), msg, false) | size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
    }
    Host* hst = host(s->host);
    bool online = hst && hst->connected();

    Elements title = {text(" " + s->name() + " ") | bold};
    std::string cwd = short_cwd(s->str("cwd"), hst && hst->config().local());
    if (hosts_.size() > 1) cwd = s->host + ":" + cwd;
    std::string label = state_label(*s);
    int room = w - 6 - width(s->name()) - width(label) - 4;
    if (room > 8) title.push_back(text(truncate(cwd, room) + " ") | dim);
    Color sc = s->state() == "blocked" ? kBlocked : s->unread() ? kUnread : s->state() == "running" ? kRunning : Color::GrayLight;
    title.push_back(text(" ● " + label + " ") | color(sc));
    if (!online) title.push_back(text(" offline ") | color(kBlocked));

    const std::vector<Line>& lines = chat_lines(cw);
    int n = static_cast<int>(lines.size());
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - inner_h));
    int end = n - scroll_;
    int begin = std::max(0, end - inner_h);
    Elements es;
    if (lines.empty()) {
        es.push_back(text(loaded_ ? "  no messages yet" : online ? "  loading…" : "  host offline") | dim);
    }
    for (int i = begin; i < end; ++i) es.push_back(hbox({text(" "), render(lines[static_cast<size_t>(i)])}));
    Element body = vbox(std::move(es));
    if (scroll_ > 0)
        body = vbox({body | flex, hbox({filler(), text(" ↓ " + std::to_string(scroll_) + " more lines · pgdn ") | dim})});
    else
        body = body | flex;
    bool focus = mode_ == Mode::Reply;
    return pane(hbox(std::move(title)), body, false, focus ? std::optional<Color>(Color::GrayDark) : std::nullopt) |
           size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
}

Element App::screen_pane(int w, int h) const {
    const Session* s = selected();
    bool blocked = s && s->state() == "blocked";
    bool interact = mode_ == Mode::Interact;
    int inner_h = std::max(1, h - 3);
    int iw = std::max(10, w - 4);
    Elements es;
    int n = static_cast<int>(screen_lines_.size());
    for (int i = std::max(0, n - inner_h); i < n; ++i)
        es.push_back(hbox({text(" "), render(truncate(screen_lines_[static_cast<size_t>(i)], iw))}));
    if (screen_lines_.empty()) es.push_back(text(" capturing…") | dim);
    Element keys = interact ? hbox({text(" typing into claude · ") | color(kBlocked), text("ctrl+] to stop") | dim})
                            : hbox({text(" "), chip("1 2 3", "answer"), chip("s", "interact"), chip("a", "attach"),
                                    chip("p", "hide")});
    Color c = blocked || interact ? kBlocked : Color::GrayDark;
    Elements title = {text(" Screen · live ") | color(blocked || interact ? kBlocked : Color::Default) | bold};
    if (s) title.push_back(text(" tmux " + s->pane() + " ") | dim);
    return pane(hbox(std::move(title)), vbox({vbox(std::move(es)) | flex, keys}), false, c) |
           size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
}

int App::reply_lines(int w) const {
    int n = 0;
    for (const Line& l : input_.lines(true)) n += static_cast<int>(wrap(l, std::max(10, w - 6)).size());
    return std::clamp(n, 1, 8);
}

Element App::reply_pane(int w, int h) const {
    const Session* s = selected();
    bool focus = mode_ == Mode::Reply;
    Elements es;
    bool can = s && !s->pane().empty() && s->state() != "exited";
    if (!can && input_.s.empty() && !focus) {
        std::string why = !s ? "" : s->pane().empty() ? "not running in tmux — read only" : "exited — r to resume";
        es.push_back(text("  " + why) | dim);
    } else if (input_.s.empty() && !focus) {
        es.push_back(text("  enter to reply") | dim);
    } else {
        std::vector<Line> all;
        for (const Line& l : input_.lines(focus))
            for (Line& x : wrap(l, std::max(10, w - 6))) all.push_back(std::move(x));
        int inner_h = std::max(1, h - 2);
        int start = std::max(0, static_cast<int>(all.size()) - inner_h);
        for (int i = start; i < static_cast<int>(all.size()); ++i) {
            Element prefix = i == 0 ? text(" › ") | color(kAccent) | bold : text("   ");
            es.push_back(hbox({prefix, render(all[static_cast<size_t>(i)])}));
        }
    }
    return pane(title_text("Reply", focus ? kAccent : Color::Default, focus), vbox(std::move(es)), focus) |
           size(WIDTH, EQUAL, w) | size(HEIGHT, EQUAL, h);
}

Element App::form_overlay() const {
    auto label = [&](int field, const std::string& l) {
        Element e = text(l) | size(WIDTH, EQUAL, 11);
        return form_.field == field ? e | color(kAccent) | bold : e | dim;
    };
    auto field = [&](int f, const TextEdit& t) -> Element {
        bool on = form_.field == f;
        Line l = t.lines(on)[0];
        return render(width(l) > 52 ? tail(l, 52) : l) | (on ? underlined : nothing);
    };

    Elements hosts_row = {label(0, "host")};
    for (size_t i = 0; i < hosts_.size(); ++i) {
        Element e = text(" " + hosts_[i]->name() + " ");
        if (i == form_.host) e = e | inverted | bold;
        else if (!hosts_[i]->connected()) e = e | dim;
        hosts_row.push_back(e);
        hosts_row.push_back(text(" "));
    }
    if (!hosts_[form_.host]->connected()) hosts_row.push_back(text("offline") | color(kBlocked));

    Elements recent;
    for (size_t i = 0; i < form_.recent.size() && i < 6; ++i) {
        bool p = static_cast<int>(i) == form_.pick;
        recent.push_back(hbox({text("           "), text(p ? "❯ " : "  ") | color(kAccent),
                               render(tail(Line{{form_.recent[i], Style()}}, 56)) | (p ? bold : dim)}));
    }
    if (form_.recent.empty()) recent.push_back(text("           no recent directories on this host") | dim);

    Element body = vbox({
        hbox(std::move(hosts_row)),
        text(""),
        hbox({label(1, "directory"), field(1, form_.dir)}),
        vbox(std::move(recent)),
        text(""),
        hbox({label(2, "name"), field(2, form_.name), text("  → tmux cc-" + form_.name.s) | dim}),
        hbox({label(3, "flags"), field(3, form_.flags),
              text(form_.flags.s.empty() && form_.field != 3 ? "e.g. --model opus" : "") | dim}),
        text(""),
        hbox({filler(), chip("enter", "create"), chip("esc", "cancel")}),
    });
    return modal(window(text(" New session ") | color(kAccent) | bold, body | color(Color::Default)) |
                 color(kAccent) | size(WIDTH, EQUAL, 76));
}

Element App::help_overlay() const {
    auto row = [](const std::string& k, const std::string& v) {
        return hbox({text(k) | bold | size(WIDTH, EQUAL, 16), text(v)});
    };
    Element body = vbox({
        text("Sessions") | color(kAccent) | bold,
        row("j k  ↑ ↓", "move"), row("g G", "first / last"), row("/", "filter"),
        row("enter i", "reply"), row("n", "new session"), row("r", "resume an exited session"),
        row("x", "kill"), row("D", "remove from the list"), row("q", "quit"),
        text(""),
        text("Claude's terminal") | color(kAccent) | bold,
        row("p", "show / hide the live screen"), row("1-9", "answer a prompt shown on screen"),
        row("s", "type straight into claude (ctrl+] stops)"),
        row("a", "attach: open the real claude UI (tmux)"),
        text(""),
        text("Chat") | color(kAccent) | bold,
        row("pgup pgdn", "scroll (ctrl+b/f, ctrl+u/d)"), row("enter", "send"),
        row("alt+enter  \\⏎", "newline"), row("esc", "back to the list"),
        text(""),
        text("hosts: " + cfg_.path) | dim,
    });
    return modal(window(text(" Keys ") | bold, body | color(Color::Default)) | size(WIDTH, EQUAL, 62));
}

Element App::view() {
    Dimensions d = Terminal::Size();
    int W = std::max(d.dimx, 40), H = std::max(d.dimy, 12);
    int body_h = H - 2;
    bool wide = W >= 100;

    int list_w = wide ? std::clamp(W * 3 / 10, 32, 46) : W;
    int right_w = wide ? W - list_w : W;
    int list_h = body_h;
    if (!wide) {
        int n = static_cast<int>(visible().size());
        list_h = std::clamp(n + 2, 4, std::max(4, body_h / 3));
    }
    int right_h = wide ? body_h : body_h - list_h;

    int reply_h = reply_lines(right_w) + 2;
    int screen_h = 0;
    if (screen_visible()) {
        int want = static_cast<int>(screen_lines_.size()) + 3;
        screen_h = std::clamp(want, 6, std::max(6, (right_h - reply_h) * 3 / 5));
    }
    int chat_h = std::max(3, right_h - reply_h - screen_h);

    Elements right = {chat_pane(right_w, chat_h)};
    if (screen_h) right.push_back(screen_pane(right_w, screen_h));
    right.push_back(reply_pane(right_w, reply_h));

    Element body = wide ? hbox({list_pane(list_w, body_h, false), vbox(std::move(right))})
                        : vbox({list_pane(list_w, list_h, true), vbox(std::move(right))});
    Element main = vbox({top_bar(W), body | flex, status_bar(W)});
    if (mode_ == Mode::Form) return dbox({main, form_overlay()});
    if (mode_ == Mode::Help) return dbox({main, help_overlay()});
    return main;
}

}  // namespace tui

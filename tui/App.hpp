#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "Config.hpp"
#include "Host.hpp"
#include "Notifier.hpp"
#include "Text.hpp"

namespace tui {

struct Session {
    std::string host;
    json d;  // row as sent by `ccbus serve`

    static std::string make_id(const std::string& host, const std::string& key) { return host + '\x1f' + key; }
    std::string id() const { return make_id(host, key()); }
    std::string str(const char* k) const {
        auto it = d.find(k);
        return it != d.end() && it->is_string() ? it->get<std::string>() : std::string();
    }
    std::string key() const { return str("key"); }
    std::string name() const { return str("name"); }
    std::string state() const { return str("state"); }
    std::string pane() const { return str("pane"); }
    bool unread() const { return d.value("unread", false); }
    bool attention() const { return state() == "blocked" || unread(); }
    int64_t updated() const { return d.value("updated", int64_t{0}); }
};

// Single- or multi-line text field.
struct TextEdit {
    std::string s;
    size_t cur = 0;
    bool handle(const ftxui::Event& e);  // true if consumed
    void insert(const std::string& t);
    void set(std::string t) {
        s = std::move(t);
        cur = s.size();
    }
    void clear() { set(""); }
    // The text with the cursor drawn in, split at newlines.
    std::vector<Line> lines(bool show_cursor, const Style& st = Style()) const;
};

class App {
public:
    App(Config cfg, ftxui::ScreenInteractive& screen);
    ~App();

    ftxui::Element view();
    bool on_event(const ftxui::Event& e);
    void tick();
    // Terminal modes we turn on ourselves (bracketed paste, focus reports).
    static void enable_modes(bool on);

private:
    enum class Mode { List, Reply, Interact, Filter, Form, Confirm, Help };

    // data
    Host* host(const std::string& name) const;
    Session* find(const std::string& id);
    const Session* selected() const;
    std::vector<const Session*> visible() const;
    int group(const Session& s) const;
    void upsert(const std::string& host, const json& row);

    // host callbacks
    void on_snapshot(Host* h, const std::vector<json>& rows, int64_t seq);
    void on_session(Host* h, const std::string& kind, const json& row, int64_t seq);
    void on_transcript(Host* h, const json& frame);

    // behaviour
    void select(const std::string& id);
    void move(int delta);
    bool looking() const;
    void maybe_mark_read();
    void notify_for(const Session& s, bool blocked);
    void send_reply();
    void send_keys(const std::vector<std::string>& keys);
    void attach();
    void resume();
    void kill_selected();
    void remove_selected();
    void open_form();
    void submit_form();
    void capture();
    bool screen_visible() const;
    void flash(const std::string& msg, bool error = false);
    void on_paste(const std::string& text);
    void update_title();

    bool key_list(const ftxui::Event& e);
    bool key_reply(const ftxui::Event& e);
    bool key_interact(const ftxui::Event& e);
    bool key_form(const ftxui::Event& e);
    bool scroll_keys(const ftxui::Event& e);

    // rendering
    ftxui::Element top_bar(int w) const;
    ftxui::Element status_bar(int w) const;
    ftxui::Element list_pane(int w, int h, bool narrow);
    ftxui::Element chat_pane(int w, int h);
    ftxui::Element screen_pane(int w, int h) const;
    ftxui::Element reply_pane(int w, int h) const;
    ftxui::Element form_overlay() const;
    ftxui::Element help_overlay() const;
    const std::vector<Line>& chat_lines(int w);
    int reply_lines(int w) const;

    ftxui::ScreenInteractive& screen_;
    Config cfg_;
    State state_;
    std::vector<std::unique_ptr<Host>> hosts_;
    std::unique_ptr<Notifier> notifier_;

    std::vector<Session> sessions_;
    std::string sel_;
    std::string pending_sel_;
    int sel_group_ = -1;
    std::map<std::string, int64_t> marked_;  // id -> attention_seq already marked read

    // transcript of the selected session
    std::vector<json> msgs_;
    std::set<int64_t> offsets_;
    bool truncated_ = false;
    bool loaded_ = false;
    std::vector<Line> chat_;
    int chat_w_ = -1;
    size_t chat_n_ = 0;
    bool chat_dirty_ = true;
    int scroll_ = 0;  // lines above the bottom
    int chat_page_ = 10;

    Mode mode_ = Mode::List;
    TextEdit input_;
    TextEdit filter_;
    std::string confirm_text_;
    std::function<void()> confirm_action_;

    struct Form {
        size_t host = 0;
        int field = 1;  // 0 host, 1 dir, 2 name, 3 flags
        TextEdit dir, name, flags;
        bool name_edited = false;
        int pick = -1;
        std::vector<std::string> recent;
    } form_;

    bool peek_ = false;
    std::vector<Line> screen_lines_;
    int64_t last_capture_ = 0;
    bool capture_busy_ = false;

    std::string status_;
    bool status_error_ = false;
    int64_t status_until_ = 0;

    bool pasting_ = false;
    std::string paste_buf_;
    bool focus_known_ = false;
    bool focused_ = true;
    int64_t last_key_ = 0;
    int64_t ticks_ = 0;
    int list_scroll_ = 0;
    int title_count_ = -1;
};

}  // namespace tui

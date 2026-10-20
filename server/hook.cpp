// `ccbus hook`: invoked by Claude Code hooks with the event JSON on stdin.
// Must be fast and must never fail loudly: it always exits 0 and prints nothing.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

#include "commands.hpp"
#include "db.hpp"
#include "transcript.hpp"

namespace ccbus {

namespace {

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

struct Update {
    std::string state;
    std::string kind;
    std::string detail;
};

void apply(const json& in) {
    std::string event = in.value("hook_event_name", "");
    std::string sid = in.value("session_id", "");
    std::string cwd = in.value("cwd", "");
    std::string transcript = in.value("transcript_path", "");
    std::string ccname = env("CCBUS_NAME");
    std::string pane = env("TMUX_PANE");

    std::string key = !ccname.empty() ? ccname : sid;
    if (key.empty()) return;
    std::string name = !ccname.empty() ? ccname : basename_of(cwd);

    Db db(db_path());
    auto current = db.session(key);
    std::string cur_state = current ? (*current)["state"].get<std::string>() : "";

    Update u;
    std::string title;
    if (event == "SessionStart") {
        u = {"idle", "start", ""};
    } else if (event == "UserPromptSubmit") {
        u = {"running", "prompt", truncate_utf8(in.value("prompt", ""), 200)};
    } else if (event == "PostToolUse") {
        // Only interesting as "permission was granted, work resumed".
        if (current && cur_state != "blocked") return;
        u = {"running", "resumed", current ? (*current)["detail"].get<std::string>() : ""};
    } else if (event == "Stop") {
        // The final assistant line may not be flushed to the transcript yet, so prefer
        // the copy Claude Code passes in; the transcript scan still supplies the title.
        TranscriptTail tail = scan_tail(transcript, 512 * 1024);
        title = tail.title;
        std::string last = in.value("last_assistant_message", "");
        if (last.empty()) last = tail.last_assistant_text;
        u = {"waiting", "stop", truncate_utf8(last, 300)};
    } else if (event == "Notification") {
        std::string type = in.value("notification_type", "");
        std::string message = in.value("message", "");
        bool permission = type == "permission_prompt" ||
                          (type.empty() && message.find("permission") != std::string::npos);
        if (!permission) return;  // idle reminders etc. add nothing over "waiting"
        // The message is generic; the pending tool call is already in the transcript.
        std::string tool = scan_tail(transcript, 256 * 1024).last_tool;
        if (auto p = tool.find(cwd + "/"); !cwd.empty() && p != std::string::npos) tool.erase(p, cwd.size() + 1);
        u = {"blocked", "blocked", tool.empty() ? message : truncate_utf8(tool, 200)};
    } else if (event == "SessionEnd") {
        if (in.value("reason", "") == "clear") return;  // a SessionStart follows immediately
        // Stale: the pane was respawned (resume) and this is the old claude dying, either
        // before (still "starting") or after the replacement registered its own session id.
        // A claude that genuinely dies is still caught by serve's dead-pane check.
        std::string cs = current ? (*current)["claude_session"].get<std::string>() : "";
        if (current && (cur_state == "starting" || (!cs.empty() && cs != sid))) return;
        u = {"exited", "end", in.value("reason", "")};
    } else {
        return;
    }

    int64_t now = now_ms();
    Tx tx(db);
    db.prepare(R"sql(
        INSERT INTO sessions(key,name,cwd,pane,claude_session,transcript,state,detail,created,updated)
        VALUES(?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(key) DO UPDATE SET
          name=excluded.name,
          cwd=CASE WHEN excluded.cwd<>'' THEN excluded.cwd ELSE sessions.cwd END,
          pane=CASE WHEN excluded.pane<>'' THEN excluded.pane ELSE sessions.pane END,
          claude_session=CASE WHEN excluded.claude_session<>'' THEN excluded.claude_session
                              ELSE sessions.claude_session END,
          transcript=CASE WHEN excluded.transcript<>'' THEN excluded.transcript
                          ELSE sessions.transcript END,
          state=excluded.state, detail=excluded.detail, updated=excluded.updated
    )sql")
        .bind(key, name, cwd, pane, sid, transcript, u.state, u.detail, now, now)
        .run();
    if (!title.empty()) db.prepare("UPDATE sessions SET title=? WHERE key=?").bind(title, key).run();
    db.add_event(key, u.kind);
    // Typing a prompt means the user has seen whatever the session last said.
    if (u.kind == "prompt")
        db.prepare("UPDATE sessions SET read_seq=attention_seq WHERE key=?").bind(key).run();
    tx.commit();
}

}  // namespace

int cmd_hook() {
    std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    try {
        json in = json::parse(input, nullptr, false);
        if (!in.is_discarded() && in.is_object()) apply(in);
    } catch (const std::exception& e) {
        std::ofstream log(state_dir() + "/hook.log", std::ios::app);
        log << now_ms() << " " << e.what() << "\n";
    }
    return 0;
}

}  // namespace ccbus

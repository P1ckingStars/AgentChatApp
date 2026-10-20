#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "util.hpp"

namespace ccbus {

class Stmt {
public:
    Stmt(sqlite3* db, const char* sql);
    ~Stmt();
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;

    template <class... A>
    Stmt& bind(A&&... args) {
        int i = 1;
        (bind_one(i++, std::forward<A>(args)), ...);
        return *this;
    }
    bool step();  // true while a row is available
    void run() {
        while (step()) {
        }
    }
    std::string text(int col);
    int64_t i64(int col);

private:
    void bind_one(int i, const std::string& v);
    void bind_one(int i, const char* v);
    void bind_one(int i, int64_t v);
    void bind_one(int i, int v) { bind_one(i, static_cast<int64_t>(v)); }

    sqlite3* db_;
    sqlite3_stmt* st_ = nullptr;
};

struct Event {
    int64_t seq;
    std::string key;
    std::string kind;
};

// Event kinds that mean "the user should look at this session".
inline bool is_attention_kind(const std::string& kind) {
    return kind == "stop" || kind == "blocked";
}

class Db {
public:
    explicit Db(const std::string& path);
    ~Db();
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    void exec(const char* sql);
    Stmt prepare(const char* sql) { return Stmt(db_, sql); }
    int changes() { return sqlite3_changes(db_); }

    std::optional<json> session(const std::string& key);
    std::vector<json> sessions();
    int64_t max_seq();
    // Appends an event and, for attention kinds, bumps the session's attention_seq.
    int64_t add_event(const std::string& key, const std::string& kind);
    std::vector<Event> events_since(int64_t seq, int limit);
    // Records a session we just launched in tmux, before claude's first hook fires.
    void register_started(const std::string& key, const std::string& cwd, const std::string& pane,
                          const json& args);

private:
    sqlite3* db_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT, rolled back if not committed.
class Tx {
public:
    explicit Tx(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Tx() {
        if (!done_) {
            try {
                db_.exec("ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit() {
        db_.exec("COMMIT");
        done_ = true;
    }

private:
    Db& db_;
    bool done_ = false;
};

}  // namespace ccbus

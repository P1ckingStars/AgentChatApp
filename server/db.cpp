#include "db.hpp"

#include <stdexcept>

namespace ccbus {

namespace {

const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS sessions(
  key            TEXT PRIMARY KEY,   -- ccbus name (also tmux session cc-<name>) or claude session id
  name           TEXT NOT NULL DEFAULT '',
  cwd            TEXT NOT NULL DEFAULT '',
  pane           TEXT NOT NULL DEFAULT '',   -- tmux pane id like %12, empty if not in tmux
  claude_session TEXT NOT NULL DEFAULT '',
  transcript     TEXT NOT NULL DEFAULT '',
  title          TEXT NOT NULL DEFAULT '',
  state          TEXT NOT NULL DEFAULT 'idle',  -- starting|idle|running|waiting|blocked|exited
  detail         TEXT NOT NULL DEFAULT '',
  created        INTEGER NOT NULL DEFAULT 0,
  updated        INTEGER NOT NULL DEFAULT 0,
  attention_seq  INTEGER NOT NULL DEFAULT 0,  -- seq of last stop/blocked event
  read_seq       INTEGER NOT NULL DEFAULT 0,  -- attention_seq the user has seen
  args           TEXT NOT NULL DEFAULT '[]'   -- claude flags it was started with (JSON array)
);
CREATE TABLE IF NOT EXISTS events(
  seq  INTEGER PRIMARY KEY AUTOINCREMENT,
  ts   INTEGER NOT NULL,
  key  TEXT NOT NULL,
  kind TEXT NOT NULL
);
)sql";

const char* kSessionCols =
    "key,name,cwd,pane,claude_session,transcript,title,state,detail,created,updated,"
    "attention_seq,read_seq,args";

json row_to_json(Stmt& s) {
    json j = {
        {"key", s.text(0)},          {"name", s.text(1)},        {"cwd", s.text(2)},
        {"pane", s.text(3)},         {"claude_session", s.text(4)},
        {"transcript", s.text(5)},   {"title", s.text(6)},       {"state", s.text(7)},
        {"detail", s.text(8)},       {"created", s.i64(9)},      {"updated", s.i64(10)},
        {"attention_seq", s.i64(11)}, {"read_seq", s.i64(12)},
    };
    json args = json::parse(s.text(13), nullptr, false);
    j["args"] = args.is_array() ? args : json::array();
    j["unread"] = s.i64(11) > s.i64(12);
    return j;
}

}  // namespace

Stmt::Stmt(sqlite3* db, const char* sql) : db_(db) {
    if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
}

Stmt::~Stmt() { sqlite3_finalize(st_); }

bool Stmt::step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("sqlite step: ") + sqlite3_errmsg(db_));
}

std::string Stmt::text(int col) {
    auto* p = reinterpret_cast<const char*>(sqlite3_column_text(st_, col));
    return p ? std::string(p, static_cast<size_t>(sqlite3_column_bytes(st_, col))) : std::string();
}

int64_t Stmt::i64(int col) { return sqlite3_column_int64(st_, col); }

void Stmt::bind_one(int i, const std::string& v) {
    sqlite3_bind_text(st_, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}
void Stmt::bind_one(int i, const char* v) { bind_one(i, std::string(v)); }
void Stmt::bind_one(int i, int64_t v) { sqlite3_bind_int64(st_, i, v); }

Db::Db(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        throw std::runtime_error("cannot open " + path + ": " + err);
    }
    sqlite3_busy_timeout(db_, 3000);
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA synchronous=NORMAL");
    exec(kSchema);
    Stmt v(db_, "PRAGMA user_version");
    v.step();
    if (v.i64(0) < 1) {
        // Databases created before the args column existed.
        sqlite3_exec(db_, "ALTER TABLE sessions ADD COLUMN args TEXT NOT NULL DEFAULT '[]'", nullptr, nullptr,
                     nullptr);
        exec("PRAGMA user_version=1");
    }
}

void Db::register_started(const std::string& key, const std::string& cwd, const std::string& pane,
                          const json& args) {
    int64_t now = now_ms();
    Tx tx(*this);
    prepare(R"sql(
        INSERT INTO sessions(key,name,cwd,pane,state,detail,created,updated,args)
        VALUES(?,?,?,?,'starting','',?,?,?)
        ON CONFLICT(key) DO UPDATE SET cwd=excluded.cwd, pane=excluded.pane, state='starting',
          detail='', updated=excluded.updated, args=excluded.args
    )sql")
        .bind(key, key, cwd, pane, now, now, dump(args))
        .run();
    add_event(key, "created");
    tx.commit();
}

Db::~Db() { sqlite3_close(db_); }

void Db::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + msg);
    }
}

std::optional<json> Db::session(const std::string& key) {
    std::string sql = std::string("SELECT ") + kSessionCols + " FROM sessions WHERE key=?";
    Stmt s(db_, sql.c_str());
    s.bind(key);
    if (!s.step()) return std::nullopt;
    return row_to_json(s);
}

std::vector<json> Db::sessions() {
    std::string sql = std::string("SELECT ") + kSessionCols + " FROM sessions ORDER BY updated DESC";
    Stmt s(db_, sql.c_str());
    std::vector<json> out;
    while (s.step()) out.push_back(row_to_json(s));
    return out;
}

int64_t Db::max_seq() {
    Stmt s(db_, "SELECT COALESCE(MAX(seq),0) FROM events");
    s.step();
    return s.i64(0);
}

int64_t Db::add_event(const std::string& key, const std::string& kind) {
    prepare("INSERT INTO events(ts,key,kind) VALUES(?,?,?)").bind(now_ms(), key, kind).run();
    int64_t seq = sqlite3_last_insert_rowid(db_);
    if (is_attention_kind(kind))
        prepare("UPDATE sessions SET attention_seq=? WHERE key=?").bind(seq, key).run();
    return seq;
}

std::vector<Event> Db::events_since(int64_t seq, int limit) {
    Stmt s(db_, "SELECT seq,key,kind FROM events WHERE seq>? ORDER BY seq LIMIT ?");
    s.bind(seq, limit);
    std::vector<Event> out;
    while (s.step()) out.push_back({s.i64(0), s.text(1), s.text(2)});
    return out;
}

}  // namespace ccbus

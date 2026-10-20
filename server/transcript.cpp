#include "transcript.hpp"

#include <fstream>

#include <sys/stat.h>

namespace ccbus {

namespace {

constexpr size_t kMaxText = 16 * 1024;
constexpr size_t kMaxToolResult = 1500;

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "<command-name>/clear</command-name>" -> "/clear"
std::string tag_body(const std::string& s, const std::string& tag) {
    std::string open = "<" + tag + ">", close = "</" + tag + ">";
    auto a = s.find(open);
    if (a == std::string::npos) return {};
    a += open.size();
    auto b = s.find(close, a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

std::string tool_summary(const std::string& name, const json& input) {
    if (!input.is_object()) return {};
    for (const char* k : {"command", "file_path", "pattern", "url", "query", "description", "prompt"}) {
        std::string v = str(input, k);
        if (!v.empty()) return truncate_utf8(v, 300);
    }
    (void)name;
    return truncate_utf8(dump(input), 300);
}

std::string tool_result_text(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string out;
    if (content.is_array()) {
        for (const auto& b : content) {
            if (str(b, "type") == "text") {
                if (!out.empty()) out += "\n";
                out += str(b, "text");
            } else if (str(b, "type") == "image") {
                out += "[image]";
            }
        }
    }
    return out;
}

json msg(int64_t offset, const char* kind, const std::string& text, const std::string& ts) {
    return {{"offset", offset}, {"kind", kind}, {"text", text}, {"ts", ts}};
}

// A transcript line can hold several content blocks; we emit one message per line,
// merging its blocks, since Claude Code writes roughly one block per line.
std::optional<json> normalize_user(const json& j, int64_t offset, const std::string& ts) {
    const json& content = j["message"]["content"];
    if (content.is_string()) {
        std::string s = content.get<std::string>();
        if (s.rfind("<command-name>", 0) == 0 || s.find("<command-name>") != std::string::npos) {
            std::string cmd = tag_body(s, "command-name");
            std::string args = tag_body(s, "command-args");
            if (!args.empty()) cmd += " " + args;
            return msg(offset, "command", cmd, ts);
        }
        if (!s.empty() && s[0] == '<') return std::nullopt;  // local-command output, reminders...
        return msg(offset, "user", truncate_utf8(s, kMaxText), ts);
    }
    if (!content.is_array()) return std::nullopt;

    std::string text;
    for (const auto& b : content) {
        std::string type = str(b, "type");
        if (type == "tool_result") {
            json m = msg(offset, "tool_result",
                         truncate_utf8(tool_result_text(b.value("content", json())), kMaxToolResult), ts);
            m["error"] = b.value("is_error", false);
            return m;
        }
        if (type == "text") {
            std::string t = str(b, "text");
            if (t.empty() || t[0] == '<') continue;
            if (!text.empty()) text += "\n\n";
            text += t;
        } else if (type == "image") {
            text += text.empty() ? "[image]" : "\n[image]";
        }
    }
    if (text.empty()) return std::nullopt;
    return msg(offset, "user", truncate_utf8(text, kMaxText), ts);
}

std::optional<json> normalize_assistant(const json& j, int64_t offset, const std::string& ts) {
    const json& content = j["message"]["content"];
    if (!content.is_array()) return std::nullopt;
    std::string text;
    for (const auto& b : content) {
        std::string type = str(b, "type");
        if (type == "tool_use") {
            std::string name = str(b, "name");
            json m = msg(offset, "tool_use", tool_summary(name, b.value("input", json())), ts);
            m["tool"] = name;
            return m;
        }
        if (type == "text") {
            if (!text.empty()) text += "\n\n";
            text += str(b, "text");
        }
    }
    if (text.empty()) return std::nullopt;
    return msg(offset, "assistant", truncate_utf8(text, kMaxText), ts);
}

}  // namespace

std::optional<json> normalize_line(const std::string& line, int64_t offset) {
    json j = json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    if (j.value("isSidechain", false) || j.value("isMeta", false)) return std::nullopt;

    std::string type = str(j, "type");
    std::string ts = str(j, "timestamp");
    if (type == "user" && j.contains("message")) return normalize_user(j, offset, ts);
    if (type == "assistant" && j.contains("message")) return normalize_assistant(j, offset, ts);
    if (type == "ai-title") return msg(offset, "title", str(j, "aiTitle"), ts);
    if (type == "system" && str(j, "subtype") == "away_summary")
        return msg(offset, "system", str(j, "content"), ts);
    return std::nullopt;
}

bool read_transcript(const std::string& path, int64_t& offset, std::vector<json>& out,
                     size_t max_bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    int64_t size = f.tellg();
    if (size < offset) offset = 0;  // file was replaced or truncated
    if (size == offset) return true;

    size_t want = static_cast<size_t>(std::min<int64_t>(size - offset, static_cast<int64_t>(max_bytes)));
    std::string buf(want, '\0');
    f.seekg(offset);
    f.read(buf.data(), static_cast<std::streamsize>(want));
    buf.resize(static_cast<size_t>(f.gcount()));

    size_t pos = 0;
    while (true) {
        size_t nl = buf.find('\n', pos);
        if (nl == std::string::npos) break;  // incomplete line: wait for the rest
        if (nl > pos) {
            if (auto m = normalize_line(buf.substr(pos, nl - pos), offset + static_cast<int64_t>(pos)))
                out.push_back(std::move(*m));
        }
        pos = nl + 1;
    }
    // A single line longer than max_bytes would stall us forever; skip it.
    if (pos == 0 && buf.size() == max_bytes) pos = buf.size();
    offset += static_cast<int64_t>(pos);
    return true;
}

int64_t initial_offset(const std::string& path, int64_t tail, bool& truncated) {
    truncated = false;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= tail) return 0;
    std::ifstream f(path, std::ios::binary);
    int64_t start = st.st_size - tail;
    f.seekg(start);
    std::string skipped;
    std::getline(f, skipped);  // drop the partial line
    truncated = true;
    return start + static_cast<int64_t>(skipped.size()) + 1;
}

TranscriptTail scan_tail(const std::string& path, size_t tail_bytes) {
    TranscriptTail out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    int64_t size = f.tellg();
    int64_t start = std::max<int64_t>(0, size - static_cast<int64_t>(tail_bytes));
    std::string buf(static_cast<size_t>(size - start), '\0');
    f.seekg(start);
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));

    // Walk lines backwards; the first partial line (if start > 0) fails to parse and is ignored.
    size_t end = buf.size();
    while (end > 0 && (out.last_assistant_text.empty() || out.title.empty() || out.last_tool.empty())) {
        size_t nl = buf.rfind('\n', end - 1);
        size_t begin = nl == std::string::npos ? 0 : nl + 1;
        if (end > begin) {
            auto m = normalize_line(buf.substr(begin, end - begin), 0);
            if (m) {
                std::string kind = (*m)["kind"];
                if (kind == "assistant" && out.last_assistant_text.empty())
                    out.last_assistant_text = (*m)["text"];
                else if (kind == "title" && out.title.empty())
                    out.title = (*m)["text"];
                else if (kind == "tool_use" && out.last_tool.empty())
                    out.last_tool = (*m)["tool"].get<std::string>() + " " + (*m)["text"].get<std::string>();
            }
        }
        if (nl == std::string::npos) break;
        end = nl;
    }
    return out;
}

}  // namespace ccbus

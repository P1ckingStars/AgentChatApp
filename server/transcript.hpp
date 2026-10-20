#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

namespace ccbus {

// Turns one Claude Code transcript JSONL line into a chat message:
//   {offset, kind, text, ts, [tool], [error]}
// kind: user | assistant | tool_use | tool_result | command | system | title
// Returns nullopt for lines that aren't worth showing (API blobs, thinking, sidechains...).
std::optional<json> normalize_line(const std::string& line, int64_t offset);

// Reads complete lines from `offset` (at most max_bytes), appending normalized messages.
// Advances offset past the last complete line. Returns false if the file can't be opened.
bool read_transcript(const std::string& path, int64_t& offset, std::vector<json>& out,
                     size_t max_bytes);

// Offset to start a fresh view from: 0, or the first line within the last `tail` bytes.
int64_t initial_offset(const std::string& path, int64_t tail, bool& truncated);

struct TranscriptTail {
    std::string last_assistant_text;
    std::string last_tool;  // "Write hello.txt": the most recent tool call
    std::string title;
};
// Scans the end of a transcript for the latest assistant text and AI title.
TranscriptTail scan_tail(const std::string& path, size_t tail_bytes);

}  // namespace ccbus

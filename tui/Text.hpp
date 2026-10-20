#pragma once
// Styled text for the TUI: spans, word wrapping, a light markdown renderer and an
// SGR (ANSI colour) parser for tmux pane captures.

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace tui {

struct Style {
    ftxui::Color fg = ftxui::Color::Default;
    ftxui::Color bg = ftxui::Color::Default;
    bool has_fg = false, has_bg = false;
    bool bold = false, dim = false, italic = false, underline = false, inverse = false;

    static Style color(ftxui::Color c) {
        Style s;
        s.fg = c;
        s.has_fg = true;
        return s;
    }
    Style& b() { bold = true; return *this; }
    Style& d() { dim = true; return *this; }
    Style& i() { italic = true; return *this; }
};

struct Span {
    std::string text;
    Style st;
};

using Line = std::vector<Span>;

int width(const std::string& s);
int width(const Line& l);
// Cut to `w` terminal columns, ending in "…" when something was cut.
std::string truncate(const std::string& s, int w);
Line truncate(const Line& l, int w);
// The last `w` columns, starting with "…" when something was cut (long paths).
Line tail(const Line& l, int w);

ftxui::Element render(const Line& l);
ftxui::Element render(const std::string& s, const Style& st);

// Greedy word wrap. `indent` spaces go before every produced line; `hang` extra spaces
// before the continuation lines (for bullets).
std::vector<Line> wrap(const Line& in, int w, int indent = 0, int hang = 0);

// Markdown subset used by Claude: headings, bullets, numbered lists, code fences,
// quotes, tables (unwrapped), **bold**, `code`, *italic*, [links](url).
std::vector<Line> markdown(const std::string& text, int w, int indent);

// One captured pane line with SGR escapes -> styled line. Other escapes are dropped.
Line parse_ansi(const std::string& s);

}  // namespace tui

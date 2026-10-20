#include "Text.hpp"

#include <ftxui/screen/string.hpp>

namespace tui {

using namespace ftxui;

namespace {

size_t utf8_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xe) return 3;
    if ((c >> 3) == 0x1e) return 4;
    return 1;
}

// Splits into glyphs (codepoints); good enough for wrapping and cutting.
std::vector<std::string> glyphs(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t n = std::min(utf8_len(static_cast<unsigned char>(s[i])), s.size() - i);
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

void push(Line& l, const std::string& text, const Style& st) {
    if (text.empty()) return;
    if (!l.empty()) {
        const Style& p = l.back().st;
        if (p.fg == st.fg && p.bg == st.bg && p.has_fg == st.has_fg && p.has_bg == st.has_bg &&
            p.bold == st.bold && p.dim == st.dim && p.italic == st.italic && p.underline == st.underline &&
            p.inverse == st.inverse) {
            l.back().text += text;
            return;
        }
    }
    l.push_back({text, st});
}

// ---- markdown inline --------------------------------------------------------

const Style kCode = Style::color(Color::Yellow);

Line inline_md(const std::string& s, Style base) {
    Line out;
    std::string cur;
    Style st = base;
    auto flush = [&] {
        push(out, cur, st);
        cur.clear();
    };
    bool bold = false, italic = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '`') {
            size_t e = s.find('`', i + 1);
            if (e != std::string::npos) {
                flush();
                push(out, s.substr(i + 1, e - i - 1), kCode);
                i = e;
                continue;
            }
        }
        if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            if (bold || s.find("**", i + 2) != std::string::npos) {
                flush();
                bold = !bold;
                st = base;
                st.bold = bold || base.bold;
                st.italic = italic;
                ++i;
                continue;
            }
        }
        if ((c == '*' || c == '_') && !bold) {
            // *italic* only when it hugs a word, so "a * b" and snake_case stay literal.
            bool opens = !italic && i + 1 < s.size() && s[i + 1] != ' ' && (i == 0 || s[i - 1] == ' ') &&
                         s.find(c, i + 1) != std::string::npos;
            bool closes = italic && i > 0 && s[i - 1] != ' ' && (i + 1 == s.size() || !isalnum((unsigned char)s[i + 1]));
            if (opens || closes) {
                flush();
                italic = !italic;
                st = base;
                st.italic = italic;
                continue;
            }
        }
        if (c == '[') {
            size_t close = s.find("](", i);
            size_t end = close == std::string::npos ? std::string::npos : s.find(')', close);
            if (end != std::string::npos && s.find('\n', i) > end) {
                flush();
                Style link = base;
                link.underline = true;
                push(out, s.substr(i + 1, close - i - 1), link);
                i = end;
                continue;
            }
        }
        cur += c;
    }
    flush();
    return out;
}

bool starts_with(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

}  // namespace

int width(const std::string& s) { return string_width(s); }

int width(const Line& l) {
    int w = 0;
    for (const Span& sp : l) w += width(sp.text);
    return w;
}

std::string truncate(const std::string& s, int w) {
    if (w <= 0) return {};
    if (width(s) <= w) return s;
    std::string out;
    int used = 0;
    for (const std::string& g : glyphs(s)) {
        int gw = width(g);
        if (used + gw > w - 1) break;
        out += g;
        used += gw;
    }
    return out + "…";
}

Line truncate(const Line& l, int w) {
    if (width(l) <= w) return l;
    Line out;
    int used = 0;
    for (const Span& sp : l) {
        int sw = width(sp.text);
        if (used + sw <= w - 1) {
            out.push_back(sp);
            used += sw;
            continue;
        }
        std::string part = truncate(sp.text, w - used);
        out.push_back({part, sp.st});
        break;
    }
    return out;
}

Line tail(const Line& l, int w) {
    if (width(l) <= w) return l;
    std::vector<std::pair<std::string, Style>> gs;
    for (const Span& sp : l)
        for (const std::string& g : glyphs(sp.text)) gs.push_back({g, sp.st});
    Line out;
    int used = 1;
    size_t start = gs.size();
    while (start > 0 && used + width(gs[start - 1].first) <= w) used += width(gs[--start].first);
    push(out, "…", Style());
    for (size_t i = start; i < gs.size(); ++i) push(out, gs[i].first, gs[i].second);
    return out;
}

Element render(const std::string& s, const Style& st) {
    Element e = text(s);
    if (st.has_fg) e = e | color(st.fg);
    if (st.has_bg) e = e | bgcolor(st.bg);
    if (st.bold) e = e | bold;
    if (st.dim) e = e | dim;
    if (st.italic) e = e | italic;
    if (st.underline) e = e | underlined;
    if (st.inverse) e = e | inverted;
    return e;
}

Element render(const Line& l) {
    Elements es;
    for (const Span& sp : l) es.push_back(render(sp.text, sp.st));
    if (es.empty()) return text("");
    return hbox(std::move(es));
}

std::vector<Line> wrap(const Line& in, int w, int indent, int hang) {
    std::vector<Line> out;
    std::string pad(static_cast<size_t>(indent), ' ');
    std::string hpad(static_cast<size_t>(indent + hang), ' ');
    w = std::max(w, indent + hang + 8);

    Line cur;
    int used = 0;
    auto start = [&](bool first) {
        cur.clear();
        cur.push_back({first ? pad : hpad, Style()});
        used = first ? indent : indent + hang;
    };
    start(true);
    bool at_start = true;

    for (const Span& sp : in) {
        // Tokens: runs of spaces or runs of non-spaces.
        const std::string& t = sp.text;
        size_t i = 0;
        while (i < t.size()) {
            bool space = t[i] == ' ';
            size_t j = i;
            while (j < t.size() && (t[j] == ' ') == space) ++j;
            std::string tok = t.substr(i, j - i);
            i = j;
            int tw = width(tok);
            if (space) {
                if (at_start) continue;
                if (used + tw >= w) {  // the space would end the line anyway
                    out.push_back(cur);
                    start(false);
                    at_start = true;
                    continue;
                }
                push(cur, tok, sp.st);
                used += tw;
                continue;
            }
            if (used + tw > w && !at_start) {
                // Drop the trailing space we may have added.
                if (!cur.empty() && !cur.back().text.empty() && cur.back().text.back() == ' ') {
                    std::string& b = cur.back().text;
                    while (!b.empty() && b.back() == ' ') b.pop_back();
                }
                out.push_back(cur);
                start(false);
            }
            if (used + tw > w) {  // a single word longer than the line: hard-break it
                for (const std::string& g : glyphs(tok)) {
                    int gw = width(g);
                    if (used + gw > w) {
                        out.push_back(cur);
                        start(false);
                    }
                    push(cur, g, sp.st);
                    used += gw;
                }
            } else {
                push(cur, tok, sp.st);
                used += tw;
            }
            at_start = false;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<Line> markdown(const std::string& text, int w, int indent) {
    std::vector<Line> out;
    std::string pad(static_cast<size_t>(indent), ' ');
    bool in_code = false;
    bool last_blank = true;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        size_t lead = raw.find_first_not_of(' ');
        std::string t = lead == std::string::npos ? std::string() : raw.substr(lead);

        if (starts_with(t, "```")) {
            in_code = !in_code;
            continue;
        }
        if (in_code) {
            out.push_back(truncate(Line{{pad + "  ", Style()}, {raw, Style::color(Color::Cyan)}}, w));
            last_blank = false;
            continue;
        }
        if (t.empty()) {
            if (!last_blank) out.push_back(Line{});
            last_blank = true;
            continue;
        }
        last_blank = false;

        if (t[0] == '#') {
            size_t h = t.find_first_not_of('#');
            if (h != std::string::npos && t[h] == ' ') {
                Style s;
                s.bold = true;
                for (Line& l : wrap(inline_md(t.substr(h + 1), s), w, indent)) out.push_back(std::move(l));
                continue;
            }
        }
        if (t[0] == '|') {
            bool sep = t.find_first_not_of("|-: ") == std::string::npos;
            Style s;
            if (sep) s.dim = true;
            Line l{{pad, Style()}};
            for (Span& sp : inline_md(t, s)) l.push_back(std::move(sp));
            out.push_back(truncate(l, w));
            continue;
        }
        if (t == "---" || t == "***" || t == "___") {
            Style s;
            s.dim = true;
            std::string bar;
            for (int i = 0; i < std::min(w - indent, 40); ++i) bar += "─";
            out.push_back(Line{{pad, Style()}, {bar, s}});
            continue;
        }
        if (starts_with(t, "> ") || t == ">") {
            Style s;
            s.dim = true;
            s.italic = true;
            Line l{{"│ ", Style().d()}};
            for (Span& sp : inline_md(t.size() > 2 ? t.substr(2) : "", s)) l.push_back(std::move(sp));
            for (Line& x : wrap(l, w, indent, 2)) out.push_back(std::move(x));
            continue;
        }

        int depth = static_cast<int>(lead == std::string::npos ? 0 : lead);
        std::string marker;
        std::string rest;
        if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            marker = "• ";
            rest = t.substr(2);
        } else {
            size_t d = 0;
            while (d < t.size() && isdigit((unsigned char)t[d])) ++d;
            if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ') {
                marker = t.substr(0, d + 1) + " ";
                rest = t.substr(d + 2);
            }
        }
        if (!marker.empty()) {
            Line l{{marker, Style()}};
            for (Span& sp : inline_md(rest, Style())) l.push_back(std::move(sp));
            int mw = width(marker);
            for (Line& x : wrap(l, w, indent + std::min(depth, 8) + 1, mw)) out.push_back(std::move(x));
            continue;
        }
        for (Line& x : wrap(inline_md(t, Style()), w, indent)) out.push_back(std::move(x));
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

namespace {

Color ansi16(int n) {
    static const Color table[16] = {
        Color::Black,     Color::Red,     Color::Green,     Color::Yellow,
        Color::Blue,      Color::Magenta, Color::Cyan,      Color::GrayLight,
        Color::GrayDark,  Color::RedLight, Color::GreenLight, Color::YellowLight,
        Color::BlueLight, Color::MagentaLight, Color::CyanLight, Color::White,
    };
    return table[n & 15];
}

void apply_sgr(Style& st, const std::vector<int>& p) {
    if (p.empty()) {
        st = Style();
        return;
    }
    for (size_t i = 0; i < p.size(); ++i) {
        int c = p[i];
        auto extended = [&](Color& out, bool& has) {
            if (i + 1 >= p.size()) return;
            if (p[i + 1] == 5 && i + 2 < p.size()) {
                out = Color::Palette256(static_cast<uint8_t>(p[i + 2]));
                has = true;
                i += 2;
            } else if (p[i + 1] == 2 && i + 4 < p.size()) {
                out = Color::RGB(static_cast<uint8_t>(p[i + 2]), static_cast<uint8_t>(p[i + 3]),
                                 static_cast<uint8_t>(p[i + 4]));
                has = true;
                i += 4;
            }
        };
        if (c == 0) st = Style();
        else if (c == 1) st.bold = true;
        else if (c == 2) st.dim = true;
        else if (c == 3) st.italic = true;
        else if (c == 4) st.underline = true;
        else if (c == 7) st.inverse = true;
        else if (c == 22) st.bold = st.dim = false;
        else if (c == 23) st.italic = false;
        else if (c == 24) st.underline = false;
        else if (c == 27) st.inverse = false;
        else if (c >= 30 && c <= 37) { st.fg = ansi16(c - 30); st.has_fg = true; }
        else if (c >= 90 && c <= 97) { st.fg = ansi16(c - 90 + 8); st.has_fg = true; }
        else if (c == 39) st.has_fg = false;
        else if (c >= 40 && c <= 47) { st.bg = ansi16(c - 40); st.has_bg = true; }
        else if (c >= 100 && c <= 107) { st.bg = ansi16(c - 100 + 8); st.has_bg = true; }
        else if (c == 49) st.has_bg = false;
        else if (c == 38) extended(st.fg, st.has_fg);
        else if (c == 48) extended(st.bg, st.has_bg);
    }
}

}  // namespace

Line parse_ansi(const std::string& s) {
    Line out;
    Style st;
    std::string cur;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
            if (j >= s.size()) break;
            if (s[j] == 'm') {
                std::vector<int> params;
                int v = 0;
                bool any = false;
                for (size_t k = i + 2; k < j; ++k) {
                    if (isdigit((unsigned char)s[k])) {
                        v = v * 10 + (s[k] - '0');
                        any = true;
                    } else if (s[k] == ';' || s[k] == ':') {
                        params.push_back(v);
                        v = 0;
                        any = false;
                    }
                }
                if (any || !params.empty()) params.push_back(v);
                push(out, cur, st);
                cur.clear();
                apply_sgr(st, params);
            }
            i = j;
            continue;
        }
        if (c == '\x1b') {  // other escapes (OSC 8 links etc.): skip to the terminator
            size_t j = s.find("\x1b\\", i + 1);
            size_t bel = s.find('\a', i + 1);
            size_t end = std::min(j == std::string::npos ? s.size() : j + 1, bel == std::string::npos ? s.size() : bel);
            i = end;
            continue;
        }
        if (c == '\t') {
            cur += "    ";
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20) continue;
        cur += c;
    }
    push(out, cur, st);
    return out;
}

}  // namespace tui

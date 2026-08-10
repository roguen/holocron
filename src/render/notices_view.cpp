// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See holocron/notices_view.hpp.

#include <holocron/notices_view.hpp>

#include <string>

namespace holocron {
namespace {

// `[label](target)` -> `label (target)`, or just `label` when they agree.
//
// See the header: the target is the LGPL section 6 "reference directing the user
// to the copy of this License", so it survives even though it makes the line
// longer.
std::string expand_links(const std::string& in)
{
    std::string out;
    out.reserve(in.size());

    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '[') {
            out.push_back(in[i]);
            ++i;
            continue;
        }

        const std::size_t label_end = in.find(']', i);
        if (label_end == std::string::npos || label_end + 1 >= in.size() ||
            in[label_end + 1] != '(') {
            out.push_back(in[i]);   // a bare bracket, not a link
            ++i;
            continue;
        }
        const std::size_t target_end = in.find(')', label_end + 2);
        if (target_end == std::string::npos) {
            out.push_back(in[i]);
            ++i;
            continue;
        }

        const std::string label  = in.substr(i + 1, label_end - i - 1);
        const std::string target = in.substr(label_end + 2, target_end - label_end - 2);

        out += label;
        if (!target.empty() && target != label) {
            out += " (";
            out += target;
            out += ")";
        }
        i = target_end + 1;
    }
    return out;
}

// Remove the characters that carry emphasis rather than meaning.
//
// Backticks and asterisks only. Underscores are LEFT ALONE: they appear inside
// identifiers this file names -- `default-features`, `swresample`, and every
// snake_case symbol -- and stripping them would corrupt the text rather than
// tidy it.
std::string strip_emphasis(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '`') {
            continue;
        }
        if (in[i] == '*') {
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

bool is_table_rule(const std::string& s)
{
    // `|---|---|` and friends: a row of nothing but pipes, dashes, colons and
    // spaces, with at least one dash so an empty row is not mistaken for one.
    bool saw_dash = false;
    for (const char c : s) {
        if (c == '-') {
            saw_dash = true;
        } else if (c != '|' && c != ':' && c != ' ' && c != '\t') {
            return false;
        }
    }
    return saw_dash;
}

std::string trim(const std::string& s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) {
        ++b;
    }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) {
        --e;
    }
    return s.substr(b, e - b);
}

// `| a | b | c |` -> `a  b  c`. Two spaces between cells rather than one, so the
// column boundary is still visible without any alignment machinery.
std::string flatten_table_row(const std::string& s)
{
    std::string out;
    std::size_t i = 0;
    if (!s.empty() && s[0] == '|') {
        i = 1;
    }
    bool first = true;
    while (i < s.size()) {
        const std::size_t bar  = s.find('|', i);
        const std::string cell = trim(s.substr(i, (bar == std::string::npos) ? bar : bar - i));
        if (!cell.empty()) {
            if (!first) {
                out += "  ";
            }
            out += cell;
            first = false;
        }
        if (bar == std::string::npos) {
            break;
        }
        i = bar + 1;
    }
    return out;
}

}  // namespace

std::vector<std::string> flatten_notices(std::string_view markdown)
{
    std::vector<std::string> out;

    std::size_t start = 0;
    while (start <= markdown.size()) {
        std::size_t end = markdown.find('\n', start);
        if (end == std::string_view::npos) {
            end = markdown.size();
        }
        std::string line(markdown.substr(start, end - start));
        start = end + 1;

        line = trim(line);

        // A `---` horizontal rule carries no words. Dropped rather than
        // rendered, because a row of dashes at 40 px is the biggest thing on the
        // page and says nothing.
        if (is_table_rule(line)) {
            continue;
        }

        if (!line.empty() && line[0] == '|') {
            line = flatten_table_row(line);
        }

        // Heading markers. The text of the heading stays; only the hashes go.
        std::size_t hash = 0;
        while (hash < line.size() && line[hash] == '#') {
            ++hash;
        }
        if (hash > 0 && hash < line.size() && line[hash] == ' ') {
            line = line.substr(hash + 1);
        }

        // Blockquote markers, which this file uses for the quoted licence text.
        if (!line.empty() && line[0] == '>') {
            line = trim(line.substr(1));
        }

        line = expand_links(line);
        line = strip_emphasis(line);

        out.push_back(trim(line));
    }

    // The file ends with a newline, which produces one trailing empty line. It
    // is dropped so a page never opens with a blank first row after a split.
    if (!out.empty() && out.back().empty()) {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> paginate_notices(const std::vector<std::string>& lines,
                                          std::size_t lines_per_page)
{
    std::vector<std::string> pages;
    if (lines.empty()) {
        return pages;
    }
    // A page of zero lines would loop forever. Clamped rather than asserted,
    // because the caller computes this from a window height and a very small
    // window is a legitimate state.
    const std::size_t per = (lines_per_page > 0) ? lines_per_page : 1;

    for (std::size_t i = 0; i < lines.size(); i += per) {
        std::string page;
        const std::size_t last = (i + per < lines.size()) ? i + per : lines.size();
        for (std::size_t k = i; k < last; ++k) {
            if (k > i) {
                page.push_back('\n');
            }
            page += lines[k];
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

std::string colophon_first_page(std::string_view version)
{
    // The four things GPL-3.0 section 0 names, in the order somebody reads them.
    // Kept deliberately short: this page exists to be read at a glance from a
    // couch, and the detail is on the pages after it.
    std::string out = "Holocron ";
    out += version;
    out += "\n";
    out += "\n";
    out += "Copyright (c) 2026 Roguen Keller\n";
    out += "\n";
    out += "This program comes with ABSOLUTELY NO WARRANTY.\n";
    out += "It is free software, and you are welcome to redistribute it\n";
    out += "under the terms of the GNU General Public License, version 3\n";
    out += "or later. See the LICENSE file, or gnu.org/licenses/gpl-3.0.\n";
    out += "\n";
    out += "Source: github.com/roguen/holocron\n";
    out += "\n";
    out += "The pages that follow are THIRD-PARTY-NOTICES.md, compiled\n";
    out += "into this program. They name every library it is built on,\n";
    out += "who holds the copyright, and where each licence text lives.";
    return out;
}

}  // namespace holocron

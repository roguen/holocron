// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See artwork_cache.hpp.

#include <holocron/artwork_cache.hpp>

#include <string>

namespace holocron {
namespace {

char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_hex_lower(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

// The stem: everything before the first dot.
std::string_view stem_of(std::string_view name)
{
    const std::size_t dot = name.find('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

// Every byte Windows will not store in a name, plus the separators, plus the
// control range. `.` is deliberately absent -- it is legal in the middle and only
// the ends are trimmed.
bool is_reserved_byte(unsigned char c)
{
    if (c < 0x20 || c == 0x7F) {
        return true;   // control bytes, and DEL
    }
    switch (c) {
    case '<': case '>': case ':': case '"':
    case '/': case '\\': case '|': case '?': case '*':
        return true;
    default:
        return false;
    }
}

// How many bytes the UTF-8 sequence starting at `at` occupies, or 0 if what is
// there is not a well-formed sequence.
//
// STRICT ON PURPOSE. Overlong encodings, surrogates and anything above U+10FFFF
// are rejected rather than passed through, because two filesystems will not agree
// about them and because the whole point of validating here is that the same tag
// produces the same filename on Windows and on Linux CI.
std::size_t utf8_length_at(std::string_view s, std::size_t at)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(s[i]); };

    const unsigned char c0 = byte(at);
    if (c0 < 0x80) {
        return 1;
    }

    std::size_t   need = 0;
    std::uint32_t cp   = 0;
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        cp   = c0 & 0x1Fu;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        cp   = c0 & 0x0Fu;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        cp   = c0 & 0x07u;
    } else {
        return 0;   // a continuation byte or an invalid lead
    }

    // The continuation bytes live at at+1 .. at+need, so all of them exist only
    // if at+need is still a valid index.
    if (at + need >= s.size()) {
        return 0;   // truncated at the end of the input
    }
    for (std::size_t k = 1; k <= need; ++k) {
        const unsigned char cn = byte(at + k);
        if ((cn & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (cn & 0x3Fu);
    }

    // Overlong, surrogate, or out of range.
    if (need == 1 && cp < 0x80) { return 0; }
    if (need == 2 && cp < 0x800) { return 0; }
    if (need == 3 && cp < 0x10000) { return 0; }
    if (cp > 0x10FFFF) { return 0; }
    if (cp >= 0xD800 && cp <= 0xDFFF) { return 0; }

    return need + 1;
}

}  // namespace

bool is_reserved_device_name(std::string_view name)
{
    const std::string_view stem = stem_of(name);
    if (stem.empty() || stem.size() > 7) {
        return false;   // nothing reserved is longer than CONOUT$
    }

    std::string s;
    s.reserve(stem.size());
    for (const char c : stem) {
        s.push_back(lower_ascii(c));
    }

    if (s == "con" || s == "prn" || s == "aux" || s == "nul" || s == "conin$" ||
        s == "conout$") {
        return true;
    }

    // COM0-9 and LPT0-9, exactly -- COM10 is not reserved, and neither is CONS.
    //
    // 0 IS INCLUDED AS POLICY RATHER THAN AS MEASUREMENT. `COM0.jpg` was created
    // successfully on the rack's Windows 10 build, but Microsoft documents COM0
    // and LPT0 as reserved and another version may enforce it. One underscore on
    // a name nobody has, against a throw on a machine that disagrees.
    if (s.size() == 4) {
        const bool com = s.compare(0, 3, "com") == 0;
        const bool lpt = s.compare(0, 3, "lpt") == 0;
        if ((com || lpt) && s[3] >= '0' && s[3] <= '9') {
            return true;
        }
    }
    return false;
}

std::string safe_artwork_label(std::string_view text, std::size_t max_bytes)
{
    // 1 and 2: map, and collapse runs of '_' as we go.
    std::string mapped;
    mapped.reserve(text.size());

    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);

        if (c < 0x80) {
            if (is_reserved_byte(c)) {
                if (mapped.empty() || mapped.back() != '_') {
                    mapped.push_back('_');
                }
            } else {
                mapped.push_back(text[i]);
            }
            ++i;
            continue;
        }

        const std::size_t len = utf8_length_at(text, i);
        if (len == 0) {
            // Not well-formed. One '_' for the offending byte, then carry on from
            // the next one -- a run of bad bytes therefore collapses to a single
            // '_' rather than one each, which is what makes `Bj\xF6rk` read as
            // `Bj_rk` instead of `Bj___rk`.
            if (mapped.empty() || mapped.back() != '_') {
                mapped.push_back('_');
            }
            ++i;
            continue;
        }
        mapped.append(text.substr(i, len));
        i += len;
    }

    // 3: truncate on a character boundary. A sequence that does not fit is not
    // started, so the result is always well-formed.
    std::string cut;
    cut.reserve(mapped.size() < max_bytes ? mapped.size() : max_bytes);
    std::size_t j = 0;
    while (j < mapped.size()) {
        const unsigned char c   = static_cast<unsigned char>(mapped[j]);
        const std::size_t   len = c < 0x80 ? 1u : utf8_length_at(mapped, j);
        const std::size_t   take = len == 0 ? 1u : len;
        if (cut.size() + take > max_bytes) {
            break;
        }
        cut.append(mapped.substr(j, take));
        j += take;
    }

    // 4: trim both ends of '.', ' ' and '_'.
    //
    // Trailing dots and spaces because Windows strips them behind our back;
    // leading dots because a name starting with one is hidden on every other
    // platform and because `..` is not a name at all; '_' because it is only ever
    // there as the residue of something we mapped, and leading or trailing
    // residue is noise.
    const auto trimmable = [](char c) { return c == '.' || c == ' ' || c == '_'; };
    std::size_t b = 0;
    std::size_t e = cut.size();
    while (b < e && trimmable(cut[b])) { ++b; }
    while (e > b && trimmable(cut[e - 1])) { --e; }
    std::string out = cut.substr(b, e - b);

    // 5: nothing left.
    if (out.empty()) {
        out = std::string(kArtUntitled);
    }

    // 6: LAST, because 3 and 4 can both manufacture a device name -- "NULLIFY"
    // truncated to three bytes, or "NUL " trimmed.
    if (is_reserved_device_name(out)) {
        out.insert(out.begin(), '_');
    }
    return out;
}

std::string artwork_cache_key(std::string_view server_id, std::string_view thumb_path,
                              int size_px)
{
    std::string out = "v1|";
    out += std::to_string(size_px);
    out += '|';
    out.append(server_id);
    out += '|';
    out.append(thumb_path);
    return out;
}

std::string artwork_cache_stem(std::string_view label, std::string_view key)
{
    static const char kHex[] = "0123456789abcdef";

    std::string out = safe_artwork_label(label, kArtLabelMaxBytes);
    out.push_back('-');

    const std::uint64_t h = artwork_identity_hash(key);
    for (std::size_t k = 0; k < kArtHashHexDigits; ++k) {
        const unsigned shift = static_cast<unsigned>((kArtHashHexDigits - 1 - k) * 4);
        out.push_back(kHex[(h >> shift) & 0xFull]);
    }
    return out;
}

bool parse_artwork_cache_name(std::string_view filename, std::uint64_t& out_hash)
{
    out_hash = 0;

    constexpr std::string_view kExt = ".jpg";
    if (filename.size() < 1 + 1 + kArtHashHexDigits + kExt.size()) {
        return false;   // at least one label byte, the '-', the hex, the extension
    }
    if (filename.substr(filename.size() - kExt.size()) != kExt) {
        return false;
    }

    const std::string_view body = filename.substr(0, filename.size() - kExt.size());
    if (body.size() < 1 + 1 + kArtHashHexDigits) {
        return false;
    }

    const std::size_t dash = body.size() - kArtHashHexDigits - 1;
    if (body[dash] != '-' || dash == 0) {
        return false;   // dash == 0 would be an empty label
    }

    std::uint64_t h = 0;
    for (std::size_t k = dash + 1; k < body.size(); ++k) {
        const char c = body[k];
        if (!is_hex_lower(c)) {
            return false;
        }
        const std::uint64_t v = (c <= '9') ? static_cast<std::uint64_t>(c - '0')
                                           : static_cast<std::uint64_t>(c - 'a' + 10);
        h = (h << 4) | v;
    }

    out_hash = h;
    return true;
}

ArtworkName artwork_cache_name(std::string_view server_id, std::string_view artist,
                               std::string_view album, std::string_view title,
                               std::string_view track_thumb, std::string_view album_thumb,
                               int size_px)
{
    // ONE TEST, USED TWICE. Whether the art is this track's own decides both which
    // thumb identifies the entry and whether the title belongs in the label. Two
    // copies of it would eventually disagree, and the symptom would be a
    // fifteen-track album quietly becoming fifteen identical files.
    const bool own_art = !track_thumb.empty() && track_thumb != album_thumb;

    const std::string_view thumb = own_art ? track_thumb
                                           : (album_thumb.empty() ? track_thumb : album_thumb);

    std::string label;
    if (!artist.empty()) {
        label.append(artist);
    }
    if (!album.empty()) {
        if (!label.empty()) {
            label += " - ";
        }
        label.append(album);
    }
    if (own_art && !title.empty()) {
        if (!label.empty()) {
            label += " - ";
        }
        label.append(title);
    }

    ArtworkName out;
    out.key  = artwork_cache_key(server_id, thumb, size_px);
    out.stem = artwork_cache_stem(label, out.key);
    return out;
}

}  // namespace holocron

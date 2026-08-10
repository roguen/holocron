// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/artwork_cache.hpp
//
// Naming a cached album sleeve so that Windows will store it and we can find it
// again.
//
// WHAT THIS IS FOR
//
// M5 asks for two things that turn out to be one problem: "album art fetched and
// cached; cache path configurable and gitignored", and "filenames derived from
// track metadata are sanitized for Windows before they ever hit disk". The second
// is the hard half, and it is hard for a reason that is easy to miss -- Windows
// does not reject every bad filename. It rejects some and SILENTLY REWRITES
// others, and a cache built on a name that was silently rewritten misses forever
// while reporting nothing at all.
//
// EVERYTHING HERE IS PURE. No filesystem, no Plex, no allocation beyond the
// strings it returns. That is the point of it being its own header: the naming is
// the part with the edge cases, and it is the part that can be tested without a
// disk, a server or a token.
//
// NOTHING CALLS ANY OF THIS, DELIBERATELY -- D-044, 2026-08-10.
//
// The on-disk cache it was written for is not being built. Measured against the
// real library: a sleeve the Plex photo transcoder has rendered before comes back
// in ONE MILLISECOND, because Plex already keeps on disk exactly what M5's
// criterion was asking Holocron to keep a second copy of. First sight is 38 ms,
// on a worker thread that never touches a frame. The in-memory cache in
// `ArtworkLoader` collapses an album to one fetch and that is where it ends.
//
// This header stays anyway, for the same reason `facet_abi.h` ships with nothing
// implementing it: it is the half that would be got WRONG, the Windows behaviour
// below was measured rather than recalled, and the decision reverses on a
// measurement -- the Shield over Wi-Fi at M8, or a library reached over the WAN.
// Finding a naming bug while this is a header is cheap. Finding it later is a
// cache that silently misses forever.
//
// -- what Windows actually does, measured rather than recalled -----------------
//
// Tested on the rack (Windows 10 Pro 19045) by writing bytes to each name.
//
// REFUSED OUTRIGHT, with an exception, so it cannot pass unnoticed:
//
//     CON.jpg  con.jpg  NUL.jpg  COM1.jpg  LPT1.jpg  AUX.jpg  conout$.jpg
//     AUX. Volume 1.jpg   CON.1.jpg   NUL.foo.jpg
//
// The device names are reserved WITH an extension attached, the test is
// case-insensitive, and -- the one people get wrong -- it applies to the stem
// before the FIRST dot, so `AUX. Volume 1` is refused even though it looks
// nothing like a device. `CONOUT$` and `CONIN$` are reserved too.
//
// ACCEPTED, THEN SILENTLY RENAMED, which is the dangerous class:
//
//     "Album."  lands as  "Album"
//     "Album "  lands as  "Album"
//
// A trailing dot or space is stripped with no error. For a cache that is worse
// than a refusal: the write appears to succeed, the lookup asks for the name it
// derived, misses forever, and the cache quietly re-fetches every single time
// while reporting nothing. An infinite-miss cache that looks like it is working.
// They also collapse into each other, so they are a collision source as well.
//
// ACCEPTED UNCHANGED, so the sanitiser must not touch them:
//
//     CONX.jpg   CONS.jpg   NULLIFY.jpg   COM10.jpg   COM0.jpg
//     Ænima.jpg  em—dash.jpg  .hidden.jpg  "a b.jpg"
//
// Only the EXACT device stems are reserved, so a prefix test would mangle any
// album beginning with those three letters. Non-ASCII and an em-dash are fine,
// which matters because Plex serves UTF-8 and this library contains both.
//
// COM0 IS THE ONE PLACE THIS IS STRICTER THAN THE MEASUREMENT. `COM0.jpg` was
// created successfully on that build, but Microsoft documents COM0 and LPT0 as
// reserved and other Windows versions may enforce it. Treating them as reserved
// costs one underscore on a name nobody has; treating them as legal risks a
// throw on a machine that disagrees. So the test for it asserts OUR POLICY, not
// Windows' observed behaviour, and says so.
//
// -- and why the readable part of the name is only cosmetic --------------------
//
// A filename here is `<label>-<16 hex digits>.jpg`. The label is derived from the
// metadata, which is what the exit criterion is about and what makes the
// directory something you can browse. The hex is the identity.
//
// NOTHING EVER RECONSTRUCTS A FILENAME TO FIND AN ENTRY. Lookup is by hash, and
// the hash is read back out of the name that is actually on disk. That is what
// makes the label safe to be cosmetic: Plex can re-title an album, a
// capitalisation can change, Windows can strip something, the truncation budget
// can be different -- and the entry is still found. Reconstructing the name and
// hoping it matches is the design that quietly orphans a whole cache the first
// time any of those happens, with no log line anywhere.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace holocron {

// The identity hash, in the name, in lowercase hex.
//
// LOWERCASE IS LOAD-BEARING, NOT COSMETIC. With a single-case alphabet, two
// filenames can differ only by case only if their hex is byte-identical -- which
// means they are the same entry. Uppercase hex would make `A3F2` and `a3f2` one
// file on NTFS and two on ext4, which is exactly the bug class rule 3 in
// CLAUDE.md exists for: invisible on the rack, visible only to Linux CI.
constexpr std::size_t kArtHashHexDigits = 16;

// How much of the readable label survives into the filename.
//
// A COMPILE-TIME CONSTANT, DELIBERATELY. Deriving it from the resolved cache
// directory at runtime looks more careful and is worse: the shipped default is a
// RELATIVE path, so the same binary launched from the repo root, from
// build\windows\bin, or from a shortcut would truncate the same label to
// different lengths. Nothing here reconstructs a name to find an entry, so that
// would not lose data -- but a fixed budget costs nothing and removes the
// question entirely.
//
// 64 bytes holds "Artist - Album" for everything on this rack and cuts the long
// classical tail, where the hash still identifies the entry exactly.
constexpr std::size_t kArtLabelMaxBytes = 64;

// What a sanitised label falls back to when there is nothing left of it.
//
// Not an empty string, because an empty stem makes `-<hash>.jpg`, which is a
// legal filename that reads as broken, and because two different albums that both
// sanitise away would then differ only in the hash -- fine for the code, useless
// for anyone looking at the directory.
constexpr std::string_view kArtUntitled = "untitled";

// FNV-1a over the bytes, then splitmix64's finalizer.
//
// constexpr SO THE COMPILER CAN PIN IT. A change to this function silently
// renames every entry in the cache -- every lookup misses, every sleeve is
// re-fetched, nothing errors. The tests static_assert three known vectors, which
// turns that from a bug somebody eventually notices into a build failure.
//
// FNV-1a alone leaves the low bits weakly mixed for the short, highly-similar
// strings this hashes -- every key shares the prefix `v1|512|` and differs late,
// in digits. The finalizer is what avalanches that, and it is four lines rather
// than a dependency.
constexpr std::uint64_t artwork_identity_hash(std::string_view s)
{
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001b3ull;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

// Is `stem` a name Windows will refuse?
//
// Tests the part before the FIRST dot, case-insensitively, against the exact
// device names -- not a prefix, because `CONS` and `NULLIFY` are perfectly legal
// and mangling them would be a bug of our own making.
//
// Pass either a bare stem or a whole filename; the dot is found here so no caller
// has to remember to strip it. That is deliberate: the interior-dot case
// (`AUX. Volume 1`) is the one that gets missed, and it is missed by callers who
// think they have already handled the extension.
bool is_reserved_device_name(std::string_view name);

// Turn arbitrary text into something Windows will store, unchanged, forever.
//
// `max_bytes` bounds the RESULT in bytes. UTF-8 sequences are never split: a
// character that does not fit is not started, so the answer is always well-formed.
//
// THE ORDER OF THE STEPS IS THE WHOLE CORRECTNESS ARGUMENT, and two of them are
// only right last:
//
//   1. map every byte Windows reserves, every control byte, every path separator
//      and every byte that is not part of a well-formed UTF-8 sequence, to '_'
//   2. collapse runs of '_'
//   3. truncate to `max_bytes`, on a character boundary
//   4. trim leading and trailing '.', ' ' and '_'
//   5. if nothing is left, `kArtUntitled`
//   6. if the stem is now a reserved device name, prefix '_'
//
// Step 6 AFTER step 3 because truncation can MANUFACTURE a device name --
// `safe_artwork_label("NULLIFY", 3)` is `NUL` -- and after step 4 because
// trimming can too: `"NUL "` trims to `NUL`. A reserved-name check placed at the
// top, where it reads most naturally, passes both of those straight through.
//
// Step 4 AFTER step 3 because truncating can expose a trailing space that was
// interior before, and Windows would then strip it behind our back.
//
// DOTS SURVIVE IN THE MIDDLE. `.` is a legal filename byte and stripping it would
// mangle "Vol. 2" and every band with an initial. Only the ends are trimmed --
// which is also why step 6 has to look before the first dot rather than assuming
// the stem is the whole name.
//
// INVALID UTF-8 BECOMES '_' RATHER THAN BEING PASSED THROUGH, and that is what
// makes Windows and Linux produce the SAME filename from the same tag. Tag bytes
// are arbitrary -- ID3v2.3 defaults to Latin-1 and plenty of rippers write the
// system codepage -- so `Bj\xF6rk` is `Bj_rk` here rather than a byte sequence
// two filesystems would disagree about.
std::string safe_artwork_label(std::string_view text, std::size_t max_bytes);

// The string that IS the identity of a cached sleeve. Hashed into the filename;
// never shown, never parsed, never stored.
//
//     v1|512|<server machine identifier>|/library/metadata/56397/thumb/1755230000
//
// THE SIGNATURE IS THE DEFENCE, NOT DISCIPLINE. It takes the server identifier
// and the thumb path by value and has no `PlayRequest` and no `PlexTrack`
// parameter, so there is no argument through which a token could arrive. A
// reviewer confirms that by reading three parameter names.
//
// That matters more than it looks. `artwork_path()` is the string already being
// fetched and is the obvious shortcut, and it embeds `X-Plex-Token` -- which
// would put a live credential in a filename, in a public repo whose .gitignore is
// already load-bearing for exactly that, and would change the key every session
// so the cache became a permanent miss that grows forever.
//
// THE THUMB PATH IS THE ALBUM-LEVEL IDENTIFIER, measured: every one of the seven
// tracks of album 1892 on the rack resolves to `/library/metadata/1892/thumb/
// 1786261670`, the album's own thumb, because music tracks carry no art of their
// own. So keying on it collapses an album to one entry with nothing here needing
// to know what an album is.
//
// It is also better than an album id would be, because that path carries a
// VERSION STAMP. Replacing an album's cover changes the path, the key, and the
// filename -- new art is a natural miss and the old file is simply orphaned. An
// album id would have pinned the stale sleeve forever.
//
// `srv` EXISTS BECAUSE A THUMB PATH IS SERVER-RELATIVE. The in-memory cache can
// ignore that because it dies with the process; a disk cache outlives the machine
// it was filled on, and two libraries can both serve /library/metadata/1892/...
// The machine identifier rather than address:port, because the address
// legitimately moves between a LAN IP and a plex.direct hostname and keying on
// that would fragment the cache on every network change.
//
// `512` IS THE REQUESTED SIZE, FOLDED IN, and it is absent from today's in-memory
// key. Harmless while the cache dies with the process; on disk it is the classic
// silent regression -- raising the size later would serve every previously cached
// sleeve at the old one forever, with nothing looking broken, the palette just
// built from a quarter of the pixels intended.
//
// `v1|` IS THE SCHEME VERSION. Any future change to what is stored, or to this
// recipe, becomes `v2|` -- which changes every hash, which turns the whole old
// cache into orphans a sweep reaps. No migration code, ever.
std::string artwork_cache_key(std::string_view server_id, std::string_view thumb_path,
                              int size_px);

// `<label>-<16 hex digits>`, with no extension.
//
// The label is sanitised and truncated here, so callers cannot forget to.
std::string artwork_cache_stem(std::string_view label, std::string_view key);

// Read the identity back out of a filename that is actually on disk.
//
// True only for a name this scheme could have produced: a non-empty stem, a '-',
// exactly kArtHashHexDigits of LOWERCASE hex, then ".jpg" and nothing else.
//
// THIS IS ON THE HIT PATH, NOT JUST THE SWEEP. Lookup asks the directory what is
// there and parses the hash out of each real name, rather than building a name
// and hoping. So a false answer here is not a file that fails to be deleted -- it
// is an entry that cannot be found, and the strictness is what stops somebody
// else's `holiday-photo.jpg` being mistaken for ours in either direction.
bool parse_artwork_cache_name(std::string_view filename, std::uint64_t& out_hash);

// Both halves of an entry's name, from one place.
//
// ONE FUNCTION BECAUSE THE LABEL MUST BE A FUNCTION OF THE SAME THING THE KEY IS.
// Plex usually sets a music track's `thumb` to the album's own thumb, so a label
// built from the track TITLE would write one identical 40 KB JPEG per track and
// turn a fifteen-track album into fifteen files. The title joins the label only
// when `track_thumb` is non-empty AND differs from `album_thumb` -- i.e. when the
// art really is this track's own -- and that is the same test that chooses which
// thumb goes into the key. Split across two functions, those two tests drift;
// here there is one, tested once.
struct ArtworkName {
    std::string key;    // the identity, for hashing -- never written anywhere
    std::string stem;   // `<label>-<hash>`, no extension
};

ArtworkName artwork_cache_name(std::string_view server_id, std::string_view artist,
                               std::string_view album, std::string_view title,
                               std::string_view track_thumb, std::string_view album_thumb,
                               int size_px);

}  // namespace holocron

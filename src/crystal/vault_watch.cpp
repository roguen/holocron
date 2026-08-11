// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See vault_watch.hpp.

#include <holocron/vault_watch.hpp>

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace holocron {

namespace {

// The two extensions a crystal is made of. See the header for why the shader is
// here even though scan_vault keys off manifests alone.
bool watched_extension(const std::filesystem::path& p)
{
    const std::filesystem::path ext = p.extension();
    return ext == ".toml" || ext == ".frag";
}

}  // namespace

VaultWatch::VaultWatch(std::string dir, Clock::time_point now, Clock::duration interval)
    : dir_(std::move(dir)), interval_(interval), last_poll_(now)
{
    // A failed look leaves `loaded_` empty, which is the right baseline: if the
    // directory turns up later with crystals in it, those crystals arrived.
    look(loaded_);
}

bool VaultWatch::look(Listing& out) const
{
    namespace fs = std::filesystem;

    // The error_code overloads throughout. A vault on a share that has gone away
    // is an expected reading here rather than an exceptional one, and the
    // throwing overloads would turn a blinking network path into an exception
    // crossing a worker thread -- which is std::terminate.
    std::error_code ec;
    if (!fs::is_directory(dir_, ec) || ec) {
        return false;
    }

    fs::directory_iterator it(dir_, ec);
    if (ec) {
        return false;
    }

    Listing found;
    for (const fs::directory_entry& e : it) {
        // Not is_regular_file(): that is a second syscall per entry and a
        // directory named `foo.toml` is not a thing anybody has. The stat below
        // is the one that decides, and anything it cannot read is skipped.
        if (!watched_extension(e.path())) {
            continue;
        }

        const auto when = fs::last_write_time(e.path(), ec);
        if (ec) {
            // ONE UNREADABLE FILE IS NOT AN UNREADABLE DIRECTORY. A file being
            // written right now can refuse a stat on Windows, and treating that
            // as a failed look would stall the watch for as long as somebody is
            // saving. Skipping it leaves the listing different from the last one,
            // so the settle rule waits -- which is exactly what should happen
            // while a file is in motion.
            ec.clear();
            continue;
        }

        const auto bytes = fs::file_size(e.path(), ec);
        if (ec) {
            ec.clear();
            continue;
        }

        // The epoch of file_time_type is unspecified and the value is never
        // converted to a wall clock -- it is only ever compared against another
        // reading of the same file.
        found.push_back(Stamp{e.path().filename().string(),
                              static_cast<std::int64_t>(when.time_since_epoch().count()),
                              static_cast<std::uint64_t>(bytes)});
    }

    std::sort(found.begin(), found.end(),
              [](const Stamp& a, const Stamp& b) { return a.name < b.name; });

    out = std::move(found);
    return true;
}

bool VaultWatch::poll(Clock::time_point now)
{
    if (now - last_poll_ < interval_) {
        return false;
    }
    last_poll_ = now;

    Listing current;
    if (!look(current)) {
        // NOT A CHANGE AND NOT A CLEAR. The directory could not be read, so
        // nothing was learned -- including nothing about whether it is still the
        // same. `pending_` is deliberately left alone as well: a share that
        // blinks for one poll in the middle of a real change should not restart
        // the settling, it should just not count.
        return false;
    }

    if (current == loaded_) {
        // Includes a change undone between two polls, which correctly reports
        // nothing rather than a re-scan to what the caller already has.
        have_pending_ = false;
        return false;
    }

    if (have_pending_ && pending_ == current) {
        // Seen twice unchanged: whatever was copying has finished.
        loaded_       = std::move(current);
        have_pending_ = false;
        return true;
    }

    // Either the first sighting, or files are still landing -- a crystal being
    // copied in is at least two writes and may be two creations. Both mean the
    // same thing: wait and look again.
    pending_      = std::move(current);
    have_pending_ = true;
    return false;
}

}  // namespace holocron

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See crystal_watch.hpp.

#include <holocron/crystal_watch.hpp>

#include <filesystem>
#include <system_error>
#include <utility>

namespace holocron {

CrystalWatch::Stamp CrystalWatch::stamp_of(const std::string& path)
{
    namespace fs = std::filesystem;

    Stamp s;

    // The error_code overloads throughout, never the throwing ones -- see the
    // declaration for why an absent file is an expected reading here.
    std::error_code ec;
    const auto      when = fs::last_write_time(path, ec);
    if (ec) {
        return s;   // absent or unreadable; `present` stays false
    }

    const auto bytes = fs::file_size(path, ec);
    if (ec) {
        return s;
    }

    // The epoch of file_time_type is unspecified and the value is never
    // converted to a wall clock -- it is only ever compared against another
    // reading of the same file, which is all change detection needs.
    s.mtime   = static_cast<std::int64_t>(when.time_since_epoch().count());
    s.size    = static_cast<std::uint64_t>(bytes);
    s.present = true;
    return s;
}

CrystalWatch::CrystalWatch(std::string manifest_path, std::string shader_path,
                           Clock::time_point now, Clock::duration interval)
    : manifest_path_(std::move(manifest_path)),
      shader_path_(std::move(shader_path)),
      interval_(interval),
      last_poll_(now)
{
    loaded_ = look();
}

CrystalWatch::Pair CrystalWatch::look() const
{
    return Pair{stamp_of(manifest_path_), stamp_of(shader_path_)};
}

bool CrystalWatch::poll(Clock::time_point now)
{
    if (now - last_poll_ < interval_) {
        return false;
    }
    last_poll_ = now;

    const Pair current = look();

    if (current == loaded_) {
        // Includes the case where an edit was undone between two polls, which
        // correctly reports nothing rather than a reload to what is already up.
        have_pending_ = false;
        return false;
    }

    if (have_pending_ && pending_ == current) {
        // Seen twice unchanged: the writer has finished.
        loaded_       = current;
        have_pending_ = false;
        return true;
    }

    // Either the first sighting of this change, or the file is still moving --
    // half-written, or momentarily absent while a temporary is renamed over it.
    // Both mean the same thing: wait and look again.
    pending_      = current;
    have_pending_ = true;
    return false;
}

}  // namespace holocron

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/platform_paths.cpp
//
// See platform_paths.hpp. Deliberately trivial: this is a value the entry point
// knows and the rest of the program does not, and the whole file exists so that
// the knowing and the using can be in different places.

#include <holocron/platform_paths.hpp>

namespace holocron {

namespace {

// Set once at startup, before any thread that reads it exists. A plain string is
// enough for the same reason the JavaVM pointer is a plain pointer.
std::string& storage()
{
    static std::string value;
    return value;
}

}  // namespace

const std::string& data_directory()
{
    return storage();
}

void set_data_directory(std::string path)
{
    // A trailing separator would produce `dir//file`, which every platform
    // tolerates and no log line should have to show a reader.
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) {
        path.pop_back();
    }
    storage() = std::move(path);
}

std::string resolve_data_path(const std::string& path)
{
    if (storage().empty() || path.empty()) {
        return path;
    }

    // Already absolute? POSIX leading slash, a Windows drive letter, or a UNC
    // path. Any of them was meant literally.
    const bool posix_absolute = path[0] == '/' || path[0] == '\\';
    const bool drive_absolute = path.size() >= 3 && path[1] == ':' &&
                                (path[2] == '/' || path[2] == '\\');
    if (posix_absolute || drive_absolute) {
        return path;
    }

    return storage() + "/" + path;
}

}  // namespace holocron

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/platform_paths.hpp
//
// Where the user's files are, when "beside the executable" is not an answer.
//
// WHY THIS EXISTS
//
// Every path this project reads is relative to the working directory:
// `gatekeeper.toml`, `crystals`, whatever `--vault` names. On Windows and Linux
// that is exactly right -- the player is started from a shell, in a directory
// somebody chose, and resolving anything cleverer would be surprising.
//
// ON ANDROID THERE IS NO WORKING DIRECTORY IN THAT SENSE. An Activity launches
// with cwd `/`, so `gatekeeper.toml` resolves to `/gatekeeper.toml`, is missing,
// and the player comes up on defaults -- losing `trim_ms`, the Plex token, the
// machine identity, fullscreen and the vault path in one go. Nothing reports a
// fault, because a missing config is a legitimate first run.
//
// THIS IS A PREFIX, NOT A PATH SCHEME. `data_directory()` is empty on every
// desktop build, so `resolve()` returns its argument unchanged and the existing
// behaviour is bit-for-bit what it was. Only a platform that sets one behaves
// differently, and only for relative paths -- an absolute path from `--vault` or
// `--config` is always taken as given.
//
// WHY THE ENTRY POINT SETS IT RATHER THAN THIS FILE WORKING IT OUT
//
// The answer on Android comes from the Java Context, and the only thing in this
// process that already holds one is SDL. Asking for it here would put SDL into a
// library that has no other business with it -- the same argument android_jni.hpp
// makes about the JavaVM, and the same resolution: the value is handed in once,
// by the entry point, which is allowed to know about SDL.

#pragma once

#include <string>

namespace holocron {

// The directory relative paths are resolved against. EMPTY on every desktop
// build, which means "the working directory" and is what a shell-launched
// process wants.
const std::string& data_directory();

// Set it. Called once, early, by the entry point. An empty string restores the
// default. Safe to call on any platform.
void set_data_directory(std::string path);

// Apply it. Returns `path` unchanged when there is no data directory, or when
// `path` is already absolute.
//
// Absolute is tested rather than assumed portable: a Windows path may begin with
// a drive letter, a POSIX one with a slash, and a caller that typed either meant
// it.
std::string resolve_data_path(const std::string& path);

}  // namespace holocron

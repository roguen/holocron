// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/run_log.hpp
//
// What the player said, kept in a file that survives the process.
//
// ISSUE 281. The Shield was found running with a live process, an `SDLThread`,
// and NO LISTENING SOCKET AT ALL -- no Companion server, no GDM -- while
// `plex.tv` still advertised its address. Selecting it in Plexamp would have
// failed with nothing on screen to explain why.
//
// The cause is still unknown, and the reason it is unknown is the whole point of
// this file: the app's output goes to logcat, logcat is a ring buffer, and by the
// time anybody noticed the fault the startup lines had rolled away. There was no
// record of how far it got.
//
// TWO FILES, AND THE SECOND ONE IS THE FEATURE.
//
// `holocron.log` is the current run. `holocron.prev.log` is the one before it,
// rotated at startup. That ordering is not tidiness -- it is the actual usage:
// you notice the player is unreachable, you force-stop it, and force-stopping is
// what destroys the evidence. The run that failed is by then the PREVIOUS run,
// and without a rotation the relaunch would overwrite the only copy of it.
//
// This is a diagnostic, not a transcript. It mirrors the lines that say what the
// player DECIDED -- which paths it read, which ports it got, what it opened, what
// it refused -- and not the per-frame chatter. A log nobody can read to the end
// is a log nobody reads.

#pragma once

#include <string>

namespace holocron {

// Start a run log in `directory`, rotating any existing one to `.prev`.
//
// Quiet on failure and safe to skip: everything below still writes to stdout
// when no log is open, so a read-only data directory costs the file and nothing
// else. Call it once, early -- before the first say() worth keeping.
void open_run_log(const std::string& directory);

// Where it ended up, or empty when there is none.
const std::string& run_log_path();

// printf to stdout, and to the run log if one is open.
//
// SAME SIGNATURE AND SAME BEHAVIOUR AS std::printf on stdout, deliberately, so
// converting a call site is a rename and cannot change what a person watching a
// terminal sees. The file gets a wall-clock stamp in front of each line, because
// the question this file exists to answer -- "how far did the run that broke
// get, and when" -- needs both halves.
void say(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// The same, to stderr.
//
// ISSUE 281 AGAIN, AND IT IS THE HALF THAT WAS MISSING. `say()` covers what the
// player DID; every line that says what it COULD NOT DO went to stderr and
// therefore into nothing that survives. On the Shield the run log recorded a
// startup reaching the end successfully and had no way to record one that did
// not -- which is the only interesting case.
//
// Converting a `std::fprintf(stderr, ...)` to this is a rename: stderr still
// gets the identical bytes, unbuffered, in the same order relative to `say()`'s
// stdout because both flush every line.
//
// NOT MARKED IN THE FILE as an error. The stamp and the line are all a reader
// needs, and a prefix would make the file harder to scan for the one sequence
// that matters -- what was tried, in order, and where it stopped.
void say_err(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Flush and close. Called at exit on platforms that have one; the file is
// flushed after every line anyway, precisely because Android does not.
void close_run_log();

}  // namespace holocron

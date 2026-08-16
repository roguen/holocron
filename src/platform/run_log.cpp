// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See run_log.hpp.

#include <holocron/run_log.hpp>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace holocron {

namespace {

// A CAP, BECAUSE THIS RUNS FOR DAYS ON A TELEVISION.
//
// The player is cast to and left; the Shield has been up for weeks at a time. An
// uncapped log on the same card as the music is a slow leak nobody is watching.
// Past the cap the file stops growing and says so once, which keeps the STARTUP
// lines -- the ones issue 281 needs -- rather than keeping the most recent
// chatter and losing them.
constexpr long kMaxBytes = 1024 * 1024;

struct State {
    std::mutex  mutex;
    std::FILE*  file = nullptr;
    std::string path;
    long        written  = 0;
    bool        capped   = false;
};

State& state()
{
    static State s;
    return s;
}

void stamp(std::FILE* f)
{
    const auto now  = std::chrono::system_clock::now();
    const auto secs = std::chrono::system_clock::to_time_t(now);
    const auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % std::chrono::seconds(1);

    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    std::fprintf(f, "%02d:%02d:%02d.%03d  ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 static_cast<int>(ms.count()));
}

}  // namespace

void open_run_log(const std::string& directory)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);

    if (s.file != nullptr) {
        return;
    }
    if (directory.empty()) {
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return;
    }

    const auto current  = std::filesystem::path(directory) / "holocron.log";
    const auto previous = std::filesystem::path(directory) / "holocron.prev.log";

    // ROTATE FIRST, AND THIS IS THE WHOLE MECHANISM. The run that failed is the
    // PREVIOUS run by the time anybody looks -- noticing the fault and
    // force-stopping the app is what makes it previous. Opening the current file
    // for writing without moving it aside first would destroy the only record of
    // the failure, on the exact relaunch performed to investigate it.
    std::filesystem::remove(previous, ec);
    std::filesystem::rename(current, previous, ec);   // fine if there was none

    // fopen_s on MSVC, which deprecates fopen under /W4 /WX. Not worth a
    // wrapper header for one call site, and not worth _CRT_SECURE_NO_WARNINGS
    // either -- that would silence the check everywhere for the sake of here.
#if defined(_MSC_VER)
    if (fopen_s(&s.file, current.string().c_str(), "wb") != 0) {
        s.file = nullptr;
    }
#else
    s.file = std::fopen(current.string().c_str(), "wb");
#endif
    if (s.file == nullptr) {
        return;
    }
    s.path    = current.string();
    s.written = 0;
    s.capped  = false;

    std::fprintf(s.file, "holocron run log -- previous run is %s\n",
                 previous.string().c_str());
    std::fflush(s.file);
}

const std::string& run_log_path() { return state().path; }

namespace {

// The file half, shared by say() and say_err() so there is one copy of the
// stamping, the accounting and the cap. `args` is consumed.
void write_to_file(const char* format, std::va_list args)
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file == nullptr || s.capped) {
        return;
    }

    stamp(s.file);
    const int n = std::vfprintf(s.file, format, args);

    if (n > 0) {
        s.written += n;
    }

    // FLUSHED EVERY LINE. The failure this file exists for ends with the process
    // being killed, and a buffered tail is exactly the part that would be lost.
    std::fflush(s.file);

    if (s.written >= kMaxBytes) {
        s.capped = true;
        std::fprintf(s.file, "-- run log full at %ld bytes; nothing further is recorded. "
                             "The startup lines above are kept deliberately.\n",
                     s.written);
        std::fflush(s.file);
    }
}

}  // namespace

void say(const char* format, ...)
{
    // stdout first and unconditionally. A caller must be able to convert a
    // printf to this without wondering whether the terminal still gets it.
    std::va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fflush(stdout);

    std::va_list file_args;
    va_start(file_args, format);
    write_to_file(format, file_args);
    va_end(file_args);
}

void say_err(const char* format, ...)
{
    // stderr first and unconditionally, for the same reason say() does stdout:
    // converting a call site must not change what a person watching a terminal
    // sees.
    std::va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);

    std::va_list file_args;
    va_start(file_args, format);
    write_to_file(format, file_args);
    va_end(file_args);
}

void close_run_log()
{
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.file != nullptr) {
        std::fflush(s.file);
        std::fclose(s.file);
        s.file = nullptr;
    }

    // AND THE PATH, WHICH IS THE HALF THAT WAS MISSING (issue 366). The header
    // says run_log_path() is "empty when there is none", and a closed log is
    // none -- leaving the string behind makes it name a file nothing is writing
    // to. The only reader outside the tests uses it to decide whether to print
    // the location at all, which is exactly the question it was answering wrong.
    s.path.clear();
    s.written = 0;
    s.capped  = false;
}

}  // namespace holocron

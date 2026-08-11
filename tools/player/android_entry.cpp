// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// tools/player/android_entry.cpp
//
// The Android entry point. Compiled ONLY on Android -- see tools/CMakeLists.txt.
//
// WHY THIS FILE EXISTS RATHER THAN FOUR LINES IN main.cpp
//
// Two jobs, and each of them would have cost main.cpp something it has kept.
//
// 1. `SDL_main`. SDL3's Android port is launched from a Java Activity, which
//    dlopens the application's shared object and looks up the C symbol
//    `SDL_main` in it. `#include <SDL3/SDL_main.h>` macro-renames `main` to
//    exactly that and exports it. Doing it in main.cpp would put SDL into the
//    one translation unit that has stayed free of it; the project holds SDL,
//    FFmpeg, GL and httplib to one translation unit each.
//
// 2. THE JavaVM. `holocron::android::set_java_vm` is what makes the Android text
//    rasterizer and HTTPS client work at all -- without it both return
//    kUnsupported, which is a silent degradation rather than a crash and is
//    therefore exactly the kind of thing found at 2am on a device. It is called
//    here because this is the first Holocron code that runs, and because
//    android_jni.hpp deliberately takes the pointer from OUTSIDE rather than
//    calling SDL for it.
//
// THE PROCESS IS ALREADY ON A JAVA THREAD when this runs. SDL's Java glue calls
// `SDL_main` on a thread it created and attached, so `SDL_GetAndroidJNIEnv`
// returns a usable env and `GetJavaVM` off it is the documented way to reach the
// VM. Later calls from the render thread and the fetch worker attach themselves
// -- see ScopedEnv.
//
// THERE IS NO argv. An Activity launch passes nothing, so every command-line flag
// is off and the player runs on its config file and the phone. That is issue 242
// and is not solved here; this file only records it where somebody debugging an
// absent `--debug-facet` will find it.

// Android only, and selected by CMake rather than by the preprocessor -- so this
// guard is the thing that says so in the file itself. It does two jobs: it turns
// "CMake compiled this on the wrong platform" into an error at the top of the
// file rather than a pile of missing-header noise, and it puts the token
// `__ANDROID__` where scripts/android-check.sh's anti-rot check can see it, so
// this file is one the guard REQUIRES to have been compiled.
#ifndef __ANDROID__
#error "android_entry.cpp is the Android entry point and must not be compiled elsewhere"
#endif

#include <holocron/android_jni.hpp>
#include <holocron/platform_paths.hpp>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>

#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

// Defined in player/main.cpp, which is the same body every platform runs.
int holocron_main(int argc, char** argv);

namespace {

// ---------------------------------------------------------------------------
// stdout and stderr into logcat.
//
// ANDROID THROWS A PROCESS'S STDOUT AWAY. The player says everything it knows on
// stdout -- 128 printf lines, including the GL version it actually got, the
// window size in pixels, the audio backend and period, whether output is
// bit-perfect, the lead budget, and every diagnostic this project added after
// losing a session to a missing log line. On a device all of it goes nowhere.
//
// `adb shell setprop log.redirect-stdio true` is the documented way round it and
// is REFUSED ON A RETAIL BUILD -- measured on this Shield: "Failed to set
// property". It needs userdebug. So the redirect has to happen in-process.
//
// A pipe, with the read end pumped into logcat by a detached thread. Every
// existing printf reaches logcat unchanged, which is the point: the alternative
// was rewriting 128 call sites onto a logging abstraction, and this project's
// own rule is that a log line removed is an instrument removed.
//
// LINE BUFFERED, not the default. A pipe is not a terminal, so stdio would pick
// full buffering and hold 4 KB before flushing -- which on a player that prints
// its startup lines and then draws would mean the startup lines appear minutes
// later, or never, exactly when they are wanted most.
//
// The thread is detached and runs for the life of the process on purpose. There
// is nothing to join: the process ends when the Activity does.
void redirect_stdio_to_logcat()
{
    static int fds[2];
    if (pipe(fds) != 0) {
        return;  // nothing to be done; the player just stays quiet
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);

    std::thread([] {
        std::string line;
        char        buffer[512];
        for (;;) {
            const ssize_t n = read(fds[0], buffer, sizeof(buffer));
            if (n <= 0) {
                return;  // writer end closed, or an error we cannot report anyway
            }
            for (ssize_t i = 0; i < n; ++i) {
                if (buffer[i] == '\n') {
                    // Empty lines are dropped rather than logged as blanks --
                    // the player uses them for spacing on a terminal and they
                    // are noise in a log with timestamps on every entry.
                    if (!line.empty()) {
                        __android_log_write(ANDROID_LOG_INFO, "holocron", line.c_str());
                        line.clear();
                    }
                } else if (buffer[i] != '\r') {
                    line.push_back(buffer[i]);
                    // A pathological line cannot be allowed to grow without
                    // bound; logcat truncates around 4 KB anyway.
                    if (line.size() >= 2048) {
                        __android_log_write(ANDROID_LOG_INFO, "holocron", line.c_str());
                        line.clear();
                    }
                }
            }
        }
    }).detach();
}

}  // namespace

int main(int argc, char** argv)
{
    // SDL_main.h has renamed this to SDL_main and given it default visibility.

    // FIRST, before anything can print. Everything the player says is on stdout.
    redirect_stdio_to_logcat();

    // KEEP RUNNING WHILE BACKGROUNDED. The owner's decision, 2026-08-11.
    //
    // SDL's default is the opposite and it is not a mild default: with
    // SDL_ANDROID_BLOCK_ON_PAUSE at its default of "1", SDL_PollEvent BLOCKS
    // INDEFINITELY once the Activity is paused. The render loop is
    // `while (window.pump())`, so the entire loop stops -- and with it every
    // command from the phone, both timelines and the herald. The music would
    // keep playing, because the decode thread and the audio callback are
    // independent of the loop, and the phone would keep showing a progress bar
    // that no longer moved and buttons that did nothing.
    //
    // For a cast target that is the wrong behaviour. Holocron on a television is
    // meant to be driven from a phone in another room; whether its Activity
    // happens to be foreground is not something the person holding the phone
    // knows or should have to care about.
    //
    // The obligation this takes on: with the loop still running there is no
    // surface, so it must not touch GL. That is handled in the render loop,
    // which skips everything below its drawing boundary while
    // `window.visible()` is false.
    //
    // BEFORE THE VIDEO SUBSYSTEM STARTS, which is why it is here rather than in
    // window.cpp -- SDL reads this hint when it initialises video, and this file
    // is the first Holocron code that runs.
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");

    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env != nullptr) {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == JNI_OK && vm != nullptr) {
            holocron::android::set_java_vm(vm);
        }
    }

    // WHERE gatekeeper.toml AND THE VAULT LIVE, because cwd is `/`.
    //
    // THE EXTERNAL app-specific directory, not the internal one, and the choice
    // is deliberate. Both are readable by this app with no permission at all on
    // API 30. The difference is that `/sdcard/Android/data/<pkg>/files` can be
    // written from outside the app -- over adb, over the network, from a file
    // manager on the TV -- and `/data/data/<pkg>/files` cannot without root.
    //
    // That matters because of what lives there. The vault is meant to be edited:
    // issue 214 made the player re-read it while running so a crystal copied in
    // appears in about three seconds. A vault nobody can copy into would turn
    // that feature off on the one platform where there is no shell to copy with.
    //
    // If SDL cannot answer -- no external storage mounted -- the path stays empty
    // and everything resolves relative to cwd exactly as it does on the desktop.
    // That is the pre-existing behaviour rather than a new failure.
    if (const char* external = SDL_GetAndroidExternalStoragePath(); external != nullptr) {
        holocron::set_data_directory(external);
    }

    // REPORTED EITHER WAY, AND TO logcat RATHER THAN stdout. Android discards a
    // process's stdout unless log.redirect-stdio is set, so the 128 printf lines
    // the player produces go nowhere by default -- which means the one line that
    // says whether the platform layer is alive has to be said here, in the one
    // place that is guaranteed to be read.
    __android_log_print(holocron::android::has_java_vm() ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        "holocron", "JavaVM %s -- the text rasterizer and HTTPS client %s",
                        holocron::android::has_java_vm() ? "handed over" : "NOT available",
                        holocron::android::has_java_vm() ? "can run" : "will report unsupported");

    // Said out loud because it is the first thing to check when the player comes
    // up on defaults: the answer is a path nobody can guess and every relative
    // path in the process now hangs off it.
    std::printf("holocron: data directory %s\n",
                holocron::data_directory().empty() ? "(none -- using the working directory)"
                                                   : holocron::data_directory().c_str());

    return holocron_main(argc, argv);
}

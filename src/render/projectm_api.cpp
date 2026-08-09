// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/projectm_api.cpp -- opening libprojectM at runtime.
//
// See projectm_api.hpp for why this is a runtime load rather than a link.

#include <holocron/projectm_api.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace holocron {
namespace {

// ---------------------------------------------------------------------------
// Module names
//
// Windows has one spelling each and it is the one CMake gives the shared library.
//
// Linux tries the VERSIONED SONAME FIRST, and that ordering matters. A
// distribution package installs `libprojectM-4.so.4` and nothing else; the
// unversioned `libprojectM-4.so` is a symlink that only the -dev package
// provides. Looking for the unversioned name first would find nothing on the
// machine that actually has the library installed.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
constexpr const char* kCoreNames[]     = {"projectM-4.dll"};
constexpr const char* kPlaylistNames[] = {"projectM-4-playlist.dll"};
#elif defined(__APPLE__)
constexpr const char* kCoreNames[]     = {"libprojectM-4.4.dylib", "libprojectM-4.dylib"};
constexpr const char* kPlaylistNames[] = {"libprojectM-4-playlist.4.dylib",
                                          "libprojectM-4-playlist.dylib"};
#else
constexpr const char* kCoreNames[]     = {"libprojectM-4.so.4", "libprojectM-4.so"};
constexpr const char* kPlaylistNames[] = {"libprojectM-4-playlist.so.4",
                                          "libprojectM-4-playlist.so"};
#endif

// A NATIVE, ABSOLUTE, NORMALISED path -- and every one of those three words was
// paid for.
//
// An empty directory is left as the bare module name on purpose: that is what
// asks the OS loader to search its own path, which is what a system-installed
// libprojectM wants.
//
// When a directory IS given, the result has to be fully qualified with native
// separators. Windows accepts a forward slash almost everywhere, but
// LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR is one of the places it does not: with
// `C:\dir/projectM-4.dll` the module itself opens and the flag silently fails to
// add its directory to the dependency search, so the load fails on glew32.dll --
// which is sitting right beside it -- with error 126 naming projectM-4.dll.
//
// That is exactly the misleading failure the message for 126 warns about, and it
// was produced here by the loader's own path handling rather than by a missing
// file. It cost a run to find.
std::string join_path(const std::string& dir, const char* name)
{
    if (dir.empty()) {
        return std::string(name);
    }

    std::filesystem::path p = std::filesystem::path(dir) / name;

    std::error_code ec;
    if (std::filesystem::path abs = std::filesystem::absolute(p, ec); !ec) {
        p = abs;
    }
    return p.lexically_normal().make_preferred().string();
}

#if defined(_WIN32)

std::wstring widen(const std::string& utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

// The last Windows error, in words, with the one code that matters spelled out.
//
// ERROR_MOD_NOT_FOUND (126) is THE failure to explain here and the message
// Windows gives for it is actively misleading: "The specified module could not be
// found" is also what you get when the module was found perfectly well and one of
// ITS imports was not. On this platform projectM-4.dll imports glew32.dll, so a
// user who copies two projectM DLLs out of a package and leaves the third behind
// gets an error naming the file that is sitting right there.
std::string last_error(const std::string& what)
{
    const DWORD code = GetLastError();

    char*       text = nullptr;
    const DWORD n    = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);

    std::string message;
    if (n != 0 && text != nullptr) {
        message.assign(text, n);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                                    message.back() == ' ' || message.back() == '.')) {
            message.pop_back();
        }
    }
    if (text != nullptr) {
        LocalFree(text);
    }
    if (message.empty()) {
        message = "error " + std::to_string(code);
    }

    std::string out = what + ": " + message + " (" + std::to_string(code) + ")";
    if (code == ERROR_MOD_NOT_FOUND) {
        out += "\n  126 also means a DEPENDENCY was missing, not just this file. "
               "projectM-4.dll\n  imports glew32.dll -- all three modules have to be in the same "
               "directory.";
    }
    return out;
}

void* open_module(const std::string& path, bool absolute)
{
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return nullptr;
    }
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR is what makes a full path work at all:
    // without it the loader resolves projectM-4.dll's own imports against the
    // process directory and the system path, never against the directory the
    // file came from -- so pointing at a self-contained folder of DLLs fails on
    // glew32.dll with an error that names projectM-4.dll. The flag requires an
    // absolute path, which is why the caller says whether it has one.
    const DWORD flags = absolute
                            ? (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
                            : 0;

    // NO MODAL DIALOG, AND THIS IS NOT DEFENSIVE PROGRAMMING -- IT HUNG CI.
    //
    // Windows reports some image-load failures as a HARD ERROR: a message box,
    // put up by the loader, not by us. On an interactive desktop it appears and
    // is dismissible, which is why this never showed up locally. On a GitHub
    // Windows runner there is nobody to dismiss it, so LoadLibraryExW never
    // returns and the job runs until the six-hour timeout.
    //
    // That is exactly what happened: a test that deliberately loads a file that
    // is not a shared library took a Windows job from 2m34s to over an hour,
    // twice, while Linux passed in 99 seconds. Two red herrings were chased
    // first -- an Actions cache at 10.02 GB against a 10 GB limit, which was a
    // real and separate problem (issue 169), and then a probe branch that
    // isolated it: the same tree with an exact cache hit still stalled.
    //
    // SEM_FAILCRITICALERRORS turns the box into a return code. The THREAD
    // variant rather than SetErrorMode, because the process-wide one is a global
    // this library has no business changing on a host that may want the default,
    // and it is restored immediately either way.
    //
    // This is also right outside CI: a user pointing `library_dir` at a
    // truncated download should get a message, not a dialog on a machine in a
    // theater with no keyboard attached.
    DWORD      previous = 0;
    const BOOL saved    = SetThreadErrorMode(SEM_FAILCRITICALERRORS, &previous);

    HMODULE module = LoadLibraryExW(wide.c_str(), nullptr, flags);

    // Restored BEFORE anything reads GetLastError, because SetThreadErrorMode
    // succeeds and would otherwise clear the error the caller is about to
    // report. The load's error code is captured first for that reason.
    if (saved != 0) {
        const DWORD load_error = GetLastError();
        SetThreadErrorMode(previous, nullptr);
        SetLastError(load_error);
    }
    return module;
}

void close_module(void* module)
{
    if (module != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module));
    }
}

void* find_symbol(void* module, const char* name)
{
    // GetProcAddress returns a function pointer; the caller memcpy's it into the
    // right type. Going through void* keeps one code path with the dlsym build.
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(module), name));
}

#else   // POSIX

std::string last_error(const std::string& what)
{
    const char* err = dlerror();
    return what + ": " + (err != nullptr ? err : "dlopen failed with no message");
}

void* open_module(const std::string& path, bool /*absolute*/)
{
    dlerror();   // clear any stale message so last_error() reports ours
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void close_module(void* module)
{
    if (module != nullptr) {
        dlclose(module);
    }
}

void* find_symbol(void* module, const char* name) { return dlsym(module, name); }

#endif

bool is_absolute(const std::string& p) { return std::filesystem::path(p).is_absolute(); }

// Resolve one entry point.
//
// The copy through memcpy rather than a cast is not superstition: converting
// between an object pointer and a function pointer is conditionally-supported in
// ISO C++, and -Wpedantic -Werror on the Linux job rejects the cast outright.
// memcpy is the portable spelling and compiles to the same nothing.
template <typename Fn>
bool bind_symbol(void* module, const char* name, Fn& out, std::string& out_missing)
{
    void* raw = find_symbol(module, name);
    if (raw == nullptr) {
        if (out_missing.empty()) {
            out_missing = name;
        }
        return false;
    }
    static_assert(sizeof(Fn) == sizeof(void*), "function and object pointers must be the same size");
    std::memcpy(&out, &raw, sizeof(out));
    return true;
}

// Try each candidate name in `dir`, and report every path attempted when none of
// them opens. A "not found" that does not say where it looked is the single most
// annoying error a dynamic loader can produce.
void* open_first(const std::string& dir, const char* const* names, std::size_t count,
                 std::string& out_found_path, std::string& out_tried)
{
    for (std::size_t i = 0; i < count; ++i) {
        const std::string path = join_path(dir, names[i]);
        if (void* m = open_module(path, is_absolute(path)); m != nullptr) {
            out_found_path = path;
            return m;
        }
        if (!out_tried.empty()) {
            out_tried += ", ";
        }
        out_tried += path;
    }
    return nullptr;
}

}   // namespace

const char* const* projectm_core_names(std::size_t& count)
{
    count = sizeof(kCoreNames) / sizeof(kCoreNames[0]);
    return kCoreNames;
}

const char* const* projectm_playlist_names(std::size_t& count)
{
    count = sizeof(kPlaylistNames) / sizeof(kPlaylistNames[0]);
    return kPlaylistNames;
}

ProjectMLibrary::~ProjectMLibrary() { unload(); }

ProjectMLibrary::ProjectMLibrary(ProjectMLibrary&& other) noexcept
    : core_(other.core_), playlist_(other.playlist_), api_(other.api_),
      version_(std::move(other.version_)), core_path_(std::move(other.core_path_))
{
    other.core_     = nullptr;
    other.playlist_ = nullptr;
    other.api_      = ProjectMApi{};
}

ProjectMLibrary& ProjectMLibrary::operator=(ProjectMLibrary&& other) noexcept
{
    if (this != &other) {
        unload();
        core_      = other.core_;
        playlist_  = other.playlist_;
        api_       = other.api_;
        version_   = std::move(other.version_);
        core_path_ = std::move(other.core_path_);

        other.core_     = nullptr;
        other.playlist_ = nullptr;
        other.api_      = ProjectMApi{};
    }
    return *this;
}

void ProjectMLibrary::unload()
{
    // Playlist first: it holds a reference to the core module's allocator, and
    // unloading the thing underneath it first is the kind of ordering bug that
    // only shows up on somebody else's loader.
    close_module(playlist_);
    close_module(core_);
    playlist_ = nullptr;
    core_     = nullptr;
    api_      = ProjectMApi{};
    version_.clear();
    core_path_.clear();
}

bool load_projectm(const std::string& library_dir, ProjectMLibrary& out, std::string& out_error)
{
    out.unload();
    out_error.clear();

    std::size_t        core_count = 0;
    const char* const* core_names = projectm_core_names(core_count);

    std::string tried;
    std::string found;
    void*       core = open_first(library_dir, core_names, core_count, found, tried);
    if (core == nullptr) {
        out_error = last_error("libprojectM could not be opened") + "\n  tried: " + tried;
        return false;
    }

    std::size_t        pl_count = 0;
    const char* const* pl_names = projectm_playlist_names(pl_count);

    std::string pl_tried;
    std::string pl_found;
    void*       playlist = open_first(library_dir, pl_names, pl_count, pl_found, pl_tried);
    if (playlist == nullptr) {
        // NOT treated as "carry on without a playlist". Preset selection,
        // shuffle and ordering are all the playlist library's, and a projectM
        // with one preset and no way to change it is not the facet anybody
        // asked for -- so this is a clean refusal rather than a degraded mode
        // nobody would want and nobody would test.
        const std::string why = last_error("libprojectM's playlist module could not be opened");
        close_module(core);
        out_error = why + "\n  tried: " + pl_tried +
                    "\n  it ships beside the core library; a build configured with "
                    "ENABLE_PLAYLIST=OFF does not have it.";
        return false;
    }

    ProjectMApi api{};
    std::string missing;

    bool ok = true;
    ok &= bind_symbol(core, "projectm_create", api.create, missing);
    ok &= bind_symbol(core, "projectm_destroy", api.destroy, missing);
    ok &= bind_symbol(core, "projectm_get_version_components", api.get_version_components, missing);
    ok &= bind_symbol(core, "projectm_get_version_string", api.get_version_string, missing);
    ok &= bind_symbol(core, "projectm_free_string", api.free_string, missing);
    ok &= bind_symbol(core, "projectm_set_window_size", api.set_window_size, missing);
    ok &= bind_symbol(core, "projectm_set_fps", api.set_fps, missing);
    ok &= bind_symbol(core, "projectm_set_mesh_size", api.set_mesh_size, missing);
    ok &= bind_symbol(core, "projectm_set_aspect_correction", api.set_aspect_correction, missing);
    ok &= bind_symbol(core, "projectm_set_preset_duration", api.set_preset_duration, missing);
    ok &= bind_symbol(core, "projectm_set_soft_cut_duration", api.set_soft_cut_duration, missing);
    ok &= bind_symbol(core, "projectm_set_hard_cut_enabled", api.set_hard_cut_enabled, missing);
    ok &= bind_symbol(core, "projectm_set_hard_cut_duration", api.set_hard_cut_duration, missing);
    ok &= bind_symbol(core, "projectm_set_beat_sensitivity", api.set_beat_sensitivity, missing);
    ok &= bind_symbol(core, "projectm_set_preset_locked", api.set_preset_locked, missing);
    ok &= bind_symbol(core, "projectm_get_preset_locked", api.get_preset_locked, missing);
    ok &= bind_symbol(core, "projectm_set_texture_search_paths", api.set_texture_search_paths,
                      missing);
    ok &= bind_symbol(core, "projectm_opengl_render_frame", api.render_frame, missing);
    ok &= bind_symbol(core, "projectm_pcm_add_float", api.pcm_add_float, missing);
    ok &= bind_symbol(core, "projectm_pcm_get_max_samples", api.pcm_get_max_samples, missing);
    ok &= bind_symbol(core, "projectm_set_preset_switch_failed_event_callback",
                      api.set_switch_failed_callback, missing);

    ok &= bind_symbol(playlist, "projectm_playlist_create", api.playlist_create, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_destroy", api.playlist_destroy, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_connect", api.playlist_connect, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_add_path", api.playlist_add_path, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_size", api.playlist_size, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_clear", api.playlist_clear, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_item", api.playlist_item, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_free_string", api.playlist_free_string, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_sort", api.playlist_sort, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_set_shuffle", api.playlist_set_shuffle, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_get_shuffle", api.playlist_get_shuffle, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_get_position", api.playlist_get_position,
                      missing);
    ok &= bind_symbol(playlist, "projectm_playlist_set_position", api.playlist_set_position,
                      missing);
    ok &= bind_symbol(playlist, "projectm_playlist_play_next", api.playlist_play_next, missing);
    ok &= bind_symbol(playlist, "projectm_playlist_play_previous", api.playlist_play_previous,
                      missing);
    ok &= bind_symbol(playlist, "projectm_playlist_set_retry_count", api.playlist_set_retry_count,
                      missing);
    ok &= bind_symbol(playlist, "projectm_playlist_set_preset_switched_event_callback",
                      api.playlist_set_switched_callback, missing);

    if (!ok) {
        close_module(playlist);
        close_module(core);
        out_error = "libprojectM opened but does not export `" + missing +
                    "`\n  " + found +
                    "\n  that is not a libprojectM 4 C API, or it was built with a reduced "
                    "export set.";
        return false;
    }

    // THE VERSION GATE.
    //
    // These signatures are written against 4.1.7. A libprojectM 5 would keep the
    // same symbol names and is free to change what they take -- and a wrong
    // signature across a C ABI is undefined behaviour with no diagnostic
    // anywhere. Refusing a major version we were not written against is the only
    // check available, so it is not optional.
    int major = 0;
    int minor = 0;
    int patch = 0;
    api.get_version_components(&major, &minor, &patch);

    if (major != 4) {
        close_module(playlist);
        close_module(core);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "libprojectM %d.%d.%d is not supported -- Holocron binds the version 4 C "
                      "API and a different major version may change these signatures",
                      major, minor, patch);
        out_error = buf;
        return false;
    }

    out.core_      = core;
    out.playlist_  = playlist;
    out.api_       = api;
    out.core_path_ = found;

    char version[64];
    std::snprintf(version, sizeof(version), "%d.%d.%d", major, minor, patch);
    out.version_ = version;

    return true;
}

}   // namespace holocron

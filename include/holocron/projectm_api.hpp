// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/projectm_api.hpp
//
// libprojectM's C API, reached at RUNTIME.
//
// WHY THERE IS NO #include <projectM-4/projectM.h> ANYWHERE IN THIS PROJECT
//
// D-012 and issue 11 require libprojectM to stay on the far side of a shared
// library boundary, reached through its C API only. The strongest form of that
// is not "link the shared library" -- it is not linking it at all: the module is
// opened with LoadLibrary/dlopen when the player starts, and every entry point is
// a function pointer resolved by name.
//
// Three things become true at once, which is exactly what issue 11 predicted:
//
//   1. A Holocron binary built without libprojectM contains no projectM code and
//      no import entry for it, so there is no LGPL obligation from it at all.
//   2. LGPL-2.1 section 6(b)'s "operates properly with a user-installed modified
//      version" is satisfied BY CONSTRUCTION, because replacing the file is the
//      loading mechanism. There is nothing further to arrange.
//   3. A machine with no libprojectM runs the player one facet type short,
//      instead of failing at startup on an unresolved symbol.
//
// And a fourth that only shows up in the build: there is no vcpkg dependency, no
// find_package, and no configure-time switch. CI compiles this file on both
// platforms every time. A path that is only built when an optional dependency is
// present is a path that rots.
//
// SO THE DECLARATIONS BELOW ARE OURS, AND THAT IS THE RISK TO BE HONEST ABOUT
//
// They mirror libprojectM 4.1.7's headers -- `projectM-4/core.h`, `audio.h`,
// `parameters.h`, `render_opengl.h`, `memory.h`, `callbacks.h`, and the playlist
// library's `playlist_*.h`. Nothing is copied: these are function pointer types
// for an ABI, written to match.
//
// A WRONG SIGNATURE HERE IS UNDEFINED BEHAVIOUR WITH NO DIAGNOSTIC. There is no
// link step to catch it and no header to disagree with. Two things guard it:
//
//   - `load_projectm` refuses anything whose major version is not 4, so a future
//     libprojectM that changes a signature cannot be called with these.
//   - Every symbol is resolved by name and a missing one is a clean failure with
//     the name in the message, so a build of libprojectM configured without the
//     playlist library says so rather than crashing.
//
// WHAT IS DELIBERATELY NOT BOUND
//
// Only what the facet calls. projectM exports about forty more entry points --
// touch, easter eggs, preset filters, int16 and uint8 PCM -- and every one bound
// here is another name that can fail to resolve on somebody's build for no
// benefit. Add one when something calls it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace holocron {

// ---------------------------------------------------------------------------
// The ABI
//
// Incomplete struct types rather than void*, so a playlist handle cannot be
// passed where an instance handle belongs. Both are ordinary object pointers, so
// this costs nothing at the boundary.
// ---------------------------------------------------------------------------

struct ProjectMInstance;   // never defined; libprojectM owns what it points at
struct ProjectMPlaylist;   // likewise

using ProjectMHandle         = ProjectMInstance*;
using ProjectMPlaylistHandle = ProjectMPlaylist*;

// projectm_channels. Fixed to int because that is what a C enum of these values
// is on both target compilers, and the static_assert below says so out loud.
enum ProjectMChannels : int {
    kProjectMMono   = 1,
    kProjectMStereo = 2,
};

// projectm_playlist_sort_predicate / _sort_order.
enum ProjectMSortPredicate : int {
    kProjectMSortFullPath     = 0,
    kProjectMSortFilenameOnly = 1,
};

enum ProjectMSortOrder : int {
    kProjectMSortAscending  = 0,
    kProjectMSortDescending = 1,
};

// The C API takes `bool` from <stdbool.h> and `size_t`. Both agree with the C++
// spellings on MSVC and on the Itanium ABI, which is every platform this project
// targets or builds on -- but the agreement is an ABI fact rather than a language
// guarantee, so it is checked rather than assumed.
static_assert(sizeof(bool) == 1, "the C API passes _Bool; C++ bool must match it");
static_assert(sizeof(ProjectMChannels) == sizeof(int),
              "the C API passes an unfixed C enum, which is int-sized on both targets");

extern "C" {

// -- core, from projectM-4.dll ----------------------------------------------

using PfnProjectMCreate  = ProjectMHandle (*)();
using PfnProjectMDestroy = void (*)(ProjectMHandle);

using PfnProjectMGetVersionComponents = void (*)(int* major, int* minor, int* patch);
using PfnProjectMGetVersionString     = char* (*)();
using PfnProjectMFreeString           = void (*)(const char*);

using PfnProjectMSetWindowSize      = void (*)(ProjectMHandle, std::size_t w, std::size_t h);
using PfnProjectMSetFps             = void (*)(ProjectMHandle, std::int32_t fps);
using PfnProjectMSetMeshSize        = void (*)(ProjectMHandle, std::size_t w, std::size_t h);
using PfnProjectMSetAspectCorrection= void (*)(ProjectMHandle, bool);
using PfnProjectMSetPresetDuration  = void (*)(ProjectMHandle, double seconds);
using PfnProjectMSetSoftCutDuration = void (*)(ProjectMHandle, double seconds);
using PfnProjectMSetHardCutEnabled  = void (*)(ProjectMHandle, bool);
using PfnProjectMSetHardCutDuration = void (*)(ProjectMHandle, double seconds);
using PfnProjectMSetBeatSensitivity = void (*)(ProjectMHandle, float);
using PfnProjectMSetPresetLocked    = void (*)(ProjectMHandle, bool);
using PfnProjectMGetPresetLocked    = bool (*)(ProjectMHandle);

using PfnProjectMSetTextureSearchPaths = void (*)(ProjectMHandle, const char** paths,
                                                  std::size_t count);

using PfnProjectMRenderFrame = void (*)(ProjectMHandle);

using PfnProjectMPcmAddFloat     = void (*)(ProjectMHandle, const float* samples,
                                            unsigned int count, ProjectMChannels);
using PfnProjectMPcmGetMaxSamples = unsigned int (*)();

// projectm_preset_switch_failed_event. Bound because a pack of thousands of
// presets always contains some that will not compile on a given GPU, and the
// only alternative to reporting them is a black screen with no explanation.
using PfnProjectMPresetSwitchFailed = void (*)(const char* preset_filename, const char* message,
                                               void* user_data);
using PfnProjectMSetPresetSwitchFailedCallback = void (*)(ProjectMHandle,
                                                          PfnProjectMPresetSwitchFailed,
                                                          void* user_data);

// -- playlist, from projectM-4-playlist.dll ---------------------------------

using PfnPlaylistCreate  = ProjectMPlaylistHandle (*)(ProjectMHandle);
using PfnPlaylistDestroy = void (*)(ProjectMPlaylistHandle);
using PfnPlaylistConnect = void (*)(ProjectMPlaylistHandle, ProjectMHandle);

using PfnPlaylistAddPath = std::uint32_t (*)(ProjectMPlaylistHandle, const char* path,
                                             bool recurse_subdirs, bool allow_duplicates);
using PfnPlaylistSize    = std::uint32_t (*)(ProjectMPlaylistHandle);
using PfnPlaylistClear   = void (*)(ProjectMPlaylistHandle);
using PfnPlaylistItem    = char* (*)(ProjectMPlaylistHandle, std::uint32_t index);
using PfnPlaylistFreeString = void (*)(char*);

using PfnPlaylistSort = void (*)(ProjectMPlaylistHandle, std::uint32_t start, std::uint32_t count,
                                 ProjectMSortPredicate, ProjectMSortOrder);

using PfnPlaylistSetShuffle = void (*)(ProjectMPlaylistHandle, bool);
using PfnPlaylistGetShuffle = bool (*)(ProjectMPlaylistHandle);

using PfnPlaylistGetPosition = std::uint32_t (*)(ProjectMPlaylistHandle);
using PfnPlaylistSetPosition = std::uint32_t (*)(ProjectMPlaylistHandle, std::uint32_t position,
                                                 bool hard_cut);
using PfnPlaylistPlayNext     = std::uint32_t (*)(ProjectMPlaylistHandle, bool hard_cut);
using PfnPlaylistPlayPrevious = std::uint32_t (*)(ProjectMPlaylistHandle, bool hard_cut);

using PfnPlaylistSetRetryCount = void (*)(ProjectMPlaylistHandle, std::uint32_t);

using PfnPlaylistPresetSwitched = void (*)(bool is_hard_cut, unsigned int index, void* user_data);
using PfnPlaylistSetPresetSwitchedCallback = void (*)(ProjectMPlaylistHandle,
                                                      PfnPlaylistPresetSwitched, void* user_data);

}   // extern "C"

// ---------------------------------------------------------------------------
// The resolved entry points
//
// One struct rather than free function pointers, so there is exactly one thing
// to pass to a facet and exactly one thing whose lifetime matters. Every member
// is non-null once load_projectm has returned true; there is no "maybe this one
// resolved" state, because a partially bound library is a crash waiting for the
// first preset that exercises the missing call.
// ---------------------------------------------------------------------------

struct ProjectMApi {
    PfnProjectMCreate                        create                    = nullptr;
    PfnProjectMDestroy                       destroy                   = nullptr;
    PfnProjectMGetVersionComponents          get_version_components    = nullptr;
    PfnProjectMGetVersionString              get_version_string        = nullptr;
    PfnProjectMFreeString                    free_string               = nullptr;
    PfnProjectMSetWindowSize                 set_window_size           = nullptr;
    PfnProjectMSetFps                        set_fps                   = nullptr;
    PfnProjectMSetMeshSize                   set_mesh_size             = nullptr;
    PfnProjectMSetAspectCorrection           set_aspect_correction     = nullptr;
    PfnProjectMSetPresetDuration             set_preset_duration       = nullptr;
    PfnProjectMSetSoftCutDuration            set_soft_cut_duration     = nullptr;
    PfnProjectMSetHardCutEnabled             set_hard_cut_enabled      = nullptr;
    PfnProjectMSetHardCutDuration            set_hard_cut_duration     = nullptr;
    PfnProjectMSetBeatSensitivity            set_beat_sensitivity      = nullptr;
    PfnProjectMSetPresetLocked               set_preset_locked         = nullptr;
    PfnProjectMGetPresetLocked               get_preset_locked         = nullptr;
    PfnProjectMSetTextureSearchPaths         set_texture_search_paths  = nullptr;
    PfnProjectMRenderFrame                   render_frame              = nullptr;
    PfnProjectMPcmAddFloat                   pcm_add_float             = nullptr;
    PfnProjectMPcmGetMaxSamples              pcm_get_max_samples       = nullptr;
    PfnProjectMSetPresetSwitchFailedCallback set_switch_failed_callback = nullptr;

    PfnPlaylistCreate                        playlist_create           = nullptr;
    PfnPlaylistDestroy                       playlist_destroy          = nullptr;
    PfnPlaylistConnect                       playlist_connect          = nullptr;
    PfnPlaylistAddPath                       playlist_add_path         = nullptr;
    PfnPlaylistSize                          playlist_size             = nullptr;
    PfnPlaylistClear                         playlist_clear            = nullptr;
    PfnPlaylistItem                          playlist_item             = nullptr;
    PfnPlaylistFreeString                    playlist_free_string      = nullptr;
    PfnPlaylistSort                          playlist_sort             = nullptr;
    PfnPlaylistSetShuffle                    playlist_set_shuffle      = nullptr;
    PfnPlaylistGetShuffle                    playlist_get_shuffle      = nullptr;
    PfnPlaylistGetPosition                   playlist_get_position     = nullptr;
    PfnPlaylistSetPosition                   playlist_set_position     = nullptr;
    PfnPlaylistPlayNext                      playlist_play_next        = nullptr;
    PfnPlaylistPlayPrevious                  playlist_play_previous    = nullptr;
    PfnPlaylistSetRetryCount                 playlist_set_retry_count  = nullptr;
    PfnPlaylistSetPresetSwitchedCallback     playlist_set_switched_callback = nullptr;
};

// ---------------------------------------------------------------------------
// The library
//
// Owns the two module handles and closes them in the destructor. Move-only, for
// the usual reason: two copies would each close the same handle.
//
// LOAD IT ONCE PER PROCESS AND KEEP IT. The facet is rebuilt on every crossfade
// and on every hot reload, and unloading a shared library that has live GL
// objects and static state behind it -- on a driver that may itself have
// registered thread-local teardown -- is the kind of thing that works for months
// and then does not. The player owns one of these for its whole run and hands
// facets a reference to the ProjectMApi inside it.
// ---------------------------------------------------------------------------

class ProjectMLibrary {
public:
    ProjectMLibrary() = default;
    ~ProjectMLibrary();

    ProjectMLibrary(const ProjectMLibrary&)            = delete;
    ProjectMLibrary& operator=(const ProjectMLibrary&) = delete;

    ProjectMLibrary(ProjectMLibrary&& other) noexcept;
    ProjectMLibrary& operator=(ProjectMLibrary&& other) noexcept;

    bool loaded() const { return core_ != nullptr; }

    const ProjectMApi& api() const { return api_; }

    // Initialise the GL loader libprojectM's own calls go through.
    //
    // REQUIRES A CURRENT GL CONTEXT, and must be called before any facet is
    // created. It is separate from load_projectm because load_projectm is
    // deliberately context-free -- that is what lets the binding be tested with
    // no window, no GPU and no driver, which is every CI runner.
    //
    // WHY THIS EXISTS AT ALL, WHICH IS THE SURPRISE OF M4
    //
    // On Windows libprojectM includes <GL/glew.h> and every GL call it makes goes
    // through GLEW's function-pointer table -- and it NEVER CALLS glewInit. The
    // only glewInit in the whole 4.1.7 source tree is in its own SDL test UI.
    // The host is expected to have initialised GLEW, which every projectM host
    // does by linking GLEW itself.
    //
    // Holocron uses glad. Two loaders, and only one of them was initialised: all
    // of GLEW's pointers are null, so projectm_create() dereferences a null
    // function pointer and the process dies at 0xC0000005 with nothing printed.
    //
    // The fix is to do what the host is supposed to do, without linking GLEW:
    // glew32.dll is already in the process as projectM-4.dll's own dependency, so
    // `glewExperimental` and `glewInit` are resolved from it by name and called
    // here. glewExperimental must be set first -- in a core profile GLEW's
    // non-experimental path asks for GL_EXTENSIONS as a single string, which is
    // invalid there, and initialisation fails.
    //
    // On Linux libprojectM includes <GL/gl.h> with GL_GLEXT_PROTOTYPES and calls
    // GL directly, so the dynamic linker has already done this and there is
    // nothing to do. It returns true.
    //
    // Idempotent, and false with a reason if the loader could not be brought up.
    bool init_gl(std::string& out_error);

    // True once init_gl has succeeded -- or immediately, where nothing is needed.
    // A facet refuses to build against a library that is not ready, because the
    // alternative is the access violation described above.
    bool gl_ready() const { return gl_ready_; }

    // "4.1.7", or empty when nothing is loaded. Reported so a bug report says
    // which libprojectM produced the picture -- the presets that compile differ
    // between point releases.
    const std::string& version() const { return version_; }

    // Where the core module was actually found, which is not always where it was
    // asked for: an empty `library_dir` lets the OS loader search, and knowing
    // what it settled on is the difference between diagnosing a stale copy in
    // five seconds and in an hour.
    const std::string& core_path() const { return core_path_; }

    void unload();

private:
    void*       core_     = nullptr;
    void*       playlist_ = nullptr;
    ProjectMApi api_{};
    std::string version_;
    std::string core_path_;
    bool        gl_ready_ = false;

    friend bool load_projectm(const std::string&, ProjectMLibrary&, std::string&);
};

// Open libprojectM and resolve every entry point above.
//
// `library_dir` is a DIRECTORY, not a file. Empty means "let the OS loader
// search", which is what a system-installed libprojectM wants. A directory is the
// unit rather than a file because the two modules always ship together and, on
// Windows, so does the GLEW that projectM-4.dll itself imports -- naming one file
// would leave the other two to luck.
//
// Returns false with a message in `out_error` and `out` left unloaded. Every
// failure mode here is ordinary rather than exceptional: no libprojectM
// installed is the DEFAULT state of a fresh machine, and the player carries on
// without it.
bool load_projectm(const std::string& library_dir, ProjectMLibrary& out, std::string& out_error);

// The file names looked for, in order, for the core and playlist modules.
//
// Exposed so the "not found" message can list what was actually tried, and so a
// test can assert the platform naming without a real library present. On Windows
// there is one spelling each; on Linux the versioned soname is tried first
// because that is what a distribution package installs, with the unversioned
// development symlink after it.
const char* const* projectm_core_names(std::size_t& count);
const char* const* projectm_playlist_names(std::size_t& count);

}   // namespace holocron

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See projectm_facet.hpp -- in particular the note about framebuffer 0, which is
// the reason this file is shaped the way it is.

#include <holocron/projectm_facet.hpp>

#include <holocron/audio_frame.hpp>
#include <holocron/projectm_api.hpp>
#include <holocron/track_context.hpp>

#include "gl_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

namespace holocron {

namespace {

// How many failing presets to name before falling silent.
//
// A pack of thousands written across twenty years of GPUs always contains some
// that will not compile on any given driver, and printing each one turns the
// first minute of a run into a wall of text that hides everything else. The first
// few are diagnostic -- they usually share a cause -- and the total is the number
// that actually tells you whether the pack is healthy.
constexpr std::size_t kMaxNamedFailures = 5;

// Retries when a preset fails to load, before the playlist gives up on advancing.
//
// Not zero, which would mean one bad file freezes the visualization on whatever
// was up. Not large either: if ten in a row fail, the pack or the driver is the
// problem and hammering through five thousand of them at frame rate is not the
// way to find that out.
constexpr std::uint32_t kPlaylistRetries = 10;

// The stem of a preset path, for display. "C:/packs/a/b/Geiss - Cosmic Dust.milk"
// becomes "Geiss - Cosmic Dust".
std::string display_name(const char* path)
{
    if (path == nullptr) {
        return {};
    }
    std::error_code ec;
    return std::filesystem::path(path).stem().string();
}

}   // namespace

struct ProjectMFacet::Impl {
    const ProjectMApi*     api      = nullptr;
    ProjectMHandle         pm       = nullptr;
    ProjectMPlaylistHandle playlist = nullptr;

    ProjectMSettings settings;

    std::chrono::steady_clock::time_point started{};
    float                                 elapsed_offset = 0.0f;

    // What projectM was last told the window is. Only pushed on a change:
    // set_window_size reallocates every internal buffer, so calling it every
    // frame would reallocate a 4K pipeline sixty times a second.
    int width  = 0;
    int height = 0;

    // The last analysis frame fed. Sentinel rather than 0 because frame_index
    // legitimately starts at 0 and the first frame must not be mistaken for a
    // repeat.
    std::uint64_t last_fed  = 0;
    bool          fed_any   = false;
    std::uint64_t dropped   = 0;

    std::size_t failures      = 0;
    std::size_t current       = 0;
    std::string current_name;

    // Called by libprojectM from inside render_frame. `user_data` is this.
    static void on_switch_failed(const char* preset_filename, const char* message, void* user_data)
    {
        auto* self = static_cast<Impl*>(user_data);
        if (self == nullptr) {
            return;
        }
        ++self->failures;
        if (self->failures <= kMaxNamedFailures) {
            std::fprintf(stderr, "holocron: projectM could not load \"%s\" -- %s\n",
                         preset_filename != nullptr ? preset_filename : "?",
                         message != nullptr ? message : "no reason given");
            if (self->failures == kMaxNamedFailures) {
                std::fprintf(stderr, "holocron: further preset failures will be counted, not "
                                     "printed\n");
            }
        }
    }

    static void on_switched(bool /*is_hard_cut*/, unsigned int index, void* user_data)
    {
        auto* self = static_cast<Impl*>(user_data);
        if (self == nullptr) {
            return;
        }
        self->current = index;

        // The name is fetched HERE rather than on demand, because
        // projectm_playlist_item allocates and the caller frees through the
        // playlist module's own allocator -- doing that from a const getter on
        // the render thread's hot path is a malloc per frame for a string that
        // changes every thirty seconds.
        if (self->api != nullptr && self->playlist != nullptr) {
            if (char* item = self->api->playlist_item(self->playlist, index); item != nullptr) {
                self->current_name = display_name(item);
                self->api->playlist_free_string(item);
            }
        }
    }
};

ProjectMFacet::ProjectMFacet() : impl_(std::make_unique<Impl>()) {}

ProjectMFacet::~ProjectMFacet() { shutdown(); }

bool ProjectMFacet::init(const ProjectMLibrary& library, const ProjectMSettings& settings,
                         std::string& out_error)
{
    shutdown();

    if (!library.loaded()) {
        out_error = "libprojectM is not loaded";
        return false;
    }
    if (!library.gl_ready()) {
        // Checked rather than trusted. See projectm_api.hpp: on Windows,
        // projectm_create against an uninitialised GLEW is an access violation
        // with no output at all, and this turns that into a sentence.
        out_error = "libprojectM's GL loader was never initialised -- call "
                    "ProjectMLibrary::init_gl with a context current first";
        return false;
    }

    const ProjectMApi& api = library.api();

    if (settings.preset_path.empty()) {
        out_error = "no preset path -- set [projectm] preset_path in gatekeeper.toml to a "
                    "directory of .milk files";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(settings.preset_path, ec)) {
        out_error = "preset path is not a directory: " + settings.preset_path;
        return false;
    }

    impl_->api      = &library.api();
    impl_->settings = settings;

    impl_->pm = api.create();
    if (impl_->pm == nullptr) {
        // projectm_create returns null when its own GL initialisation fails,
        // which on this project's context should not happen -- but a null
        // dereference on the first frame is a far worse way to find out.
        impl_->api = nullptr;
        out_error  = "projectm_create() returned null -- libprojectM could not initialise "
                     "against this GL context";
        return false;
    }

    // Textures BEFORE the playlist: a preset that samples an image resolves it
    // when it loads, and the first one loads at set_position below.
    if (!settings.texture_path.empty()) {
        const char* paths[1] = {settings.texture_path.c_str()};
        api.set_texture_search_paths(impl_->pm, paths, 1);
    }

    api.set_mesh_size(impl_->pm, static_cast<std::size_t>(std::max(8, settings.mesh_x)),
                      static_cast<std::size_t>(std::max(8, settings.mesh_y)));
    api.set_fps(impl_->pm, settings.fps > 0 ? settings.fps : 60);
    api.set_aspect_correction(impl_->pm, settings.aspect_correction);
    api.set_preset_duration(impl_->pm, settings.preset_duration);
    api.set_soft_cut_duration(impl_->pm, settings.soft_cut_duration);
    api.set_hard_cut_enabled(impl_->pm, settings.hard_cut_enabled);
    api.set_hard_cut_duration(impl_->pm, settings.hard_cut_duration);
    api.set_beat_sensitivity(impl_->pm, settings.beat_sensitivity);
    api.set_preset_locked(impl_->pm, settings.locked);

    api.set_switch_failed_callback(impl_->pm, &Impl::on_switch_failed, impl_.get());

    impl_->playlist = api.playlist_create(impl_->pm);
    if (impl_->playlist == nullptr) {
        api.destroy(impl_->pm);
        impl_->pm  = nullptr;
        impl_->api = nullptr;
        out_error  = "projectm_playlist_create() returned null";
        return false;
    }

    api.playlist_set_switched_callback(impl_->playlist, &Impl::on_switched, impl_.get());
    api.playlist_set_retry_count(impl_->playlist, kPlaylistRetries);

    // RECURSIVE, and that is not a default worth leaving to chance -- real packs
    // are nested several directories deep, and a non-recursive scan of the top of
    // one returns zero and is indistinguishable from a wrong path.
    const std::uint32_t added = api.playlist_add_path(impl_->playlist,
                                                      settings.preset_path.c_str(),
                                                      /*recurse_subdirs=*/true,
                                                      /*allow_duplicates=*/false);
    if (added == 0) {
        api.playlist_destroy(impl_->playlist);
        api.destroy(impl_->pm);
        impl_->playlist = nullptr;
        impl_->pm       = nullptr;
        impl_->api      = nullptr;
        out_error = "no presets found under " + settings.preset_path +
                    "\n  the scan is recursive, so this means there are no .milk files "
                    "anywhere below it";
        return false;
    }

    // Sorted before shuffling, for the same reason the vault is sorted by name:
    // directory order differs between filesystems, so an UNSHUFFLED playlist
    // would be in a different order on two machines with the same pack -- and
    // "preset 400" would mean two different things.
    api.playlist_sort(impl_->playlist, 0, added, kProjectMSortFullPath, kProjectMSortAscending);
    api.playlist_set_shuffle(impl_->playlist, settings.shuffle);

    // Put something on screen. Without this projectM draws its built-in idle
    // animation until the first preset_duration elapses, which reads as "the
    // preset path did not work" for the first thirty seconds of every run.
    api.playlist_set_position(impl_->playlist, 0, /*hard_cut=*/true);

    impl_->started        = std::chrono::steady_clock::now();
    impl_->elapsed_offset = 0.0f;

    std::printf("holocron: projectM has %u preset%s from %s%s\n", added, added == 1 ? "" : "s",
                settings.preset_path.c_str(), settings.shuffle ? ", shuffled" : "");
    std::fflush(stdout);

    return true;
}

void ProjectMFacet::shutdown()
{
    // WHAT THE RUN LOOKED LIKE, and only when there is something to say.
    //
    // Both of these are branches nothing else prints, and a branch no log prints
    // is a branch that cannot be diagnosed. A scope with gaps in it because the
    // render thread fell behind the analysis rate, and a scope with gaps in it
    // because the PCM feed is broken, look identical on screen.
    //
    // Silent when both are zero, because shutdown also runs at the end of every
    // crossfade and a healthy line per transition is noise.
    if (impl_->failures > 0 || impl_->dropped > 0) {
        std::printf("holocron: projectM -- %zu preset(s) failed to load, %llu analysis frame(s) "
                    "never fed (the render thread was slower than 93.75 Hz)\n",
                    impl_->failures, static_cast<unsigned long long>(impl_->dropped));
        std::fflush(stdout);
    }

    if (impl_->api != nullptr) {
        // Playlist first: it holds a pointer to the projectM instance and
        // disconnects itself from its callbacks on destroy.
        if (impl_->playlist != nullptr) {
            impl_->api->playlist_destroy(impl_->playlist);
        }
        if (impl_->pm != nullptr) {
            impl_->api->destroy(impl_->pm);
        }
    }
    impl_->playlist = nullptr;
    impl_->pm       = nullptr;
    impl_->api      = nullptr;
    impl_->width    = 0;
    impl_->height   = 0;
    impl_->fed_any  = false;
    impl_->current_name.clear();
}

bool ProjectMFacet::ready() const { return impl_->pm != nullptr && impl_->playlist != nullptr; }

void ProjectMFacet::draw(const AudioFrame& frame, const TrackContext& /*track*/, int width,
                         int height)
{
    // TrackContext is unused and that is a decision rather than an omission. A
    // MilkDrop preset has no idea what a palette or an album sleeve is; there is
    // no uniform to bind one to and no equation that reads one. Tinting projectM
    // towards the record's colours would mean post-processing its output, which
    // is a look to choose on a projector and not a thing to smuggle in here.
    if (!ready() || width <= 0 || height <= 0) {
        return;
    }

    const ProjectMApi& api = *impl_->api;

    // -- audio -------------------------------------------------------------
    //
    // Once per NEW analysis frame. Feeding on every render frame would push the
    // same 512 samples twice whenever the render thread outruns 93.75 Hz, which
    // is most of the time, and a repeated stretch inside projectM's PCM ring is a
    // visible stutter in every scope a preset draws.
    if (!impl_->fed_any || frame.frame_index != impl_->last_fed) {
        if (impl_->fed_any && frame.frame_index > impl_->last_fed + 1) {
            impl_->dropped += frame.frame_index - impl_->last_fed - 1;
        }
        impl_->last_fed = frame.frame_index;
        impl_->fed_any  = true;

        api.pcm_add_float(impl_->pm, frame.waveform.data(),
                          static_cast<unsigned int>(kWaveformLen), kProjectMMono);
    }

    // -- size --------------------------------------------------------------
    if (width != impl_->width || height != impl_->height) {
        impl_->width  = width;
        impl_->height = height;
        api.set_window_size(impl_->pm, static_cast<std::size_t>(width),
                            static_cast<std::size_t>(height));
    }

    // -- where the caller wanted the picture --------------------------------
    //
    // Read BEFORE the render, because libprojectM is about to bind framebuffer 0
    // over the top of it and put nothing back. See the header.
    GLint dest = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dest);

    api.render_frame(impl_->pm);

    // -- back into the layer -------------------------------------------------
    //
    // projectM drew into the bottom-left `width` by `height` of the back buffer,
    // because that is the window size it was given. The layer is the same size,
    // so this is a straight rectangle copy and GL_NEAREST is exact rather than a
    // choice about filtering.
    //
    // The back buffer is never presented in this state: the compositor clears it
    // and draws the assembled stack onto it before the swap.
    if (dest != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(dest));
        glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }

    // -- put the world back --------------------------------------------------
    //
    // Not tidiness. The layer loop draws the next facet immediately after this
    // one and CrystalFacet does not touch blend state -- it draws a full-screen
    // triangle and expects it to REPLACE what is in the layer. libprojectM leaves
    // GL_BLEND enabled with its own function, so a crystal layered over projectM
    // would blend into it instead of covering it, and the fault would look like a
    // compositor bug two files away.
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(dest));
    glViewport(0, 0, width, height);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);

    glUseProgram(0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

float ProjectMFacet::elapsed() const
{
    if (!ready()) {
        return impl_->elapsed_offset;
    }
    const auto since = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                                    impl_->started)
                           .count();
    return impl_->elapsed_offset + since;
}

void ProjectMFacet::set_elapsed(float seconds)
{
    // Recorded, and it moves nothing on screen. libprojectM owns its animation
    // clock and offers no way to set it, so this keeps elapsed() consistent for a
    // caller that asks and does not pretend to more. Nothing calls it -- hot
    // reload is a crystal's mechanism and there is no shader here to save.
    impl_->elapsed_offset = seconds;
    impl_->started        = std::chrono::steady_clock::now();
}

std::size_t ProjectMFacet::preset_count() const
{
    if (!ready()) {
        return 0;
    }
    return impl_->api->playlist_size(impl_->playlist);
}

std::string ProjectMFacet::current_preset() const { return impl_->current_name; }

std::size_t ProjectMFacet::current_index() const { return impl_->current; }

void ProjectMFacet::next_preset(bool hard_cut)
{
    if (ready()) {
        impl_->api->playlist_play_next(impl_->playlist, hard_cut);
    }
}

void ProjectMFacet::previous_preset(bool hard_cut)
{
    if (ready()) {
        impl_->api->playlist_play_previous(impl_->playlist, hard_cut);
    }
}

void ProjectMFacet::set_shuffle(bool on)
{
    impl_->settings.shuffle = on;
    if (ready()) {
        impl_->api->playlist_set_shuffle(impl_->playlist, on);
    }
}

bool ProjectMFacet::shuffle() const
{
    if (!ready()) {
        return impl_->settings.shuffle;
    }
    return impl_->api->playlist_get_shuffle(impl_->playlist);
}

void ProjectMFacet::set_locked(bool on)
{
    impl_->settings.locked = on;
    if (ready()) {
        impl_->api->set_preset_locked(impl_->pm, on);
    }
}

bool ProjectMFacet::locked() const
{
    if (!ready()) {
        return impl_->settings.locked;
    }
    return impl_->api->get_preset_locked(impl_->pm);
}

std::size_t ProjectMFacet::failed_presets() const { return impl_->failures; }

std::uint64_t ProjectMFacet::dropped_audio_frames() const { return impl_->dropped; }

}   // namespace holocron

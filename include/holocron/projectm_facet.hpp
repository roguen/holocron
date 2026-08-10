// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/projectm_facet.hpp
//
// A facet that draws MilkDrop presets through libprojectM.
//
// THE ONE THING THAT SHAPES THIS WHOLE FILE
//
// `projectm_opengl_render_frame` UNCONDITIONALLY BINDS FRAMEBUFFER 0. It is not
// "renders into whatever you bound"; libprojectM 4.1.7 calls
// glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0) itself, sets its own viewport from
// the window size it was given, and restores none of it on the way out.
//
// That is checked, not assumed -- it is in ProjectM::RenderFrame in the 4.1.7
// source, and 4.1.7 is the newest release. There is no entry point taking a
// framebuffer object; a `projectm_opengl_render_frame_fbo` exists in some
// downstream discussions and in no shipped version.
//
// So the M4 criterion "renders to a texture, not the default framebuffer" cannot
// be met by binding a layer and calling render. It is met by rendering into the
// BACK BUFFER and blitting the result into the layer:
//
//   1. remember the framebuffer the compositor bound
//   2. let projectM draw into framebuffer 0, at layer size, from the origin
//   3. glBlitFramebuffer that rectangle into the layer
//   4. put the framebuffer, the viewport and the state projectM stamped on back
//
// The back buffer is scratch until SwapBuffers, and the compositor clears it
// before assembling the picture, so nothing projectM leaves there is ever seen.
// The cost is one blit of layer_w x layer_h -- measured, see the PR -- and one
// real limitation: the round trip goes through the window's 8-bit format, so a
// projectM layer cannot carry values above 1.0 and gets no bloom. That is not a
// loss in practice, because MilkDrop output is 0..1 by construction.
//
// `--no-compositor` needs none of this. The destination is already framebuffer 0,
// so step 3 is skipped and projectM draws straight to the window -- the fallback
// path is simpler here rather than more complicated, which is unusual enough to
// be worth saying.
//
// WHERE THE AUDIO COMES FROM, AND WHY NOT THE PCM RING
//
// From `AudioFrame::waveform` -- 512 mono samples at 48 kHz, which is exactly one
// analysis hop, so consecutive frames tile the signal with no overlap and no gap.
//
// PcmRing was the obvious source and is the wrong one. It is single-producer,
// single-consumer with its consumer already spoken for (the audio callback), and
// a second tap on the decode side would be UNCORRECTED: the decoder runs ahead of
// the speakers by the measured 51 ms, and the picture is pulled a further 90 ms
// earlier by the trim. projectM would react about 140 ms away from every crystal
// on screen beside it. Taking the waveform off the frame that FrameHistory
// already selected by position puts projectM on the same clock as everything
// else, for free.
//
// It is fed once per NEW analysis frame, detected by `frame_index`. A render
// thread faster than 93.75 Hz would otherwise feed the same 512 samples twice and
// put a visible stutter in every scope; one slower than that skips frames, which
// leaves a small gap in the scope and is counted and reported rather than hidden.
//
// Mono, because that is what the contract carries. Presets that draw separate
// left and right scopes will draw them on top of each other. Adding
// `waveform_left` / `waveform_right` to AudioFrame is the sanctioned fix and is
// deliberately not done here: adding a field later is safe and cheap, and
// shipping the simple thing first is how it gets judged on a projector before
// 4 KB is added to a struct that crosses a thread boundary 93.75 times a second.
//
// PRESETS ARE NEVER SHIPPED. `preset_path` points outside the repository at a
// pack the user obtained themselves. See the wiki's Preset-Packs page for why
// that is a licence rule and not a preference.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <holocron/facet.hpp>

namespace holocron {

struct AudioFrame;
class ProjectMLibrary;
struct TrackContext;

// Everything `[projectm]` in gatekeeper.toml can say, plus the two things only
// the player knows. Defaults are what a fresh install gets.
struct ProjectMSettings {
    // A DIRECTORY OF .milk FILES, SCANNED RECURSIVELY. Real packs are nested
    // several levels deep, so a non-recursive scan of the top of one finds
    // nothing and looks exactly like a wrong path.
    //
    // Empty is a configuration error rather than a default: projectM with no
    // presets draws its idle animation, which is not what anybody pointed it at.
    std::string preset_path;

    // Where presets look for the images some of them sample. Optional; a preset
    // asking for a texture that is not there falls back to a built-in.
    std::string texture_path;

    // How long a preset stays up, and how long the blend between two takes.
    double preset_duration   = 30.0;
    double soft_cut_duration = 3.0;

    // A hard cut is an instant change on a loud transient rather than a timed
    // blend. Off by default: it fires on the music rather than on the clock, and
    // whether that reads as responsive or as flickering is a judgement to make on
    // the projector, not one to impose from a default.
    bool   hard_cut_enabled  = false;
    double hard_cut_duration = 60.0;

    float beat_sensitivity = 1.0f;

    // Playlist order. Shuffle on, because a pack is thousands of files in
    // whatever order the filesystem gave them and alphabetical order through it
    // means a whole evening in the a's.
    bool shuffle = true;

    // Hold the current preset instead of advancing. Runtime state as much as
    // configuration -- the control page toggles it, which is the point of it.
    bool locked = false;

    // The per-preset warp/composite grid. projectM's own default is 48x32.
    // Larger is smoother and costs real time on a 4K layer.
    int mesh_x = 48;
    int mesh_y = 32;

    bool aspect_correction = true;

    // What projectM believes the frame rate is. It uses this to convert its
    // durations above into frame counts, so a wrong value makes every preset
    // duration wrong by the same ratio -- and nothing else, which is why it is
    // survivable to give it the vsync rate rather than a measurement.
    int fps = 60;
};

class ProjectMFacet final : public Facet {
public:
    ProjectMFacet();
    ~ProjectMFacet() override;

    ProjectMFacet(const ProjectMFacet&)            = delete;
    ProjectMFacet& operator=(const ProjectMFacet&) = delete;

    // Create the projectM instance and load the playlist. Requires a current
    // GL 4.5 core context -- libprojectM builds its programs and textures inside
    // projectm_create.
    //
    // `library` must outlive this facet -- the player loads one and keeps it for
    // the whole run; see projectm_api.hpp for why the module is never unloaded
    // and reopened.
    //
    // THE LIBRARY RATHER THAN JUST ITS ProjectMApi, so this can refuse one whose
    // GL loader has not been brought up. On Windows that is not a theoretical
    // check: calling projectm_create against an uninitialised GLEW is an access
    // violation with nothing printed, and a facet that can detect it should.
    //
    // Returns false with a reason in `out_error`. An empty playlist is a failure
    // rather than a facet that draws the idle animation: someone who configured a
    // preset path and got the wrong one should be told, not shown something that
    // looks deliberate.
    bool init(const ProjectMLibrary& library, const ProjectMSettings& settings,
              std::string& out_error);

    void shutdown();

    bool ready() const override;

    void draw(const AudioFrame& frame, const TrackContext& track, int width, int height) override;

    // Seconds since init.
    //
    // set_elapsed IS RECORDED AND CANNOT BE HONOURED, and saying so is better
    // than pretending. libprojectM owns its own animation clock and exposes no
    // way to move it, so a value set here changes what elapsed() reports and
    // nothing on screen. Nothing calls it: hot reload is a crystal's mechanism
    // and there is no .frag here to save.
    float elapsed() const override;
    void  set_elapsed(float seconds) override;

    // -- the playlist, which is what M4 calls "facet parameters" --------------

    std::size_t preset_count() const;

    // The preset on screen, as a display name: the file's stem, with the
    // directories and the .milk taken off. Empty before the first one loads.
    std::string current_preset() const;
    std::size_t current_index() const;

    // hard_cut true switches instantly; false blends over soft_cut_duration.
    void next_preset(bool hard_cut);
    void previous_preset(bool hard_cut);

    void set_shuffle(bool on);
    bool shuffle() const;

    // Hold this preset: no timer advance, no hard cut.
    void set_locked(bool on);
    bool locked() const;

    // -- what the run looked like, for the log --------------------------------

    // Presets libprojectM refused to compile. NORMAL AND EXPECTED in a pack of
    // thousands written for twenty years of different hardware -- reported as a
    // count so the log says "41 of 4000" rather than printing 41 stack traces.
    std::size_t failed_presets() const;

    // Analysis frames that went past between two draws and were never fed to
    // projectM, because the render thread was slower than 93.75 Hz. Zero at any
    // frame rate above the analysis rate. Reported because a scope with gaps in
    // it and a scope with a bug in it look the same.
    std::uint64_t dropped_audio_frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace holocron

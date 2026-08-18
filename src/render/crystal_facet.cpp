// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See crystal_facet.hpp.

#include <holocron/crystal_facet.hpp>

#include <holocron/audio_frame.hpp>
#include <holocron/crystal.hpp>
#include <holocron/envelope.hpp>
#include <holocron/frame_binding.hpp>
#include <holocron/track_context.hpp>

#include <holocron/shader_cache.hpp>

#include "gl_api.hpp"

#include "gl_bind.hpp"

#include <chrono>
#include <cmath>
#include <utility>
#include <vector>

namespace holocron {

namespace {

// A FULL-SCREEN TRIANGLE, NOT A QUAD, AND WITH NO VERTEX BUFFER AT ALL.
//
// Three vertices generated from gl_VertexID cover the screen with one triangle
// whose corners sit outside it. That beats a two-triangle quad twice over: no
// buffer, no VAO contents and no attribute plumbing to get wrong, and no seam
// down the diagonal where the two triangles of a quad meet -- which shows up as
// a visible line in any shader doing derivative-based work.
//
// An empty VAO must still be bound; core profile refuses to draw without one.
const char* kVertexShader = R"glsl(#version 300 es
out vec2 v_uv;
void main()
{
    // (0,0) (2,0) (0,2) in UV, which is (-1,-1) (3,-1) (-1,3) in clip space.
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

GLuint compile(GLenum stage, const char* src, std::string& log)
{
    const GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, buf.data());
        log.assign(buf.data());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

// Step one binding's envelope state forward.
//
// TWO alpha VALUES FOR THE WHOLE ARRAY, NOT TWO PER ELEMENT. The coefficient
// depends only on (tau, hops), and there are only two taus -- one for rising and
// one for falling. Computing them per element would be 2,048 `exp` calls a frame
// for `fft_magnitude` instead of two.
//
// A NON-FINITE FIELD VALUE IS SKIPPED, AND THAT IS A NEW HAZARD THIS FEATURE
// CREATES. Today a NaN in an AudioFrame field reaches a uniform for exactly one
// frame and is gone when the next one arrives. With state behind it,
// `y + alpha * (nan - y)` is nan and every later step keeps it -- permanently,
// for the life of the facet, over a picture that has gone black with nothing to
// say why. Holding the previous value instead turns that back into the
// single-frame glitch it is now.
void advance_envelope(std::vector<float>& state, const EnvelopeSpec& spec,
                      const AudioFrame& frame, const Binding& binding, const HopStep& step)
{
    const bool   scalar = binding.kind == BindingKind::kScalar;
    const float* src    = scalar ? nullptr : read_array(frame, binding);
    const float  single = scalar ? read_scalar(frame, binding) : 0.0f;

    const float attack_alpha = envelope_alpha(spec.attack, step.hops);
    const float decay_alpha  = envelope_alpha(spec.decay, step.hops);

    for (std::size_t i = 0; i < state.size(); ++i) {
        const float raw = scalar ? single : src[i];
        if (!std::isfinite(raw)) {
            continue;
        }
        const float x = spec.scale * raw;

        if (spec.mode == EnvelopeMode::kAccumulate) {
            // A new track restarts the phase rather than carrying the previous
            // one's position into it, which is the same choice the envelope
            // branch makes and for the same reason.
            state[i] = step.reseed ? 0.0f : accumulate_apply(state[i], x, step.hops);
        } else {
            state[i] = step.reseed ? x : envelope_apply(state[i], x, attack_alpha, decay_alpha);
        }
    }
}

}  // namespace

struct CrystalFacet::Impl {
    GLuint program = 0;
    GLuint vao     = 0;

    GLint u_resolution = -1;
    GLint u_time       = -1;

    GLint u_palette         = -1;
    GLint u_palette_primary = -1;
    GLint u_palette_accent  = -1;
    GLint u_album_art       = -1;
    GLint u_has_art         = -1;
    GLint u_feedback        = -1;
    GLint u_has_feedback    = -1;

    // Set from outside once per frame, before draw(). Not owned here -- the
    // compositor owns the target it belongs to.
    TextureHandle feedback = 0;

    // One per manifest entry, resolved once. -1 means the compiler removed it.
    struct Bound {
        GLint          location;
        const Binding* binding;
        EnvelopeSpec   envelope;

        // The envelope's running value, one per element -- `binding->count` of
        // them, so an array binding smooths bin by bin rather than as a whole.
        // Empty when the spec is inactive, which is what keeps the zero-copy
        // upload path for every crystal that does not ask for an envelope.
        std::vector<float> state;
    };
    std::vector<Bound> bound;
    std::size_t        unused = 0;

    // Where the analysis had got to when the envelopes were last advanced.
    //
    // `saw_any` is separate from `last_index` because THE FIRST FRAME OF EVERY
    // TRACK HAS INDEX 0 -- see hops_between() in envelope.hpp. Without it the
    // first frame compares 0 against 0, finds no change, and never seeds.
    std::uint64_t last_index = 0;
    bool          saw_any    = false;

    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

CrystalFacet::CrystalFacet() : impl_(std::make_unique<Impl>()) {}
CrystalFacet::~CrystalFacet() { shutdown(); }

bool CrystalFacet::init(const Crystal& crystal, std::string& out_log, const ShaderCache* cache)
{
    shutdown();
    out_log.clear();

    // ISSUE 288. `duel` takes 23,859 ms to compile and link on Tegra, on this
    // thread, which is a twenty-four second freeze of the picture every time it
    // is switched to. A restored binary skips all of it.
    //
    // THE KEY IS BOTH STAGES. Keying on the fragment source alone would let two
    // crystals that share a `.frag` but are built against different vertex
    // shaders collide -- and that failure would be a wrong PICTURE rather than a
    // stall, which is the one kind of failure a cache here must not have.
    const std::string cache_key =
        cache != nullptr ? std::string(kVertexShader) + "\n\x1e\n" + crystal.fragment_source
                         : std::string{};

    if (cache != nullptr) {
        if (const std::uint32_t restored = cache->load(cache_key); restored != 0) {
            impl_->program = restored;
            return finish_init(crystal);
        }
    }

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, out_log);
    if (vs == 0) {
        // Our own vertex shader failing is a bug in this file, not in the
        // crystal, and saying so saves an author looking in the wrong place.
        out_log = "internal vertex shader failed to compile:\n" + out_log;
        return false;
    }

    const GLuint fs = compile(GL_FRAGMENT_SHADER, crystal.fragment_source.c_str(), out_log);
    if (fs == 0) {
        glDeleteShader(vs);
        out_log = crystal.shader_path + ":\n" + out_log;
        return false;
    }

    impl_->program = glCreateProgram();
    glAttachShader(impl_->program, vs);
    glAttachShader(impl_->program, fs);

    // BEFORE glLinkProgram, and it has to be: a driver is entitled to discard the
    // binary of a program that was never hinted as retrievable, and then every
    // store() silently keeps nothing while looking exactly like a working cache.
    if (cache != nullptr) {
        cache->prepare(impl_->program);
    }

    glLinkProgram(impl_->program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(impl_->program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        GLint len = 0;
        glGetProgramiv(impl_->program, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> buf(static_cast<std::size_t>(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(impl_->program, len, nullptr, buf.data());
        out_log = crystal.shader_path + " failed to link:\n" + std::string(buf.data());
        glDeleteProgram(impl_->program);
        impl_->program = 0;
        return false;
    }

    // Stored only after the link is known good, so a program that failed can
    // never be handed back to a later run.
    if (cache != nullptr) {
        cache->store(cache_key, impl_->program);
    }

    return finish_init(crystal);
}

// Everything that depends on a linked program and nothing on how it was linked.
//
// SHARED BY BOTH PATHS ON PURPOSE. A uniform resolved after a fresh compile but
// not after a restore -- or resolved differently -- would make the cache change
// the picture rather than the duration, which is the one failure this design
// does not tolerate. One body means the two cannot drift.
bool CrystalFacet::finish_init(const Crystal& crystal)
{
    impl_->u_resolution = glGetUniformLocation(impl_->program, "u_resolution");
    impl_->u_time       = glGetUniformLocation(impl_->program, "u_time");

    // The TrackContext half. Resolved once, like everything else here, and each
    // one may legitimately be -1 -- a crystal that ignores the record's colours
    // is a crystal, not a mistake.
    //
    // The array is looked up by its FIRST ELEMENT. `glGetUniformLocation` on a
    // bare array name works on every driver in practice but is only specified
    // for "name" or "name[0]", and the explicit form is what the spec
    // guarantees.
    impl_->u_palette         = glGetUniformLocation(impl_->program, "u_palette[0]");
    impl_->u_palette_primary = glGetUniformLocation(impl_->program, "u_palette_primary");
    impl_->u_palette_accent  = glGetUniformLocation(impl_->program, "u_palette_accent");
    impl_->u_album_art       = glGetUniformLocation(impl_->program, "u_album_art");
    impl_->u_has_art         = glGetUniformLocation(impl_->program, "u_has_art");
    impl_->u_feedback        = glGetUniformLocation(impl_->program, "u_feedback");
    impl_->u_has_feedback    = glGetUniformLocation(impl_->program, "u_has_feedback");

    // Resolve every manifest binding ONCE. Looking these up per frame would be a
    // string hash per uniform per frame for a value that cannot change while the
    // program is linked.
    impl_->bound.reserve(crystal.uniforms.size());
    impl_->unused = 0;
    for (const UniformBinding& u : crystal.uniforms) {
        const GLint loc = glGetUniformLocation(impl_->program, u.uniform.c_str());
        if (loc < 0) {
            ++impl_->unused;
        }
        Impl::Bound b{loc, u.binding, u.envelope, {}};

        // ALLOCATED HERE AND NEVER AGAIN. The envelope state is the only
        // per-frame storage this facet has, and sizing it at init keeps draw()
        // allocation-free -- which matters because draw() runs once per layer per
        // frame and there are up to four layers.
        //
        // The largest case is `fft_magnitude` or `fft_smoothed` at 1024 floats,
        // 4 KB. A crystal binding every array field is 10.4 KB, against a 66 MB
        // layer, so there is no reason to refuse arrays here the way an archive's
        // single-number opacity has to.
        if (u.envelope.active()) {
            b.state.assign(u.binding->count, 0.0f);
        }
        impl_->bound.push_back(std::move(b));
    }

    // A fresh program has drawn nothing, so the first frame it sees must seed
    // rather than step, whatever index that frame carries.
    impl_->last_index = 0;
    impl_->saw_any    = false;

    glGenVertexArrays(1, &impl_->vao);
    impl_->start = std::chrono::steady_clock::now();
    return true;
}

void CrystalFacet::shutdown()
{
    if (impl_->vao != 0) {
        glDeleteVertexArrays(1, &impl_->vao);
        impl_->vao = 0;
    }
    if (impl_->program != 0) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
    impl_->bound.clear();
    impl_->unused = 0;

    // CLEARED ALONGSIDE `bound`, because init() calls shutdown() first. Left
    // behind, a re-init on a live object would leave a stale index beside a
    // freshly sized state array, and the first frame after it would step from
    // the old track's position instead of seeding.
    impl_->last_index = 0;
    impl_->saw_any    = false;
}

bool CrystalFacet::ready() const { return impl_->program != 0 && impl_->vao != 0; }

std::size_t CrystalFacet::unused_uniforms() const { return impl_->unused; }

void CrystalFacet::set_feedback(TextureHandle texture) { impl_->feedback = texture; }

float CrystalFacet::elapsed() const
{
    const auto now = std::chrono::steady_clock::now();
    const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - impl_->start);
    return static_cast<float>(ms.count()) / 1000.0f;
}

void CrystalFacet::set_elapsed(float seconds)
{
    const auto ms = std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0f));
    impl_->start  = std::chrono::steady_clock::now() - ms;
}

void CrystalFacet::draw(const AudioFrame& frame, const TrackContext& track, int width, int height)
{
    if (!ready() || width <= 0 || height <= 0) {
        return;
    }

    glViewport(0, 0, width, height);
    glUseProgram(impl_->program);

    if (impl_->u_resolution >= 0) {
        glUniform2f(impl_->u_resolution, static_cast<float>(width), static_cast<float>(height));
    }
    if (impl_->u_time >= 0) {
        glUniform1f(impl_->u_time, elapsed());
    }

    // -- what the record looks like ------------------------------------------

    if (impl_->u_palette >= 0) {
        // glm::vec3 is three floats with no padding, and std::array of them is
        // contiguous, so the whole palette uploads in one call. static_assert
        // rather than trust: a padded vec3 would upload garbage into swatches 1
        // through 4 and nothing would say so.
        static_assert(sizeof(glm::vec3) == 3 * sizeof(float),
                      "glm::vec3 must be tightly packed to upload the palette directly");
        static_assert(sizeof(track.palette) == kPaletteSize * 3 * sizeof(float),
                      "the palette array must be contiguous to upload directly");

        glUniform3fv(impl_->u_palette, static_cast<GLsizei>(kPaletteSize),
                     &track.palette[0].x);
    }
    if (impl_->u_palette_primary >= 0) {
        glUniform3fv(impl_->u_palette_primary, 1, &track.palette_primary.x);
    }
    if (impl_->u_palette_accent >= 0) {
        glUniform3fv(impl_->u_palette_accent, 1, &track.palette_accent.x);
    }
    if (impl_->u_has_art >= 0) {
        // GLSL has no bool uniform on the wire; glUniform1i with 0 or 1 is how
        // one is set.
        glUniform1i(impl_->u_has_art, track.has_art && track.album_art_texture != 0 ? 1 : 0);
    }
    if (impl_->u_album_art >= 0) {
        // Unit 0, always, and bound even when there is no art so the sampler
        // never points at whatever a previous crystal left there.
        bind_texture_unit(0, static_cast<GLuint>(track.album_art_texture));
        glUniform1i(impl_->u_album_art, 0);
    }
    if (impl_->u_has_feedback >= 0) {
        glUniform1i(impl_->u_has_feedback, impl_->feedback != 0 ? 1 : 0);
    }
    if (impl_->u_feedback >= 0) {
        // Unit 1. Same rule as the art above: bound even when empty, so the
        // sampler never points at another crystal's leftovers.
        bind_texture_unit(1, static_cast<GLuint>(impl_->feedback));
        glUniform1i(impl_->u_feedback, 1);
    }

    // -- the author's own envelopes ------------------------------------------
    //
    // ADVANCED ONCE PER DRAW AND GATED ON frame_index, not on wall clock. See
    // envelope.hpp: the render thread skips and repeats analysis frames
    // constantly, so anything stepped per drawn frame would run at a rate set by
    // the monitor.
    //
    // Every layer of a stack keeps its own `last_index`, so during a crossfade
    // both facets advance by the same number of hops off the same frame. The
    // incoming one has `saw_any` false and therefore SEEDS on its first draw --
    // without which a `decay = 1.5` uniform would climb out of zero for 1.5 s
    // while the 0.4 s fade completed, and the new crystal would arrive wrong.
    const HopStep step = hops_between(impl_->last_index, frame.frame_index, impl_->saw_any);
    impl_->last_index  = frame.frame_index;
    impl_->saw_any     = true;

    for (Impl::Bound& b : impl_->bound) {
        if (!b.state.empty()) {
            advance_envelope(b.state, b.envelope, frame, *b.binding, step);
        }
        if (b.location < 0) {
            continue;   // compiler removed it; not an error, see the header
        }
        if (!b.state.empty()) {
            // The enveloped value lives here rather than in the frame, so this
            // is the one binding kind that uploads from our own storage.
            glUniform1fv(b.location, static_cast<GLsizei>(b.state.size()), b.state.data());
        } else if (b.binding->kind == BindingKind::kScalar) {
            glUniform1f(b.location, read_scalar(frame, *b.binding));
        } else {
            // The array is contiguous inside AudioFrame, so it uploads directly
            // with no repacking -- which is a consequence of the contract being
            // a plain struct of floats rather than anything clever.
            glUniform1fv(b.location, static_cast<GLsizei>(b.binding->count),
                         read_array(frame, *b.binding));
        }
    }

    glBindVertexArray(impl_->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace holocron

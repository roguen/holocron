// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// The second and last translation unit that includes GL. See debug_facet.hpp.
//
// EVERYTHING IS A RECTANGLE
//
// One shader, one dynamic vertex buffer, one draw call per frame. Every element
// -- spectrum bars, meters, the beat marker, the progress line -- is a coloured
// rectangle pushed into the same buffer.
//
// That is not laziness, it is the cheapest thing that cannot go wrong. This
// facet exists to tell the truth about the analysis stage, so it must not be
// able to fail in ways that look like the analysis failing. A single buffer
// upload and a single draw call has almost no state to get wrong, and if the
// screen is black the cause is unambiguous.
//
// IT USES DSA ON PURPOSE
//
// glCreateBuffers / glNamedBufferData / glVertexArrayAttribFormat rather than
// the bind-to-edit dance. D-012 chose 4.5 core precisely because direct state
// access is in it, and code that asks for 4.5 and then never uses anything past
// 3.3 has not tested the decision at all -- the first thing to actually need
// DSA would be the thing that discovers the context request was wrong.

#include <holocron/debug_facet.hpp>

#include <holocron/audio_frame.hpp>

#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace holocron {

namespace {

// Position in pixels, colour as straight RGBA. The vertex shader turns pixels
// into clip space, so every layout calculation below reads in pixels with the
// origin at the top left, and none of it has to think in NDC.
struct Vertex {
    float x, y;
    float r, g, b, a;
};

struct Colour {
    float r, g, b, a;
};

// A deliberately flat, unglamorous palette. This is an instrument panel, not a
// crystal. Nothing in M2 should inherit it.
constexpr Colour kBackground = {0.055f, 0.059f, 0.071f, 1.00f};
constexpr Colour kPanel      = {0.102f, 0.110f, 0.129f, 1.00f};
constexpr Colour kGrid       = {0.180f, 0.192f, 0.220f, 1.00f};
constexpr Colour kBar        = {0.298f, 0.686f, 0.937f, 1.00f};
constexpr Colour kBarEnv     = {0.702f, 0.878f, 1.000f, 0.60f};
constexpr Colour kBass       = {0.937f, 0.365f, 0.365f, 1.00f};
constexpr Colour kMid        = {0.514f, 0.831f, 0.443f, 1.00f};
constexpr Colour kTreble     = {0.976f, 0.816f, 0.376f, 1.00f};
constexpr Colour kRms        = {0.855f, 0.855f, 0.878f, 1.00f};
constexpr Colour kPeak       = {0.976f, 0.545f, 0.290f, 1.00f};
constexpr Colour kBeat       = {0.976f, 0.976f, 0.976f, 1.00f};
constexpr Colour kOnset      = {1.000f, 0.400f, 0.700f, 1.00f};
constexpr Colour kMarker     = {0.600f, 0.400f, 0.900f, 1.00f};
constexpr Colour kStopped    = {0.400f, 0.420f, 0.470f, 1.00f};

const char* kVertexShader = R"glsl(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_colour;
uniform vec2 u_viewport;
out vec4 v_colour;
void main()
{
    // Pixels to clip space with the origin at the top left, so the layout code
    // reads like a screen rather than like graph paper.
    vec2 ndc = vec2((a_pos.x / u_viewport.x) * 2.0 - 1.0,
                    1.0 - (a_pos.y / u_viewport.y) * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_colour = a_colour;
}
)glsl";

const char* kFragmentShader = R"glsl(#version 300 es
precision highp float;
in vec4 v_colour;
out vec4 frag_colour;
void main()
{
    frag_colour = v_colour;
}
)glsl";

GLuint compile(GLenum stage, const char* src, const char* label)
{
    const GLuint sh = glCreateShader(stage);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok == 0) {
        char log[1024] = {};
        glGetShaderInfoLog(sh, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
        std::fprintf(stderr, "[facet] %s shader failed to compile:\n%s\n", label, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

float clamp01(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

// loudness_short is LUFS and lives roughly in [-70, 0]. Mapped here for display
// only. The field itself is deliberately left un-normalized (D-007) so it stays
// comparable across tracks, and that is exactly why the mapping belongs in the
// facet rather than in the analysis stage.
float lufs_to_unit(float lufs)
{
    constexpr float kFloor = -70.0f;
    return clamp01((lufs - kFloor) / (0.0f - kFloor));
}

}  // namespace

// ---------------------------------------------------------------------------

struct DebugFacet::Impl {
    GLuint program = 0;
    GLuint vao     = 0;
    GLuint vbo     = 0;
    GLint  u_viewport = -1;

    std::vector<Vertex> verts;
    std::size_t         capacity_bytes = 0;

    void rect(float x, float y, float w, float h, const Colour& c)
    {
        if (w <= 0.0f || h <= 0.0f) {
            return;
        }
        const Vertex a{x,     y,     c.r, c.g, c.b, c.a};
        const Vertex b{x + w, y,     c.r, c.g, c.b, c.a};
        const Vertex d{x + w, y + h, c.r, c.g, c.b, c.a};
        const Vertex e{x,     y + h, c.r, c.g, c.b, c.a};

        verts.push_back(a);
        verts.push_back(b);
        verts.push_back(d);
        verts.push_back(a);
        verts.push_back(d);
        verts.push_back(e);
    }

    // A meter that fills left to right within a track, with the track drawn
    // behind it so an empty meter is still visibly a meter rather than absent.
    void meter(float x, float y, float w, float h, float value01, const Colour& c)
    {
        rect(x, y, w, h, kPanel);
        rect(x, y, w * clamp01(value01), h, c);
    }

    // A thin vertical line at a fractional position within a track. Used for
    // anything that is a POSITION rather than a QUANTITY -- beat phase,
    // spectral centroid -- because drawing those as fills would imply an
    // amount, which is the wrong reading.
    void marker(float x, float y, float w, float h, float pos01, const Colour& c)
    {
        constexpr float kWidth = 3.0f;
        const float px = x + (w - kWidth) * clamp01(pos01);
        rect(px, y, kWidth, h, c);
    }
};

DebugFacet::DebugFacet() : impl_(std::make_unique<Impl>()) {}

DebugFacet::~DebugFacet()
{
    shutdown();
}

bool DebugFacet::init()
{
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, "vertex");
    if (vs == 0) {
        return false;
    }
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, "fragment");
    if (fs == 0) {
        glDeleteShader(vs);
        return false;
    }

    impl_->program = glCreateProgram();
    glAttachShader(impl_->program, vs);
    glAttachShader(impl_->program, fs);
    glLinkProgram(impl_->program);

    // Attached shaders are reference-counted by the program, so they can be
    // deleted immediately after linking whether or not it succeeded.
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint linked = 0;
    glGetProgramiv(impl_->program, GL_LINK_STATUS, &linked);
    if (linked == 0) {
        char log[1024] = {};
        glGetProgramInfoLog(impl_->program, static_cast<GLsizei>(sizeof(log) - 1), nullptr, log);
        std::fprintf(stderr, "[facet] program failed to link:\n%s\n", log);
        glDeleteProgram(impl_->program);
        impl_->program = 0;
        return false;
    }

    impl_->u_viewport = glGetUniformLocation(impl_->program, "u_viewport");

    glCreateBuffers(1, &impl_->vbo);
    glCreateVertexArrays(1, &impl_->vao);

    glVertexArrayVertexBuffer(impl_->vao, 0, impl_->vbo, 0, static_cast<GLsizei>(sizeof(Vertex)));

    glEnableVertexArrayAttrib(impl_->vao, 0);
    glVertexArrayAttribFormat(impl_->vao, 0, 2, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(impl_->vao, 0, 0);

    glEnableVertexArrayAttrib(impl_->vao, 1);
    glVertexArrayAttribFormat(impl_->vao, 1, 4, GL_FLOAT, GL_FALSE,
                              static_cast<GLuint>(sizeof(float) * 2));
    glVertexArrayAttribBinding(impl_->vao, 1, 0);

    return true;
}

void DebugFacet::shutdown()
{
    if (impl_->vbo != 0) {
        glDeleteBuffers(1, &impl_->vbo);
        impl_->vbo = 0;
    }
    if (impl_->vao != 0) {
        glDeleteVertexArrays(1, &impl_->vao);
        impl_->vao = 0;
    }
    if (impl_->program != 0) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
    impl_->capacity_bytes = 0;
}

bool DebugFacet::ready() const
{
    return impl_->program != 0 && impl_->vao != 0 && impl_->vbo != 0;
}

void DebugFacet::draw(const AudioFrame& frame, int width, int height, bool playing)
{
    glViewport(0, 0, width, height);
    glClearColor(kBackground.r, kBackground.g, kBackground.b, kBackground.a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (!ready() || width <= 0 || height <= 0) {
        return;
    }

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    Impl& g = *impl_;
    g.verts.clear();

    constexpr float kPad = 16.0f;

    // -- onset flash ---------------------------------------------------------
    //
    // Drawn FIRST, as a full-bleed wash behind everything, and driven by
    // onset_strength rather than the bare boolean so a weak onset looks weak.
    // The boolean is true for exactly one frame (D-005), which at 93.75 Hz is
    // ~10 ms -- long enough to see as a flash and too short to read as a level.
    if (frame.onset) {
        const float strength = clamp01(frame.onset_strength);
        g.rect(0.0f, 0.0f, w, h,
               Colour{kOnset.r, kOnset.g, kOnset.b, 0.06f + 0.14f * strength});
    }

    // -- spectrum ------------------------------------------------------------
    //
    // band_norm as the solid bar, band_env as a lighter ghost behind it. Seeing
    // both at once is the point: if the auto-gained bar is pinned at full scale
    // while the enveloped one is halfway up, the auto-gain is the thing that is
    // wrong, and no single trace would show that.

    // Laid out from the BOTTOM UP, so the spectrum absorbs whatever vertical
    // space is left over. A first pass gave the spectrum a fixed 52% of the
    // window and stacked fixed-height rows under it, which left the bottom
    // third of a 720p window empty -- fine at one size and wrong at every
    // other. Rows are sized relative to the window for the same reason.
    const float row = std::max(18.0f, h * 0.030f);
    const float gap = std::max(6.0f, h * 0.010f);

    const float t_h = std::max(6.0f, h * 0.011f);

    float cursor = h - kPad;

    const float t_y = cursor - t_h;
    cursor = t_y - kPad;

    const float rhythm_h = row + gap + row * 0.6f;
    const float rhythm_y = cursor - rhythm_h;
    cursor = rhythm_y - kPad;

    const float meters_h = row * 3.0f + gap * 2.0f;
    const float meters_y = cursor - meters_h;
    cursor = meters_y - kPad;

    const float spec_x = kPad;
    const float spec_y = kPad;
    const float spec_w = w - kPad * 2.0f;
    const float spec_h = std::max(1.0f, cursor - spec_y);

    g.rect(spec_x, spec_y, spec_w, spec_h, kPanel);

    // Horizontal quarters, so a bar's height can be read without a number.
    for (int i = 1; i < 4; ++i) {
        const float gy = spec_y + spec_h * (static_cast<float>(i) / 4.0f);
        g.rect(spec_x, gy, spec_w, 1.0f, kGrid);
    }

    constexpr int   kBands   = AudioFrame::kBands;
    const float     slot     = spec_w / static_cast<float>(kBands);
    const float     bar_w    = std::max(1.0f, slot - 3.0f);

    for (int i = 0; i < kBands; ++i) {
        const float bx = spec_x + slot * static_cast<float>(i);

        const float env = clamp01(frame.band_env[static_cast<std::size_t>(i)]);
        const float nrm = clamp01(frame.band_norm[static_cast<std::size_t>(i)]);

        const float nrm_h = spec_h * nrm;
        g.rect(bx, spec_y + spec_h - nrm_h, bar_w, nrm_h, kBar);

        // band_env as a TICK ON TOP of the bar, not a ghost bar behind it.
        // Behind was the first attempt and it was invisible: band_norm is the
        // auto-gained version of the same signal and is almost always the
        // taller of the two, so the ghost sat entirely inside the solid bar.
        // A tick is legible whichever value is larger, which is the whole
        // point -- an auto-gained bar pinned at full scale while the enveloped
        // tick sits halfway up is a broken auto-gain, visible at a glance.
        const float env_y = spec_y + spec_h - (spec_h * env);
        g.rect(bx, env_y - 1.0f, bar_w, 2.0f, kBarEnv);
    }

    // Spectral centroid and rolloff sit ON the spectrum, because they are
    // statements ABOUT it and putting them anywhere else makes them impossible
    // to relate to the bars they describe. Both are already 0..1 log-mapped.
    g.marker(spec_x, spec_y, spec_w, spec_h, frame.spectral_centroid, kMarker);
    g.marker(spec_x, spec_y, spec_w, spec_h, frame.spectral_rolloff, kGrid);

    // -- meters --------------------------------------------------------------

    const float m_x = kPad;
    const float m_w = w - kPad * 2.0f;
    const float m_y = meters_y;

    const float kRow = row;
    const float kGap = gap;

    const float half = (m_w - kGap) * 0.5f;

    // Left column: the three coarse aggregates, normalized. These are what nine
    // out of ten crystals will actually read.
    g.meter(m_x, m_y, half, kRow, frame.bass_norm, kBass);
    g.meter(m_x, m_y + kRow + kGap, half, kRow, frame.mid_norm, kMid);
    g.meter(m_x, m_y + (kRow + kGap) * 2.0f, half, kRow, frame.treble_norm, kTreble);

    // Right column: level and loudness. rms and peak share a row -- peak drawn
    // as a marker over the rms fill -- because the interesting thing is the gap
    // between them, and stacking them makes that gap hard to see.
    const float r_x = m_x + half + kGap;
    g.meter(r_x, m_y, half, kRow, frame.rms, kRms);
    g.marker(r_x, m_y, half, kRow, frame.peak, kPeak);

    g.meter(r_x, m_y + kRow + kGap, half, kRow, lufs_to_unit(frame.loudness_short), kRms);

    // stereo_correlation is -1..1, so it gets a centre-anchored bar rather than
    // a fill: the meaningful reading is which side of zero it is on, and a
    // left-filling meter would put "perfectly mono" and "out of phase" at
    // opposite ends of the same sweep with no visible centre.
    {
        const float sy  = m_y + (kRow + kGap) * 2.0f;
        g.rect(r_x, sy, half, kRow, kPanel);
        const float mid_x = r_x + half * 0.5f;
        g.rect(mid_x - 1.0f, sy, 2.0f, kRow, kGrid);
        const float corr = std::clamp(frame.stereo_correlation, -1.0f, 1.0f);
        const float ext  = (half * 0.5f) * std::fabs(corr);
        if (corr >= 0.0f) {
            g.rect(mid_x, sy, ext, kRow, kMid);
        } else {
            g.rect(mid_x - ext, sy, ext, kRow, kBass);
        }
    }

    // -- rhythm --------------------------------------------------------------
    //
    // beat_phase and bar_phase are POSITIONS in a cycle, so they are markers
    // sweeping a track. A marker that jumps backwards is a phase reset and a
    // marker that stalls is a lost lock -- both are obvious to the eye and
    // neither is obvious in a column of numbers.

    g.rect(m_x, rhythm_y, m_w, kRow, kPanel);
    g.marker(m_x, rhythm_y, m_w, kRow, frame.beat_phase, kBeat);
    g.marker(m_x, rhythm_y, m_w, kRow, frame.bar_phase, kMarker);

    // bpm_confidence, not bpm. The BPM value cannot be drawn without text, and
    // the confidence is the part worth watching anyway: #46 is precisely a case
    // of a confident-looking number being wrong, and a confidence bar sitting
    // at full scale while the beat marker drifts is that bug, visible.
    g.meter(m_x, rhythm_y + kRow + kGap, m_w, kRow * 0.6f,
            frame.bpm_confidence, playing ? kBeat : kStopped);

    // -- transport -----------------------------------------------------------

    g.rect(m_x, t_y, m_w, t_h, kPanel);
    if (frame.track_duration > 0.0) {
        const float pos =
            clamp01(static_cast<float>(frame.track_position / frame.track_duration));
        g.rect(m_x, t_y, m_w * pos, t_h, playing ? kBar : kStopped);
    }

    // -- upload and draw -----------------------------------------------------

    if (g.verts.empty()) {
        return;
    }

    const std::size_t bytes = g.verts.size() * sizeof(Vertex);

    // Orphan-and-refill only when the buffer has to grow. glNamedBufferSubData
    // into an existing allocation avoids a reallocation every frame, and the
    // vertex count here is small and bounded so the buffer settles almost
    // immediately.
    if (bytes > g.capacity_bytes) {
        glNamedBufferData(g.vbo, static_cast<GLsizeiptr>(bytes), g.verts.data(), GL_DYNAMIC_DRAW);
        g.capacity_bytes = bytes;
    } else {
        glNamedBufferSubData(g.vbo, 0, static_cast<GLsizeiptr>(bytes), g.verts.data());
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(g.program);
    if (g.u_viewport >= 0) {
        glUniform2f(g.u_viewport, w, h);
    }
    glBindVertexArray(g.vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(g.verts.size()));
    glBindVertexArray(0);
    glUseProgram(0);

    glDisable(GL_BLEND);
}

}  // namespace holocron

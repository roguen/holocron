// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See final_pass.hpp.

#include <holocron/final_pass.hpp>

#include <holocron/render_target.hpp>

#include "gl_api.hpp"

#include "gl_bind.hpp"

#include <string>
#include <vector>

namespace holocron {
namespace {

const char* kVertexShader = R"glsl(#version 300 es
out vec2 v_uv;
void main()
{
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// THE BRIGHT PASS AND THE BLUR, IN ONE SHADER WITH A MODE.
//
// Three passes share a full-screen triangle and differ in three lines, so three
// programs would be three places to keep a sampler binding in step. `u_step` is
// the blur direction in texels, zero for the extract.
//
// SEPARABLE, because a 2D Gaussian of radius r costs r^2 taps and two 1D passes
// cost 2r for the same result. At quarter resolution with a 9-tap kernel that is
// 18 taps a pixel instead of 81.
const char* kBloomShader = R"glsl(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_source;
uniform vec2      u_step;        // texel offset per tap; (0,0) means extract
uniform float     u_threshold;

void main()
{
    if (u_step == vec2(0.0)) {
        // EXTRACT. Subtracting the threshold rather than testing against it: a
        // hard cutoff makes a visible edge wherever the picture crosses it, and
        // that edge crawls as the picture moves. Subtracting fades the bloom in
        // from nothing, which is what a lens does.
        vec3 c = texture(u_source, v_uv).rgb;
        frag_colour = vec4(max(c - u_threshold, 0.0), 1.0);
        return;
    }

    // A 9-tap Gaussian, weights from Pascal's triangle at row 8, normalised.
    const float w[5] = float[](0.2270270270, 0.1945945946, 0.1216216216,
                               0.0540540541, 0.0162162162);

    vec3 sum = texture(u_source, v_uv).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 o = u_step * float(i);
        sum += texture(u_source, v_uv + o).rgb * w[i];
        sum += texture(u_source, v_uv - o).rgb * w[i];
    }
    frag_colour = vec4(sum, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_picture;
uniform sampler2D u_bloom;
uniform float     u_bloom_amount;
uniform vec2      u_resolution;
uniform float     u_time;
uniform float     u_grain;
uniform float     u_vignette;
uniform float     u_safe_area;

// A cheap hash, and cheap is the point: this runs once per pixel per frame and
// it only has to be uncorrelated enough that the eye reads it as noise.
float hash(vec2 p)
{
    p = fract(p * vec2(443.897, 441.423));
    p += dot(p, p + 19.19);
    return fract((p.x + p.y) * p.x);
}

void main()
{
    vec3 c = texture(u_picture, v_uv).rgb;

    // -- bloom ------------------------------------------------------------
    //
    // ADDED, NOT MIXED. The bloom texture holds only the OVERSHOOT above the
    // threshold, so adding it back is putting light where light already was;
    // mixing towards it would dim everything that was not bright, which is the
    // opposite of what a lens does.
    //
    // Upsampled by the sampler, which is bilinear at quarter resolution and
    // therefore already most of a blur. That is why 9 taps is enough here and
    // would not be at full resolution.
    if (u_bloom_amount > 0.0) {
        c += texture(u_bloom, v_uv).rgb * u_bloom_amount;
    }

    // -- vignette ---------------------------------------------------------
    //
    // Aspect-corrected, so it is a circle on the screen rather than an ellipse
    // stretched with the window. The crystals' own vignettes do the same, which
    // is what makes a value here comparable with one of theirs.
    if (u_vignette > 0.0) {
        vec2  d   = (v_uv - 0.5) * vec2(u_resolution.x / max(u_resolution.y, 1.0), 1.0);
        float rad = length(d);
        c *= 1.0 - u_vignette * rad * rad;
    }

    // -- grain ------------------------------------------------------------
    //
    // SIGNED AND ADDED, not multiplied. Multiplied noise scales with the picture
    // and therefore vanishes in exactly the dark gradients it exists to dither;
    // adding a signed value of about one 8-bit step breaks the band wherever the
    // band is.
    //
    // u_time enters the hash so the pattern moves. A static one is worse than no
    // grain at all -- the eye finds a fixed pattern within a second and reads it
    // as a dirty lens rather than as film.
    if (u_grain > 0.0) {
        float n = hash(gl_FragCoord.xy + fract(u_time) * 1731.0) - 0.5;
        c += n * u_grain * (1.0 / 255.0);
    }

    // -- safe area --------------------------------------------------------
    //
    // A HARD BLACK EDGE, not a fade. This is a mask for a projector that loses
    // its edges, so the boundary needs to be somewhere the eye can find while
    // measuring it against a test pattern; a soft one cannot be lined up.
    if (u_safe_area > 0.0) {
        vec2 e = step(vec2(u_safe_area), v_uv) * step(v_uv, vec2(1.0 - u_safe_area));
        c *= e.x * e.y;
    }

    frag_colour = vec4(c, 1.0);
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

}  // namespace

struct FinalPass::Impl {
    GLuint program = 0;
    GLuint vao     = 0;

    // The bloom chain. `bright` holds the extract, `blur` is the other half of a
    // ping-pong: horizontal into blur, vertical back into bright, so the result
    // always ends up in `bright` and the combine pass has one texture to name.
    GLuint bloom_program = 0;
    GLint  b_source      = -1;
    GLint  b_step        = -1;
    GLint  b_threshold   = -1;

    RenderTarget bright;
    RenderTarget blur;
    int          bloom_w = 0;
    int          bloom_h = 0;

    GLint u_bloom        = -1;
    GLint u_bloom_amount = -1;

    GLint u_picture    = -1;
    GLint u_resolution = -1;
    GLint u_time       = -1;
    GLint u_grain      = -1;
    GLint u_vignette   = -1;
    GLint u_safe_area  = -1;
};

FinalPass::FinalPass() : impl_(std::make_unique<Impl>()) {}
FinalPass::~FinalPass() { shutdown(); }

bool FinalPass::any(const FinalPassSettings& s)
{
    return s.grain > 0.0f || s.vignette > 0.0f || s.safe_area > 0.0f || s.bloom > 0.0f;
}

bool FinalPass::resize(int width, int height)
{
    // QUARTER RESOLUTION EACH WAY, so a sixteenth of the pixels. Bloom is a wide
    // soft thing and the detail is thrown away by the blur anyway; doing it at
    // full resolution costs sixteen times as much to produce a result nobody can
    // tell apart. It is also what makes a 9-tap kernel reach far enough to look
    // like a glow rather than a smudge.
    const int w = width / 4 > 0 ? width / 4 : 1;
    const int h = height / 4 > 0 ? height / 4 : 1;

    if (impl_->bloom_w == w && impl_->bloom_h == h && impl_->bright.ready()) {
        return true;
    }
    if (!impl_->bright.resize(w, h) || !impl_->blur.resize(w, h)) {
        return false;
    }
    impl_->bloom_w = w;
    impl_->bloom_h = h;
    return true;
}

bool FinalPass::init(std::string& out_log)
{
    shutdown();
    out_log.clear();

    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, out_log);
    if (vs == 0) {
        out_log = "final pass vertex shader:\n" + out_log;
        return false;
    }
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, out_log);
    if (fs == 0) {
        glDeleteShader(vs);
        out_log = "final pass fragment shader:\n" + out_log;
        return false;
    }

    impl_->program = glCreateProgram();
    glAttachShader(impl_->program, vs);
    glAttachShader(impl_->program, fs);
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
        out_log = "final pass failed to link:\n" + std::string(buf.data());
        glDeleteProgram(impl_->program);
        impl_->program = 0;
        return false;
    }

    // The bloom chain's own program. Built here rather than lazily: a shader
    // that only compiles the first time somebody turns bloom on is a shader
    // whose compile error arrives mid-track.
    {
        std::string log;
        const GLuint bvs = compile(GL_VERTEX_SHADER, kVertexShader, log);
        const GLuint bfs = compile(GL_FRAGMENT_SHADER, kBloomShader, log);
        if (bvs == 0 || bfs == 0) {
            out_log = "bloom shader:\n" + log;
            return false;
        }
        impl_->bloom_program = glCreateProgram();
        glAttachShader(impl_->bloom_program, bvs);
        glAttachShader(impl_->bloom_program, bfs);
        glLinkProgram(impl_->bloom_program);
        glDeleteShader(bvs);
        glDeleteShader(bfs);

        GLint blinked = 0;
        glGetProgramiv(impl_->bloom_program, GL_LINK_STATUS, &blinked);
        if (blinked == 0) {
            out_log = "bloom shader failed to link";
            return false;
        }
        impl_->b_source    = glGetUniformLocation(impl_->bloom_program, "u_source");
        impl_->b_step      = glGetUniformLocation(impl_->bloom_program, "u_step");
        impl_->b_threshold = glGetUniformLocation(impl_->bloom_program, "u_threshold");
    }

    impl_->u_bloom        = glGetUniformLocation(impl_->program, "u_bloom");
    impl_->u_bloom_amount = glGetUniformLocation(impl_->program, "u_bloom_amount");
    impl_->u_picture    = glGetUniformLocation(impl_->program, "u_picture");
    impl_->u_resolution = glGetUniformLocation(impl_->program, "u_resolution");
    impl_->u_time       = glGetUniformLocation(impl_->program, "u_time");
    impl_->u_grain      = glGetUniformLocation(impl_->program, "u_grain");
    impl_->u_vignette   = glGetUniformLocation(impl_->program, "u_vignette");
    impl_->u_safe_area  = glGetUniformLocation(impl_->program, "u_safe_area");

    glGenVertexArrays(1, &impl_->vao);
    return true;
}

void FinalPass::shutdown()
{
    impl_->bright.shutdown();
    impl_->blur.shutdown();
    impl_->bloom_w = 0;
    impl_->bloom_h = 0;
    if (impl_->bloom_program != 0) {
        glDeleteProgram(impl_->bloom_program);
        impl_->bloom_program = 0;
    }
    if (impl_->vao != 0) {
        glDeleteVertexArrays(1, &impl_->vao);
        impl_->vao = 0;
    }
    if (impl_->program != 0) {
        glDeleteProgram(impl_->program);
        impl_->program = 0;
    }
}

bool FinalPass::ready() const { return impl_->program != 0 && impl_->vao != 0; }

void FinalPass::draw(TextureHandle picture, const FinalPassSettings& settings, float seconds,
                     int screen_width, int screen_height)
{
    if (!ready() || picture == 0) {
        return;
    }

    glDisable(GL_BLEND);
    glBindVertexArray(impl_->vao);

    // -- the bloom chain ------------------------------------------------------
    //
    // extract at quarter res -> blur horizontally -> blur vertically back into
    // the extract target, so the result is always in `bright` and the combine
    // pass below has one texture to name whatever happened here.
    float bloom_amount = 0.0f;
    if (settings.bloom > 0.0f && impl_->bright.ready() && impl_->blur.ready()) {
        bloom_amount = settings.bloom;

        glUseProgram(impl_->bloom_program);
        glUniform1i(impl_->b_source, 0);
        glUniform1f(impl_->b_threshold, settings.bloom_threshold);

        const float tx = 1.0f / static_cast<float>(impl_->bloom_w);
        const float ty = 1.0f / static_cast<float>(impl_->bloom_h);

        impl_->bright.bind();
        glUniform2f(impl_->b_step, 0.0f, 0.0f);
        bind_texture_unit(0, static_cast<GLuint>(picture));
        glDrawArrays(GL_TRIANGLES, 0, 3);

        impl_->blur.bind();
        glUniform2f(impl_->b_step, tx, 0.0f);
        bind_texture_unit(0, static_cast<GLuint>(impl_->bright.texture()));
        glDrawArrays(GL_TRIANGLES, 0, 3);

        impl_->bright.bind();
        glUniform2f(impl_->b_step, 0.0f, ty);
        bind_texture_unit(0, static_cast<GLuint>(impl_->blur.texture()));
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Back to the window, which the caller left bound before all of this.
        RenderTarget::bind_default(screen_width, screen_height);
    }

    glUseProgram(impl_->program);
    glUniform1f(impl_->u_bloom_amount, bloom_amount);
    bind_texture_unit(1, static_cast<GLuint>(impl_->bright.texture()));
    glUniform1i(impl_->u_bloom, 1);

    bind_texture_unit(0, static_cast<GLuint>(picture));
    glUniform1i(impl_->u_picture, 0);
    glUniform2f(impl_->u_resolution, static_cast<float>(screen_width),
                static_cast<float>(screen_height));
    glUniform1f(impl_->u_time, seconds);
    glUniform1f(impl_->u_grain, settings.grain);
    glUniform1f(impl_->u_vignette, settings.vignette);
    glUniform1f(impl_->u_safe_area, settings.safe_area);

    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace holocron

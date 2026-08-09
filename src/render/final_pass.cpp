// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See final_pass.hpp.

#include <holocron/final_pass.hpp>

#include <glad/glad.h>

#include <string>
#include <vector>

namespace holocron {
namespace {

const char* kVertexShader = R"glsl(
#version 450 core
out vec2 v_uv;
void main()
{
    v_uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

const char* kFragmentShader = R"glsl(
#version 450 core
in  vec2 v_uv;
out vec4 frag_colour;

uniform sampler2D u_picture;
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
    return s.grain > 0.0f || s.vignette > 0.0f || s.safe_area > 0.0f;
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

    impl_->u_picture    = glGetUniformLocation(impl_->program, "u_picture");
    impl_->u_resolution = glGetUniformLocation(impl_->program, "u_resolution");
    impl_->u_time       = glGetUniformLocation(impl_->program, "u_time");
    impl_->u_grain      = glGetUniformLocation(impl_->program, "u_grain");
    impl_->u_vignette   = glGetUniformLocation(impl_->program, "u_vignette");
    impl_->u_safe_area  = glGetUniformLocation(impl_->program, "u_safe_area");

    glCreateVertexArrays(1, &impl_->vao);
    return true;
}

void FinalPass::shutdown()
{
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
    glUseProgram(impl_->program);
    glBindVertexArray(impl_->vao);

    glBindTextureUnit(0, static_cast<GLuint>(picture));
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

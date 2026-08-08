// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See crystal_facet.hpp.

#include <holocron/crystal_facet.hpp>

#include <holocron/audio_frame.hpp>
#include <holocron/crystal.hpp>
#include <holocron/frame_binding.hpp>
#include <holocron/track_context.hpp>

#include <glad/glad.h>

#include <chrono>
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
const char* kVertexShader = R"glsl(
#version 450 core
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

    // One per manifest entry, resolved once. -1 means the compiler removed it.
    struct Bound {
        GLint          location;
        const Binding* binding;
    };
    std::vector<Bound> bound;
    std::size_t        unused = 0;

    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

CrystalFacet::CrystalFacet() : impl_(std::make_unique<Impl>()) {}
CrystalFacet::~CrystalFacet() { shutdown(); }

bool CrystalFacet::init(const Crystal& crystal, std::string& out_log)
{
    shutdown();
    out_log.clear();

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
        impl_->bound.push_back(Impl::Bound{loc, u.binding});
    }

    glCreateVertexArrays(1, &impl_->vao);
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
}

bool CrystalFacet::ready() const { return impl_->program != 0 && impl_->vao != 0; }

std::size_t CrystalFacet::unused_uniforms() const { return impl_->unused; }

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
        glBindTextureUnit(0, static_cast<GLuint>(track.album_art_texture));
        glUniform1i(impl_->u_album_art, 0);
    }

    for (const Impl::Bound& b : impl_->bound) {
        if (b.location < 0) {
            continue;   // compiler removed it; not an error, see the header
        }
        if (b.binding->kind == BindingKind::kScalar) {
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

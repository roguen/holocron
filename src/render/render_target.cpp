// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See render_target.hpp.

#include <holocron/render_target.hpp>

#include <glad/glad.h>

namespace holocron {

struct RenderTarget::Impl {
    GLuint fbo     = 0;
    GLuint colour  = 0;
    int    width   = 0;
    int    height  = 0;
};

RenderTarget::RenderTarget() : impl_(std::make_unique<Impl>()) {}
RenderTarget::~RenderTarget() { shutdown(); }

// Moved-from targets must own nothing, or the first destructor to run deletes GL
// objects the other one still points at. A default-constructed Impl in the
// source is cheaper than nulling three fields and cannot forget one.
RenderTarget::RenderTarget(RenderTarget&& other) noexcept : impl_(std::move(other.impl_))
{
    other.impl_ = std::make_unique<Impl>();
}

RenderTarget& RenderTarget::operator=(RenderTarget&& other) noexcept
{
    if (this != &other) {
        shutdown();
        impl_       = std::move(other.impl_);
        other.impl_ = std::make_unique<Impl>();
    }
    return *this;
}

bool RenderTarget::resize(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (impl_->fbo != 0 && impl_->width == width && impl_->height == height) {
        return true;   // already this shape -- see the header on why this matters
    }

    shutdown();

    // GL_RGBA16F: crystals exceed 1.0 before their vignette and an 8-bit target
    // would clamp that at every intermediate stage. See the header.
    glCreateTextures(GL_TEXTURE_2D, 1, &impl_->colour);
    glTextureStorage2D(impl_->colour, 1, GL_RGBA16F, width, height);

    // LINEAR so a layer drawn at a fraction of the screen -- which decision 2 of
    // the M3 issue leaves open -- resolves without visible blocking. CLAMP_TO_EDGE
    // because a compositing pass that samples outside the picture is a bug, and
    // repeating the far edge into it would disguise one.
    glTextureParameteri(impl_->colour, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(impl_->colour, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(impl_->colour, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(impl_->colour, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &impl_->fbo);
    glNamedFramebufferTexture(impl_->fbo, GL_COLOR_ATTACHMENT0, impl_->colour, 0);

    // Named explicitly rather than left to default. A framebuffer with one
    // colour attachment defaults correctly, but the default is a property of the
    // object's initial state and a second attachment added later would silently
    // not be written to.
    const GLenum draw_buffer = GL_COLOR_ATTACHMENT0;
    glNamedFramebufferDrawBuffers(impl_->fbo, 1, &draw_buffer);

    if (glCheckNamedFramebufferStatus(impl_->fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        shutdown();
        return false;
    }

    impl_->width  = width;
    impl_->height = height;
    return true;
}

void RenderTarget::shutdown()
{
    if (impl_->fbo != 0) {
        glDeleteFramebuffers(1, &impl_->fbo);
        impl_->fbo = 0;
    }
    if (impl_->colour != 0) {
        glDeleteTextures(1, &impl_->colour);
        impl_->colour = 0;
    }
    impl_->width  = 0;
    impl_->height = 0;
}

bool RenderTarget::ready() const { return impl_->fbo != 0 && impl_->colour != 0; }

void RenderTarget::bind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, impl_->fbo);
    glViewport(0, 0, impl_->width, impl_->height);
}

void RenderTarget::bind_default(int width, int height)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (width > 0 && height > 0) {
        glViewport(0, 0, width, height);
    }
}

TextureHandle RenderTarget::texture() const { return static_cast<TextureHandle>(impl_->colour); }

int RenderTarget::width() const { return impl_->width; }
int RenderTarget::height() const { return impl_->height; }

}  // namespace holocron

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// See art_texture.hpp.

#include <holocron/art_texture.hpp>

#include <holocron/palette.hpp>

#include "gl_api.hpp"

namespace holocron {

TextureHandle upload_art(const ImageRgba8& image)
{
    if (image.empty()) {
        return 0;
    }

    GLuint texture = 0;

    // BIND-THEN-MODIFY, and this call site is the one that lost something real to
    // the M8 port.
    //
    // Under direct state access uploading a sleeve could not disturb whatever the
    // crystal being drawn had bound, and that was worth having: art arrives on a
    // worker and is uploaded on the render thread at whatever moment it finishes,
    // which is to say in the middle of ordinary drawing. ES has no DSA at any
    // version (D-047), so the upload now binds -- on unit 0, and it unbinds when
    // it is finished, which is the convention every caller in src/render follows.
    // Nothing here samples a texture it has not just bound, so the guarantee is
    // preserved by discipline instead of by the API.
    glGenTextures(1, &texture);
    if (texture == 0) {
        return 0;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    // How many mip levels a full chain needs. Computed rather than passed as 1,
    // because glGenerateTextureMipmap writes into levels that must already be
    // allocated and silently does nothing useful otherwise.
    int levels = 1;
    for (int extent = image.width > image.height ? image.width : image.height; extent > 1;
         extent >>= 1) {
        ++levels;
    }

    // SRGB8_ALPHA8, and this is the whole reason the palette agrees with the
    // sleeve.
    //
    // Album art is sRGB-encoded. Declaring the texture as sRGB makes the sampler
    // linearise on read, so a shader gets linear values -- the same space
    // extract_palette() returns. Uploading as plain RGBA8 instead would leave
    // the crystal sampling sRGB numbers while its palette uniforms were linear,
    // and the two would disagree about the colour of the same pixel. No error,
    // no warning, and nothing to see except art that never quite matches the
    // colours drawn beside it.
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, image.width, image.height);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.width, image.height, GL_RGBA, GL_UNSIGNED_BYTE,
                    image.pixels.data());
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // CLAMP, NOT REPEAT. A crystal sampling outside 0..1 -- which anything doing
    // a warp or a zoom will -- would otherwise tile the sleeve, and a wall of
    // repeated album covers is not a thing anyone asked for.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    return static_cast<TextureHandle>(texture);
}

void release_art(TextureHandle& texture)
{
    if (texture == 0) {
        return;
    }
    const GLuint name = static_cast<GLuint>(texture);
    glDeleteTextures(1, &name);
    texture = 0;
}

}  // namespace holocron

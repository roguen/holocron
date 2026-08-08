// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/art_texture.hpp
//
// Getting a decoded sleeve onto the GPU.
//
// TWO FUNCTIONS, AND THEY EXIST TO KEEP GL WHERE IT BELONGS. The render loop
// owns TrackContext and therefore owns the texture handle in it, but main.cpp
// has no GL: this project confines GL to the render library's translation units,
// the same rule FFmpeg, SDL and winsock are held to. Without these the player
// would have to include glad to upload one image per track.
//
// MUST BE CALLED ON THE THREAD THAT OWNS THE CONTEXT. The art arrives on a
// worker thread and is decoded there -- decoding is pure CPU work on bytes and
// has no business blocking the render loop -- but the upload is a GL call and
// happens on the render thread like every other GL call here.

#pragma once

#include <holocron/track_context.hpp>

namespace holocron {

struct ImageRgba8;

// Upload `image` as an RGBA8 texture and return its handle, or 0 on failure.
//
// Zero is a usable answer rather than an error to handle: TrackContext documents
// 0 as "no art", every crystal already has to cope with it, and a sleeve that
// will not upload is worth exactly as much attention as one that was never
// there.
//
// Mipmaps are generated. A 512-pixel sleeve drawn small without them shimmers
// on every camera move, and the crystal that samples it cannot fix that.
TextureHandle upload_art(const ImageRgba8& image);

// Delete a texture and zero the handle. Safe to call with 0.
//
// Takes a reference so the caller cannot keep a handle that no longer names
// anything -- which would read as valid art and sample as garbage or as black.
void release_art(TextureHandle& texture);

}  // namespace holocron

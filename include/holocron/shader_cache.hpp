// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/shader_cache.hpp
//
// Linked GL programs, kept on disk so they are compiled once per machine rather
// than once per switch.
//
// WHY THIS EXISTS: 24 SECONDS.
//
// Measured on the Shield 2026-08-11 (issue 288, D-065): `duel` takes
// **23,859 ms** to compile and link on Tegra, against 35 ms on the rack. That
// happens on the render thread, so it is not a slow switch -- it is a
// twenty-four second freeze of the picture, and it is what the owner reported as
// "switching the crystals has a significant delay, duel is particularly slow".
//
// `glGetProgramBinary` and `glProgramBinary` are core in ES 3.0 and in desktop
// GL 4.1, so both of this project's targets have them with no extension check.
//
// WHAT A PROGRAM BINARY IS NOT
//
// It is not portable. It is specific to the driver that produced it, and a
// driver update may invalidate every one of them. That is why the binaries are
// NOT shipped in the APK -- they would be wrong on any other device and possibly
// on the same device next month -- and why every load is checked rather than
// trusted:
//
//   * the file records the GL vendor, renderer and version string it was made
//     under, and a load against different strings is a miss
//   * it records the source length and a hash of the source, so an edited
//     crystal is a miss -- which matters because hot reload exists and an author
//     saving a `.frag` must not get last week's program back
//   * `glProgramBinary` is followed by a GL_LINK_STATUS check, because the spec
//     allows a driver to reject a binary it wrote itself and says nothing about
//     giving a reason
//
// Any of those failing costs a compile, which is exactly what would have
// happened without a cache. THERE IS NO FAILURE MODE THAT IS WORSE THAN NOT
// HAVING ONE -- that is the property to preserve when changing this file.
//
// NOT A CORRECTNESS MECHANISM. If this whole class did nothing at all, the
// player would behave identically and take longer. Anything that makes a cache
// miss produce a different PICTURE rather than a different DURATION is a bug.

#pragma once

#include <cstdint>
#include <string>

namespace holocron {

class ShaderCache {
public:
    ShaderCache();
    ~ShaderCache();

    ShaderCache(const ShaderCache&)            = delete;
    ShaderCache& operator=(const ShaderCache&) = delete;

    // Point it at a directory and read the driver's identity out of the current
    // GL context. NEEDS A CONTEXT: the vendor and renderer strings are part of
    // every key, so calling this before the window exists produces a cache that
    // is keyed on nothing.
    //
    // An empty path, an unwritable directory or a driver that reports no binary
    // formats all leave it unavailable, which is a supported state and not an
    // error -- the player then compiles from source exactly as it always did.
    void open(const std::string& directory);

    bool available() const;

    // Why it is not available, for the one line the player prints. Empty when it
    // is.
    const std::string& unavailable_reason() const;

    // A linked program for this source, or 0.
    //
    // `source` is every string that went into the program, concatenated, and it
    // must include the vertex shader as well as the fragment one: two crystals
    // sharing a fragment shader but built against different vertex stages would
    // otherwise collide, and the failure would be a picture rather than a stall.
    std::uint32_t load(const std::string& source) const;

    // Keep a linked program. Quiet on failure: a cache that cannot be written is
    // a slower player, not a broken one.
    //
    // The program must have been linked with GL_PROGRAM_BINARY_RETRIEVABLE_HINT
    // set, or the driver is entitled to have thrown the binary away. See
    // prepare().
    void store(const std::string& source, std::uint32_t program) const;

    // Set GL_PROGRAM_BINARY_RETRIEVABLE_HINT before linking.
    //
    // SEPARATE CALL BECAUSE IT HAS TO HAPPEN BEFORE glLinkProgram, and store()
    // is necessarily after it. Getting this wrong produces a cache that silently
    // never stores anything, which reads exactly like a cache that is working.
    void prepare(std::uint32_t program) const;

    // How many programs this run restored rather than compiled, and how many it
    // wrote. Printed once at exit; the numbers are how anyone can tell the cache
    // is doing anything at all.
    std::uint64_t hits() const;
    std::uint64_t misses() const;
    std::uint64_t writes() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace holocron

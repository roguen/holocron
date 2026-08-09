/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 Roguen Keller
 *
 * holocron/facet_abi.h
 *
 * WHAT A FACET LOOKS LIKE FROM THE OTHER SIDE OF A C ABI.
 *
 * This is a C header on purpose and it is compiled as C in CI. That compile is
 * the whole point: "the facet interface is expressible across a C ABI boundary"
 * is an M3 exit criterion, and an assertion that it is expressible is worth much
 * less than a compiler agreeing.
 *
 * WHY IT MATTERS, AND WHY NOW RATHER THAN AT M4
 *
 * libprojectM arrives at M4 and must be reached through its C API, dynamically
 * linked, to keep the LGPL boundary intact (D-012). A projectM facet therefore
 * cannot be a C++ class in this project's own vocabulary -- it has to be
 * something that can be handed across a plain function-pointer boundary. The
 * Roadmap flags this as "free now, expensive at M4" and it is right: settling the
 * shape while there is nothing on the other side of it costs a header, and
 * settling it while also fighting an unfamiliar library costs a milestone.
 *
 * THE ONE THING THAT DOES NOT CROSS, WHICH IS THE FINDING
 *
 * `AudioFrame` crosses unchanged. It is a plain struct of floats and integers,
 * trivially copyable by contract (O-005), with a size CI already pins -- it was
 * designed for a memcpy across a thread boundary and an ABI boundary asks for
 * nothing more.
 *
 * `TrackContext` DOES NOT. It holds five std::strings, a std::array of glm::vec3
 * and a bool, none of which has a guaranteed layout across compilers or even
 * across two builds of the same compiler with different flags. That is not a
 * problem with TrackContext -- it is the right shape for the C++ side and its own
 * header explains why it is deliberately not part of the contract -- but it means
 * a facet ABI cannot simply pass a pointer to one.
 *
 * So the ABI takes a FLATTENED VIEW: `const char*` for the words and `float[3]`
 * for the colours, built at the boundary and valid only for the duration of the
 * call. That is the whole cost of the criterion, and finding it now is the
 * difference between a struct definition and a redesign.
 *
 * NOTHING IMPLEMENTS THIS YET, AND THAT IS DELIBERATE.
 *
 * Writing a C shim around CrystalFacet today would add a second way to call the
 * renderer with no second caller -- the exact dead path this project keeps
 * refusing to build. The criterion asks whether the interface is EXPRESSIBLE.
 * This expresses it, CI proves it compiles as C, and M4 is when something on the
 * far side of it exists.
 */

#ifndef HOLOCRON_FACET_ABI_H
#define HOLOCRON_FACET_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped when anything below changes shape. A host and a facet built against
 * different versions must refuse each other rather than crash: with a dynamically
 * loaded facet there is no link step to catch a mismatch, so the check has to be
 * a value passed at runtime. */
#define HOLOCRON_FACET_ABI_VERSION 1

/* Five swatches, matching kPaletteSize on the C++ side. Spelled out rather than
 * shared, because a C header cannot include the C++ one that defines it -- and a
 * mismatch here is caught by a static_assert on the C++ side rather than left to
 * be discovered. */
#define HOLOCRON_PALETTE_SIZE 5

/* What a facet is told about the record, flattened.
 *
 * EVERY POINTER IS BORROWED AND VALID ONLY FOR THE CALL. A facet that wants to
 * keep a title must copy it. This is the flattening of TrackContext described
 * above: strings become const char*, colours become float triples, and the
 * album art is the GL texture name it already was.
 *
 * Strings are never null -- an absent title is the empty string. A facet should
 * not have to check both.
 */
typedef struct holocron_track_view {
    const char* title;
    const char* artist;
    const char* album;
    const char* genre;
    const char* year;

    /* GL texture name, 0 when there is none. `has_art` is a mirror of that and
     * exists because a facet reading it as a flag is clearer than one comparing
     * a handle to zero. */
    uint32_t album_art_texture;
    int32_t  has_art;

    /* LINEAR rgb, most to least dominant, then the two chosen colours. Linear
     * rather than sRGB for the same reason the C++ side is: a facet works in the
     * space it draws in. */
    float palette[HOLOCRON_PALETTE_SIZE][3];
    float palette_primary[3];
    float palette_accent[3];

    /* True for exactly one drawn frame after a track starts, which is what
     * TrackContext promises and what lets a facet keeping its own state trust
     * seeing it once. */
    int32_t track_changed_this_frame;
    int32_t playing;
} holocron_track_view;

/* The facet itself.
 *
 * A struct of function pointers rather than an opaque handle plus free
 * functions: the host needs to hold several facets of different kinds at once,
 * and a vtable is how that is done without the host knowing any of their types.
 *
 * `self` is whatever the facet allocated in its own create function. The host
 * never looks inside it.
 *
 * NO ERROR STRINGS OUT OF draw(). It runs inside a frame and there is nothing
 * useful to do with a message there; failures belong to create, which returns
 * null and writes a reason.
 */
typedef struct holocron_facet {
    /* Must equal HOLOCRON_FACET_ABI_VERSION or the host must refuse it. */
    uint32_t abi_version;

    void* self;

    /* Draw into the framebuffer the host has bound, at `width` by `height`.
     *
     * `frame` points at an AudioFrame -- a plain struct of floats whose layout is
     * pinned by the contract and by a CI size check. Passed as void* because a C
     * header cannot name the C++ type without duplicating 10 KB of it, and
     * duplicating it is how the two definitions drift.
     *
     * The host guarantees: a current GL 4.5 core context on the calling thread,
     * a bound framebuffer, and a viewport already set to width x height.
     */
    void (*draw)(void* self, const void* frame, const holocron_track_view* track, int32_t width,
                 int32_t height);

    /* Seconds since this facet was created, and a way to set it.
     *
     * PRESENT FOR HOT RELOAD, which is the one piece of host behaviour a facet
     * cannot provide for itself: the host builds a replacement beside the live
     * one and has to carry the clock across, or every save restarts any slow
     * motion at zero.
     */
    float (*elapsed)(void* self);
    void  (*set_elapsed)(void* self, float seconds);

    /* Release everything. The host calls this while the GL context is still
     * current, because a facet's textures and programs belong to it. */
    void (*destroy)(void* self);
} holocron_facet;

/* What a dynamically loaded facet exports.
 *
 * One entry point rather than several, so a host has exactly one symbol to look
 * up and one thing to fail on. `out_error` receives a NUL-terminated message the
 * host may print; it is owned by the facet and must stay valid until destroy.
 *
 * Returns 0 on success.
 */
typedef int32_t (*holocron_facet_create_fn)(const char* argument, holocron_facet* out_facet,
                                            const char** out_error);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* HOLOCRON_FACET_ABI_H */

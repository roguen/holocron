// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/render/gl_api.hpp
//
// Where GL's declarations come from. Include this instead of <glad/glad.h>.
//
// WHY THIS EXISTS (issue 237)
//
// `vcpkg.json` pins glad to the feature `gl-api-45`, and a vcpkg feature is not
// triplet-dependent -- so installing the same manifest for `arm64-android`
// generates the DESKTOP OpenGL 4.5 loader. Measured on 2026-08-11 against a real
// arm64-android install: the generated glad.h declares `GL_VERSION_4_5`,
// `gladLoadGLLoader`, and even `glCreateTextures` and the rest of direct state
// access -- which the Shield reports as absent under both ARB and EXT names.
//
// EVERY ONE OF THOSE DECLARATIONS COMPILES ON ANDROID. That is the trap. The
// render library cross-compiles for aarch64-linux-android without a single
// error against that header, and every entry point in it would resolve to null
// at load time on an ES driver. A compile check cannot see this; only running it
// can, and nothing has run yet.
//
// ON ANDROID THERE IS NOTHING TO LOAD
//
// The NDK ships <GLES3/gl32.h> and the vendor ships libGLESv3.so, and ES entry
// points are ordinary exported symbols -- they link like any other library
// function. There is no loader, no function-pointer table and no initialisation
// step. So Android does not get a second glad configuration; it gets the
// platform's own header and library, which is what every Android application
// does.
//
// A SECOND GLAD CONFIGURATION WAS THE ALTERNATIVE AND WAS REJECTED. The port
// does offer `gles2-api-32`, so it could have been asked for. It was rejected on
// two counts: the loader ENTRY POINT changes name with it -- `gladLoadGLLoader`
// becomes `gladLoadGLES2Loader` -- so the source changes anyway, and it would
// make which symbols exist depend on which vcpkg features resolved for which
// triplet. A build-configuration difference showing up as a source-level symbol
// difference is the harder thing to reason about, not the easier one.
//
// THIS IS A HEADER SWAP AND NOT A SECOND CODE PATH. There are no `#ifdef`s in
// any calling file, and there must not be. The DSA port already did the work
// that makes that possible: every call site is bind-based, which is legal on
// GL 4.5 and on ES 3.2 alike, so one body of code compiles against either
// header. See docs/shield.md section 4, and gl_bind.hpp for the convention that
// keeps bind-then-modify honest.

#pragma once

#if defined(__ANDROID__)

// ES 3.2 core, which is what the Shield reports: "OpenGL ES 3.2 NVIDIA 495.00".
// gl32.h includes gl31.h includes gl3.h, so everything from ES 3.0 up is here.
//
// GL_KHR_debug was promoted to ES 3.2 core, so glDebugMessageCallback is
// declared without the KHR suffix and without an extension check -- the same
// argument window.cpp already makes for the desktop 4.5 context.
#include <GLES3/gl32.h>

// GL's calling-convention macro has two spellings and the ES headers only
// define one of them. Desktop headers -- including glad's -- define APIENTRY;
// the Khronos ES headers define GL_APIENTRY and leave APIENTRY alone.
//
// It matters for exactly one declaration, the KHR_debug callback, whose
// signature has to match the GLDEBUGPROC typedef. Defining the missing spelling
// here keeps that one signature identical on both platforms instead of putting
// a preprocessor conditional into the middle of a function declaration.
//
// Guarded, because on a build that somehow has both, whatever defined it first
// is the one GL's own typedef was written against.
#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif

#else

#include <glad/glad.h>

#endif

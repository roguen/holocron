// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/android_jni.hpp
//
// The one thing the Android platform layer needs from outside itself: a pointer
// to the process's Java VM.
//
// WHY THIS EXISTS RATHER THAN CALLING SDL FOR IT
//
// SDL3 has SDL_GetAndroidJNIEnv() and the player links SDL anyway, so the
// obvious move is to call it from the two places that need JNI -- the text
// rasterizer and the HTTPS client.
//
// That would put SDL in `holocron_visual` and in `holocron_plex`, and this
// project holds a rule that SDL, FFmpeg, GL and httplib each appear in exactly
// one translation unit and in no header. The rule is what makes "swap the sink"
// real rather than aspirational, and it is worth more than the dozen lines it
// saves here. So the VM is HANDED IN instead, once, by whoever starts the
// process -- which on Android is the entry point, and which may perfectly well
// get the pointer from SDL.
//
// THE POINTER IS A void*, AND THAT IS DELIBERATE. Typing it as JavaVM* would
// mean <jni.h> in a public header, and every translation unit that includes
// text_render.hpp would then need the NDK's include path. The two .cpp files
// that actually make JNI calls cast it back.
//
// WITHOUT IT NOTHING CRASHES. Every JNI-backed call checks first and returns its
// interface's "unsupported" error, the same value a Linux build returns. An
// Android build whose entry point forgot to call this degrades exactly like a
// platform with no rasterizer, which is a behaviour that already exists and is
// already handled, rather than a null-pointer dereference on the render thread.

#pragma once

namespace holocron::android {

// Hand over the process's JavaVM (a JavaVM*, passed as void*). Call once, early,
// before anything draws or fetches. Passing nullptr clears it.
//
// Safe to call on any platform: off Android it is a no-op, so the entry point
// needs no preprocessor conditional of its own.
void set_java_vm(void* vm);

// Whether a VM has been handed over AND this build has a JNI path at all. False
// on every non-Android build, so callers can ask instead of testing __ANDROID__.
bool has_java_vm();

// Hand over an Android Context (a jobject, passed as void*). Same arrangement as
// the VM above and for the same reason: the platform layer is TOLD what it needs
// rather than calling SDL for it, so SDL stays in one translation unit.
//
// PASS THE LOCAL REFERENCE YOU WERE GIVEN AND THEN DELETE IT. This promotes it to
// a global reference internally, because a local one is valid on one thread
// until the native call that produced it returns -- and this has to outlive both.
//
// ANY Context, NOT SPECIFICALLY THE ACTIVITY. This was `set_activity` until the
// Service arrived (issue 333), and the rename is what the platform layer always
// actually wanted: every user of this pointer immediately calls
// `getApplicationContext()` on it and then `getSystemService`, both of which any
// ContextWrapper answers. The Activity was simply the only Context the process
// had. It is not any more -- a Service is a Context too, and on the cold-start
// path it is the only one that exists.
//
// Nothing here keeps the Context alive in any way that matters: the process ends
// with the component that owns it.
//
// Safe to call on any platform; off Android it is a no-op.
void set_context(void* context_local_ref);

// Whether a Context has been handed over. False on every non-Android build.
bool has_context();

}  // namespace holocron::android

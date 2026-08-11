// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/android_jni_internal.hpp
//
// The JNI helpers shared by the Android branches of the platform layer. PRIVATE
// to src/ -- it includes <jni.h>, which is exactly what android_jni.hpp exists
// to keep out of the public headers. Same arrangement as src/render/gl_bind.hpp.
//
// ONLY INCLUDE THIS INSIDE AN `#ifdef __ANDROID__`. There is no other-platform
// branch in here on purpose: a header that quietly compiles to nothing
// everywhere else would let a caller forget the guard and find out at link time
// on the one platform that cannot be built casually.

#pragma once

#ifndef __ANDROID__
#error "android_jni_internal.hpp is Android-only; guard the include with __ANDROID__"
#endif

#include <jni.h>

namespace holocron::android {

// The process's Activity, as a global reference, or null if none was handed
// over. It is the only Context this process has, which is what makes it worth
// keeping: `getSystemService` hangs off a Context and nothing else here does.
jobject activity();

// A JNIEnv for the calling thread, attaching it if the VM has never seen it,
// and detaching on the way out ONLY if this object was the one that attached.
//
// Construct one per call rather than caching. A cached JNIEnv is valid for
// exactly one thread and using it from another is undefined -- the kind of bug
// that works for months and then does not. The calls this guards happen a few
// times an hour (a track title, a lyric line, one HTTP request), so the attach
// is not on any hot path and the safe shape is also the cheap one.
class ScopedEnv {
public:
    ScopedEnv();
    ~ScopedEnv();

    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

    // Null when there is no VM, or when attaching failed. Every caller checks.
    JNIEnv* get() const { return env_; }
    explicit operator bool() const { return env_ != nullptr; }
    JNIEnv* operator->() const { return env_; }

    // True if there is no env, or if a Java exception is pending -- in which
    // case it is described to logcat and CLEARED, so the next JNI call is legal.
    //
    // Call this after every step that can throw. `what` names the step and ends
    // up in logcat beside the Java stack trace.
    bool failed(const char* what) const;

private:
    JNIEnv* env_      = nullptr;
    bool    attached_ = false;
};

// A local reference that is released on the way out.
//
// The JNI local reference table is SMALL -- the specification guarantees only
// 16 slots without EnsureLocalCapacity -- and every FindClass, NewStringUTF and
// object-returning call consumes one. Rasterizing a string touches a dozen, so
// leaking them is not theoretical; the failure is a table overflow abort, and it
// arrives long after the call that leaked.
template <typename T>
class Local {
public:
    Local(JNIEnv* env, T ref) : env_(env), ref_(ref) {}
    ~Local()
    {
        if (env_ != nullptr && ref_ != nullptr) {
            env_->DeleteLocalRef(ref_);
        }
    }

    Local(const Local&)            = delete;
    Local& operator=(const Local&) = delete;

    T    get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    JNIEnv* env_;
    T       ref_;
};

}  // namespace holocron::android

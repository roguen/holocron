// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/android_jni.cpp
//
// The JavaVM pointer, and the two pieces of JNI hygiene that every call in the
// Android platform layer needs. See android_jni.hpp for why the VM is handed in
// rather than fetched from SDL.
//
// Compiled on every platform, with the body behind `__ANDROID__` -- the same
// arrangement as wasapi_sink.cpp, text_render.cpp and https_client.cpp, and for
// the reason those files give: the other compiler still reads the file.
//
// scripts/android-check.sh is what compiles the Android side, because until
// there is a real Android build nothing else does, and this project has a rule
// about paths nothing can reach.
//
// TWO THINGS HERE ARE NOT OPTIONAL AND BOTH ARE SILENT WHEN WRONG
//
// 1. A THREAD THAT MAKES A JNI CALL MUST BE ATTACHED TO THE VM. The render
//    thread was created by C++ and the VM has never heard of it. Calling
//    GetEnv() on an unattached thread returns JNI_EDETACHED and a null JNIEnv,
//    and using that pointer is an immediate crash rather than an error.
//
//    A thread this code attaches must also be DETACHED before it exits, or the
//    VM holds a reference to a dead thread and the process will not shut down
//    cleanly. ScopedEnv detaches only if it was the one that attached -- the
//    main thread arrives already attached and must be left that way.
//
// 2. A PENDING JAVA EXCEPTION POISONS EVERY SUBSEQUENT JNI CALL. Most JNI
//    functions do not report failure in their return value; they set an
//    exception and carry on returning something. Making a second call with one
//    pending is undefined and in practice aborts the process. So every step
//    that can throw is followed by a check that clears it and turns it into an
//    ordinary error return.

#include <holocron/android_jni.hpp>

#ifdef __ANDROID__

#include "android_jni_internal.hpp"

#include <android/log.h>

namespace holocron::android {

namespace {

// Set once at startup and read from the render thread and the fetch worker.
// Written before any of them exist and never again, which is why a plain
// pointer is enough and an atomic would be decoration.
JavaVM* g_vm = nullptr;

// The Activity, as a GLOBAL reference. Never released: it lives for the life of
// the process, and DeleteGlobalRef at static-destruction time would need a
// JNIEnv on a thread that may already be detached.
jobject g_activity = nullptr;

}  // namespace

void set_java_vm(void* vm)
{
    g_vm = static_cast<JavaVM*>(vm);
}

bool has_java_vm()
{
    return g_vm != nullptr;
}

void set_activity(void* activity_local_ref)
{
    if (activity_local_ref == nullptr) {
        g_activity = nullptr;
        return;
    }
    // PROMOTED HERE rather than by the caller, so the caller needs no jni.h --
    // which is the whole reason android_jni.hpp takes a void*.
    ScopedEnv env;
    if (!env) {
        return;
    }
    g_activity = env->NewGlobalRef(static_cast<jobject>(activity_local_ref));
}

bool has_activity()
{
    return g_activity != nullptr;
}

jobject activity()
{
    return g_activity;
}

// ---------------------------------------------------------------------------

ScopedEnv::ScopedEnv()
{
    if (g_vm == nullptr) {
        return;
    }

    void* raw = nullptr;
    const jint status = g_vm->GetEnv(&raw, JNI_VERSION_1_6);

    if (status == JNI_OK) {
        env_ = static_cast<JNIEnv*>(raw);
        return;  // already attached; NOT ours to detach
    }

    if (status == JNI_EDETACHED) {
        // A C++-created thread. The name is what shows up in a Java stack trace
        // and in `adb shell ps -T`, which is the only reason it is set.
        JavaVMAttachArgs args{};
        args.version = JNI_VERSION_1_6;
        args.name    = "holocron";
        args.group   = nullptr;

        if (g_vm->AttachCurrentThread(&env_, &args) == JNI_OK) {
            attached_ = true;
        } else {
            env_ = nullptr;
        }
    }

    // JNI_EVERSION falls through with env_ null, which every caller checks.
}

ScopedEnv::~ScopedEnv()
{
    if (attached_ && g_vm != nullptr) {
        g_vm->DetachCurrentThread();
    }
}

bool ScopedEnv::failed(const char* what) const
{
    if (env_ == nullptr) {
        return true;
    }
    if (env_->ExceptionCheck() == JNI_FALSE) {
        return false;
    }

    // Logged rather than swallowed. The caller turns this into an error return
    // and the string it produces says WHICH step threw, but the Java stack
    // trace only exists here and only until it is cleared.
    __android_log_print(ANDROID_LOG_WARN, "holocron", "JNI exception during %s",
                        what != nullptr ? what : "(unknown)");
    env_->ExceptionDescribe();
    env_->ExceptionClear();
    return true;
}

}  // namespace holocron::android

#else

namespace holocron::android {

// No JNI on this platform. set_java_vm is still callable so the entry point
// needs no #ifdef, and has_java_vm answers honestly.
void set_java_vm(void* /*vm*/) {}

bool has_java_vm()
{
    return false;
}

}  // namespace holocron::android

#endif

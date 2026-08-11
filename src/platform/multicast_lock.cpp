// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/multicast_lock.cpp
//
// See multicast_lock.hpp for what Android does to multicast and why this exists.
//
// Compiled on every platform, like android_jni.cpp beside it. Off Android the
// body is two lines and there is nothing to link -- which is the point: the
// caller writes the same code everywhere and asks the return value what
// happened.
//
// THE JAVA THIS PERFORMS, spelled out because reading it back out of JNI calls
// is unpleasant:
//
//     Context      ctx  = activity.getApplicationContext();
//     WifiManager  wm   = (WifiManager) ctx.getSystemService("wifi");
//     MulticastLock lock = wm.createMulticastLock("holocron-gdm");
//     lock.setReferenceCounted(false);
//     lock.acquire();
//
// THE APPLICATION CONTEXT, NOT THE ACTIVITY. `getSystemService` works on either,
// but a WifiManager obtained from an Activity holds a reference to it; Android's
// own lint calls this out. The application context outlives every Activity and
// is the documented way to ask.
//
// setReferenceCounted(false) IS LOAD-BEARING. The default lock counts acquires,
// so a second acquire would need a second release, and a caller that lost count
// would leave the Wi-Fi chip out of power-saving mode forever. Uncounted makes
// acquire idempotent and release absolute, which is the contract in the header.

#include <holocron/multicast_lock.hpp>

namespace holocron {

const char* to_string(MulticastLockState s)
{
    switch (s) {
    case MulticastLockState::kHeld:        return "multicast lock held";
    case MulticastLockState::kUnsupported: return "this platform does not filter multicast";
    case MulticastLockState::kUnavailable: return "the Wi-Fi service could not be reached";
    case MulticastLockState::kFailed:      return "the multicast lock could not be acquired";
    }
    return "unknown";
}

}  // namespace holocron

#ifdef __ANDROID__

#include <holocron/android_jni.hpp>

#include "android_jni_internal.hpp"

#include <android/log.h>

namespace holocron {

namespace {

// The lock, as a global reference, held for as long as it is acquired.
//
// Not atomic: acquired once during startup and released once during shutdown,
// both from the thread that starts discovery. If that ever stops being true this
// needs revisiting, which is why it is said here.
jobject g_lock = nullptr;

}  // namespace

MulticastLockState acquire_multicast_lock()
{
    if (g_lock != nullptr) {
        return MulticastLockState::kHeld;  // idempotent; see the header
    }

    android::ScopedEnv env;
    if (!env || android::activity() == nullptr) {
        return MulticastLockState::kUnavailable;
    }

    // Context.getApplicationContext()
    android::Local<jclass> context_cls(env.get(), env->FindClass("android/content/Context"));
    if (env.failed("FindClass android/content/Context") || !context_cls) {
        return MulticastLockState::kUnavailable;
    }
    const jmethodID get_app = env->GetMethodID(context_cls.get(), "getApplicationContext",
                                               "()Landroid/content/Context;");
    const jmethodID get_svc = env->GetMethodID(context_cls.get(), "getSystemService",
                                               "(Ljava/lang/String;)Ljava/lang/Object;");
    if (env.failed("Context method ids") || get_app == nullptr || get_svc == nullptr) {
        return MulticastLockState::kUnavailable;
    }

    android::Local<jobject> ctx(env.get(),
                                env->CallObjectMethod(android::activity(), get_app));
    if (env.failed("getApplicationContext") || !ctx) {
        return MulticastLockState::kUnavailable;
    }

    // The literal rather than Context.WIFI_SERVICE, which is a compile-time
    // String constant with this exact value -- reading the static field would be
    // two more calls that can throw, to arrive at the same four characters.
    android::Local<jstring> wifi_name(env.get(), env->NewStringUTF("wifi"));
    if (env.failed("NewStringUTF wifi") || !wifi_name) {
        return MulticastLockState::kUnavailable;
    }

    android::Local<jobject> wifi(env.get(),
                                 env->CallObjectMethod(ctx.get(), get_svc, wifi_name.get()));
    // A DEVICE WITH NO WI-FI ANSWERS NULL AND DOES NOT THROW. That is not a
    // fault: it is a wired-only box, which is the case where the filter this
    // lock lifts does not exist in the first place.
    if (env.failed("getSystemService(wifi)") || !wifi) {
        return MulticastLockState::kUnavailable;
    }

    android::Local<jclass> wifi_cls(env.get(), env->GetObjectClass(wifi.get()));
    if (env.failed("GetObjectClass WifiManager") || !wifi_cls) {
        return MulticastLockState::kUnavailable;
    }
    const jmethodID create = env->GetMethodID(
        wifi_cls.get(), "createMulticastLock",
        "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    if (env.failed("createMulticastLock id") || create == nullptr) {
        return MulticastLockState::kUnavailable;
    }

    // The tag is what shows up in `adb shell dumpsys wifi`, so it is worth being
    // the thing somebody would search for.
    android::Local<jstring> tag(env.get(), env->NewStringUTF("holocron-gdm"));
    android::Local<jobject> lock(env.get(),
                                 env->CallObjectMethod(wifi.get(), create, tag.get()));
    if (env.failed("createMulticastLock") || !lock) {
        return MulticastLockState::kFailed;
    }

    android::Local<jclass> lock_cls(env.get(), env->GetObjectClass(lock.get()));
    const jmethodID set_counted =
        env->GetMethodID(lock_cls.get(), "setReferenceCounted", "(Z)V");
    const jmethodID acquire = env->GetMethodID(lock_cls.get(), "acquire", "()V");
    if (env.failed("MulticastLock method ids") || set_counted == nullptr || acquire == nullptr) {
        return MulticastLockState::kFailed;
    }

    env->CallVoidMethod(lock.get(), set_counted, JNI_FALSE);
    if (env.failed("setReferenceCounted")) {
        return MulticastLockState::kFailed;
    }
    env->CallVoidMethod(lock.get(), acquire);
    // THE ONE THAT ACTUALLY NEEDS THE PERMISSION. Without
    // CHANGE_WIFI_MULTICAST_STATE in the manifest this throws SecurityException,
    // and the whole point of ScopedEnv::failed is that it lands in logcat with
    // its Java stack trace rather than aborting the process.
    if (env.failed("MulticastLock.acquire")) {
        return MulticastLockState::kFailed;
    }

    g_lock = env->NewGlobalRef(lock.get());
    return g_lock != nullptr ? MulticastLockState::kHeld : MulticastLockState::kFailed;
}

void release_multicast_lock()
{
    if (g_lock == nullptr) {
        return;
    }
    android::ScopedEnv env;
    if (!env) {
        // Nothing safe to do. The process is ending anyway, and Android drops
        // the lock with it.
        g_lock = nullptr;
        return;
    }
    android::Local<jclass> lock_cls(env.get(), env->GetObjectClass(g_lock));
    if (lock_cls) {
        if (const jmethodID release = env->GetMethodID(lock_cls.get(), "release", "()V");
            release != nullptr) {
            env->CallVoidMethod(g_lock, release);
            static_cast<void>(env.failed("MulticastLock.release"));
        }
    }
    env->DeleteGlobalRef(g_lock);
    g_lock = nullptr;
}

}  // namespace holocron

#else

namespace holocron {

// Windows and Linux do not filter multicast away from a process, so there is
// nothing to unlock and nothing to give back. kUnsupported is the honest answer
// and the header says explicitly that it is not an error.
MulticastLockState acquire_multicast_lock()
{
    return MulticastLockState::kUnsupported;
}

void release_multicast_lock() {}

}  // namespace holocron

#endif

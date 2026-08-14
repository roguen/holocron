// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/screen_wake.cpp
//
// See screen_wake.hpp for what this is for, which thread may call it, and why
// the wake lock is acquired with a timeout and never released.
//
// Compiled on every platform, like multicast_lock.cpp beside it. Off Android the
// body is one line -- which is the point: the caller writes the same code
// everywhere and asks the return value what happened.

#include <holocron/screen_wake.hpp>

namespace holocron {

const char* to_string(ScreenWakeState s)
{
    switch (s) {
    case ScreenWakeState::kWoken:       return "the display was asked to come on";
    case ScreenWakeState::kUnsupported: return "this platform has no display to wake";
    case ScreenWakeState::kUnavailable: return "the power service could not be reached";
    case ScreenWakeState::kFailed:      return "the wake lock could not be acquired";
    }
    return "unknown";
}

}  // namespace holocron

#ifdef __ANDROID__

#include <holocron/android_jni.hpp>

#include "android_jni_internal.hpp"

namespace holocron {

namespace {

// PowerManager's flag constants, written out rather than read back off the class
// as static fields.
//
// The same trade as the "wifi" literal in multicast_lock.cpp: these are Java
// compile-time `int` constants with these exact values, fixed since API 1 and
// part of the public API, so reading the fields would be several more calls that
// can each throw to arrive at the same four numbers.
//
// SCREEN_BRIGHT rather than FULL: FULL_WAKE_LOCK additionally lights the
// keyboard backlight, which a television does not have.
constexpr jint kScreenBrightWakeLock = 0x0000000a;
constexpr jint kAcquireCausesWakeup  = 0x10000000;
constexpr jint kOnAfterRelease       = 0x20000000;

// How long the lock is held before Android drops it on its own.
//
// Long enough for the Activity to resume and for SDL to disable the screensaver,
// which is what holds the screen on from then on. Short enough that a fault here
// costs three seconds of television rather than a projector lamp left burning --
// see the header, which is where the reasoning for having no release() lives.
constexpr jlong kHoldMs = 3000;

}  // namespace

ScreenWakeState wake_screen()
{
    android::ScopedEnv env;
    if (!env || android::context() == nullptr) {
        return ScreenWakeState::kUnavailable;
    }

    // Context.getApplicationContext(), then getSystemService("power").
    //
    // The APPLICATION context, not the Activity, for the reason spelled out in
    // multicast_lock.cpp: a manager obtained from an Activity holds a reference
    // to it, and Android's own lint says so.
    android::Local<jclass> context_cls(env.get(), env->FindClass("android/content/Context"));
    if (env.failed("FindClass android/content/Context") || !context_cls) {
        return ScreenWakeState::kUnavailable;
    }
    const jmethodID get_app = env->GetMethodID(context_cls.get(), "getApplicationContext",
                                               "()Landroid/content/Context;");
    const jmethodID get_svc = env->GetMethodID(context_cls.get(), "getSystemService",
                                               "(Ljava/lang/String;)Ljava/lang/Object;");
    if (env.failed("Context method ids") || get_app == nullptr || get_svc == nullptr) {
        return ScreenWakeState::kUnavailable;
    }

    android::Local<jobject> ctx(env.get(), env->CallObjectMethod(android::context(), get_app));
    if (env.failed("getApplicationContext") || !ctx) {
        return ScreenWakeState::kUnavailable;
    }

    android::Local<jstring> power_name(env.get(), env->NewStringUTF("power"));
    if (env.failed("NewStringUTF power") || !power_name) {
        return ScreenWakeState::kUnavailable;
    }

    android::Local<jobject> power(env.get(),
                                  env->CallObjectMethod(ctx.get(), get_svc, power_name.get()));
    if (env.failed("getSystemService(power)") || !power) {
        return ScreenWakeState::kUnavailable;
    }

    android::Local<jclass> power_cls(env.get(), env->GetObjectClass(power.get()));
    if (env.failed("GetObjectClass PowerManager") || !power_cls) {
        return ScreenWakeState::kUnavailable;
    }
    const jmethodID new_lock =
        env->GetMethodID(power_cls.get(), "newWakeLock",
                         "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;");
    if (env.failed("newWakeLock id") || new_lock == nullptr) {
        return ScreenWakeState::kUnavailable;
    }

    // The tag is what `adb shell dumpsys power` prints, so it is worth being the
    // thing somebody would search for. Android asks for `package:tag` shape.
    android::Local<jstring> tag(env.get(), env->NewStringUTF("holocron:cast"));
    android::Local<jobject> lock(
        env.get(), env->CallObjectMethod(power.get(), new_lock,
                                         kScreenBrightWakeLock | kAcquireCausesWakeup |
                                             kOnAfterRelease,
                                         tag.get()));
    if (env.failed("newWakeLock") || !lock) {
        return ScreenWakeState::kFailed;
    }

    android::Local<jclass> lock_cls(env.get(), env->GetObjectClass(lock.get()));
    if (env.failed("GetObjectClass WakeLock") || !lock_cls) {
        return ScreenWakeState::kFailed;
    }
    // THE TIMED OVERLOAD, acquire(long), not acquire(). See the header: nothing
    // releases this, so an untimed one would hold the display on forever.
    const jmethodID acquire = env->GetMethodID(lock_cls.get(), "acquire", "(J)V");
    if (env.failed("WakeLock.acquire(long) id") || acquire == nullptr) {
        return ScreenWakeState::kFailed;
    }

    env->CallVoidMethod(lock.get(), acquire, kHoldMs);
    // THE ONE THAT ACTUALLY NEEDS THE PERMISSION. Without android.permission.
    // WAKE_LOCK in the manifest this throws SecurityException, and ScopedEnv::
    // failed puts it in logcat with its Java stack trace rather than aborting.
    if (env.failed("WakeLock.acquire")) {
        return ScreenWakeState::kFailed;
    }

    return ScreenWakeState::kWoken;
}

}  // namespace holocron

#else

namespace holocron {

// A desktop's display is the desktop's business, and there is no cast arriving
// at a machine somebody is not sitting at. kUnsupported is the honest answer and
// the header says explicitly that it is not an error.
ScreenWakeState wake_screen()
{
    return ScreenWakeState::kUnsupported;
}

}  // namespace holocron

#endif  // __ANDROID__

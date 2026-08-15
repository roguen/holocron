// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// src/platform/launch_player.cpp
//
// See holocron/launch_player.hpp, including why the caller must wake the display
// before calling this.
//
// Compiled on every platform with the body behind `__ANDROID__`, the same
// arrangement as screen_wake.cpp and multicast_lock.cpp beside it.

#include <holocron/launch_player.hpp>

namespace holocron {

const char* to_string(LaunchPlayerState s)
{
    switch (s) {
    case LaunchPlayerState::kUnsupported: return "no Activity to start on this platform";
    case LaunchPlayerState::kStarted:     return "asked Android to bring the player up";
    case LaunchPlayerState::kUnavailable: return "no Context, or no launch intent for this package";
    case LaunchPlayerState::kFailed:      return "startActivity threw";
    }
    return "unknown";
}

}  // namespace holocron

#ifdef __ANDROID__

#include "android_jni_internal.hpp"

namespace holocron {

namespace {

// Intent.FLAG_ACTIVITY_NEW_TASK.
//
// MANDATORY HERE AND NOT A TUNING KNOB. startActivity from a non-Activity
// Context -- which a Service is -- throws AndroidRuntimeException without it,
// with a message naming this exact flag. There is no task to attach to, because
// the caller is not on one.
constexpr jint kFlagActivityNewTask = 0x10000000;

}  // namespace

LaunchPlayerState launch_player()
{
    android::ScopedEnv env;
    if (!env || android::context() == nullptr) {
        return LaunchPlayerState::kUnavailable;
    }

    // THE APPLICATION'S OWN ROUTE FIRST, IF IT HAS ONE.
    //
    // A bare startActivity from a Service is refused by Android's background
    // activity start restriction -- see the header for the log line that says
    // so. The sanctioned way needs a full-screen-intent notification, which is
    // Java, so the component that can do it supplies `launchPlayer()`.
    //
    // Looked up rather than required: the Activity has no such method and does
    // not need one, because an app with a visible window is exempt from the
    // restriction and the fallback below works there.
    {
        android::Local<jclass> ctx_cls(env.get(), env->GetObjectClass(android::context()));
        if (ctx_cls) {
            const jmethodID launch = env->GetMethodID(ctx_cls.get(), "launchPlayer", "()V");
            if (launch != nullptr) {
                env->CallVoidMethod(android::context(), launch);
                if (env.failed("launchPlayer")) {
                    return LaunchPlayerState::kFailed;
                }
                return LaunchPlayerState::kStarted;
            }
            // GetMethodID THROWS NoSuchMethodError WHEN IT MISSES, and a pending
            // exception poisons every JNI call after it. Cleared here rather
            // than left for the next call to trip over -- this is the expected
            // path for a Context that is not the Service.
            env->ExceptionClear();
        }
    }

    android::Local<jclass> context_cls(env.get(), env->FindClass("android/content/Context"));
    if (env.failed("FindClass android/content/Context") || !context_cls) {
        return LaunchPlayerState::kUnavailable;
    }

    // The APPLICATION context, for the reason multicast_lock.cpp gives: a
    // manager obtained from a component holds a reference to that component.
    const jmethodID get_app = env->GetMethodID(context_cls.get(), "getApplicationContext",
                                               "()Landroid/content/Context;");
    const jmethodID get_pm  = env->GetMethodID(context_cls.get(), "getPackageManager",
                                               "()Landroid/content/pm/PackageManager;");
    const jmethodID get_pkg = env->GetMethodID(context_cls.get(), "getPackageName",
                                               "()Ljava/lang/String;");
    const jmethodID start   = env->GetMethodID(context_cls.get(), "startActivity",
                                               "(Landroid/content/Intent;)V");
    if (env.failed("Context method ids") || get_app == nullptr || get_pm == nullptr ||
        get_pkg == nullptr || start == nullptr) {
        return LaunchPlayerState::kUnavailable;
    }

    android::Local<jobject> ctx(env.get(), env->CallObjectMethod(android::context(), get_app));
    if (env.failed("getApplicationContext") || !ctx) {
        return LaunchPlayerState::kUnavailable;
    }

    android::Local<jobject> pm(env.get(), env->CallObjectMethod(ctx.get(), get_pm));
    if (env.failed("getPackageManager") || !pm) {
        return LaunchPlayerState::kUnavailable;
    }

    android::Local<jstring> package(env.get(),
                                    static_cast<jstring>(env->CallObjectMethod(ctx.get(), get_pkg)));
    if (env.failed("getPackageName") || !package) {
        return LaunchPlayerState::kUnavailable;
    }

    // getLaunchIntentForPackage -- THE SAME INTENT THE LEANBACK LAUNCHER FIRES,
    // which is why no class name appears anywhere in this file. See the header.
    android::Local<jclass> pm_cls(env.get(), env->GetObjectClass(pm.get()));
    if (env.failed("GetObjectClass PackageManager") || !pm_cls) {
        return LaunchPlayerState::kUnavailable;
    }
    const jmethodID get_launch =
        env->GetMethodID(pm_cls.get(), "getLaunchIntentForPackage",
                         "(Ljava/lang/String;)Landroid/content/Intent;");
    if (env.failed("getLaunchIntentForPackage id") || get_launch == nullptr) {
        return LaunchPlayerState::kUnavailable;
    }

    android::Local<jobject> intent(
        env.get(), env->CallObjectMethod(pm.get(), get_launch, package.get()));
    if (env.failed("getLaunchIntentForPackage")) {
        return LaunchPlayerState::kUnavailable;
    }
    if (!intent) {
        // NULL RATHER THAN AN EXCEPTION is what this returns for a package with
        // no launcher activity. Not reachable for this app -- the manifest has
        // LAUNCHER and LEANBACK_LAUNCHER on HolocronActivity -- but it is the
        // documented answer and reporting it as kUnavailable beats dereferencing.
        return LaunchPlayerState::kUnavailable;
    }

    android::Local<jclass> intent_cls(env.get(), env->GetObjectClass(intent.get()));
    if (env.failed("GetObjectClass Intent") || !intent_cls) {
        return LaunchPlayerState::kUnavailable;
    }
    const jmethodID add_flags =
        env->GetMethodID(intent_cls.get(), "addFlags", "(I)Landroid/content/Intent;");
    if (env.failed("addFlags id") || add_flags == nullptr) {
        return LaunchPlayerState::kUnavailable;
    }

    // The return value is the same Intent, so it is released rather than used.
    android::Local<jobject> same(
        env.get(), env->CallObjectMethod(intent.get(), add_flags, kFlagActivityNewTask));
    if (env.failed("addFlags")) {
        return LaunchPlayerState::kFailed;
    }

    env->CallVoidMethod(ctx.get(), start, intent.get());
    if (env.failed("startActivity")) {
        return LaunchPlayerState::kFailed;
    }
    return LaunchPlayerState::kStarted;
}

}  // namespace holocron

#else

namespace holocron {

LaunchPlayerState launch_player()
{
    return LaunchPlayerState::kUnsupported;
}

}  // namespace holocron

#endif

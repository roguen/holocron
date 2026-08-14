// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// tools/player/android_service.cpp
//
// The Service's JNI glue, and nothing else. Compiled ONLY on Android.
//
// WHAT THIS IS FOR
//
// Issue 333 / issue 338 step 2, the cold case: with the Shield's display off, a
// launch never enters `SDL_main` at all -- SDL creates its native thread only in
// `handleNativeState`'s RESUMED branch, gated on a ready surface AND
// `mIsResumedCalled`, and `onStop` clears the latter about 15 ms later. So there
// is no Companion socket, nothing on the network, and nothing to cast TO.
//
// The network half never needed any of that. `GdmResponder` and
// `CompanionServer` are sockets and threads; neither header mentions SDL. What
// gated them was the entry point they sat behind, not a dependency. This file is
// a second entry point that does not sit behind it.
//
// THE LOGIC IS NOT HERE. It is in src/plex/service_network.cpp, because the
// player has to call the same code -- it yields the sockets before binding its
// own and hands them back afterwards -- and the player must not depend on the
// Service's JNI. This file is the three-function shim between Java and that.
//
// WHAT THIS DELIBERATELY DOES NOT DO
//
// It does not play anything, and it must not be made to. There is no decoder, no
// audio sink and no GL here; those belong to the Activity, and duplicating them
// would mean two things owning one audio device.
//
// IT CANNOT SURVIVE A FORCE-STOP, AND NOTHING HERE PRETENDS OTHERWISE. D-076.
// Android disables a force-stopped app's services and receivers as a matter of
// policy. This addresses a reboot and Android reclaiming the process -- two of
// the three causes issue 338 names in one breath.

#ifndef __ANDROID__
#error "android_service.cpp is Android-only and must not be compiled elsewhere"
#endif

#include <holocron/android_jni.hpp>
#include <holocron/platform_paths.hpp>
#include <holocron/service_network.hpp>

#include <jni.h>

extern "C" {

// Bring the network half up. Returns the bound Companion port, or 0.
//
// THE PORT IS THE RETURN VALUE RATHER THAN A LOG LINE because the Companion
// server MOVES to a free port rather than refusing to start (issue 247), so the
// caller cannot assume the configured one.
//
// Zero is not necessarily a failure: it is also what "the player holds the
// sockets, so this Service is standing by" returns. The Java side treats it as
// "nothing bound", which is true either way, and the run log says which.
JNIEXPORT jint JNICALL
Java_io_github_roguen_holocron_HolocronService_nativeStartNetwork(JNIEnv* env, jobject thiz)
{
    // THE VM AND THE CONTEXT, exactly as android_entry.cpp does it -- and when
    // the Service is what started the process, this is the first Holocron code
    // to run, so there is nothing before it to have done this.
    //
    // `thiz` is the Service, and a Service IS a Context: every platform user of
    // this pointer calls getApplicationContext() on it first, which any
    // ContextWrapper answers. See android_jni.hpp's note on set_context.
    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) == JNI_OK && vm != nullptr) {
        holocron::android::set_java_vm(vm);
    }
    if (!holocron::android::has_context()) {
        holocron::android::set_context(thiz);
    }

    return static_cast<jint>(holocron::start_service_network());
}

JNIEXPORT void JNICALL
Java_io_github_roguen_holocron_HolocronService_nativeStopNetwork(JNIEnv*, jobject)
{
    holocron::stop_service_network();
}

// Where gatekeeper.toml, the vault and the run log live.
//
// HANDED IN FROM JAVA rather than asked of SDL, the same arrangement
// android_jni.hpp uses for the JavaVM and for the same reason -- and here SDL is
// not even initialised, which is the entire point of the Service. Java gets this
// from Context.getExternalFilesDir(null), the same directory
// SDL_GetAndroidExternalStoragePath returns.
JNIEXPORT void JNICALL
Java_io_github_roguen_holocron_HolocronService_nativeSetDataDirectory(JNIEnv* env, jobject,
                                                                     jstring path)
{
    if (path == nullptr) {
        return;
    }
    if (const char* chars = env->GetStringUTFChars(path, nullptr); chars != nullptr) {
        holocron::set_data_directory(chars);
        env->ReleaseStringUTFChars(path, chars);
    }
}

}  // extern "C"

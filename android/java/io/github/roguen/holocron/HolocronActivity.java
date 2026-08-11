// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller

package io.github.roguen.holocron;

import org.libsdl.app.SDLActivity;

/**
 * The Activity Android launches. It exists to say ONE thing, and that one thing
 * is not optional.
 *
 * <p>SDLActivity's default {@code getLibraries()} returns {@code {"SDL3", "main"}},
 * which is right for the usual arrangement where SDL is a shared library beside
 * the app's own. It is wrong here: vcpkg's {@code arm64-android} triplet builds
 * static libraries, so there is no {@code libSDL3.so} to load and
 * {@code System.loadLibrary("SDL3")} would throw before anything else ran.
 *
 * <p>SDL is linked INTO {@code libholocron.so} instead — all of it, including its
 * {@code JNI_OnLoad} and the sixty-seven {@code Java_org_libsdl_app_*} natives
 * this class's superclass calls. One library, loaded once.
 *
 * <p>SDLActivity then takes the LAST entry of this array as the object to look
 * {@code SDL_main} up in, so a single-element array is both the list of things to
 * load and the nomination of which one holds the entry point. That entry point is
 * in {@code tools/player/android_entry.cpp}, which also hands the JavaVM to the
 * platform layer — without which the text rasterizer and the HTTPS client both
 * return "unsupported" and the app runs on with no now-playing card, no lyrics
 * and no licence panel.
 */
public class HolocronActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "holocron" };
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller

package io.github.roguen.holocron;

import android.content.Intent;
import android.os.Bundle;

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

    /**
     * Start {@link HolocronService}, so the box stays castable after this
     * Activity goes away.
     *
     * <p>Issue 333. Starting it here rather than only from the boot receiver is
     * what covers the ordinary case: the Service exists from the first time
     * anybody launches Holocron, and keeps existing when the Activity is ended
     * with BACK — which leaves the process cached (D-070) and, before this,
     * left the box off the network until somebody launched it again.
     *
     * <p><b>This does not race the player for the sockets.</b> Ownership is
     * explicit and the player always wins: {@code holocron_main} calls
     * {@code yield_service_network()} before binding, which marks the player as
     * owner, and the Service's {@code start_service_network()} sees that and
     * stands by instead of binding. The player hands them back on the way out.
     * Neither order of arrival matters, which is the point — see
     * service_network.hpp.
     */
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        startService(new Intent(this, HolocronService.class));
    }
}

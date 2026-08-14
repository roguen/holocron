// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller

package io.github.roguen.holocron;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.util.Log;

/**
 * Brings {@link HolocronService} back after the box reboots.
 *
 * <p>Issue 333. Without this, a Shield that has been power-cycled is off the
 * network until somebody picks Holocron out of the launcher — and the whole
 * point of the Service is that nobody should have to.
 *
 * <p><b>This does not cover a force-stop, and it cannot.</b> D-076. Android
 * disables a force-stopped app's broadcast receivers along with its services, so
 * {@code BOOT_COMPLETED} is not delivered here until the app is explicitly
 * launched once. That is platform policy, not an omission — issue 338 names
 * reboot, force-stop and reclaim in one breath and only two of the three are
 * addressable.
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "holocron";

    @Override
    public void onReceive(Context context, Intent intent) {
        final String action = intent != null ? intent.getAction() : null;
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action)) {
            // A receiver is handed whatever its filter matched, and matching is
            // the manifest's business rather than something to trust blindly.
            return;
        }

        Intent service = new Intent(context, HolocronService.class);
        // startForegroundService FROM API 26, because a plain startService from
        // a background context throws IllegalStateException there -- and a boot
        // receiver is the most background a context gets. The Service calls
        // startForeground in its own onCreate, which is what that contract
        // requires.
        if (Build.VERSION.SDK_INT >= 26 /* Android 8.0 (O) */) {
            context.startForegroundService(service);
        } else {
            context.startService(service);
        }
        Log.i(TAG, "boot: asked the discovery service to start");
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller

package io.github.roguen.holocron;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.File;

/**
 * Keeps Holocron discoverable and castable when its Activity is not running.
 *
 * <p><b>Why this exists.</b> Issue 333: launching with the Shield's display off
 * never enters {@code SDL_main}. SDL creates its native thread only in
 * {@code handleNativeState}'s RESUMED branch, gated on a ready surface
 * <em>and</em> {@code mIsResumedCalled}, and {@code onStop} clears the latter
 * about 15 ms later. The result is a live process with no sockets, nothing on
 * the network, and nothing for a phone to cast to.
 *
 * <p>The network half never needed the Activity. {@code GdmResponder} and
 * {@code CompanionServer} are sockets and threads with no SDL dependency in
 * either header — what gated them was the entry point they sat behind, not
 * anything they use. This Service is a second entry point that does not sit
 * behind it.
 *
 * <p><b>Same process as the Activity, deliberately.</b> No {@code android:process}
 * attribute, so the handoff when a cast arrives is a mutex in native code rather
 * than IPC across a process boundary — and the two can never be running two
 * copies of the network stack in two address spaces, each convinced it is the
 * player.
 *
 * <p><b>It cannot survive a force-stop, and must not be described as if it
 * could.</b> See D-076. Android disables a force-stopped app's services and
 * receivers as policy; {@code BOOT_COMPLETED} does not fire for one until it is
 * explicitly launched again. This addresses a reboot and Android reclaiming the
 * process, which is two of the three causes issue 338 names in one breath.
 *
 * <p><b>Foreground, and that costs a visible notification.</b> A background
 * service holding sockets is exactly what Android's background execution limits
 * exist to stop; a foreground one is resistant to reclaim rather than immune,
 * and {@code START_STICKY} asks for a restart if it is killed anyway. On a
 * television the notification is close to invisible, but it is a real thing this
 * adds and is named here rather than discovered.
 */
public class HolocronService extends Service {
    private static final String TAG = "holocron";
    private static final String CHANNEL_ID = "holocron.discovery";
    private static final int NOTIFICATION_ID = 1;

    /** Set once the native library is in this process, whoever loaded it. */
    private static boolean sLibraryLoaded = false;

    private int mPort = 0;

    private native int nativeStartNetwork();
    private native void nativeStopNetwork();
    private native void nativeSetDataDirectory(String path);

    /**
     * Load libholocron.so.
     *
     * <p>{@code System.loadLibrary} is idempotent within a process, so this is
     * safe whether the Activity got here first or this Service did — the second
     * call is a no-op rather than a second copy. The flag only avoids the
     * pointless work.
     *
     * <p><b>Not SDL's loader.</b> {@link org.libsdl.app.SDL#loadLibrary} wants an
     * SDLActivity context for its own bookkeeping and there is not one here.
     * The plain call is what is wanted: this Service uses the network half of
     * the library, and SDL's own initialisation belongs to the Activity.
     */
    private static synchronized boolean ensureLibrary() {
        if (sLibraryLoaded) {
            return true;
        }
        try {
            System.loadLibrary("holocron");
            sLibraryLoaded = true;
        } catch (UnsatisfiedLinkError e) {
            Log.e(TAG, "service: libholocron.so did not load", e);
            return false;
        }
        return true;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        startForegroundNotification();

        if (!ensureLibrary()) {
            stopSelf();
            return;
        }

        // WHERE THE CONFIG IS, handed to native code rather than discovered by
        // it. android_entry.cpp asks SDL for this; SDL is not initialised in
        // this process and asking it to be would defeat the purpose of the
        // Service. getExternalFilesDir(null) returns the same directory
        // SDL_GetAndroidExternalStoragePath does.
        File files = getExternalFilesDir(null);
        if (files == null) {
            // No external storage mounted. Nothing here can read a config or
            // write a run log, and starting the network without either would
            // produce a player with an invented identity that can never be a
            // cast target -- issue 308's failure, on the platform where nobody
            // can see a terminal to find out.
            Log.e(TAG, "service: no external files directory, so there is no config -- stopping");
            stopSelf();
            return;
        }
        nativeSetDataDirectory(files.getAbsolutePath());

        mPort = nativeStartNetwork();
        if (mPort == 0) {
            Log.e(TAG, "service: the network did not come up; see the run log");
            stopSelf();
            return;
        }
        Log.i(TAG, "service: discoverable on TCP " + mPort + " with no Activity");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        // START_STICKY: if Android reclaims this under memory pressure, it is
        // restarted. Deliberately NOT START_REDELIVER_INTENT -- there is no
        // command to redeliver, the Service's whole job is to exist.
        return START_STICKY;
    }

    /**
     * Not bound. The Activity and this Service share an address space, so what
     * they need to say to each other is said in native code behind a mutex;
     * a Binder interface would be a second channel saying the same things.
     */
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        if (sLibraryLoaded) {
            nativeStopNetwork();
        }
        super.onDestroy();
    }

    /**
     * The notification a foreground service is required to show.
     *
     * <p>Its text says what is actually true and useful — the box is reachable —
     * rather than "Holocron is running", which on a television tells nobody
     * anything they can act on.
     */
    private void startForegroundNotification() {
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26 /* Android 8.0 (O) */ && manager != null) {
            // IMPORTANCE_LOW: no sound, no heads-up. This is a status, not an
            // event, and a television that pings when it becomes castable would
            // be worse than one that says nothing.
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, "Discovery", NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Keeps the theater castable from Plexamp.");
            manager.createNotificationChannel(channel);
        }

        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CHANNEL_ID)
                : new Notification.Builder(this);

        Notification notification = builder
                .setContentTitle("Holocron")
                .setContentText("Ready to cast to")
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setOngoing(true)
                .build();

        startForeground(NOTIFICATION_ID, notification);
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller

package io.github.roguen.holocron;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
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

    /**
     * A SECOND channel, at high importance, purely so a full-screen intent is
     * honoured. Android only fires one from a notification whose channel is
     * IMPORTANCE_HIGH; the discovery channel is deliberately IMPORTANCE_LOW so
     * that a television does not ping every time the box becomes castable, and
     * raising it would trade a real annoyance for this one moment.
     */
    private static final String CAST_CHANNEL_ID = "holocron.cast";
    private static final int CAST_NOTIFICATION_ID = 2;

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
     * Bring the player up because a cast arrived while nothing was on screen.
     *
     * <p><b>Called from native code by name</b> — {@code launch_player()} looks
     * this method up on the stored Context and calls it if it is there. Renaming
     * it silently disables the cold-cast handoff, because a missing method is
     * the expected case for a Context that is not this Service.
     *
     * <p><b>Why not simply {@code startActivity}.</b> Measured on the Shield:
     * Android refuses it and says so only in the log —
     * {@code W ActivityTaskManager: Background activity start [... isCallingUidForeground:
     * false; callingUidProcState: FOREGROUND_SERVICE; isBgStartWhitelisted: false]}.
     * Android 10 blocks activity starts from the background and <b>being a
     * foreground service is not an exemption</b>. The call returns without
     * throwing and nothing comes up, which is the worst shape a failure can
     * have.
     *
     * <p>A notification carrying a <b>full-screen intent</b> is the sanctioned
     * route — the mechanism alarm clocks and calling apps use, and the one case
     * Android intends to raise an Activity over a sleeping screen. The
     * notification is a side effect rather than the point: on a television
     * nobody reads it, and it is dismissed as soon as the Activity is up.
     */
    public void launchPlayer() {
        // startActivity FIRST, because with SYSTEM_ALERT_WINDOW granted it is
        // exempt from the background-activity-start restriction and is the only
        // mechanism measured to actually work on this device.
        //
        // The notification below is kept as a fallback for the case the appop is
        // NOT granted -- it costs nothing when the start succeeds, and without
        // the grant it is the only thing that could conceivably raise the
        // player. Both are attempted rather than one being chosen, because
        // neither reports failure in a way this code can see: startActivity
        // returns void and the refusal appears only in the platform log.
        try {
            Intent direct = new Intent(this, HolocronActivity.class);
            direct.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(direct);
            Log.i(TAG, "service: startActivity issued");
        } catch (Exception e) {
            Log.e(TAG, "service: startActivity threw", e);
        }

        NotificationManager manager = getSystemService(NotificationManager.class);
        if (manager == null) {
            Log.e(TAG, "service: no NotificationManager, so no way to raise the player");
            return;
        }

        if (Build.VERSION.SDK_INT >= 26 /* Android 8.0 (O) */) {
            // IMPORTANCE_HIGH IS LOAD-BEARING. A full-screen intent on a lower
            // channel is silently downgraded to an ordinary notification, which
            // on a dark television is indistinguishable from nothing happening.
            NotificationChannel channel = new NotificationChannel(
                    CAST_CHANNEL_ID, "Casting", NotificationManager.IMPORTANCE_HIGH);
            channel.setDescription("Brings the picture up when something is cast.");
            // Silent even at high importance: the sound belongs to the music
            // that is about to start, not to the notification announcing it.
            channel.setSound(null, null);
            channel.enableVibration(false);
            manager.createNotificationChannel(channel);
        }

        Intent intent = new Intent(this, HolocronActivity.class);
        // FLAG_ACTIVITY_NEW_TASK is required for an Activity started from a
        // non-Activity context, exactly as it is for the native fallback path.
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);

        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= 23 /* Android 6.0 (M) */) {
            // FLAG_IMMUTABLE is required from API 31 and harmless before it.
            flags |= PendingIntent.FLAG_IMMUTABLE;
        }
        PendingIntent pending = PendingIntent.getActivity(this, 0, intent, flags);

        Notification.Builder builder = Build.VERSION.SDK_INT >= 26
                ? new Notification.Builder(this, CAST_CHANNEL_ID)
                : new Notification.Builder(this);

        Notification notification = builder
                .setContentTitle("Holocron")
                .setContentText("Starting what was cast")
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentIntent(pending)
                // `true` means "this is time-critical", which is what allows the
                // launch over a locked or sleeping screen.
                .setFullScreenIntent(pending, true)
                .setAutoCancel(true)
                .build();

        manager.notify(CAST_NOTIFICATION_ID, notification);
        Log.i(TAG, "service: asked for the player with a full-screen intent");
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

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Collect everything about a Shield that has gone unreachable, BEFORE touching it.
#
# ISSUE 281. Twice now the Shield has been found with a live Holocron process and
# no listening socket, while plex.tv still advertised its address. Both times the
# evidence was thinner than it needed to be, and the second time the reason was
# not subtle: the person investigating force-stopped the app to get it working
# again, which is exactly the act that destroys what there was to look at.
#
# THE ISSUE ALREADY SAID "do not force-stop first". Prose is not a script. This is
# one command that gathers the whole picture in the order that matters, so the
# recovery can be done immediately afterwards without a decision to make under
# time pressure.
#
# ORDER IS LOAD-BEARING and is why this is a script rather than a list:
#
#   1. The process and its threads -- whether SDL_main was ever entered at all is
#      the single fact that separates the two occurrences from each other, and it
#      is only readable while the process is alive.
#   2. The sockets, from /proc/net, filtered by this app's uid. `netstat` is not
#      on the device and `ss` reports the whole system.
#   3. logcat, which is a RING BUFFER and is losing the startup lines while you
#      read this.
#   4. The activity stack, which says what Android thinks the Activity is doing.
#   5. Only then the log files, which are on disk and are the one part that will
#      still be there in an hour.
#
# It never force-stops, never relaunches, and never writes to the device. Nothing
# here changes the state being measured.
#
# Usage, from anywhere in the tree:
#
#   scripts/shield-capture.sh [serial]
#
# With no serial it uses whatever single device adb sees. Everything lands in a
# timestamped directory under the current working directory and the path is
# printed at the end.

set -uo pipefail   # NOT -e: a missing file must not abandon the rest of the capture

package=io.github.roguen.holocron

serial="${1:-}"
if [ -n "$serial" ]; then
    adb_args=(-s "$serial")
else
    adb_args=()
fi

adb() { command adb "${adb_args[@]}" "$@"; }

if ! command -v adb >/dev/null 2>&1; then
    echo "adb is not on PATH" >&2
    exit 1
fi

out="shield-capture-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$out"
echo "capturing into $out"

note() { echo; echo "=== $1"; }

# --- 0. what we are even looking at ----------------------------------------
{
    note "adb devices"
    adb devices -l
    note "date on the device"
    # THE TIMEZONE MATTERS. The 2026-08-11 occurrence had a plex.tv `lastSeen` in
    # UTC against launches recorded in local time, and whether the advert PREDATES
    # the socketless process or was published by it changes which half of the issue
    # is even about Holocron.
    adb shell date
    adb shell date -u
    note "uptime"
    adb shell uptime

    # THE DISPLAY STATE, AND IT IS NOT A CURIOSITY -- IT IS A KNOWN CAUSE.
    #
    # Measured 2026-08-13: launching Holocron while the display is OFF gives
    # onCreate -> onStart -> onResume -> onPause -> onStop in about 15 ms. SDL's
    # gate for creating its native thread needs a ready surface AND
    # mIsResumedCalled, and onStop clears the latter -- so SDL_main is never
    # entered. The result is a live process with no SDLThread, no sockets, no run
    # log and no GDM: exactly issue 281's 2026-08-12 shape.
    #
    # So read this BEFORE concluding anything from an absent SDLThread. Asleep
    # plus no SDLThread is the known cause; awake plus no SDLThread is not.
    note "display and wakefulness -- READ THIS BEFORE JUDGING A MISSING SDLThread"
    adb shell dumpsys power | grep -i -E "mWakefulness=|Display Power"
    adb shell dumpsys display | grep -i -E "mGlobalDisplayState|mScreenState"
    note "build"
    adb shell getprop ro.build.version.release
    adb shell getprop ro.build.version.sdk
    note "installed version"
    adb shell dumpsys package "$package" | grep -E "versionCode|versionName|firstInstallTime|lastUpdateTime"
} > "$out/00-device.txt" 2>&1

# --- 1. the process and its THREADS BY NAME -------------------------------
#
# THE PRESENCE OR ABSENCE OF `SDLThread` IS THE ONE FACT THIS WHOLE SCRIPT EXISTS
# TO ESTABLISH. It says whether SDL's native main was ever entered, which is what
# separates the two known occurrences from each other.
#
# `ps -A -T -o ...,ARGS` DOES NOT ANSWER IT, and the first version of this script
# used exactly that. `ARGS` is the process command line, so every thread prints
# `io.github.roguen.holocron` and the thread NAMES are nowhere in the output. The
# script ran, produced a plausible-looking table, and could not answer its own
# question -- found by using it on a real occurrence.
#
# `/proc/<pid>/task/*/comm` is the thread name. Read directly, because `ps -o CMD`
# on this device's toybox prints the process name there too.
pid=$(adb shell pidof "$package" 2>/dev/null | tr -d '\r' | awk '{print $1}')

{
    note "pid"
    if [ -n "$pid" ]; then
        echo "$pid"
    else
        echo "NO holocron PROCESS AT ALL"
    fi

    note "thread names -- look for SDLThread"
    if [ -n "$pid" ]; then
        MSYS_NO_PATHCONV=1 adb shell "for t in /proc/$pid/task/*; do echo \"\$(basename \$t) \$(cat \$t/comm)\"; done"
    else
        echo "(no process)"
    fi

    note "the verdict on that"
    if [ -z "$pid" ]; then
        echo "NO PROCESS -- neither known occurrence; the app is simply not running"
    elif MSYS_NO_PATHCONV=1 adb shell "cat /proc/$pid/task/*/comm" 2>/dev/null | tr -d '\r' | grep -qx "SDLThread"; then
        echo "SDLThread PRESENT -- native code ran. If there are also no sockets,"
        echo "this is the 2026-08-11 shape, which is the one still unexplained."
    else
        echo "NO SDLThread -- SDL_main was never entered, so no native code ran at all."
        echo "Check the display state in 00-device.txt FIRST: a launch with the display"
        echo "OFF produces exactly this, and that cause is known (see issue 281)."
    fi

    note "elapsed times, for how long ago it started"
    adb shell ps -A -o PID,ETIME,ARGS | grep -E "holocron|PID" || echo "(no process)"
} > "$out/01-process.txt" 2>&1

uid=$(adb shell dumpsys package "$package" 2>/dev/null | sed -n 's/.*userId=\([0-9]*\).*/\1/p' | head -1 | tr -d '\r')
echo "uid=$uid" >> "$out/01-process.txt"

# --- 2. the sockets -------------------------------------------------------
#
# BY uid, because that is the only thing tying a socket to this app -- /proc/net
# gives no pid. Hex uid columns are in decimal here, so the uid is compared as a
# number. Both v4 and v6: the Companion server binds 0.0.0.0 and the kernel may
# list it either way.
{
    note "uid is $uid -- state 0A is LISTEN"
    for f in tcp tcp6 udp udp6; do
        note "/proc/net/$f"
        adb shell cat "/proc/net/$f"
    done
} > "$out/02-sockets-raw.txt" 2>&1

if [ -n "$uid" ]; then
    {
        note "only this app's sockets (uid $uid)"
        for f in tcp tcp6 udp udp6; do
            echo "-- $f"
            adb shell cat "/proc/net/$f" | awk -v u="$uid" 'NR>1 && $8==u'
        done
    } > "$out/02-sockets.txt" 2>&1
else
    echo "could not determine the uid; see 02-sockets-raw.txt" > "$out/02-sockets.txt"
fi

# --- 3. logcat, which is losing what we want while we work ----------------
{
    note "logcat -d, everything"
    adb logcat -d -v time
} > "$out/03-logcat.txt" 2>&1
{
    note "logcat, holocron and SDL only"
    grep -E -i "holocron|SDL|DEBUG|AndroidRuntime|libc" "$out/03-logcat.txt"
} > "$out/03-logcat-filtered.txt" 2>&1

# --- 4. what Android thinks the Activity is doing -------------------------
{
    note "activities"
    adb shell dumpsys activity activities | grep -i -A 4 holocron
    note "processes"
    adb shell dumpsys activity processes | grep -i -B 2 -A 6 holocron
} > "$out/04-activity.txt" 2>&1

# --- 5. the log files, last, because they are the durable part ------------
#
# MSYS_NO_PATHCONV=1: Git Bash on Windows rewrites a leading `/` in an argument
# into a Windows path, so `/sdcard/...` reaches adb mangled.
files_dir="/sdcard/Android/data/$package/files"
{
    note "what is in the app's external files directory"
    adb shell ls -la "$files_dir"
} > "$out/05-files.txt" 2>&1

for f in holocron.log holocron.prev.log holocron.java.log holocron.java.prev.log; do
    if MSYS_NO_PATHCONV=1 adb pull "$files_dir/$f" "$out/$f" >/dev/null 2>&1; then
        echo "pulled $f"
    else
        echo "no $f on the device"
    fi
done

# --- the one line to read first ------------------------------------------
{
    note "was the native side ever running"
    sed -n '/the verdict on that/,/^$/p' "$out/01-process.txt" | grep -v "the verdict on that" | grep -v '^===' | grep -v '^$'

    note "display state at capture time"
    grep -i -E "mWakefulness=|Display Power" "$out/00-device.txt" || echo "(not captured)"

    note "the startup summary from each log, if there is one"
    grep -h "startup --" "$out"/holocron*.log 2>/dev/null || echo "no startup line -- either an older build, or it never got that far"

    note "anything about the listener going"
    grep -h -i "listener is GONE\|listener is back\|not registering" "$out"/holocron*.log 2>/dev/null || echo "nothing"
} > "$out/06-verdict.txt" 2>&1

echo
echo "captured into $out"
echo "read $out/06-verdict.txt first -- it answers, in order: did native code run,"
echo "was the display on, and what did the last complete startup manage to bind."
echo
echo "THE APP HAS NOT BEEN TOUCHED. Recover it however you like now."

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Roguen Keller
#
# scripts/android-apk.sh
#
# Build, sign and (optionally) install Holocron's APK.
#
#   scripts/android-apk.sh              build only
#   scripts/android-apk.sh install      build, then adb install -r
#
# Run it from Git Bash on Windows -- same as android-check.sh and
# setup-git-identity.sh, and for the same reason: there is no WSL on the rack
# machine.
#
# WHY THIS IS NOT GRADLE
#
# The standard way to build an Android app is Gradle plus the Android Gradle
# Plugin, and that was the alternative. It was rejected for this application:
# neither Gradle nor AGP is installed, both come from Google's Maven at build
# time, and they exist to solve problems Holocron does not have -- variants,
# flavours, resource merging across libraries, R8 shrinking of a Java codebase
# that is one class long.
#
# What is actually needed is four tools that the Android SDK build-tools already
# ship, in a fixed order, with no configuration: aapt2, javac, d8, apksigner.
# That is the whole of this file, and it is inspectable in a way a Gradle build is
# not.
#
# REVISIT IT if the app grows a real resource tree, a second Activity, or any
# Java dependency. At that point Gradle is earning its keep and this is not.
#
# SDL'S JAVA IS COPIED FROM THE VCPKG BUILDTREE, NOT VENDORED
#
# SDLActivity and its eleven siblings are Java; vcpkg installs only the compiled
# native library, so the .java files have to come from somewhere.
#
# They are NOT committed to this repo, deliberately. The Java half and the native
# half of SDL must be the same version -- SDLActivity calls sixty-seven native
# entry points by name -- and a vendored copy drifts from whatever vcpkg is
# building the moment the baseline moves, with the failure arriving as an
# UnsatisfiedLinkError at launch on a device rather than as a compile error.
# Copying them out of the tree vcpkg actually built makes that impossible.
#
# The cost is that this needs the buildtree present. If vcpkg has cleaned it,
# `vcpkg install sdl3:arm64-android` puts it back.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

want_install=0
[ "${1:-}" = "install" ] && want_install=1

# ---------------------------------------------------------------------------
# Locate the toolchain.
# ---------------------------------------------------------------------------

local_appdata="${LOCALAPPDATA:-$HOME/AppData/Local}"
sdk="${ANDROID_HOME:-$local_appdata/Android/sdk}"
jdk="${JAVA_HOME:-}"

if [ -z "$jdk" ]; then
    for candidate in "$local_appdata"/Android/jdk-*; do
        [ -d "$candidate" ] && jdk="$candidate" && break
    done
fi

[ -d "$sdk" ]  || { echo "android-apk: no Android SDK at $sdk (set ANDROID_HOME)" >&2; exit 2; }
[ -d "$jdk" ]  || { echo "android-apk: no JDK (set JAVA_HOME)" >&2; exit 2; }

# Newest build-tools present, rather than a pinned version that rots.
build_tools="$(ls -d "$sdk"/build-tools/*/ 2>/dev/null | sort -V | tail -1)"
[ -n "$build_tools" ] || { echo "android-apk: no build-tools under $sdk" >&2; exit 2; }
build_tools="${build_tools%/}"

# THE PLATFORM JAR IS THE NEWEST INSTALLED, NOT THE SHIELD'S OWN LEVEL, and the
# difference matters enough to write down.
#
# The Shield is API 30 and the manifest targets 30. But SDL3's Java refers to
# VibratorManager and Light/LightState, which are API 31, guarded at runtime by
# Build.VERSION checks -- so it COMPILES against 31+ and RUNS on 30. Compiling
# against android-30 gives twenty-five "cannot find symbol" errors on code that
# would never execute on this device.
#
# compileSdk and targetSdk are independent, which is exactly what this relies on:
# compile against the newest, target 30.
platform_jar="$(ls -d "$sdk"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -1)"
[ -n "$platform_jar" ] && [ -f "$platform_jar" ] || {
    echo "android-apk: no android.jar under $sdk/platforms" >&2
    echo "  sdkmanager \"platforms;android-34\"" >&2
    exit 2
}

# -f, NOT -x. The Windows build-tools ship d8 and apksigner as .bat files, and a
# .bat does not carry an executable bit on a filesystem MSYS is looking at -- so
# an -x test silently falls through to the extensionless name, which does not
# exist, and the failure arrives several steps later as "No such file or
# directory" on a tool that is sitting right there.
pick() {
    for candidate in "$@"; do
        if [ -f "$candidate" ]; then echo "$candidate"; return 0; fi
    done
    echo "android-apk: none of these exist: $*" >&2
    exit 2
}

aapt2="$(pick "$build_tools/aapt2.exe" "$build_tools/aapt2")"
aapt="$(pick "$build_tools/aapt.exe" "$build_tools/aapt")"
d8="$(pick "$build_tools/d8.bat" "$build_tools/d8")"
apksigner="$(pick "$build_tools/apksigner.bat" "$build_tools/apksigner")"
zipalign="$(pick "$build_tools/zipalign.exe" "$build_tools/zipalign")"
javac="$(pick "$jdk/bin/javac.exe" "$jdk/bin/javac")"
keytool="$(pick "$jdk/bin/keytool.exe" "$jdk/bin/keytool")"

# WINDOWS TOOLS NEED WINDOWS PATHS. javac and d8 are native Windows programs;
# they cannot open the /c/Users/... form MSYS hands them, and the error they give
# ("file not found") names a path that looks perfectly real in the terminal. Only
# the arguments crossing that boundary are converted -- everything bash itself
# touches stays in MSYS form.
winpath() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else echo "$1"; fi
}

echo "android-apk: sdk         $sdk"
echo "android-apk: build-tools $(basename "$build_tools")"
echo "android-apk: jdk         $(basename "$jdk")"

# ---------------------------------------------------------------------------
# The native library must already be built.
#
# NOT built here, on purpose. This script packages; cmake builds. Doing both
# would mean this file owning a second copy of the configure line, which is the
# kind of duplication that drifts silently.
# ---------------------------------------------------------------------------

abi="arm64-v8a"
so="build/android/lib/$abi/libholocron.so"
if [ ! -f "$so" ]; then
    echo "android-apk: $so is missing." >&2
    echo "  Configure and build for Android first -- see docs/shield.md section 5." >&2
    exit 1
fi
echo "android-apk: native      $so ($(du -h "$so" | cut -f1))"

# ---------------------------------------------------------------------------
# SDL's Java, from the tree vcpkg built.
# ---------------------------------------------------------------------------

vcpkg_root="${VCPKG_ROOT:-$HOME/vcpkg}"
sdl_java=""
for candidate in "$vcpkg_root"/buildtrees/sdl3/src/*/android-project/app/src/main/java; do
    [ -d "$candidate" ] && sdl_java="$candidate" && break
done

if [ -z "$sdl_java" ]; then
    echo "android-apk: SDL's Java sources are not in the vcpkg buildtree." >&2
    echo "  They are not vendored on purpose -- the Java and native halves must be" >&2
    echo "  the same SDL version. Restore them with:" >&2
    echo "      vcpkg install sdl3:arm64-android" >&2
    exit 1
fi
echo "android-apk: sdl java    $(basename "$(dirname "$(dirname "$(dirname "$(dirname "$sdl_java")")")")")"

# ---------------------------------------------------------------------------

out="build/android/apk"
rm -rf "$out"
mkdir -p "$out/compiled" "$out/classes" "$out/gen" "$out/staging/lib/$abi"

# 1. Resources. One flat file per input, then a link step that also emits R.java.
echo "android-apk: aapt2 compile"
"$aapt2" compile --dir android/res -o "$out/compiled/res.zip"

echo "android-apk: aapt2 link"
"$aapt2" link \
    -o "$out/base.apk" \
    --manifest android/AndroidManifest.xml \
    -I "$platform_jar" \
    -R "$out/compiled/res.zip" \
    --java "$out/gen" \
    --auto-add-overlay

# 2. Java. SDL's twelve classes, our one, and the generated R.
echo "android-apk: javac"
find "$sdl_java" android/java "$out/gen" -name '*.java' \
    | while read -r f; do winpath "$f"; done > "$out/sources.txt"
# CAPTURED, NOT PIPED. Piping javac into grep hands the pipeline grep's exit
# status, so a compile that produced twenty-five errors reported success and the
# build carried on to fail three steps later on a missing classes.dex. The status
# is checked before anything is filtered.
if ! "$javac" -nowarn -encoding UTF-8 \
        -source 8 -target 8 \
        -bootclasspath "$(winpath "$platform_jar")" \
        -classpath "$(winpath "$platform_jar")" \
        -d "$(winpath "$out/classes")" \
        "@$(winpath "$out/sources.txt")" > "$out/javac.log" 2>&1; then
    echo "android-apk: javac FAILED" >&2
    grep -E 'error:' "$out/javac.log" | head -20 >&2
    echo "  full log: $out/javac.log" >&2
    exit 1
fi

# 3. Dex.
echo "android-apk: d8"
find "$out/classes" -name '*.class' \
    | while read -r f; do winpath "$f"; done > "$out/classes.txt"
"$d8" --min-api 24 --output "$(winpath "$out")" --lib "$(winpath "$platform_jar")" \
    "@$(winpath "$out/classes.txt")"

# 4. Assemble. The linked base APK is a zip; add the dex and the native library.
echo "android-apk: package"
cp "$out/base.apk" "$out/unsigned.apk"
cp "$so" "$out/staging/lib/$abi/"
cp "$out/classes.dex" "$out/staging/"

# `aapt add`, NOT `zip`. Git Bash on Windows ships no zip, and this is the tool
# the Android SDK provides for the job -- aapt v1, not aapt2, which has no `add`.
#
# Entries are stored under exactly the path given, so this runs from inside the
# staging root: the loader finds the native library by the literal path
# lib/arm64-v8a/libholocron.so and nothing else will do.
(
    cd "$out/staging"
    "$aapt" add -f "../unsigned.apk" "classes.dex" "lib/$abi/libholocron.so" > /dev/null
)

# 5. Align, then sign. In that order: zipalign rewrites offsets and would
# invalidate a signature applied first.
echo "android-apk: zipalign + sign"
rm -f "$out/holocron.apk"
"$zipalign" -f 4 "$out/unsigned.apk" "$out/aligned.apk"

keystore="build/android/debug.keystore"
if [ ! -f "$keystore" ]; then
    echo "android-apk: generating a debug keystore (build/ is gitignored)"
    "$keytool" -genkeypair -v -keystore "$keystore" -storepass android -keypass android \
        -alias holocrondebug -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Holocron Debug, OU=Holocron, O=Holocron, L=, ST=, C=" >/dev/null 2>&1
fi

"$apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias holocrondebug --out "$out/holocron.apk" "$out/aligned.apk"
"$apksigner" verify "$out/holocron.apk" && echo "android-apk: signature verifies"

echo
echo "android-apk: $out/holocron.apk ($(du -h "$out/holocron.apk" | cut -f1))"

if [ "$want_install" = "1" ]; then
    adb_bin="$(command -v adb || true)"
    [ -n "$adb_bin" ] || adb_bin="$sdk/platform-tools/adb.exe"
    echo "android-apk: installing"
    "$adb_bin" install -r "$out/holocron.apk"
    echo "android-apk: launch with"
    echo "  adb shell am start -n io.github.roguen.holocron/.HolocronActivity"
fi

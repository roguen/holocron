#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Roguen Keller
#
# scripts/android-check.sh
#
# Compile every translation unit in src/ for aarch64-linux-android, under the
# project's own warning discipline, and fail if any of them does not build.
#
# WHY THIS EXISTS
#
# M8 adds a THIRD branch to files that already had two. `render_text`,
# `https_client` and `wasapi_sink` are each one translation unit with the
# platform choice made inside it, and the reason given for that arrangement --
# in text_render.cpp's own header comment -- is that Linux CI still reads the
# file even though it takes the other branch. That argument only holds for a
# branch some compiler actually compiles.
#
# Nothing in this project compiles for Android. So an `#elif defined(__ANDROID__)`
# block would be the exact thing the project has a rule against: a path nothing
# can reach on purpose is a path nobody finds out is broken -- the argument
# written on `--no-compositor` and again on the DSA port. This script is what
# makes the Android branch reachable by a compiler on every push.
#
# It is NOT a substitute for building the app. It compiles; it does not link, it
# does not package, and it has never run anything. When the real Android build
# exists this becomes redundant and should be deleted rather than kept as a
# second, weaker check.
#
# IT ALREADY EARNED ITS KEEP, AND THE FIRST THING IT CAUGHT WAS ITSELF.
#
# Before issue 237 was fixed, pointing HOLOCRON_ANDROID_INCLUDE at a real
# arm64-android vcpkg install compiled the WHOLE render library cleanly with
# nothing skipped -- and that was not evidence of anything. `vcpkg.json` pinned
# glad to `gl-api-45`, and a vcpkg feature is not triplet-dependent, so the
# header installed for the Android triplet was the DESKTOP GL 4.5 loader. It
# declared GL_VERSION_4_5, gladLoadGLLoader and the DSA entry points, all of
# which compile anywhere and all of which are null on an ES driver.
#
# src/render/gl_api.hpp fixed that: Android now compiles against the NDK's
# <GLES3/gl32.h>, ES 3.2 core and nothing else. So a green run here now does say
# that every GL name the render library uses exists in ES 3.2 -- which is a real
# claim, and it immediately found two that did not: APIENTRY, which the ES
# headers spell GL_APIENTRY, and GL_BGR in the --shot screenshot path, which ES
# does not have at any version.
#
# WHAT IT STILL DOES NOT SAY: that anything links, packages, or behaves. A
# declaration existing is not a driver implementing it.
#
# NO HAND-MAINTAINED FILE LIST, AND THAT IS DELIBERATE
#
# The obvious shape is a list of the files worth checking. It rots silently: the
# next platform file is added, nobody adds it here, and the check goes on passing
# while covering less. So this walks src/ instead.
#
# The cost of walking is that most translation units need a third-party header --
# toml++, httplib, pocketfft, ebur128, FFmpeg, SDL, glad -- which are not present
# for an Android triplet on an ordinary machine. Those are SKIPPED AND COUNTED,
# never quietly dropped: the script prints what it skipped and why, because a
# check that silently covers three files out of twenty reads exactly like a check
# that covers twenty.
#
# Point HOLOCRON_ANDROID_INCLUDE at a vcpkg arm64-android include directory and
# the skips go away.
#
# USAGE
#   scripts/android-check.sh
#   HOLOCRON_ANDROID_INCLUDE=/path/to/vcpkg/installed/arm64-android/include \
#       scripts/android-check.sh
#
# Run it from Git Bash on Windows -- same as setup-git-identity.sh, and for the
# same reason: there is no WSL on the rack machine.

set -uo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# ---------------------------------------------------------------------------
# Find an NDK.
#
# ANDROID_NDK_LATEST_HOME is set on GitHub's ubuntu runners, which is what makes
# this free in CI -- no download step, no setup action.
# ---------------------------------------------------------------------------

ndk=""
for candidate in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_LATEST_HOME:-}"; do
    if [ -n "$candidate" ] && [ -d "$candidate" ]; then
        ndk="$candidate"
        break
    fi
done

if [ -z "$ndk" ]; then
    # The rack machine's own install, and any sibling version of it.
    for candidate in "${LOCALAPPDATA:-$HOME/AppData/Local}"/Android/android-ndk-*; do
        if [ -d "$candidate" ]; then
            ndk="$candidate"
            break
        fi
    done
fi

if [ -z "$ndk" ]; then
    echo "android-check: no NDK found." >&2
    echo "  Set ANDROID_NDK_HOME, or install one under %LOCALAPPDATA%/Android." >&2
    exit 2
fi

# The prebuilt directory is named for the HOST, not the target.
host=""
case "$(uname -s)" in
    Linux*)            host="linux-x86_64" ;;
    Darwin*)           host="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) host="windows-x86_64" ;;
    *)                 host="linux-x86_64" ;;
esac

cxx="$ndk/toolchains/llvm/prebuilt/$host/bin/clang++"
[ -x "$cxx" ] || cxx="$cxx.exe"
if [ ! -x "$cxx" ]; then
    echo "android-check: no clang++ at $cxx" >&2
    exit 2
fi

# API 30 is the Shield's own level -- Android 11, SDK 30, measured over ADB and
# recorded in docs/shield.md section 3. Checking against a newer platform would
# let a call through that the target does not have.
api=30
target="aarch64-linux-android${api}"

echo "android-check: $("$cxx" --version | head -1)"
echo "android-check: target $target"

# ---------------------------------------------------------------------------
# The same warning discipline the non-MSVC half of CMakeLists.txt applies.
#
# Kept in step by hand, which is a real cost, so it is worth saying why it is
# not read from CMake: getting it from CMake means configuring the project,
# which means resolving every dependency for an Android triplet, which is the
# thing this script exists to avoid needing.
# ---------------------------------------------------------------------------

warnings=(
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wconversion -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Werror
)

includes=(-Iinclude)
if [ -n "${HOLOCRON_ANDROID_INCLUDE:-}" ]; then
    includes+=(-isystem "$HOLOCRON_ANDROID_INCLUDE")
fi

objdir="$(mktemp -d)"
trap 'rm -rf "$objdir"' EXIT

compiled=0
skipped=0
failed=0
skipped_list=""
failed_list=""

# ---------------------------------------------------------------------------
# A missing header from a dependency is a SKIP. A missing header of OURS is a
# failure -- that is a real broken include and exactly what this should catch.
# ---------------------------------------------------------------------------

while IFS= read -r src; do
    obj="$objdir/$(echo "$src" | tr '/' '_').o"
    if out="$("$cxx" --target="$target" -std=c++20 -c "${warnings[@]}" "${includes[@]}" \
                    "$src" -o "$obj" 2>&1)"; then
        compiled=$((compiled + 1))
        continue
    fi

    missing="$(printf '%s\n' "$out" \
        | sed -n "s/.*fatal error: '\([^']*\)' file not found.*/\1/p" | head -1)"

    if [ -n "$missing" ] && [ "${missing#holocron/}" = "$missing" ]; then
        skipped=$((skipped + 1))
        skipped_list="${skipped_list}    $src -- needs $missing"$'\n'
        continue
    fi

    failed=$((failed + 1))
    failed_list="${failed_list}--- $src"$'\n'"$out"$'\n'
done < <(find src -name '*.cpp' | sort)

echo
echo "android-check: $compiled compiled, $skipped skipped, $failed failed"

if [ "$skipped" -gt 0 ]; then
    echo
    echo "skipped (a dependency has no arm64-android build here):"
    printf '%s' "$skipped_list"
fi

# ---------------------------------------------------------------------------
# Every file carrying an __ANDROID__ branch must have been COMPILED, not
# skipped. Without this the script passes on a machine where the platform layer
# happened to be skipped for a missing dependency -- which is the one outcome
# that would make it worthless.
# ---------------------------------------------------------------------------

android_sources="$(grep -rl '__ANDROID__' src --include='*.cpp' 2>/dev/null | sort || true)"
uncovered=""
for src in $android_sources; do
    case "$skipped_list" in
        *"    $src -- "*) uncovered="${uncovered}    $src"$'\n' ;;
    esac
done

if [ -n "$uncovered" ]; then
    echo
    echo "android-check: FAILED -- these carry an __ANDROID__ branch and were skipped," >&2
    echo "so the branch this script exists to compile was not compiled:" >&2
    printf '%s' "$uncovered" >&2
    exit 1
fi

if [ "$failed" -gt 0 ]; then
    echo
    echo "android-check: FAILED" >&2
    printf '%s' "$failed_list" >&2
    exit 1
fi

if [ -n "$android_sources" ]; then
    echo
    echo "covered (carries an __ANDROID__ branch):"
    for src in $android_sources; do echo "    $src"; done
fi

echo
echo "android-check: OK -- these translation units COMPILE for $target."
echo "  What that says: every name they use, GL included, exists in ES 3.2 core."
echo "  What it does not: that anything links, packages, or runs. A declaration"
echo "  existing is not a driver implementing it, and nothing has run on a device."

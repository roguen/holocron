// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// Waking the display when a cast arrives -- issue 338, step 1.
//
// WHAT THIS CAN AND CANNOT CHECK, said plainly, because a test that looks like
// it covers the feature and does not is worse than no test.
//
// The feature is a JNI call into Android's PowerManager. CI has no Android
// device and neither does the rack, so nothing here exercises the wake at all.
// It is confirmed on the Shield or it is not confirmed.
//
// What IS worth pinning is the shape of the contract the caller writes against,
// because that shape is what lets the caller be identical on both destinations
// and it is checkable everywhere: the desktop answer is a value rather than a
// crash, that value is the distinguishable "not an error" one, and every state
// has its own sentence. That last one is not busywork -- to_string's result goes
// into the run log on a device with no terminal, so two states sharing a
// sentence would make a fault report ambiguous in the one place it is read.

#include <holocron/screen_wake.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

using namespace holocron;

TEST_CASE("waking the display off Android is a value, not a failure", "[screen_wake][338]")
{
    // kUnsupported IS NOT AN ERROR and the header says so. The caller runs the
    // same line on the rack and on the Shield and asks the result what happened;
    // a desktop answering "unsupported" must not read as "the wake broke", or
    // every cast on the rack would log a failure that is not one.
#if defined(__ANDROID__)
    // On a device this reaches real JNI. With no VM handed over it is
    // kUnavailable, which is the documented degradation rather than a crash.
    const ScreenWakeState s = wake_screen();
    CHECK((s == ScreenWakeState::kWoken || s == ScreenWakeState::kUnavailable ||
           s == ScreenWakeState::kFailed));
#else
    CHECK(wake_screen() == ScreenWakeState::kUnsupported);
#endif
}

TEST_CASE("asking twice is safe", "[screen_wake][338]")
{
    // The header promises this is cheap enough to call on every cast and safe
    // when the screen is already on, which is what lets the caller skip a
    // "is the display off" query -- an answer that would be stale by the time it
    // was used.
    const ScreenWakeState first  = wake_screen();
    const ScreenWakeState second = wake_screen();
    CHECK(first == second);
}

TEST_CASE("every wake state has its own sentence", "[screen_wake][338]")
{
    const ScreenWakeState all[] = {ScreenWakeState::kWoken, ScreenWakeState::kUnsupported,
                                   ScreenWakeState::kUnavailable, ScreenWakeState::kFailed};

    std::set<std::string> seen;
    for (const ScreenWakeState s : all) {
        const std::string text = to_string(s);
        CHECK_FALSE(text.empty());
        CHECK(text != "unknown");
        seen.insert(text);
    }
    // Distinct, because this lands in the run log on the one box with no
    // terminal and a fault report has nothing else to go on.
    CHECK(seen.size() == 4u);
}

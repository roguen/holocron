// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Roguen Keller
//
// holocron/drag_log.hpp
//
// What a slider did, in one line, once it stopped moving.
//
// -- THE INSTRUMENT WAS DESTROYING THE MEASUREMENT --------------------------
//
// Dragging Plexamp's volume slider is one command per pixel -- 44 of them for a
// single gesture, measured on the rack 2026-08-08 -- so the Companion transcript
// collapses a run of `setParameters` to its FIRST line plus a count of the rest.
// That was the right trade when it was made, and for the same reason the
// timeline polls are collapsed: 415 of 424 requests in one session were polls,
// and printing each one buried the four that mattered.
//
// It also throws away the only number issue 312 needs. The owner reports the
// slider topping out at half its travel; the player's arithmetic and its
// coalescing have both been measured correct against the receiver; what nobody
// has is what the phone SENDS at the top of the drag. That value is the LAST one
// in a run, or the LARGEST -- and a collapse that keeps the first keeps neither.
//
// So the run carries its extremes as it goes. Still one line per gesture, still
// nothing per pixel; the line now says where the drag started, where it ended,
// and how far it reached in each direction. A drag to the top of the travel
// prints its ceiling whatever else it did on the way there.
//
// -- WHY THIS IS A HEADER AND NOT FOUR MEMBERS OF THE SERVER ----------------
//
// So that the test tests it. `render_from_ring` left the audio callback for the
// same reason: a test that reimplements the thing it is checking has
// demonstrated nothing about the thing that runs.
//
// -- WHAT IT DELIBERATELY DOES NOT DO ---------------------------------------
//
// It does not range-check. A value outside 0..100 is refused by the handler and
// still recorded here, because "the phone sent 200" is a finding and a log that
// quietly drops it is the same mistake this file exists to correct.

#pragma once

#include <cstdint>
#include <string>

namespace holocron {

// One unbroken run of slider commands.
//
// Not thread-safe on purpose: the Companion server calls it from cpp-httplib's
// worker threads and holds its own mutex, and a type that locked internally
// would still need that outer lock to make "is this the first command" and "feed
// it" one decision.
class DragRun {
public:
    // A `setParameters` that carried no `volume=`.
    //
    // Shuffle and repeat arrive on the same endpoint and belong in the count --
    // they are part of the run being collapsed -- but they are not a slider and
    // must not colour its numbers.
    void saw_command() noexcept { ++commands_; }

    // A `setParameters` carrying `volume=level`.
    void saw_volume(int level) noexcept
    {
        ++commands_;
        if (levels_ == 0) {
            first_ = level;
            low_   = level;
            high_  = level;
        } else if (level < low_) {
            low_ = level;
        } else if (level > high_) {
            high_ = level;
        }
        last_ = level;
        ++levels_;
    }

    std::uint32_t commands() const noexcept { return commands_; }
    std::uint32_t levels() const noexcept { return levels_; }
    bool          empty() const noexcept { return commands_ == 0; }

    int first() const noexcept { return first_; }
    int last() const noexcept { return last_; }
    int lowest() const noexcept { return low_; }

    // THE ANSWER TO ISSUE 312, once a real drag has been through here. The top
    // of the phone's travel, as the phone reports it, before any scaling.
    int highest() const noexcept { return high_; }

    // The line, or empty when a reader would learn nothing from it.
    //
    // A run of one has already been printed in full by the caller, so summarising
    // it would double every isolated shuffle press -- which is the noise the
    // collapse exists to prevent.
    std::string summary() const
    {
        if (commands_ <= 1) {
            return {};
        }

        std::string line = "... and " + std::to_string(commands_ - 1) +
                           " more setParameters (a slider being dragged)";
        if (levels_ == 0) {
            return line;
        }

        line += " -- volume " + std::to_string(first_) + " to " + std::to_string(last_) +
                ", low " + std::to_string(low_) + ", high " + std::to_string(high_);
        return line;
    }

    void reset() noexcept { *this = DragRun{}; }

private:
    std::uint32_t commands_ = 0;
    std::uint32_t levels_   = 0;
    int           first_    = 0;
    int           last_     = 0;
    int           low_      = 0;
    int           high_     = 0;
};

}  // namespace holocron

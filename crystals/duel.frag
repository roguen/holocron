// SPDX-License-Identifier: GPL-3.0-or-later
//
// duel -- two stick figures fighting, and the clashes land on the beat.
//
// THE IDEA THAT MAKES THIS A CRYSTAL RATHER THAN A FACET
//
// A fight looks like it needs animation state: poses carried frame to frame, a
// choreographer deciding what happens next. It does not. Two things remove the
// need entirely:
//
//   1. FIGURES ARE SIGNED DISTANCE FIELDS. A stick figure is a handful of
//      capsules, and posing it is choosing joint angles. No geometry, no
//      buffers, nothing to remember.
//
//   2. CHOREOGRAPHY IS A HASH OF THE BEAT NUMBER, not remembered state. Beat 41
//      always produces the same move, computed from 41. The fight therefore looks
//      choreographed and varied while being a pure function of the audio frame.
//
// That second point is what makes it survive a hot reload mid-track without the
// figures teleporting, and what makes it identical on two machines given the same
// audio. A crystal that accumulated pose state would have neither property.
//
// WHY THE STRIKE LANDS WHERE IT DOES
//
// `beat_phase` wraps AT the beat, so phase 0 is the moment of impact. The
// wind-up therefore occupies the end of the previous beat and the recovery the
// start of this one -- which means this crystal needs the move for the beat that
// just landed AND the one being wound up for. Both come from hashes, so both are
// free.
//
// This depends on the beat grid actually sitting on the beat, which it did not
// until issue 94 was fixed: the phase used to carry a per-track error of up to a
// fifth of a beat. A pulse tolerates that. Two swords meeting do not.
//
// For the impact itself the crystal uses `onset_strength` rather than the phase
// wrap. The grid is accurate to a few tens of milliseconds and that is fine for
// motion; a flash wants the actual transient, which has no grid to be offset
// from.

#version 450 core

in  vec2 v_uv;
out vec4 frag_colour;

uniform vec2  u_resolution;
uniform float u_time;

// The record's own colours, supplied to every crystal. LINEAR rgb.
uniform vec3  u_palette_primary;
uniform vec3  u_palette_accent;
uniform bool  u_has_art;

// Bound in duel.toml.
uniform float u_beat;        // beat_phase   -- 0 at the impact
uniform float u_beats;       // beat_count   -- which beat, for the choreography
uniform float u_bar;         // bar_phase    -- for the camera drift
uniform float u_onset;       // onset_strength
uniform float u_bass;        // bass_norm
uniform float u_confidence;  // bpm_confidence

const float kPi = 3.14159265359;

// ---------------------------------------------------------------------------
// Distance fields
// ---------------------------------------------------------------------------

float sd_segment(vec2 p, vec2 a, vec2 b, float r)
{
    vec2  pa = p - a;
    vec2  ba = b - a;
    float h  = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

float sd_circle(vec2 p, vec2 c, float r)
{
    return length(p - c) - r;
}

// Smooth union, so joints read as one body rather than as separate sticks meeting
// at a corner.
float smin(float a, float b, float k)
{
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// ---------------------------------------------------------------------------
// Choreography
//
// One hash per beat. Deliberately cheap and deliberately deterministic: the same
// beat index must always give the same move, on every machine and after every
// reload.
// ---------------------------------------------------------------------------

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// A move is three numbers, which is enough to read as a distinct attack:
//   x  how high it strikes      0 low, 1 high
//   y  how far it commits       0 a jab, 1 a full lunge
//   z  which way the body leans
vec3 move_for(float beat)
{
    float a = hash11(beat * 1.7);
    float b = hash11(beat * 3.1 + 11.0);
    return vec3(a, 0.35 + 0.65 * b, (a - 0.5) * 1.4);
}

// Who is striking on this beat. Alternating rather than random: two fighters
// both attacking on the same beat reads as neither reacting to the other, and
// trading blows is what makes it look like a fight.
float attacker_for(float beat)
{
    return mod(beat, 2.0) < 1.0 ? -1.0 : 1.0;
}

// ---------------------------------------------------------------------------
// One figure
//
// `side` is -1 for the fighter on the left and +1 for the one on the right; it
// flips the whole body so both face the middle. `commit` is 0 at rest and 1 at
// full extension, and `move` is the three numbers above.
// ---------------------------------------------------------------------------

// Where the striking hand is, on its own.
//
// SPLIT OUT BECAUSE THE POSE IS A PURE FUNCTION, which is the payoff of the
// hash-based choreography. Knowing the hand's position at any `commit` means the
// crystal can evaluate it at a slightly EARLIER moment and draw the arc between
// -- a motion trail with no history, no buffer and no state. An animation that
// accumulated pose could not do this without storing the last frame.
vec2 hand_of(vec3 move, float commit, float bob, float lean_extra)
{
    float hip_y     = 0.42 + bob;
    float lean      = move.z * 0.10 + commit * 0.12 + lean_extra;
    float shoulder_y = hip_y + 0.30;
    float reach     = mix(0.10, 0.30, move.y) * commit;
    return vec2(0.06 + reach + lean * 0.0, shoulder_y + mix(-0.10, 0.10, move.x));
}

float figure(vec2 p, float side, vec3 move, float commit, float bob, float recoil,
             out vec2 out_hand)
{
    // Mirror into a local frame where the fighter always faces +x.
    //
    // `side` is +1 for a fighter already facing +x -- the one on the LEFT, since
    // it must face right to face the middle -- and -1 for the one on the right.
    // Getting this backwards draws both of them facing outward, which is
    // immediately obvious on screen and was the first thing this crystal did.
    p.x *= side;

    // Thin enough to read as a drawn line. 0.022 was tried first and produced
    // limbs so thick that the smooth unions merged them into a single blob;
    // a stick figure is mostly negative space and the limbs have to be thinner
    // than the gaps between them.
    const float kLimb = 0.012;

    // Feet apart, weight shifting with the lunge. The rear foot slides back as
    // the figure commits, which is what stops a lunge looking like a lean.
    vec2 hip   = vec2(-0.02 * commit, 0.42 + bob);
    vec2 front = vec2(0.08 + 0.17 * commit, 0.0);
    vec2 back   = vec2(-0.10 - 0.13 * commit, 0.0);
    vec2 knee_f = mix(hip, front, 0.5) + vec2(0.045, 0.02);
    vec2 knee_b = mix(hip, back, 0.5) + vec2(-0.03, 0.03);

    // Torso leans into the strike -- and AWAY from one that lands on it.
    //
    // `recoil` is what makes this a fight rather than one figure hitting a post.
    // The defender's head snaps back and its weight goes onto the rear foot at
    // exactly the moment of contact. Without it the struck figure stands calmly
    // through a blow to the head, which is the single thing that most makes an
    // animation look unfinished.
    float lean     = move.z * 0.10 + commit * 0.12 - recoil * 0.09;
    vec2  shoulder = hip + vec2(lean, 0.30 - recoil * 0.02);
    vec2  head     = shoulder + vec2(lean * 0.4 + 0.01 - recoil * 0.03, 0.10 - recoil * 0.012);

    // THE STRIKING ARM. Its reach and height are the move; `commit` is how far
    // through the strike we are. At full commitment the hand is out past the
    // body, which is where the two figures meet.
    //
    // HEIGHTS ARE RELATIVE TO THE SHOULDER, which sits at hip + 0.30. The first
    // version ranged 0.28 to 0.52 -- all of it BELOW the shoulder -- so every
    // move was a downward swing that reached past the opponent's knees. A strike
    // wants to land between the ribs and the head.
    float strike_y = shoulder.y + mix(-0.10, 0.10, move.x);
    float reach    = mix(0.10, 0.30, move.y) * commit;
    out_hand       = vec2(0.06 + reach, strike_y);
    vec2  elbow    = mix(shoulder, out_hand, 0.5) + vec2(0.0, 0.05 * (1.0 - commit));

    // The guard arm comes up as the other extends, which reads as a fighter
    // rather than as a figure waving.
    vec2 guard_hand  = shoulder + vec2(0.10 - 0.04 * commit, 0.06 + 0.05 * commit);
    vec2 guard_elbow = mix(shoulder, guard_hand, 0.5) + vec2(-0.02, -0.04);

    // The smoothing radius has to stay well under the limb spacing, or joints
    // that should be distinct bleed into each other.
    const float kJoint = 0.012;

    float d = sd_circle(p, head, 0.040);

    // A NECK, DRAWN EXPLICITLY. The head used to be held on only by the smooth
    // union overlapping the top of the torso, which works right up until the
    // recoil moves it -- and then the head detaches and floats beside the body.
    // That is what the first version with knockback did, and it is the kind of
    // fault that only a rendered frame shows: the arithmetic is all valid.
    d = smin(d, sd_segment(p, shoulder, head, kLimb * 0.9), kJoint);
    d = smin(d, sd_segment(p, hip, shoulder, kLimb * 1.25), kJoint);

    d = smin(d, sd_segment(p, hip, knee_f, kLimb), kJoint);
    d = smin(d, sd_segment(p, knee_f, front, kLimb), kJoint);
    d = smin(d, sd_segment(p, hip, knee_b, kLimb), kJoint);
    d = smin(d, sd_segment(p, knee_b, back, kLimb), kJoint);

    d = smin(d, sd_segment(p, shoulder, elbow, kLimb), kJoint);
    d = smin(d, sd_segment(p, elbow, out_hand, kLimb * 0.9), kJoint);

    d = smin(d, sd_segment(p, shoulder, guard_elbow, kLimb), kJoint);
    d = smin(d, sd_segment(p, guard_elbow, guard_hand, kLimb * 0.9), kJoint);

    // Hand position back in world space for the clash.
    out_hand.x *= side;
    return d;
}

// ---------------------------------------------------------------------------

void main()
{
    // World space: x aspect-corrected, y from 0 at the floor to about 1 at the
    // top of frame, so the figures can be written in body-height units.
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2  p      = vec2((v_uv.x - 0.5) * aspect, v_uv.y) * 1.25;
    p.y          -= 0.14;   // floor a little above the bottom edge

    // -- timing ------------------------------------------------------------
    //
    // The strike lands at phase 0. Before it, the attacker is winding up for the
    // move belonging to the NEXT beat; after it, recovering from this one.
    float beat  = floor(u_beats);
    float phase = clamp(u_beat, 0.0, 1.0);

    vec3  landed  = move_for(beat);
    vec3  coming  = move_for(beat + 1.0);
    float striker = attacker_for(beat);
    float next    = attacker_for(beat + 1.0);

    // HIT STOP, and it is the most characteristic device in this whole genre.
    //
    // On contact everything HOLDS for a moment before the recovery starts. Two
    // figures moving smoothly through the point of impact reads as them passing
    // through each other; a brief freeze reads as force. It costs one line and it
    // is the difference between motion and a hit.
    //
    // Implemented by warping time rather than by branching on the pose, so
    // everything downstream -- limbs, trail, knockback -- freezes together. A
    // freeze applied to some of those and not others looks like a glitch.
    const float kHold = 0.07;   // fraction of a beat, about 20 ms at 200 BPM
    float t = phase < kHold ? 0.0 : (phase - kHold) / (1.0 - kHold);

    // Commitment over the beat: snap out to full extension at the impact, ease
    // back to guard by the middle, then draw back up for the next one. The
    // asymmetry is what makes it read as a strike rather than a pendulum --
    // fast out, slower back.
    float recover = 1.0 - smoothstep(0.0, 0.42, t);
    float windup  = smoothstep(0.55, 1.0, t);

    // Held to a jab when the tempo is not trusted. bpm_confidence low means the
    // grid is free-running, and a full-blooded lunge landing on nothing looks
    // like the crystal is broken rather than like the estimate is thin.
    float trust = clamp(u_confidence * 2.0, 0.25, 1.0);

    // A slow bob on the bar, plus a shove from the bass. Both fighters share it,
    // so the frame breathes without the two drifting apart.
    float bob = 0.012 * sin(u_bar * 2.0 * kPi) + 0.02 * u_bass;

    // -- the two figures ---------------------------------------------------

    vec2  hand_left  = vec2(0.0);
    vec2  hand_right = vec2(0.0);

    float commit_l = (striker < 0.0 ? recover : 0.0) + (next < 0.0 ? windup * 0.55 : 0.0);
    float commit_r = (striker > 0.0 ? recover : 0.0) + (next > 0.0 ? windup * 0.55 : 0.0);

    vec3 pose_l = striker < 0.0 ? landed : coming;
    vec3 pose_r = striker > 0.0 ? landed : coming;

    // +1 on the left so it faces right, -1 on the right so it faces left. Close
    // enough together that a committed strike actually reaches: max reach is
    // about 0.36 from the root, so at 0.26 apart the hands meet just past centre.
    const float kApart = 0.26;

    // KNOCKBACK. The struck figure is driven back, and it is driven back at the
    // moment of contact rather than gradually -- so it shares the hit-stop.
    float recoil_l = striker > 0.0 ? recover : 0.0;   // right strikes, left recoils
    float recoil_r = striker < 0.0 ? recover : 0.0;

    // CAMERA SHAKE on the transient, not on the phase. A shake tied to the grid
    // fires even on a beat where nothing was actually played; tied to the onset it
    // fires when something hit. Deterministic from u_time so it does not need a
    // random source, and small enough not to become the effect.
    vec2 shake = vec2(sin(u_time * 137.0), cos(u_time * 101.0)) * 0.012 * u_onset;
    p += shake;

    float knock = 0.035;
    float d_l = figure(p - vec2(-kApart - recoil_l * knock, 0.0), 1.0, pose_l,
                       commit_l * trust, bob, recoil_l, hand_left);
    float d_r = figure(p - vec2(kApart + recoil_r * knock, 0.0), -1.0, pose_r,
                       commit_r * trust, bob, recoil_r, hand_right);

    hand_left  += vec2(-kApart - recoil_l * knock, 0.0);
    hand_right += vec2(kApart + recoil_r * knock, 0.0);

    float d = min(d_l, d_r);

    // -- the trail ---------------------------------------------------------
    //
    // The arc the striking hand just swept, drawn by evaluating the pose at an
    // EARLIER commitment and joining the two positions. Possible only because the
    // pose is a pure function -- there is no stored previous frame to read.
    //
    // Only while actually striking. A trail on a hand returning to guard reads as
    // the figure waving.
    float lead   = max(recover, 0.0);
    float trail_a = clamp(lead * trust, 0.0, 1.0);
    float trail_b = clamp((lead - 0.35) * trust, 0.0, 1.0);

    if (trail_a > 0.02) {
        vec2 from_l = hand_of(pose_l, trail_b, bob, 0.0);
        vec2 to_l   = hand_of(pose_l, trail_a, bob, 0.0);
        vec2 from_r = hand_of(pose_r, trail_b, bob, 0.0);
        vec2 to_r   = hand_of(pose_r, trail_a, bob, 0.0);

        // Back into world space, mirroring the same way the figures were.
        from_l.x += -kApart - recoil_l * knock;
        to_l.x   += -kApart - recoil_l * knock;
        from_r.x  = kApart + recoil_r * knock - from_r.x;
        to_r.x    = kApart + recoil_r * knock - to_r.x;

        float smear = 1e9;
        if (striker < 0.0) {
            smear = sd_segment(p, from_l, to_l, 0.006);
        } else {
            smear = sd_segment(p, from_r, to_r, 0.006);
        }
        d = min(d, smear + 0.004);   // slightly inset so it reads as thinner
    }

    // -- colour ------------------------------------------------------------

    vec3 ink   = u_has_art ? pow(clamp(u_palette_primary, 0.0, 1.0), vec3(1.0 / 2.2))
                           : vec3(0.90, 0.92, 0.96);
    vec3 spark = u_has_art ? pow(clamp(u_palette_accent, 0.0, 1.0), vec3(1.0 / 2.2))
                           : vec3(1.00, 0.85, 0.45);

    // Bodies. A hard-ish edge with a LITTLE bloom. The first version used 0.35 of
    // halo and the figures read as neon tubes rather than as drawn lines -- the
    // glow was wider than the limbs it surrounded.
    float body  = 1.0 - smoothstep(0.0, 0.006, d);
    float halo  = exp(-max(d, 0.0) * 90.0) * 0.12;

    vec3 colour = ink * (body + halo);

    // -- the clash ---------------------------------------------------------
    //
    // Where the two hands are closest, flashed by the actual transient rather
    // than by the phase wrap. The grid is accurate to a few tens of milliseconds,
    // which is fine for motion and not for an impact.
    vec2  clash    = mix(hand_left, hand_right, 0.5);
    float near     = length(hand_left - hand_right);
    float meeting  = 1.0 - smoothstep(0.06, 0.34, near);
    float flash    = meeting * u_onset;

    float r = length(p - clash);
    colour += spark * flash * exp(-r * 9.0) * 1.6;

    // A ring travelling out from the impact, so a heavy hit reads as force
    // rather than as a brighter dot.
    float ring = exp(-pow((r - (0.05 + recover * 0.22)) * 22.0, 2.0));
    colour += spark * ring * meeting * u_onset * 0.7;

    // Floor: a soft contact shadow under each fighter, which is most of what
    // stops them looking like they are floating.
    float shade = exp(-abs(p.y) * 26.0) *
                  (exp(-pow((p.x + 0.30) * 3.2, 2.0)) + exp(-pow((p.x - 0.30) * 3.2, 2.0)));
    colour += ink * shade * 0.06;

    // Vignette, so the edges do not compete with the fight.
    float rad = length((v_uv - 0.5) * vec2(aspect, 1.0));
    colour *= 1.0 - 0.55 * rad * rad;

    frag_colour = vec4(colour, 1.0);
}

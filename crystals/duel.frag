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
//      capsules, and posing it is choosing joint positions. No geometry, no
//      buffers, nothing to remember.
//
//   2. CHOREOGRAPHY IS A HASH OF THE BEAT NUMBER, not remembered state. Beat 41
//      always produces the same move, computed from 41. The fight therefore looks
//      choreographed and varied while being a pure function of the audio frame.
//
// That second point is what makes it survive a hot reload mid-track without the
// figures teleporting, and what makes the motion trail possible with no stored
// previous frame -- the pose can simply be evaluated at an earlier moment.
//
// WHY THE STRIKE LANDS WHERE IT DOES
//
// `beat_phase` wraps AT the beat, so phase 0 is the moment of impact. The wind-up
// occupies the end of the previous beat and the recovery the start of this one,
// which means this crystal needs the move for the beat that just landed AND the
// one being wound up for. Both come from hashes, so both are free.
//
// This depends on the beat grid actually sitting on the beat, which it did not
// until issue 94 was fixed. A pulse tolerates a fifth of a beat of error. Two
// figures meeting do not.
//
// THE BANDS DRIVE DIFFERENT PARTS OF THE BODY
//
// Timing alone produces a metronome with arms: it lands on the beat and looks
// identical whatever is playing. Splitting the spectrum is what makes it respond
// to the CHARACTER of a record. Borrowed from stem-separating visualizers; three
// bands is cruder than real stems and costs nothing, because the analysis already
// publishes them.
//
//   bass    picks LEG moves -- kicks, sweeps, jump kicks -- and how far they commit
//   mid     the striking arm and the body lean
//   treble  picks HAND moves, and drives the guard hand's small fast movement

#version 450 core

in  vec2 v_uv;
out vec4 frag_colour;

uniform vec2  u_resolution;
uniform float u_time;

uniform vec3  u_palette_primary;
uniform vec3  u_palette_accent;
uniform bool  u_has_art;

uniform float u_beat;        // beat_phase   -- 0 at the impact
uniform float u_beats;       // beat_count   -- which beat, for the choreography
uniform float u_bar;         // bar_phase
uniform float u_onset;       // onset_strength
uniform float u_confidence;  // bpm_confidence
uniform float u_bass;        // bass_norm
uniform float u_mid;         // mid_norm
uniform float u_treble;      // treble_norm

const float kPi = 3.14159265359;

// -- proportions -------------------------------------------------------------
//
// CONVENTIONAL STICK-FIGURE PROPORTIONS, not human ones. The first version used
// roughly realistic ratios -- a head about a ninth of the height -- and read as a
// wire armature rather than as the drawing everyone has in mind. A stick figure
// has a BIG round head, a short torso and long thin limbs; the head is closer to
// a fifth of the total height.
//
// THE TORSO IS SHORT AND THE ARMS ARE LONG, and both were asked for after seeing
// the first version on the projector. A short body between long limbs is what
// makes a stick figure read as a stick figure rather than as a mannequin, and
// long arms are also what let a punch actually arrive: see kApart.
const float kFootY     = 0.00;
const float kHipY      = 0.46;    // was 0.40 -- torso 0.23 rather than 0.30
const float kShoulderY = 0.69;
const float kHeadY     = 0.845;
const float kHeadR     = 0.105;   // 0.21 diameter against ~0.95 total height
const float kLimb      = 0.0135;
const float kJoint     = 0.010;

// Shoulder to hand, fully extended. Named because three places have to agree
// about it -- the punch, the guard, and the distance the fighters stand apart --
// and they silently disagreed before, which is why nothing connected.
const float kArm       = 0.44;
const float kLeg       = 0.46;

// Move kinds. Deliberately an int rather than a float threshold chain at the use
// site: the poses differ structurally, not by degree.
const int kPunch    = 0;
const int kKick     = 1;
const int kBlock    = 2;
const int kJumpKick = 3;
const int kSweep    = 4;
// A GUARD IS NOT A BLOCK, and collapsing the two was a visible mistake. Every
// fighter that was not attacking used kBlock, which puts both hands against the
// head -- so for most of every bar both figures stood covering their faces, which
// reads as cringing rather than as fighting. kBlock is now only for the instant a
// strike is actually arriving; the rest of the time a fighter is on guard, hands
// forward at chest height where a boxer holds them.
const int kGuard    = 5;

// ---------------------------------------------------------------------------
// Distance fields
// ---------------------------------------------------------------------------

float sd_segment(vec2 p, vec2 a, vec2 b, float r)
{
    vec2  pa = p - a;
    vec2  ba = b - a;

    // A DEGENERATE SEGMENT IS A CIRCLE, NOT A DIVISION BY ZERO.
    //
    // dot(ba, ba) is zero whenever two joints coincide, and 0/0 in GLSL is NaN --
    // which then propagates through every smin and every exp to the output, where
    // it writes as white. The symptom is not a bad limb: it is the ENTIRE FRAME
    // blown to white, on the one beat where some pose happened to collapse a
    // joint. Guarding here rather than hunting the pose that did it, because any
    // future move can produce the same coincidence.
    float len2 = dot(ba, ba);
    if (len2 < 1e-9) {
        return length(pa) - r;
    }

    float h = clamp(dot(pa, ba) / len2, 0.0, 1.0);
    return length(pa - ba * h) - r;
}

float smin(float a, float b, float k)
{
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// Scale a colour so its brightest channel is 1, keeping the hue.
//
// THE FIX FOR FIGURES THAT VANISHED INTO THE BACKGROUND. Tinting them straight
// from the palette made them almost invisible on any record with a dark sleeve --
// and "dominant colour of an album cover" is dark far more often than not.
// Normalising guarantees a visible figure while still saying something about the
// record.
vec3 brighten(vec3 c)
{
    float m = max(max(c.r, c.g), c.b);
    return m > 0.004 ? c / m : vec3(1.0);
}

// ---------------------------------------------------------------------------
// A pose: every joint, in local space, facing +x
//
// EXPLICIT JOINTS RATHER THAN ANGLES. The moves differ in which limb does the
// work -- a kick is the leg, a block is both arms, a sweep is the whole body
// dropping -- and expressing that as joint angles would mean a different set of
// angles per move anyway. Positions also make the motion trail trivial: the hand
// or foot is simply a field to read.
// ---------------------------------------------------------------------------

struct Pose {
    vec2 hip;
    vec2 shoulder;
    vec2 head;
    vec2 elbow_a; vec2 hand_a;   // striking arm
    vec2 elbow_b; vec2 hand_b;   // guard arm
    vec2 knee_f;  vec2 foot_f;   // front leg
    vec2 knee_b;  vec2 foot_b;   // back leg
    vec2 contact;                // where this move would land a blow
};

// `shape.x` is the height of the strike, 0 low to 1 high.
// `shape.y` is how far it commits.
// `shape.z` is the lean.
Pose build_pose(int kind, vec3 shape, float commit, float bob, float recoil, float flick)
{
    Pose s;

    float lift  = 0.0;
    float crouch = 0.0;
    if (kind == kJumpKick) {
        // BOTH FEET LEAVE THE GROUND, which is the whole point of a jump and the
        // thing that makes it read as one rather than as a high kick.
        lift = 0.20 * commit;
    } else if (kind == kSweep) {
        crouch = 0.16 * commit;
    }

    float hip_y = kHipY + bob + lift - crouch;
    float lean  = shape.z * 0.06 + commit * (kind == kKick ? -0.07 : 0.09) - recoil * 0.09;

    s.hip      = vec2(-0.015 * commit, hip_y);
    s.shoulder = s.hip + vec2(lean, kShoulderY - kHipY - crouch * 0.25);
    s.head     = s.shoulder + vec2(lean * 0.35 + 0.008 - recoil * 0.030,
                                   kHeadY - kShoulderY - recoil * 0.012);

    // -- legs ---------------------------------------------------------------

    // A NEUTRAL STANCE FIRST, THEN BLENDED TOWARDS THE MOVE BY `commit`.
    //
    // This is what makes a half-committed move read as a stance rather than as a
    // broken pose. Writing each move's geometry directly meant that a figure
    // winding up -- which only ever reaches about half commitment -- was drawn in
    // the move's FULL shape at a small scale: a jump kick at commit 0.2 had both
    // knees folded and the feet barely off the floor, which reads as sitting down.
    //
    // Blending from a stance also means every move gets its own anticipation for
    // free, and no move can produce a pose that is nonsense at low commitment.
    float ground   = kFootY + lift;
    vec2  n_foot_b = vec2(-0.09 - 0.03 * commit, ground);
    vec2  n_knee_b = mix(s.hip, n_foot_b, 0.5) + vec2(-0.025, 0.02);
    vec2  n_foot_f = vec2(0.07 + 0.03 * commit, ground);
    vec2  n_knee_f = mix(s.hip, n_foot_f, 0.5) + vec2(0.035, 0.015);

    vec2 m_foot_f = n_foot_f;
    vec2 m_knee_f = n_knee_f;
    vec2 m_foot_b = n_foot_b;
    vec2 m_knee_b = n_knee_b;

    // EVERY STRIKE REACHES kLeg, because the distance the figures stand apart is
    // derived from that. Before, each move picked its own extension by eye -- 0.34,
    // 0.40, 0.44 -- and the shortest of them stopped well short of the opponent, so
    // the impact glow appeared in the air between two figures that never touched.
    if (kind == kKick) {
        // Front leg out, roughly horizontal at the height the move asked for.
        m_foot_f = vec2(kLeg, hip_y * mix(0.35, 0.95, shape.x));
        m_knee_f = mix(s.hip, m_foot_f, 0.55) + vec2(0.0, 0.02);
        m_foot_b = vec2(-0.05, ground);
        m_knee_b = mix(s.hip, m_foot_b, 0.5) + vec2(-0.02, 0.0);
    } else if (kind == kJumpKick) {
        // Lead leg driven out HORIZONTALLY at hip height, trailing leg folded up
        // underneath. That pair is the silhouette everyone recognises, and the fold
        // matters more than the extension: without it the figure reads as falling
        // over rather than as attacking.
        m_foot_f = vec2(kLeg + 0.04, hip_y + 0.03);
        m_knee_f = mix(s.hip, m_foot_f, 0.5) + vec2(0.0, 0.035);
        m_knee_b = s.hip + vec2(0.02, -0.11);
        m_foot_b = m_knee_b + vec2(-0.11, 0.035);
    } else if (kind == kSweep) {
        // Low and long, along the ground.
        m_foot_f = vec2(kLeg + 0.06, ground + 0.015);
        m_knee_f = mix(s.hip, m_foot_f, 0.5) + vec2(0.0, -0.01);
        m_foot_b = vec2(-0.11, ground);
        m_knee_b = mix(s.hip, m_foot_b, 0.5) + vec2(-0.05, 0.0);
    }

    // Eased, so the limb accelerates out of the stance rather than moving at a
    // constant rate -- which is most of what separates a strike from a stretch.
    float leg = smoothstep(0.0, 1.0, commit);
    s.foot_f  = mix(n_foot_f, m_foot_f, leg);
    s.knee_f  = mix(n_knee_f, m_knee_f, leg);
    s.foot_b  = mix(n_foot_b, m_foot_b, leg);
    s.knee_b  = mix(n_knee_b, m_knee_b, leg);

    // -- arms ---------------------------------------------------------------

    // A DRAWN-BACK GUARD, not a short arm. An arm that is kLeg long has to go
    // somewhere when it is not extended, and the answer is a deep elbow bend: the
    // hand sits close to the shoulder while shoulder-elbow-hand traces most of
    // kArm. Putting the hand further out instead would have made the guard read
    // as a permanent half-punch.
    // CHEST HEIGHT, NOT FACE HEIGHT. The head sits 0.155 above the shoulder with a
    // 0.105 radius, so anything above about shoulder+0.03 puts the fist inside the
    // skull -- which is why every non-attacking figure looked like it was covering
    // its face rather than holding a guard.
    float guard_x = 0.105 + 0.030 * flick;
    float guard_y = -0.005 + 0.040 * flick;
    float bend    = kArm * 0.34;   // how far the elbow swings off the direct line

    if (kind == kPunch) {
        // REACHES kArm AT FULL COMMITMENT. The old maximum was 0.30 against
        // fighters standing 0.48 apart, so even a perfect punch finished 0.14
        // short of the other body. The minimum was raised as well -- a jab that
        // lands is better than a jab that is technically a different move.
        float reach = mix(kArm * 0.72, kArm, shape.y) * commit;
        s.hand_a    = vec2(0.045 + reach, s.shoulder.y + mix(-0.075, 0.095, shape.x));
        s.elbow_a   = mix(s.shoulder, s.hand_a, 0.5) +
                      vec2(-0.02 * (1.0 - commit), bend * (1.0 - commit) + 0.010);
        s.hand_b    = s.shoulder + vec2(guard_x - 0.03 * commit, guard_y + 0.04 * commit);
        s.elbow_b   = mix(s.shoulder, s.hand_b, 0.5) + vec2(-bend * 0.55, -bend * 0.75);
    } else if (kind == kBlock) {
        // BOTH ARMS UP AND CROSSED IN FRONT OF THE HEAD. Only while a strike is
        // actually arriving -- see kGuard for the rest of the time.
        s.hand_a  = s.shoulder + vec2(0.060 + 0.02 * flick, 0.140);
        s.elbow_a = s.shoulder + vec2(0.075 + bend * 0.55, -0.020);
        s.hand_b  = s.shoulder + vec2(0.018, 0.155);
        s.elbow_b = s.shoulder + vec2(0.045, -0.050);
    } else if (kind == kGuard) {
        // Hands FORWARD at chest height, elbows tucked down and in. The lead hand
        // drifts on the treble so a fighter waiting its turn is never quite still.
        //
        // BOTH hands have to clear the head, and the rear one was the one that did
        // not: at shoulder+0.055 it sat exactly on the jaw, so every waiting
        // fighter appeared to be holding its own face with one hand.
        s.hand_a  = s.shoulder + vec2(0.125 + 0.030 * flick, -0.010 + 0.030 * flick);
        s.elbow_a = s.shoulder + vec2(0.030, -bend * 0.85);
        s.hand_b  = s.shoulder + vec2(0.070, 0.000 + 0.015 * flick);
        s.elbow_b = s.shoulder + vec2(-0.010, -bend * 0.90);
    } else {
        // Legs are doing the work; arms balance, out and slightly back.
        s.hand_a  = s.shoulder + vec2(0.060 - 0.05 * commit, 0.080 + 0.05 * commit);
        s.elbow_a = mix(s.shoulder, s.hand_a, 0.5) + vec2(bend * 0.5, -bend * 0.45);
        s.hand_b  = s.shoulder + vec2(-0.075 - 0.06 * commit, 0.045);
        s.elbow_b = mix(s.shoulder, s.hand_b, 0.5) + vec2(-bend * 0.4, -bend * 0.55);
    }

    // Where this move would connect. The trail and the impact both read it, so
    // each move names its own contact point rather than the caller guessing.
    s.contact = (kind == kPunch || kind == kBlock || kind == kGuard) ? s.hand_a : s.foot_f;
    return s;
}

float draw_pose(vec2 p, Pose s)
{
    float d = length(p - s.head) - kHeadR;

    // A neck, drawn explicitly. The head used to be held on only by the smooth
    // union overlapping the torso, so the recoil detached it and it floated
    // beside the body.
    d = smin(d, sd_segment(p, s.shoulder, s.head, kLimb * 0.9), kJoint);
    d = smin(d, sd_segment(p, s.hip, s.shoulder, kLimb * 1.3), kJoint);

    d = smin(d, sd_segment(p, s.hip, s.knee_f, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.knee_f, s.foot_f, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.hip, s.knee_b, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.knee_b, s.foot_b, kLimb), kJoint);

    d = smin(d, sd_segment(p, s.shoulder, s.elbow_a, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.elbow_a, s.hand_a, kLimb * 0.9), kJoint);
    d = smin(d, sd_segment(p, s.shoulder, s.elbow_b, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.elbow_b, s.hand_b, kLimb * 0.9), kJoint);

    return d;
}

// ---------------------------------------------------------------------------
// Choreography
// ---------------------------------------------------------------------------

vec3 shape_for(float beat)
{
    float a = hash11(beat * 1.7);
    float b = hash11(beat * 3.1 + 11.0);
    return vec3(a, 0.35 + 0.65 * b, (a - 0.5) * 1.4);
}

// Blended half and half with the band character. Pure hash gives variety
// unrelated to the music; pure band-following makes the fight do the same thing
// through a whole section, because a record's band balance barely moves.
vec3 shape_blended(float beat, float bass, float mid, float treble)
{
    float total  = bass + mid + treble + 1e-4;
    float height = clamp(0.5 + 0.9 * (treble - bass) / total, 0.0, 1.0);
    float commit = clamp(0.30 + 1.10 * bass / total, 0.0, 1.0);
    float lean   = (mid / total - 0.33) * 2.2;
    return mix(shape_for(beat), vec3(height, commit, lean), 0.5);
}

// WHICH KIND OF MOVE, biased by the spectrum.
//
// Bass-led material gets legs -- kicks, sweeps, jump kicks -- because a kick is
// the heaviest thing a stick figure can do and low end is weight. Treble-led
// material gets hands, because they are fast.
int kind_for(float beat, float bass, float treble)
{
    float total   = bass + treble + 1e-4;
    float legness = clamp(0.5 + 1.1 * (bass - treble) / total, 0.0, 1.0);
    float pick    = fract(hash11(beat * 2.3 + 7.0) * 0.85 + legness * 0.30);

    if (pick < 0.40) {
        return kPunch;
    }
    if (pick < 0.66) {
        return kKick;
    }
    if (pick < 0.85) {
        return kJumpKick;
    }
    return kSweep;
}

float attacker_for(float beat)
{
    return mod(beat, 2.0) < 1.0 ? -1.0 : 1.0;
}

// ---------------------------------------------------------------------------
// Knockdowns
//
// GOING DOWN AND GETTING BACK UP IS STILL A PURE FUNCTION OF THE BEAT, which is
// the whole reason this crystal has no state. A fall obviously *looks* like
// state -- it lasts across beats and it remembers what happened -- but it does
// not need to be stored, because "was I knocked down recently?" can be answered
// by looking BACKWARDS at a few beats and asking each one the same hash question
// it would have answered at the time.
//
// So the fighter's whole condition at any instant is: walk back kDownBeats
// beats, find the most recent one that knocked me down, and the age of that fall
// is the only variable the pose needs. A hot reload mid-fall lands in exactly
// the same place, and so does a seek.
// ---------------------------------------------------------------------------

const float kDownBeats = 3.0;   // fall, lie there, get up -- then fight again

// Did the strike on `beat` put the defender down?
//
// Deliberately not every landed hit. A fight where someone goes down on every
// other beat is a comedy, and the recovery would swallow the fighting. About one
// beat in six, which at 128 bpm is a knockdown every three seconds or so.
bool knockdown_on(float beat)
{
    return hash11(beat * 5.7 + 3.0) > 0.83;
}

// How long ago this side was knocked down, in beats, or -1 for "on its feet".
//
// Returns the MOST RECENT one, so a second knockdown during a recovery restarts
// the fall rather than being ignored.
float down_age(float side, float beat, float phase)
{
    for (float k = 0.0; k < kDownBeats; k += 1.0) {
        float b = beat - k;
        if (attacker_for(b) == -side && knockdown_on(b)) {
            return k + phase;
        }
    }
    return -1.0;
}

// 0 upright, 1 flat out. Down fast, up slowly -- which is both what happens and
// what reads as weight.
float fall_curve(float age)
{
    if (age < 0.0)  { return 0.0; }
    if (age < 0.32) { return smoothstep(0.0, 0.32, age); }
    if (age < 1.60) { return 1.0; }
    return 1.0 - smoothstep(1.60, 2.60, age);
}

// Draw the limbs in towards the trunk.
//
// A FALL HAS TO BE COMPACT OR IT LEAVES THE FRAME. A stick figure is nearly a
// metre tall in this crystal's units and the screen is about two wide, so a body
// laid out flat from a pivot at the heels reaches the edge and the head goes off
// it -- which is what the first attempt did, and it looked like the figure had
// been deleted rather than knocked over. Curling is also just what a person does
// when they hit the floor.
Pose curl_pose(Pose s, float k)
{
    s.knee_f = mix(s.knee_f, s.hip, k * 0.45);
    s.foot_f = mix(s.foot_f, s.hip, k * 0.40);
    s.knee_b = mix(s.knee_b, s.hip, k * 0.35);
    s.foot_b = mix(s.foot_b, s.hip, k * 0.30);
    s.elbow_a = mix(s.elbow_a, s.shoulder, k * 0.35);
    s.hand_a  = mix(s.hand_a, s.shoulder, k * 0.30);
    s.elbow_b = mix(s.elbow_b, s.shoulder, k * 0.35);
    s.hand_b  = mix(s.hand_b, s.shoulder, k * 0.30);
    s.head    = mix(s.head, s.shoulder, k * 0.12);
    return s;
}

// Spin every joint about a pivot near the heels.
//
// ROTATING THE WHOLE POSE rather than authoring a lying-down pose. A fall is the
// same body at a different angle, and writing separate geometry for it would mean
// a second set of joints to keep in step with the first -- the mistake the move
// blending already exists to avoid.
Pose rotate_pose(Pose s, float ang, vec2 pivot)
{
    float c = cos(ang);
    float sn = sin(ang);
    // Positive `ang` takes the head towards -x, which is AWAY from the opponent.
    // Falling towards the person who just hit you is the wrong picture.
    #define ROT(v) (pivot + vec2((v - pivot).x * c - (v - pivot).y * sn, \
                                 (v - pivot).x * sn + (v - pivot).y * c))
    s.hip = ROT(s.hip);           s.shoulder = ROT(s.shoulder);
    s.head = ROT(s.head);
    s.elbow_a = ROT(s.elbow_a);   s.hand_a = ROT(s.hand_a);
    s.elbow_b = ROT(s.elbow_b);   s.hand_b = ROT(s.hand_b);
    s.knee_f = ROT(s.knee_f);     s.foot_f = ROT(s.foot_f);
    s.knee_b = ROT(s.knee_b);     s.foot_b = ROT(s.foot_b);
    s.contact = ROT(s.contact);
    #undef ROT
    return s;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// One pair, fought inside its own little world
//
// WHY THIS IS A FUNCTION NOW. The crystal used to be one duel written straight
// into main(). Three duels share every line of it and differ only in where they
// stand, how big they are, which part of the spectrum drives them, and where
// their choreography starts -- so all five of those are parameters and the fight
// itself is written once.
//
// `bandw` weights the three bands on the way in, and that is the whole mechanism
// behind "a fighter for each instrument": the bass pair is handed a spectrum in
// which the bass dominates, so kind_for gives it legs and shape_blended gives it
// weight. It is not told to be the bass pair; it hears mostly bass.
//
// `seed` is added to the beat before anything hashes it, which shifts the whole
// choreography -- including who attacks -- without changing the tempo. Two pairs
// on the same grid therefore throw different moves at each other rather than
// mirroring, which is what a shared hash would have given.
//
// `cover` comes back so the caller can composite. Additive alone made the far
// pairs shine THROUGH the near one, which reads as ghosts rather than distance.
// ---------------------------------------------------------------------------

struct Fight {
    vec3  colour;
    float cover;
};

Fight duel_layer(vec2 p, float beat, float phase, vec3 bandw, float seed,
                 vec3 ink_l, vec3 ink_r, vec3 spark)
{
    float bass   = clamp(u_bass * bandw.x, 0.0, 1.4);
    float mid    = clamp(u_mid * bandw.y, 0.0, 1.4);
    float treble = clamp(u_treble * bandw.z, 0.0, 1.4);

    beat += seed;

    // HIT STOP. On contact everything HOLDS for a moment before the recovery
    // starts; smooth motion through the point of impact reads as no impact at
    // all. Applied by warping time so the limbs, the trail and the knockback all
    // freeze together.
    const float kHold = 0.07;
    float t = phase < kHold ? 0.0 : (phase - kHold) / (1.0 - kHold);

    float recover = 1.0 - smoothstep(0.0, 0.42, t);
    float windup  = smoothstep(0.55, 1.0, t);

    // Held back when the tempo is not trusted: a full-blooded jump kick landing
    // on nothing looks like the crystal is broken rather than like the estimate
    // being thin.
    float trust = clamp(u_confidence * 2.0, 0.30, 1.0);

    float striker = attacker_for(beat);
    float next    = attacker_for(beat + 1.0);

    int kind_now  = kind_for(beat, bass, treble);
    int kind_next = kind_for(beat + 1.0, bass, treble);
    vec3 shape_now  = shape_blended(beat, bass, mid, treble);
    vec3 shape_next = shape_blended(beat + 1.0, bass, mid, treble);

    float bob = 0.010 * sin(u_bar * 2.0 * kPi) + 0.018 * bass;

    // Each figure is either the attacker on this beat or the defender, and the
    // defender BLOCKS rather than idling. Winding up for the next beat is layered
    // on top at reduced commitment.
    float commit_l = (striker < 0.0 ? recover : 0.0) + (next < 0.0 ? windup * 0.55 : 0.0);
    float commit_r = (striker > 0.0 ? recover : 0.0) + (next > 0.0 ? windup * 0.55 : 0.0);

    int  kind_l  = striker < 0.0 ? kind_now : (next < 0.0 ? kind_next : kGuard);
    int  kind_r  = striker > 0.0 ? kind_now : (next > 0.0 ? kind_next : kGuard);
    vec3 shape_l = striker < 0.0 ? shape_now : shape_next;
    vec3 shape_r = striker > 0.0 ? shape_now : shape_next;

    // A MOVE AT NO COMMITMENT IS NOT A MOVE. A fighter waiting for its turn was
    // being drawn in the arm shape of the strike it had not started yet, which at
    // commit zero is just hands tucked at the chest -- close enough to a guard to
    // be confusing and far enough to look wrong. Give it an actual guard until it
    // begins.
    if (commit_l < 0.06) { kind_l = kGuard; }
    if (commit_r < 0.06) { kind_r = kGuard; }

    // Only for the instant a strike is arriving. Wider than this and both figures
    // spend most of the bar with their hands against their faces.
    if (striker > 0.0 && recover > 0.50) { kind_l = kBlock; }
    if (striker < 0.0 && recover > 0.50) { kind_r = kBlock; }

    float recoil_l = striker > 0.0 ? recover : 0.0;
    float recoil_r = striker < 0.0 ? recover : 0.0;

    // -- who is on the floor -------------------------------------------------
    //
    // A downed fighter throws nothing. Its move is forced to the guard so the
    // rotated pose is a body with its arms tucked rather than a body frozen
    // mid-kick, and its commitment is zeroed so the blend never leaves the stance.
    float age_l  = down_age(-1.0, beat, phase);
    float age_r  = down_age(1.0, beat, phase);
    float fall_l = fall_curve(age_l);
    float fall_r = fall_curve(age_r);

    if (fall_l > 0.0) { kind_l = kGuard; commit_l = 0.0; recoil_l = 0.0; }
    if (fall_r > 0.0) { kind_r = kGuard; commit_r = 0.0; recoil_r = 0.0; }

    float flick_l = treble * (0.6 + 0.4 * sin(u_time * 11.0 + seed));
    float flick_r = treble * (0.6 + 0.4 * sin(u_time * 11.0 + 2.1 + seed));

    // A FIGHTER STEPS IN TO STRIKE, which is how the reach and the spacing are
    // reconciled. Deriving the gap from the arm alone did make strikes land, but
    // it put the two of them barely a head apart at rest -- standing inside each
    // other's guard all the time, which reads as a shoving match. So they stand a
    // comfortable kApart apart and the attacker LUNGES kLunge as it commits:
    //
    //     contact = -kApart + kLunge + kArm  ==  +kApart   at full commitment
    //
    // which lands the strike on the other root rather than in the air, and leaves
    // them properly spaced for the rest of the beat.
    const float kApart = 0.30;
    const float kLunge = kApart * 2.0 - kArm;   // 0.16 -- exactly closes the gap
    float knock = 0.030;

    float lunge_l = kLunge * clamp(commit_l * trust, 0.0, 1.0);
    float lunge_r = kLunge * clamp(commit_r * trust, 0.0, 1.0);

    // Sliding back along the floor while down. A body that goes over on the spot
    // reads as fainting rather than as being hit. Small, though: a curled body
    // still reaches half its height sideways, and any more than this puts the
    // head off the edge of the frame.
    float root_l = -kApart + lunge_l - recoil_l * knock - fall_l * 0.035;
    float root_r = kApart - lunge_r + recoil_r * knock + fall_r * 0.035;

    Pose pose_l = build_pose(kind_l, shape_l, commit_l * trust, bob, recoil_l, flick_l);
    Pose pose_r = build_pose(kind_r, shape_r, commit_r * trust, bob, recoil_r, flick_r);

    // Curl first, then rotate. The other order rotates a spread-out body and then
    // pulls its limbs towards a hip that has already moved, which folds the figure
    // in the wrong direction.
    //
    // The pivot sits under the hips rather than at the heels: at the heels the
    // body swings through a quarter circle and the head leaves the frame.
    if (fall_l > 0.0) {
        pose_l = rotate_pose(curl_pose(pose_l, fall_l), fall_l * 1.34, vec2(-0.03, 0.13));
    }
    if (fall_r > 0.0) {
        pose_r = rotate_pose(curl_pose(pose_r, fall_r), fall_r * 1.34, vec2(-0.03, 0.13));
    }

    // +1 on the left so it faces right; -1 on the right so it faces left. Getting
    // this backwards draws both facing outward, which is what the first version
    // did.
    float d_l = draw_pose(vec2((p.x - root_l) * 1.0, p.y), pose_l);
    float d_r = draw_pose(vec2((p.x - root_r) * -1.0, p.y), pose_r);

    vec2 contact_l = vec2(root_l + pose_l.contact.x, pose_l.contact.y);
    vec2 contact_r = vec2(root_r - pose_r.contact.x, pose_r.contact.y);

    // -- the trail ---------------------------------------------------------
    //
    // The arc the striking limb just swept, from evaluating the pose at an EARLIER
    // commitment. Possible only because the pose is a pure function; there is no
    // stored previous frame to read.
    float trail = 1e9;
    if (recover > 0.05) {
        float a = clamp(recover * trust, 0.0, 1.0);
        float b = clamp((recover - 0.35) * trust, 0.0, 1.0);
        if (striker < 0.0) {
            Pose from = build_pose(kind_l, shape_l, b, bob, 0.0, flick_l);
            Pose to   = build_pose(kind_l, shape_l, a, bob, 0.0, flick_l);
            trail = sd_segment(vec2(p.x - root_l, p.y), from.contact, to.contact, 0.006);
        } else {
            Pose from = build_pose(kind_r, shape_r, b, bob, 0.0, flick_r);
            Pose to   = build_pose(kind_r, shape_r, a, bob, 0.0, flick_r);
            trail = sd_segment(vec2((p.x - root_r) * -1.0, p.y), from.contact, to.contact, 0.006);
        }
    }

    // -- colour ------------------------------------------------------------

    float body_l = 1.0 - smoothstep(0.0, 0.006, d_l);
    float body_r = 1.0 - smoothstep(0.0, 0.006, d_r);
    float halo_l = exp(-max(d_l, 0.0) * 90.0) * 0.10;
    float halo_r = exp(-max(d_r, 0.0) * 90.0) * 0.10;

    vec3 colour = ink_l * (body_l + halo_l) + ink_r * (body_r + halo_r);

    // The trail takes the striker's colour, so it reads as belonging to a fighter.
    //
    // SOFT AND FAINT. At 0.55 with a hard edge it read as a grey bar stuck to the
    // foot rather than as motion -- a solid object the same width as the limb. A
    // wide falloff and a low weight is what makes it a smear.
    float smear = exp(-max(trail, 0.0) * 55.0);
    colour += (striker < 0.0 ? ink_l : ink_r) * smear * 0.30;

    // -- the clash ---------------------------------------------------------

    // DID THE STRIKE ACTUALLY REACH A BODY? The old test asked whether the two
    // fighters' contact points were near each other, which is a different
    // question and answers yes whenever both happen to be reaching -- so the
    // flash fired for two limbs waving at each other in the gap.
    //
    // This asks the only question that matters: evaluate the DEFENDER'S distance
    // field at the striker's contact point. Negative means the limb is inside the
    // other body. It costs one more pose evaluation and it is the difference
    // between a hit and a light show.
    vec2  strike   = striker < 0.0 ? contact_l : contact_r;
    float into     = striker < 0.0
                         ? draw_pose(vec2((strike.x - root_r) * -1.0, strike.y), pose_r)
                         : draw_pose(vec2(strike.x - root_l, strike.y), pose_l);
    float landed   = 1.0 - smoothstep(-0.01, 0.055, into);

    vec2  clash   = strike;
    float meeting = landed;
    float r       = length(p - clash);

    colour += spark * meeting * u_onset * exp(-r * 9.0) * 1.5;

    float ring = exp(-pow((r - (0.05 + recover * 0.22)) * 22.0, 2.0));
    colour += spark * ring * meeting * u_onset * 0.65;

    // Contact shadows. Most of what stops them looking like they are floating --
    // and they correctly lift away during a jump, because the feet do.
    float shade = exp(-abs(p.y) * 26.0) *
                  (exp(-pow((p.x - root_l) * 3.2, 2.0)) + exp(-pow((p.x - root_r) * 3.2, 2.0)));
    colour += vec3(0.7, 0.75, 0.9) * shade * 0.05;

    Fight f;
    f.colour = colour;
    // Bodies only. The halo, the trail and the impact deliberately do NOT occlude
    // what is behind them -- they are light, and light from a near fight falling
    // across a far one is right.
    f.cover  = max(body_l, body_r);
    return f;
}

// ---------------------------------------------------------------------------

void main()
{
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2  p      = vec2((v_uv.x - 0.5) * aspect, v_uv.y) * 1.22;
    p.y          -= 0.10;

    // Camera shake on the TRANSIENT, not the phase: tied to the grid it would fire
    // on a beat where nothing was played. Applied ONCE, to the whole arena --
    // shaking each pair separately would read as three cameras.
    p += vec2(sin(u_time * 137.0), cos(u_time * 101.0)) * 0.010 * u_onset;

    float beat  = floor(u_beats);
    float phase = clamp(u_beat, 0.0, 1.0);

    // ONE FIGURE WHITE, THE OTHER TINTED FROM THE RECORD -- and the tint is
    // NORMALISED so it cannot be dark. Tinting both straight from the palette made
    // them vanish into the background on any record with a dark sleeve, which is
    // most of them, and telling the two apart was hard even when it worked.
    vec3 ink_l = vec3(0.96, 0.97, 1.00);
    vec3 ink_r = u_has_art
                     ? mix(vec3(1.0), brighten(pow(clamp(u_palette_accent, 0.0, 1.0),
                                                   vec3(1.0 / 2.2))), 0.85)
                     : vec3(1.00, 0.80, 0.42);
    vec3 spark = u_has_art
                     ? brighten(pow(clamp(u_palette_primary, 0.0, 1.0), vec3(1.0 / 2.2)))
                     : vec3(1.00, 0.88, 0.55);

    // THREE PAIRS, ONE PER BAND, AND THE BAND IS WHAT MAKES THEM DIFFERENT.
    //
    // The mid pair is downstage at full size, because mid is where voices and
    // most instruments live and it is the fight you are meant to watch. The bass
    // pair and the treble pair stand further back and to the sides, smaller and
    // dimmer, and they read as depth rather than as clutter.
    //
    // They also fight on the OFF-BEAT -- half a beat behind -- which is the thing
    // that stops three pairs looking like one pair drawn three times. On the grid
    // together they land every blow in unison, which is a chorus line.
    float off = 0.5;
    float ob  = beat + (phase + off >= 1.0 ? 1.0 : 0.0);
    float oph = fract(phase + off);

    // PLACED FOR DEPTH, NOT FOR WIDTH. Three pairs side by side do not fit: the
    // frame is about 2.2 units across, a pair needs 0.9 of it, and the outer two
    // ran off the edges -- a fighter half out of frame reads as a bug. Setting
    // them BACK instead buys the room, and it buys it in the direction that
    // already means "further away": smaller, higher up the frame, dimmer.
    //
    // The near pair reaches ±0.42 including its lunge, and a far pair is at its
    // widest when one of them is on the floor -- a body lying down covers about
    // half its own height sideways. At 0.45 scale the two planes interpenetrated
    // and a downed far fighter read as a lump attached to the near fighter's
    // knee. Smaller and higher separates them, and smaller is the more honest
    // depth cue of the two.
    const vec2  kFarL  = vec2(-0.62, 0.30);
    const vec2  kFarR  = vec2(0.62, 0.30);
    const float kFarSc = 0.34;

    // BOUNDED, BECAUSE A FAR PAIR OCCUPIES A FIFTH OF THE FRAME AND WAS BEING
    // EVALUATED OVER ALL OF IT. Six fighters measured 6.02 ms per frame at 4K
    // against 1.36 for two -- 4.4 times the cost for three times the fighters,
    // because every pixel on screen was solving two distance fields it could not
    // possibly be inside. This is not premature: the figure was measured first,
    // and the Shield at M8 has nothing like this GPU's headroom.
    //
    // The box is deliberately generous. The impact glow falls off as exp(-9r), so
    // cutting it at 0.62 leaves under 0.4 percent -- below what an 8-bit output
    // can even represent, so there is no visible edge to the box.
    vec2 half_box = vec2(0.62, 0.62);
    vec2 box_mid  = vec2(0.0, 0.42 * kFarSc);

    Fight bassf;
    bassf.colour = vec3(0.0);
    bassf.cover  = 0.0;
    if (all(lessThan(abs(p - kFarL - box_mid), half_box))) {
        bassf = duel_layer((p - kFarL) / kFarSc, ob, oph,
                           vec3(1.7, 0.5, 0.25), 37.0, ink_l, ink_r, spark);
    }

    Fight treblef;
    treblef.colour = vec3(0.0);
    treblef.cover  = 0.0;
    if (all(lessThan(abs(p - kFarR - box_mid), half_box))) {
        treblef = duel_layer((p - kFarR) / kFarSc, ob, oph,
                             vec3(0.3, 0.6, 1.8), 91.0, ink_l, ink_r, spark);
    }
    Fight midf    = duel_layer(p, beat, phase,
                               vec3(0.6, 1.6, 0.7), 0.0, ink_l, ink_r, spark);

    // Dimmer AND cooler. Aerial perspective -- distance takes the warmth out of
    // things before it takes the brightness -- and it is what stops the far pairs
    // reading as small figures standing next to you.
    vec3 far = (bassf.colour + treblef.colour) * vec3(0.50, 0.56, 0.70);

    vec3 colour = far * (1.0 - midf.cover) + midf.colour;

    float rad = length((v_uv - 0.5) * vec2(aspect, 1.0));
    colour *= 1.0 - 0.50 * rad * rad;

    // BELT AND BRACES AGAINST A BLOWN FRAME. sd_segment no longer produces NaN,
    // but any future move that divides by something derived from a pose could, and
    // the failure mode is the whole screen going white rather than one wrong limb
    // -- which on a projector in a dark room is genuinely unpleasant. Cheap to
    // check and it says why it is here.
    if (any(isnan(colour)) || any(isinf(colour))) {
        colour = vec3(0.0);
    }

    frag_colour = vec4(colour, 1.0);
}

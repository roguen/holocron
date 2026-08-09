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
const float kFootY     = 0.00;
const float kHipY      = 0.40;
const float kShoulderY = 0.70;
const float kHeadY     = 0.855;
const float kHeadR     = 0.105;   // 0.21 diameter against ~0.96 total height
const float kLimb      = 0.0135;
const float kJoint     = 0.010;

// Move kinds. Deliberately an int rather than a float threshold chain at the use
// site: the poses differ structurally, not by degree.
const int kPunch    = 0;
const int kKick     = 1;
const int kBlock    = 2;
const int kJumpKick = 3;
const int kSweep    = 4;

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

    if (kind == kKick) {
        // Front leg out, roughly horizontal at the height the move asked for.
        m_foot_f = vec2(0.34, hip_y * mix(0.35, 0.95, shape.x));
        m_knee_f = mix(s.hip, m_foot_f, 0.55) + vec2(0.0, 0.02);
        m_foot_b = vec2(-0.05, ground);
        m_knee_b = mix(s.hip, m_foot_b, 0.5) + vec2(-0.02, 0.0);
    } else if (kind == kJumpKick) {
        // Lead leg driven out HORIZONTALLY at hip height, trailing leg folded up
        // underneath. That pair is the silhouette everyone recognises, and the fold
        // matters more than the extension: without it the figure reads as falling
        // over rather than as attacking.
        m_foot_f = vec2(0.40, hip_y + 0.03);
        m_knee_f = mix(s.hip, m_foot_f, 0.5) + vec2(0.0, 0.035);
        m_knee_b = s.hip + vec2(0.02, -0.11);
        m_foot_b = m_knee_b + vec2(-0.11, 0.035);
    } else if (kind == kSweep) {
        // Low and long, along the ground.
        m_foot_f = vec2(0.44, ground + 0.015);
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

    float guard_x = 0.075 + 0.030 * flick;
    float guard_y = 0.055 + 0.045 * flick;

    if (kind == kPunch) {
        float reach = mix(0.10, 0.30, shape.y) * commit;
        s.hand_a    = vec2(0.045 + reach, s.shoulder.y + mix(-0.075, 0.095, shape.x));
        s.elbow_a   = mix(s.shoulder, s.hand_a, 0.5) + vec2(0.0, 0.045 * (1.0 - commit));
        s.hand_b    = s.shoulder + vec2(guard_x - 0.03 * commit, guard_y + 0.04 * commit);
        s.elbow_b   = mix(s.shoulder, s.hand_b, 0.5) + vec2(-0.02, -0.035);
    } else if (kind == kBlock) {
        // BOTH ARMS UP AND CROSSED IN FRONT OF THE HEAD. The defender used to
        // stand in the same guard it uses while idle, so a beat where it was being
        // hit looked identical to a beat where nothing happened.
        s.hand_a  = s.shoulder + vec2(0.055 + 0.02 * flick, 0.135);
        s.elbow_a = s.shoulder + vec2(0.075, 0.035);
        s.hand_b  = s.shoulder + vec2(0.020, 0.150);
        s.elbow_b = s.shoulder + vec2(0.055, 0.010);
    } else {
        // Legs are doing the work; arms balance, out and slightly back.
        s.hand_a  = s.shoulder + vec2(0.055 - 0.05 * commit, 0.075 + 0.05 * commit);
        s.elbow_a = mix(s.shoulder, s.hand_a, 0.5) + vec2(0.02, -0.01);
        s.hand_b  = s.shoulder + vec2(-0.07 - 0.05 * commit, 0.045);
        s.elbow_b = mix(s.shoulder, s.hand_b, 0.5) + vec2(-0.02, -0.02);
    }

    // Where this move would connect. The trail and the impact both read it, so
    // each move names its own contact point rather than the caller guessing.
    s.contact = (kind == kPunch || kind == kBlock) ? s.hand_a : s.foot_f;
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

void main()
{
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2  p      = vec2((v_uv.x - 0.5) * aspect, v_uv.y) * 1.22;
    p.y          -= 0.10;

    float beat  = floor(u_beats);
    float phase = clamp(u_beat, 0.0, 1.0);

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

    int kind_now  = kind_for(beat, u_bass, u_treble);
    int kind_next = kind_for(beat + 1.0, u_bass, u_treble);
    vec3 shape_now  = shape_blended(beat, u_bass, u_mid, u_treble);
    vec3 shape_next = shape_blended(beat + 1.0, u_bass, u_mid, u_treble);

    float bob = 0.010 * sin(u_bar * 2.0 * kPi) + 0.018 * u_bass;

    // Each figure is either the attacker on this beat or the defender, and the
    // defender BLOCKS rather than idling. Winding up for the next beat is layered
    // on top at reduced commitment.
    float commit_l = (striker < 0.0 ? recover : 0.0) + (next < 0.0 ? windup * 0.55 : 0.0);
    float commit_r = (striker > 0.0 ? recover : 0.0) + (next > 0.0 ? windup * 0.55 : 0.0);

    int  kind_l  = striker < 0.0 ? kind_now : (next < 0.0 ? kind_next : kBlock);
    int  kind_r  = striker > 0.0 ? kind_now : (next > 0.0 ? kind_next : kBlock);
    vec3 shape_l = striker < 0.0 ? shape_now : shape_next;
    vec3 shape_r = striker > 0.0 ? shape_now : shape_next;

    // While actually being struck, the defender is blocking whatever else it
    // might have been winding up for.
    if (striker > 0.0 && recover > 0.35) { kind_l = kBlock; }
    if (striker < 0.0 && recover > 0.35) { kind_r = kBlock; }

    float recoil_l = striker > 0.0 ? recover : 0.0;
    float recoil_r = striker < 0.0 ? recover : 0.0;

    // Camera shake on the TRANSIENT, not the phase: tied to the grid it would fire
    // on a beat where nothing was played.
    p += vec2(sin(u_time * 137.0), cos(u_time * 101.0)) * 0.010 * u_onset;

    float flick_l = u_treble * (0.6 + 0.4 * sin(u_time * 11.0));
    float flick_r = u_treble * (0.6 + 0.4 * sin(u_time * 11.0 + 2.1));

    // CLOSE ENOUGH THAT A COMMITTED MOVE ACTUALLY CONNECTS. At 0.30 apart the
    // limbs reached toward each other and stopped short, so the impact glow
    // appeared in empty space between two figures that never touched -- which
    // reads as a lighting effect rather than as a hit. A kick reaches about 0.40
    // from the root, so 0.24 apart means contact just past centre.
    const float kApart = 0.24;
    float knock = 0.030;

    float root_l = -kApart - recoil_l * knock;
    float root_r = kApart + recoil_r * knock;

    Pose pose_l = build_pose(kind_l, shape_l, commit_l * trust, bob, recoil_l, flick_l);
    Pose pose_r = build_pose(kind_r, shape_r, commit_r * trust, bob, recoil_r, flick_r);

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
    //
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

    vec2  clash   = mix(contact_l, contact_r, 0.5);
    float near    = length(contact_l - contact_r);
    float meeting = 1.0 - smoothstep(0.08, 0.40, near);
    float r       = length(p - clash);

    colour += spark * meeting * u_onset * exp(-r * 9.0) * 1.5;

    float ring = exp(-pow((r - (0.05 + recover * 0.22)) * 22.0, 2.0));
    colour += spark * ring * meeting * u_onset * 0.65;

    // Contact shadows. Most of what stops them looking like they are floating --
    // and they correctly lift away during a jump, because the feet do.
    float shade = exp(-abs(p.y) * 26.0) *
                  (exp(-pow((p.x - root_l) * 3.2, 2.0)) + exp(-pow((p.x - root_r) * 3.2, 2.0)));
    colour += vec3(0.7, 0.75, 0.9) * shade * 0.05;

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

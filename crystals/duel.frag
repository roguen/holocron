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
//   bass    picks LEG moves -- kicks, sweeps, knees -- and how far they commit
//   mid     the striking arm and the body lean
//   treble  picks HAND moves, and drives the guard hand's small fast movement
//
// -- what the second pass at this changed, and why -----------------------------
//
// FOUR THINGS WERE ASKED FOR AFTER THE FIRST VERSION WAS SEEN, and each of them
// turned out to need a structural answer rather than a tweak.
//
// NO NECKS. The head used to be held on by an explicit shoulder-to-head segment,
// because the recoil moved the head far enough to detach it from the torso. A
// stick figure has no neck: the head sits ON the shoulders. So the head is lower
// -- its centre is 0.8 of a radius above the shoulder, which puts the shoulder
// joint INSIDE the skull and makes the union unbreakable no matter what the
// recoil does. The knock-on cost is that every hand position near the face had to
// move: a guard at chest height was fine when the jaw was 0.155 up and is inside
// the head when it is 0.078 up. See kGuardX.
//
// A BLOW MUST NOT BE BLOCKED WHEN NONE WAS THROWN. The old code put the defender
// into a block whenever the striker was inside the recovery window, which was
// wrong in two separate ways. It blocked while the striker was flat on the floor
// and had thrown nothing at all, and it blocked EVERY strike, so a block carried
// no information. Both are fixed by deciding the outcome of the exchange in the
// choreography -- see outcome_for -- and by working out who is down BEFORE
// working out who is attacking.
//
// MOVES ARE A TABLE OF NUMBERS NOW, not geometry written per move. The first
// version had five moves as five branches of hand-placed joint positions, and
// adding a sixth meant writing another one and getting its reach to agree with
// the other five by eye -- which they did not, so the shortest strike stopped
// short of the opponent. `move_for` returns limb TARGETS and body parameters;
// `build_pose` blends the stance towards them and is written once. Twenty-six
// moves cost the same per pixel as five did, because the move is uniform across
// the figure so the branch is coherent across the warp.
//
// THE FIGHT RANGES ACROSS THE STAGE. Both fighters share an offset that moves
// them together, driven by who has been landing -- see stage_x. Two figures
// trading blows on one spot reads as a machine; ground being won and lost is most
// of what a fight looks like from the back of a room.

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
const float kHipY      = 0.46;
const float kShoulderY = 0.69;

// THE HEAD SITS ON THE SHOULDERS, WITH NO NECK BETWEEN THEM.
//
// Its centre is 0.8 of a radius above the shoulder joint, which puts the joint
// 0.02 INSIDE the skull. That margin is what replaces the neck segment: the
// recoil moves the head by at most 0.030 in x and 0.012 in y, which leaves the
// shoulder 0.070 from the head centre against a radius of 0.098 -- still inside,
// so the union cannot come apart. The old fix for a detaching head was to draw a
// neck, which worked and was the wrong drawing.
const float kHeadR     = 0.098;
const float kHeadY     = kShoulderY + kHeadR * 0.80;

// THICKER THAN THE FIRST VERSION, which was asked for after seeing it on the
// projector -- 0.0135 of a 0.87-tall figure is a hairline at the back of a room,
// and a stick figure is a drawing made with a marker rather than with a pen.
const float kLimb      = 0.0175;
const float kJoint     = 0.012;

// Ankle to toe. A stick figure without feet reads as balancing on points, and it
// is also the only thing that says which way a planted foot is facing.
const float kFootLen   = 0.055;

// Shoulder to hand, and hip to foot, fully extended. Named because everything
// has to agree about them -- every strike, the guard, the distance the fighters
// stand apart -- and they silently disagreed before, which is why nothing
// connected. Every target in the move table is CLAMPED to these; see reach_to.
const float kArm       = 0.44;
const float kLeg       = 0.46;

// How far forward a guard hand sits.
//
// NAMED BECAUSE THE LOWERED HEAD MOVED IT. A hand at 0.105 forward and chest
// height was clear of the old jaw at shoulder+0.155 and is buried in the new
// skull at shoulder+0.078. At 0.145 it clears the head by 0.057, which is more
// than the hand's own radius, so the two never merge.
const float kGuardX    = 0.145;

// -- what a fighter can do ---------------------------------------------------
//
// Deliberately ints rather than a float threshold chain at the use site: the
// poses differ structurally, not by degree. Grouped, because kind_for picks a
// GROUP from the spectrum and then a member of it from a hash -- which is what
// makes "bass gets legs" a real statement about the music rather than a bias
// buried in one threshold.

// Hands. Boxing, karate, Muay Thai.
const int kJab        = 0;
const int kCross      = 1;
const int kHook       = 2;
const int kUppercut   = 3;
const int kElbow      = 4;
const int kBackfist   = 5;
const int kChop       = 6;
const int kHammer     = 7;
const int kHandFirst  = 0;
const int kHandLast   = 7;

// Legs. Karate, taekwondo, Muay Thai, capoeira, wrestling.
const int kFrontKick  = 8;
const int kRoundhouse = 9;
const int kSideKick   = 10;
const int kAxeKick    = 11;
const int kSpinKick   = 12;
const int kJumpKick   = 13;
const int kKnee       = 14;
const int kFlyKnee    = 15;
const int kSweep      = 16;
const int kLowKick    = 17;
const int kDropkick   = 18;
const int kLegFirst   = 8;
const int kLegLast    = 18;

// Throws. Judo, freestyle wrestling, and the professional-wrestling repertoire.
//
// THESE ARE THE ONLY MOVES THAT POSE BOTH FIGHTERS. Everything above is one body
// doing something and the other body reacting to it; a throw is a single shape
// made of two people, and the victim's joints have no meaning except relative to
// the hands holding it. See build_victim.
const int kHipToss    = 19;
const int kSuplex     = 20;
const int kClothesline = 21;
const int kTakedown   = 22;
const int kThrowFirst = 19;
const int kThrowLast  = 22;

// Getting somewhere rather than hitting something. These land no blow, which is
// deliberate: a fight in which every beat connects has no shape to it.
const int kSlide      = 23;
const int kCartwheel  = 24;
const int kBackflip   = 25;
const int kMoveFirst  = 23;
const int kMoveLast   = 25;

// Everything below this throws nothing, which is the test the exchange logic
// needs rather than a list of the moves that happen to be harmless.
const int kNoBlowFirst = 23;

// Not attacking.
//
// A GUARD IS NOT A BLOCK, and collapsing the two was a visible mistake. Every
// fighter that was not attacking used the block, which puts both hands against
// the head -- so for most of every bar both figures stood covering their faces,
// which reads as cringing rather than as fighting. kBlock is now only for a
// strike that the choreography says was actually blocked; kDuck and kLean are for
// one it says was evaded; kGuard is the rest of the time.
const int kGuard      = 26;
const int kBlock      = 27;
const int kDuck       = 28;
const int kLean       = 29;

// Where a move would connect. Named rather than inferred, because an elbow
// strike and a knee strike connect with the JOINT and not with the end of the
// limb, and guessing "the far end of the working limb" puts the contact point on
// a tucked hand behind the fighter's own shoulder.
const int kAtNone  = 0;
const int kAtHand  = 1;
const int kAtFoot  = 2;
const int kAtElbow = 3;
const int kAtKnee  = 4;

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

// A direction, safely.
//
// normalize() of a zero vector is NaN, and a pose that happens to put two joints
// on top of each other is exactly the case the foot direction has to survive --
// see the note in sd_segment about what one NaN does to the frame.
vec2 dir_or(vec2 v, vec2 fallback)
{
    float l = length(v);
    return l > 1e-5 ? v / l : fallback;
}

// Push a joint out of the head.
//
// THE KNOCK-ON COST OF REMOVING THE NECK, AND IT IS NOT A SMALL ONE. With the
// skull sitting ON the shoulders there is no room beside or above the shoulder
// joint that is not head: a guard hand at chest height was 0.163 from the old jaw
// and is 0.113 from the new one, against a radius of 0.098. So every hand
// position inherited from the version with a neck was inside the head, and the
// first render of this pass had four of six fighters apparently holding their own
// faces -- which is exactly the symptom an earlier comment recorded and thought
// it had fixed for good.
//
// Fixing the offenders one at a time is a game of whack-a-mole across twenty-six
// moves, and it only covers the moves that exist today. Pushing any joint that
// lands inside the skull back out along its own radius fixes all of them at once,
// including the ones not written yet, and it costs a length and a compare.
//
// The fallback direction is forward and slightly down -- towards where a fist
// would actually be -- for the degenerate case of a joint exactly at the centre
// of the head.
vec2 clear_head(vec2 v, vec2 head, float r)
{
    vec2  d = v - head;
    float l = length(d);
    return l < r ? head + dir_or(d, vec2(0.94, -0.34)) * r : v;
}

// A limb target, clamped to what the limb can actually reach.
//
// THE SAFETY NET FOR THE WHOLE MOVE TABLE. Twenty-six moves name their own
// targets by eye, and the failure mode of one that asks for too much is a limb
// that visibly stretches -- which reads as a bug rather than as a strike, and
// which no amount of staring at the table finds. Saturating is always the right
// answer: a move that wants more reach than the arm has should look like a
// fully-extended arm.
vec2 reach_to(vec2 v, float limit)
{
    float l = length(v);
    return l > limit ? v * (limit / l) : v;
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
// A move, as parameters rather than as geometry
//
// WHY THIS IS A TABLE AND NOT A BRANCH PER MOVE. The moves differ in which limb
// does the work and where it goes; everything else about them -- blending out of
// the stance, easing, the arms balancing when the legs are busy, clamping the
// reach -- is identical and was being written out five times. It disagreed with
// itself five ways, which is how the first version ended up with strikes that
// stopped short of the opponent.
//
// TARGETS ARE RELATIVE TO THE JOINT THE LIMB HANGS FROM: hand and elbow from the
// shoulder, foot and knee from the hip. That is what makes them survive a crouch
// or a jump without being restated -- the root moves and the limb goes with it.
// The one exception is the NEUTRAL stance, whose feet are absolute, because a
// planted foot stays on the floor when the hips drop. Both of those are right and
// they are right for different reasons.
// ---------------------------------------------------------------------------

struct Move {
    int   contact;   // which joint lands the blow
    int   limb;      // 0 none, 1 lead arm, 2 lead leg, 3 both legs
    vec2  hand;      // from the shoulder
    vec2  elbow;
    vec2  foot;      // from the hip
    vec2  knee;
    float lift;      // hips off the ground
    float crouch;
    float lean;      // on top of the lean the shape asks for
    // A TILT, NOT A PIROUETTE, and the difference cost a render to learn. The
    // first pass gave the spinning backfist 0.95 radians and the spinning back
    // kick 1.20, reasoning that a spinning move should spin. In a side-on
    // silhouette a picture-plane rotation is not a turn about the spine -- it is
    // the figure FALLING OVER, and both of them read as exactly that.
    //
    // A turn about the vertical axis is not expressible here at all, so what sells
    // a spinning strike is the limb arriving from behind on an arc, which the
    // motion trail already draws, plus enough torque in the body to say the hips
    // went first. That is a fifth of a radian, not a whole one. Big rotations are
    // reserved for moves whose bodies really are horizontal or inverted -- the
    // dropkick, the cartwheel, the flip.
    float spin;      // radians of body tilt, out and back with the commit
    float flip;      // whole revolutions, driven monotonically -- see build_pose
    float travel;    // forward drive, beyond the lunge
    float fold;      // trailing leg tucked up under the body
};

Move move_for(int kind, vec3 shape)
{
    // Neutral. Anything a move does not mention it does not do, which is what
    // keeps each entry below to the two or three numbers that are actually the
    // move rather than a restatement of a whole body.
    Move m;
    m.contact = kAtNone;
    m.limb    = 0;
    m.hand    = vec2(kGuardX, -0.010);
    m.elbow   = vec2(0.030, -kArm * 0.29);
    m.foot    = vec2(0.0);
    m.knee    = vec2(0.0);
    m.lift    = 0.0;
    m.crouch  = 0.0;
    m.lean    = 0.0;
    m.spin    = 0.0;
    m.flip    = 0.0;
    m.travel  = 0.0;
    m.fold    = 0.0;

    // `shape.x` is the height the strike is aimed at, 0 low to 1 high. Every
    // strike that can sensibly vary its height reads it, which is most of what
    // stops the same move looking the same twice.
    float high = shape.x;

    // -- hands ---------------------------------------------------------------

    if (kind == kJab) {
        // The lead hand, straight and fast. Nothing else moves much: that is
        // what makes it read as a jab rather than as a committed punch.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.41, mix(-0.02, 0.10, high));
        m.elbow = vec2(0.20, 0.020);
        m.lean  = 0.15;
    } else if (kind == kCross) {
        // The rear hand, with the whole body behind it -- more lean, more reach,
        // and the shoulder turns over.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(kArm, mix(-0.04, 0.08, high));
        m.elbow = vec2(0.22, 0.010);
        m.lean  = 0.55;
        m.spin  = -0.16;
    } else if (kind == kHook) {
        // COMES AROUND, WHICH MEANS THE ELBOW IS THE TELL. A hook drawn as a
        // shorter straight punch is just a worse jab; what makes it a hook is a
        // high wide elbow with the forearm horizontal across the front.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.30, mix(0.02, 0.15, high));
        m.elbow = vec2(0.17, 0.115);
        m.lean  = 0.35;
        m.spin  = -0.26;
    } else if (kind == kUppercut) {
        // Rising from underneath, elbow low and close. Aimed at the jaw, so it
        // ignores `high` -- an uppercut to the body is a different punch.
        //
        // FORWARD AS WELL AS UP. At 0.23 forward the forearm was near vertical
        // and the fist finished above the figure's own head, which reads as an
        // arm raised in celebration rather than as a punch travelling anywhere.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.305, 0.190);
        m.elbow = vec2(0.150, -0.055);
        m.lean  = 0.30;
        m.crouch = 0.030;   // drops to load it, which is where the power is
    } else if (kind == kElbow) {
        // MUAY THAI. The contact is the ELBOW, and the hand comes back rather
        // than going anywhere -- which is why contact is named per move instead
        // of being taken as the end of the working limb.
        //
        // The fist pulls DOWN to the chest rather than back past the ear, which
        // is where it went first: past the ear is inside the skull now that there
        // is no neck, and the whole forearm disappeared into the head.
        m.limb = 1; m.contact = kAtElbow;
        m.hand  = vec2(0.030, -0.105);
        m.elbow = vec2(0.275, 0.090);
        m.lean  = 0.60;
    } else if (kind == kBackfist) {
        // Spinning backfist. The arm comes ACROSS with a high elbow and the back
        // of the fist leading, and the trail is what says it arrived on an arc.
        // See the note on Move.spin for why the body barely turns.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.375, mix(0.06, 0.16, high));
        m.elbow = vec2(0.150, 0.160);
        m.spin  = 0.26;
        m.lean  = -0.35;
    } else if (kind == kChop) {
        // Karate shuto -- a knife-hand coming down diagonally onto the
        // collarbone. High elbow, hand descending.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.32, mix(0.12, 0.24, high));
        m.elbow = vec2(0.185, 0.170);
        m.lean  = 0.40;
    } else if (kind == kHammer) {
        // Hammerfist, straight down. The most compact hand strike there is, and
        // the one that reads best against a fighter already bent over.
        m.limb = 1; m.contact = kAtHand;
        m.hand  = vec2(0.255, 0.205);
        m.elbow = vec2(0.165, 0.195);
        m.lean  = 0.45;

    // -- legs ----------------------------------------------------------------

    } else if (kind == kFrontKick) {
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.43, mix(-0.06, 0.15, high));
        m.knee = vec2(0.235, 0.030);
        m.lean = -0.55;    // the body goes back as the leg goes out
    } else if (kind == kRoundhouse) {
        // ARCS IN FROM THE SIDE, and the KNEE HIGH ABOVE THE FOOT'S LINE is what
        // says so.
        //
        // The first pass put the knee at 0.135 and the roundhouse, the front kick
        // and the side kick were three drawings of a leg sticking out -- the
        // spectrum picked between them and nobody could have told which arrived.
        // A roundhouse folds up and comes over the top, so the knee leads at
        // nearly the height of the target and the shin swings down through it.
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.395, mix(0.06, 0.27, high));
        m.knee = vec2(0.200, 0.215);
        m.lean = -0.85;
        m.spin = -0.22;
    } else if (kind == kSideKick) {
        // THE BODY LEANS HARD AWAY AND THE LEG IS STRAIGHT. Those two together
        // are the whole silhouette, and they are what separate it from the front
        // kick -- the knee sits ON the line from hip to foot instead of above it,
        // so there is no fold in the leg at all.
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.445, mix(-0.05, 0.11, high));
        m.knee = vec2(0.250, 0.030);
        m.lean = -1.70;
    } else if (kind == kAxeKick) {
        // Taekwondo. The leg goes UP and comes down on the target, so this is
        // the one strike whose contact is above the head it is aimed at.
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.285, 0.355);
        m.knee = vec2(0.215, 0.235);
        m.lean = -0.45;
    } else if (kind == kSpinKick) {
        // Spinning back kick -- the hips go first and the heel arrives last. Like
        // the backfist, the turn is carried by the trail rather than by rotating
        // the body over; see the note on Move.spin.
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.435, mix(0.02, 0.19, high));
        m.knee = vec2(0.230, 0.135);
        m.spin = 0.30;
        m.lean = -1.05;
    } else if (kind == kJumpKick) {
        // BOTH FEET LEAVE THE GROUND, which is the whole point of a jump and the
        // thing that makes it read as one rather than as a high kick. The
        // trailing leg folding up underneath matters more than the extension:
        // without it the figure reads as falling over rather than as attacking.
        m.limb = 2; m.contact = kAtFoot;
        m.foot   = vec2(0.44, 0.105);
        m.knee   = vec2(0.255, 0.105);
        m.lift   = 0.20;
        m.fold   = 1.0;
        m.travel = 0.045;
    } else if (kind == kKnee) {
        // Muay Thai knee. Contact is the KNEE; the foot tucks back under, which
        // is the opposite of a kick and looks wrong if the foot leads.
        m.limb = 2; m.contact = kAtKnee;
        m.knee = vec2(0.235, 0.160);
        m.foot = vec2(0.135, -0.095);
        m.lean = 0.35;   // leans IN, because a knee is thrown at close range
    } else if (kind == kFlyKnee) {
        m.limb = 2; m.contact = kAtKnee;
        m.knee   = vec2(0.275, 0.205);
        m.foot   = vec2(0.105, -0.020);
        m.lift   = 0.225;
        m.fold   = 0.55;
        m.lean   = 0.25;
        m.travel = 0.060;
    } else if (kind == kSweep) {
        // Low and long along the ground, taking the other one's feet away. The
        // crouch is what buys the horizontal reach: dropping the hip 0.17 turns
        // a leg that could only reach 0.35 forward at standing height into one
        // that reaches most of kLeg.
        m.limb = 2; m.contact = kAtFoot;
        m.crouch = 0.175;
        m.foot   = vec2(0.40, -0.265);
        m.knee   = vec2(0.235, -0.185);
        m.lean   = -0.25;
    } else if (kind == kLowKick) {
        // Calf kick. Short, heavy, and aimed at the leg -- the strike that reads
        // as wearing somebody down rather than as trying to finish them.
        m.limb = 2; m.contact = kAtFoot;
        m.foot = vec2(0.385, -0.235);
        m.knee = vec2(0.235, -0.095);
        m.lean = -0.30;
    } else if (kind == kDropkick) {
        // BOTH LEGS, BODY HORIZONTAL. The wrestling one, and the reason `limb`
        // has a "both" value at all: mirroring the front leg onto the back is
        // the difference between a dropkick and a very committed side kick.
        m.limb = 3; m.contact = kAtFoot;
        m.foot   = vec2(0.42, 0.045);
        m.knee   = vec2(0.235, 0.055);
        m.lift   = 0.185;
        m.spin   = 1.30;
        m.travel = 0.055;

    // -- throws --------------------------------------------------------------
    //
    // These are only the THROWER. The body being thrown is built by build_victim
    // in this figure's own coordinate space, because the two of them are one
    // shape for the duration.

    } else if (kind == kHipToss) {
        // Judo o-goshi. Drop the hips under, turn in, and haul across -- so the
        // crouch and the turn are the move, and the arms end up high and behind
        // where the other body is going.
        m.limb  = 0; m.contact = kAtNone;
        m.crouch = 0.165;
        m.hand   = vec2(0.055, 0.215);
        m.elbow  = vec2(0.165, 0.055);
        m.lean   = 0.80;
        m.spin   = 0.36;
    } else if (kind == kSuplex) {
        // ARCHES BACKWARDS, and that is the entire silhouette. The thrower's own
        // rotation is why this needs a real spin where the striking moves do not:
        // its body genuinely is going past horizontal.
        m.limb  = 0; m.contact = kAtNone;
        m.hand   = vec2(0.075, 0.185);
        m.elbow  = vec2(0.145, 0.020);
        m.lift   = 0.090;
        m.spin   = 1.10;
        m.lean   = -0.85;
    } else if (kind == kClothesline) {
        // A running straight arm across the chest. The arm is held out rather
        // than thrown, and the legs drive -- so unlike a punch the travel is
        // most of it.
        m.limb  = 1; m.contact = kAtHand;
        m.hand   = vec2(0.400, 0.055);
        m.elbow  = vec2(0.210, 0.045);
        m.travel = 0.095;
        m.lean   = 0.45;
    } else if (kind == kTakedown) {
        // Double-leg. Drop under the guard, drive through the hips, both arms low
        // and wrapping.
        m.limb  = 0; m.contact = kAtNone;
        m.crouch = 0.260;
        m.hand   = vec2(0.295, -0.115);
        m.elbow  = vec2(0.165, -0.135);
        m.travel = 0.130;
        m.lean   = 1.05;

    // -- getting somewhere ---------------------------------------------------

    } else if (kind == kSlide) {
        // Baseball slide in under the guard. Front leg out along the floor, back
        // leg folded, body leaning back off it.
        m.limb   = 2; m.contact = kAtNone;
        m.crouch = 0.300;
        m.foot   = vec2(0.44, -0.145);
        m.knee   = vec2(0.235, -0.115);
        m.lean   = -0.95;
        m.travel = 0.105;
        m.fold   = 0.65;
    } else if (kind == kCartwheel) {
        // Capoeira au. A full turn on the hands, which at this scale reads as
        // the whole figure rotating with its limbs spread -- there is no
        // hand-planting to draw at a figure this size.
        m.limb   = 0; m.contact = kAtNone;
        m.hand   = vec2(0.185, 0.290);
        m.elbow  = vec2(0.155, 0.115);
        m.foot   = vec2(0.185, 0.330);
        m.knee   = vec2(0.145, 0.155);
        m.lift   = 0.135;
        m.flip   = -1.0;
        m.travel = 0.075;
    } else if (kind == kBackflip) {
        // Away and over. Tucked, because a tucked flip stays inside the frame
        // and a laid-out one does not.
        m.limb   = 0; m.contact = kAtNone;
        m.hand   = vec2(0.155, 0.045);
        m.elbow  = vec2(0.175, -0.070);
        m.foot   = vec2(0.145, 0.075);
        m.knee   = vec2(0.130, -0.055);
        m.lift   = 0.300;
        m.flip   = 1.0;
        m.travel = -0.135;
        m.fold   = 1.0;

    // -- not attacking -------------------------------------------------------

    } else if (kind == kBlock) {
        // BOTH ARMS UP AND CROSSED IN FRONT OF THE HEAD, and only while a strike
        // the choreography says was blocked is actually arriving.
        //
        // Forward of the face rather than on it: with the head down on the
        // shoulders, hands at the old height are inside the skull.
        m.limb  = 0; m.contact = kAtNone;
        m.hand  = vec2(0.165, 0.120);
        m.elbow = vec2(0.150, -0.030);
    } else if (kind == kDuck) {
        // Slipping under it. The crouch is the move; the hands stay where they
        // were, which is what a boxer does.
        m.limb   = 0; m.contact = kAtNone;
        m.crouch = 0.155;
        m.hand   = vec2(kGuardX - 0.010, 0.050);
        m.elbow  = vec2(0.040, -kArm * 0.26);
        m.lean   = 0.30;
    } else if (kind == kLean) {
        // Pulling back off it, weight onto the rear foot.
        m.limb  = 0; m.contact = kAtNone;
        m.lean  = -1.35;
        m.hand  = vec2(kGuardX - 0.020, 0.010);
        m.elbow = vec2(0.010, -kArm * 0.30);
    }
    // kGuard falls through to the neutral values above, which ARE the guard.

    m.hand  = reach_to(m.hand, kArm);
    m.elbow = reach_to(m.elbow, kArm * 0.62);
    m.foot  = reach_to(m.foot, kLeg);
    m.knee  = reach_to(m.knee, kLeg * 0.62);
    return m;
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
    vec2 toe_f;
    vec2 knee_b;  vec2 foot_b;   // back leg
    vec2 toe_b;
    vec2 contact;                // where this move would land a blow
};

// `commit` is how far into the move, and it goes OUT AND BACK across the beat --
// it peaks at the impact. `progress` runs monotonically forward through the
// recovery instead, and exists for the flips: a full revolution driven from
// `commit` would turn one way and then unwind, which reads as a figure changing
// its mind mid-air.
Pose build_pose(int kind, vec3 shape, float commit, float progress, float bob,
                float recoil, float flick)
{
    Move mv = move_for(kind, shape);

    Pose s;

    float lift   = mv.lift * commit;
    float crouch = mv.crouch * commit;
    float hip_y  = kHipY + bob + lift - crouch;

    // The lean the shape asks for, plus the lean the move needs. Both are small
    // and they add: a cross thrown by a figure already leaning in leans further.
    float lean = shape.z * 0.055 + mv.lean * 0.085 * commit - recoil * 0.090;

    s.hip      = vec2(-0.015 * commit + mv.travel * commit, hip_y);
    s.shoulder = s.hip + vec2(lean, kShoulderY - kHipY - crouch * 0.22);
    s.head     = s.shoulder + vec2(lean * 0.32 + 0.008 - recoil * 0.030,
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
    // BOTH KNEES BEND FORWARD, and the rear one is the easy one to get backwards.
    //
    // The knee is offset off the straight line from hip to foot, and which way it
    // goes is which way the joint bends. The front foot is ahead of the hip so a
    // positive offset is obviously forward; the rear foot is BEHIND the hip, and
    // the first version reasoned from the foot instead of from the body and
    // offset it by -0.025. That gives the rear leg a knee that bends backwards --
    // a bird's leg -- which is unmistakable once seen and easy to miss while
    // looking at what the arms are doing.
    float ground   = kFootY + lift;
    vec2  n_foot_b = vec2(-0.09 - 0.03 * commit, ground);
    vec2  n_knee_b = mix(s.hip, n_foot_b, 0.5) + vec2(0.032, 0.02);
    vec2  n_foot_f = vec2(0.07 + 0.03 * commit, ground);
    vec2  n_knee_f = mix(s.hip, n_foot_f, 0.5) + vec2(0.035, 0.015);

    vec2 m_foot_f = n_foot_f;
    vec2 m_knee_f = n_knee_f;
    vec2 m_foot_b = n_foot_b;
    vec2 m_knee_b = n_knee_b;

    if (mv.limb == 2 || mv.limb == 3) {
        m_foot_f = s.hip + mv.foot;
        m_knee_f = s.hip + mv.knee;
        if (mv.limb == 3) {
            // Both legs out, the trailing one slightly short and low so the pair
            // reads as two legs rather than as one thick one.
            m_foot_b = s.hip + mv.foot * vec2(0.88, 1.0) - vec2(0.0, 0.075);
            m_knee_b = s.hip + mv.knee * vec2(0.88, 1.0) - vec2(0.0, 0.055);
        } else {
            // The standing leg takes the weight, so it comes under the hip. Knee
            // forward, for the reason in the note on the neutral stance above.
            m_foot_b = vec2(-0.045, ground);
            m_knee_b = mix(s.hip, m_foot_b, 0.5) + vec2(0.026, 0.0);
        }
    } else if (mv.limb == 0 && (mv.foot != vec2(0.0) || mv.knee != vec2(0.0))) {
        // A move with no striking limb that still says where the legs go -- the
        // flips and the cartwheel, whose whole content is the shape of the body.
        m_foot_f = s.hip + mv.foot;
        m_knee_f = s.hip + mv.knee;
        m_foot_b = s.hip + mv.foot * vec2(-0.55, 0.80);
        m_knee_b = s.hip + mv.knee * vec2(-0.45, 0.85);
    }

    // The trailing leg tucking up under the body. Applied after the move so it
    // composes with whatever the move asked for, which is how a flying knee gets
    // both a driving knee and a folded trailing leg without stating either twice.
    if (mv.fold > 0.0) {
        float f = mv.fold * commit;
        m_knee_b = mix(m_knee_b, s.hip + vec2(0.020, -0.100), f);
        m_foot_b = mix(m_foot_b, s.hip + vec2(-0.095, -0.060), f);
    }

    // Eased, so the limb accelerates out of the stance rather than moving at a
    // constant rate -- which is most of what separates a strike from a stretch.
    float leg = smoothstep(0.0, 1.0, commit);
    s.foot_f  = mix(n_foot_f, m_foot_f, leg);
    s.knee_f  = mix(n_knee_f, m_knee_f, leg);
    s.foot_b  = mix(n_foot_b, m_foot_b, leg);
    s.knee_b  = mix(n_knee_b, m_knee_b, leg);

    // -- arms ---------------------------------------------------------------
    //
    // A DRAWN-BACK GUARD, not a short arm. An arm that is kArm long has to go
    // somewhere when it is not extended, and the answer is a deep elbow bend: the
    // hand sits close to the shoulder while shoulder-elbow-hand traces most of
    // kArm. Putting the hand further out instead would have made the guard read
    // as a permanent half-punch.
    vec2 g_hand  = vec2(kGuardX + 0.030 * flick, -0.010 + 0.035 * flick);
    vec2 g_elbow = vec2(0.030, -kArm * 0.29);
    // THE REAR GUARD HAND IS THE ONE THAT KEEPS ENDING UP IN THE FACE. It sits
    // inside the lead hand and therefore closest to the skull, so it needs the
    // most forward bias of anything on the figure -- 0.115 rather than the 0.090
    // that reads correctly on a body with a neck.
    vec2 g_hand_b  = vec2(0.115, -0.030 + 0.015 * flick);
    vec2 g_elbow_b = vec2(-0.010, -kArm * 0.31);

    vec2 t_hand  = mv.hand;
    vec2 t_elbow = mv.elbow;

    if (mv.limb == 2 || mv.limb == 3) {
        // LEGS ARE DOING THE WORK, so the arms balance. The lead arm goes out and
        // DOWN across the front, which is what a kicker actually does with it --
        // and, more prosaically, the only place it can go: out and up is the
        // skull, and back is where the other arm is going.
        t_hand  = vec2(0.215 + 0.055 * commit, -0.030);
        t_elbow = vec2(0.105, -0.095);
    }

    float arm = smoothstep(0.0, 1.0, commit);
    s.hand_a  = s.shoulder + mix(g_hand, t_hand, arm);
    s.elbow_a = s.shoulder + mix(g_elbow, t_elbow, arm);

    if (kind == kBlock) {
        // The second arm comes up too, inside the first and slightly lower, which
        // is what makes it a block rather than one raised hand.
        s.hand_b  = s.shoulder + vec2(0.140, 0.070);
        s.elbow_b = s.shoulder + vec2(0.120, -0.060);
    } else if (mv.limb == 2 || mv.limb == 3) {
        // The rear arm swings BACK as the leg goes forward, which is the
        // counterweight and is most of what stops a kick reading as a fall.
        s.hand_b  = s.shoulder + mix(g_hand_b, vec2(-0.105 - 0.055 * commit, 0.030), arm);
        s.elbow_b = s.shoulder + mix(g_elbow_b, vec2(-0.095, -0.085), arm);
    } else if (mv.limb == 0 && mv.flip != 0.0) {
        // Tucked in, mirroring the lead arm -- a flip with one arm out reads as
        // falling rather than as a flip.
        s.hand_b  = s.shoulder + mix(g_hand_b, mv.hand * vec2(0.80, 0.85), arm);
        s.elbow_b = s.shoulder + mix(g_elbow_b, mv.elbow * vec2(0.70, 0.85), arm);
    } else {
        // The rear hand stays on guard while the lead one works, which is the
        // whole reason a boxer can throw anything at all. It tucks in a little as
        // the shoulder turns over -- FORWARD, not back towards the jaw, which is
        // where it went first and where the skull now is.
        s.hand_b  = s.shoulder + mix(g_hand_b, g_hand_b + vec2(0.015, -0.025), arm);
        s.elbow_b = s.shoulder + g_elbow_b;
    }

    // AND THEN THE SAFETY NET, over all four joints. The targets above are all
    // clear of the skull as written, but a lean, a recoil and a flick move the
    // head and the hands independently -- and a move added later will get this
    // wrong on its first render exactly as six of the ones above did.
    float skull = kHeadR + kLimb * 1.15;
    s.hand_a  = clear_head(s.hand_a, s.head, skull);
    s.hand_b  = clear_head(s.hand_b, s.head, skull);
    s.elbow_a = clear_head(s.elbow_a, s.head, skull);
    s.elbow_b = clear_head(s.elbow_b, s.head, skull);

    // -- feet ---------------------------------------------------------------
    //
    // A PLANTED FOOT IS FLAT AND A MOVING ONE POINTS WHERE IT IS GOING. One rule
    // rather than a foot angle per move: how far the ankle is off the floor is
    // already exactly the question "is this foot standing on something", and it
    // answers itself for every move including the ones not written yet.
    float air_f = clamp((s.foot_f.y - ground) * 7.0, 0.0, 1.0);
    float air_b = clamp((s.foot_b.y - ground) * 7.0, 0.0, 1.0);
    s.toe_f = s.foot_f + dir_or(mix(vec2(1.0, 0.0),
                                    dir_or(s.foot_f - s.knee_f, vec2(1.0, 0.0)), air_f),
                                vec2(1.0, 0.0)) * kFootLen;
    s.toe_b = s.foot_b + dir_or(mix(vec2(1.0, 0.0),
                                    dir_or(s.foot_b - s.knee_b, vec2(1.0, 0.0)), air_b),
                                vec2(1.0, 0.0)) * kFootLen;

    // -- where it lands ------------------------------------------------------

    if (mv.contact == kAtHand)       { s.contact = s.hand_a; }
    else if (mv.contact == kAtFoot)  { s.contact = s.foot_f; }
    else if (mv.contact == kAtElbow) { s.contact = s.elbow_a; }
    else if (mv.contact == kAtKnee)  { s.contact = s.knee_f; }
    else                             { s.contact = s.hand_a; }

    // -- the turn ------------------------------------------------------------
    //
    // Applied last, to the finished pose, about the hip. Every spinning move in
    // the vocabulary is therefore the same code, and a move that spins does not
    // have to restate where any of its joints went.
    float ang = mv.spin * commit + mv.flip * 2.0 * kPi * progress;
    if (abs(ang) > 1e-4) {
        vec2  pivot = s.hip;
        float c = cos(ang);
        float sn = sin(ang);
        #define TURN(v) (pivot + vec2((v - pivot).x * c - (v - pivot).y * sn, \
                                      (v - pivot).x * sn + (v - pivot).y * c))
        s.shoulder = TURN(s.shoulder); s.head    = TURN(s.head);
        s.elbow_a  = TURN(s.elbow_a);  s.hand_a  = TURN(s.hand_a);
        s.elbow_b  = TURN(s.elbow_b);  s.hand_b  = TURN(s.hand_b);
        s.knee_f   = TURN(s.knee_f);   s.foot_f  = TURN(s.foot_f);
        s.knee_b   = TURN(s.knee_b);   s.foot_b  = TURN(s.foot_b);
        s.toe_f    = TURN(s.toe_f);    s.toe_b   = TURN(s.toe_b);
        s.contact  = TURN(s.contact);
        #undef TURN
    }

    return s;
}

float draw_pose(vec2 p, Pose s)
{
    float d = length(p - s.head) - kHeadR;

    // NO NECK. The torso runs hip to shoulder and the shoulder is inside the
    // head, so the union holds the head on without a segment between them. The
    // old explicit neck was a fix for a head that detached under recoil, and it
    // fixed it by drawing something a stick figure does not have.
    d = smin(d, sd_segment(p, s.hip, s.shoulder, kLimb * 1.3), kJoint);

    d = smin(d, sd_segment(p, s.hip, s.knee_f, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.knee_f, s.foot_f, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.foot_f, s.toe_f, kLimb * 0.85), kJoint);
    d = smin(d, sd_segment(p, s.hip, s.knee_b, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.knee_b, s.foot_b, kLimb), kJoint);
    d = smin(d, sd_segment(p, s.foot_b, s.toe_b, kLimb * 0.85), kJoint);

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

// WHICH MOVE, biased by the spectrum.
//
// A GROUP FIRST, THEN A MEMBER OF IT. Bass-led material gets legs, because a
// kick is the heaviest thing a stick figure can do and low end is weight;
// treble-led material gets hands, because they are fast. Choosing the group from
// the music and the member from a hash is what keeps "bass gets legs" true while
// still giving eleven different legs.
int kind_for(float beat, float bass, float treble)
{
    float total   = bass + treble + 1e-4;
    float legness = clamp(0.5 + 1.1 * (bass - treble) / total, 0.0, 1.0);

    float group = hash11(beat * 2.3 + 7.0);
    float pick  = hash11(beat * 4.7 + 19.0);

    // ACROBATICS ARE RARE ON PURPOSE. A cartwheel is the most conspicuous thing
    // in the vocabulary and a fight made of them reads as gymnastics rather than
    // as fighting. One beat in eleven.
    if (group > 0.91) {
        return kMoveFirst + min(int(pick * float(kMoveLast - kMoveFirst + 1)),
                                kMoveLast - kMoveFirst);
    }
    if (group < 0.19 + legness * 0.60) {
        return kLegFirst + min(int(pick * float(kLegLast - kLegFirst + 1)),
                               kLegLast - kLegFirst);
    }
    return kHandFirst + min(int(pick * float(kHandLast - kHandFirst + 1)),
                            kHandLast - kHandFirst);
}

float attacker_for(float beat)
{
    return mod(beat, 2.0) < 1.0 ? -1.0 : 1.0;
}

// -- is this beat a throw ----------------------------------------------------
//
// DECIDED BEFORE THE MOVE IS, AND FROM THE BEAT ALONE rather than through
// kind_for like everything else. That is not tidiness: a throw always ends with
// the other one on the floor, so the knockdown machinery has to be able to look
// backwards and ask "was that beat a throw?" -- and it has no bands from that
// beat to ask kind_for with. Keeping it pure is what makes the fall coherent with
// the throw that caused it instead of the two being independently random.
//
// One beat in nine here, but the EFFECTIVE rate is about half that -- roughly one
// in nineteen -- because a throw also has to pass the clear-run gate before it is
// performed at all. See the note on falls for why it is gated twice.
//
// That second gate is easy to forget and it invalidates the obvious way to test a
// throw. Forcing this function true to see one makes every beat seed a knockdown,
// which means no beat ever HAS a clear run behind it, so no throw is ever
// performed and the probe shows nothing happening at all. Bypass the gate as well.
bool throw_on(float beat)
{
    return hash11(beat * 12.7 + 5.0) > 0.885;
}

int throw_kind_for(float beat)
{
    float pick = hash11(beat * 6.1 + 23.0);
    return kThrowFirst + min(int(pick * float(kThrowLast - kThrowFirst + 1)),
                             kThrowLast - kThrowFirst);
}

// -- what happened to the strike ---------------------------------------------
//
// THE FIX FOR BLOCKING A BLOW THAT WAS NEVER THROWN, and for blocking every
// single one. The old code had no notion of an exchange having an outcome: the
// defender was put into a block whenever the attacker was inside its recovery
// window, so a block said nothing except "it is not my turn", and it happened
// even when the attacker was flat on the floor.
//
// Deciding it here, from the beat, costs one hash and makes three things true at
// once. A block only ever appears against a strike that was actually blocked. A
// recoil only ever appears where one landed. And the flash cannot fire on a beat
// where the defender got out of the way -- which is the difference between a hit
// and a light show.
const int kLanded  = 0;
const int kBlocked = 1;
const int kEvaded  = 2;

int outcome_for(float beat)
{
    float h = hash11(beat * 9.13 + 41.0);
    if (h < 0.46) { return kLanded; }
    if (h < 0.80) { return kBlocked; }
    return kEvaded;
}

// The pose that answers a strike with that outcome.
//
// kGuard for one that landed, which is the honest answer: nothing the defender
// did stopped it, so it is standing there when it arrives. You duck a head shot
// and pull back off a body shot, so the evasion reads the height the strike was
// aimed at.
int answer_to(int outcome, vec3 shape)
{
    if (outcome == kBlocked) { return kBlock; }
    if (outcome == kEvaded)  { return shape.x > 0.5 ? kDuck : kLean; }
    return kGuard;
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

// Would the strike on `beat` have put the defender down, considered alone?
//
// Deliberately not every landed hit. A fight where someone goes down on every
// other beat is a comedy, and the recovery would swallow the fighting.
bool knockdown_seed(float beat)
{
    // A THROW ALWAYS ENDS WITH THE OTHER ONE DOWN, by definition rather than by
    // luck. This is the reason throw_on has to be a pure function of the beat.
    if (throw_on(beat)) {
        return true;
    }
    return outcome_for(beat) == kLanded && hash11(beat * 5.7 + 3.0) > 0.72;
}

// Both fighters' falls, and how long ago each of them happened.
//
// A KNOCKDOWN NEEDS A CLEAR RUN BEHIND IT. Without that, the fall of one fighter
// could be attributed to a strike thrown by the other while IT was on the floor
// -- the same incoherence as blocking a blow nobody threw, one level further
// back. Requiring nothing to have gone down in the preceding kDownBeats makes it
// impossible by construction, because any fall still in progress must have started
// inside that window. That is what the `!s1 && !s2 && !s3` chains are.
//
// Testing the SEED of the earlier beats rather than the full answer is what keeps
// this bounded instead of recursive. It is conservative -- it can suppress a
// knockdown whose predecessor was itself suppressed -- and erring towards fewer
// knockdowns is the safe direction: it buys back time actually fighting.
//
// -- and two things about the shape of it, both measured ----------------------
//
// BOTH FIGHTERS IN ONE PASS, with the seeds hoisted. The per-side version asked a
// knockdown_on() three times per fighter and each of those asked the seed four
// times, so the seed for a given beat was computed by three overlapping clear-run
// windows and then again for the other fighter: ninety-six hashes per pixel where
// twelve will do.
//
// NAMED SCALARS RATHER THAN AN ARRAY IN A LOOP, which is the counter-intuitive
// half. Every one of these hashes gives the same answer for every pixel on the
// screen and a fragment shader has nowhere to put a value computed once per draw,
// so the obvious move is to fill a small array of seeds and index it. That made
// the crystal SLOWER -- 8.36 ms per frame at 4K became 11.71 -- because an array
// indexed by a non-constant cannot stay in registers, goes to scratch memory, and
// costs more to read back than the arithmetic it replaced. On a GPU,
// recomputation is frequently cheaper than remembering. Six named variables the
// compiler can keep in registers is the shape that actually helps: 6.18 ms.
//
// At most one of the three can be true, because of the clear run -- so the
// assignments below cannot fight. They are still written nearest-last so that
// "the most recent fall wins" survives anyone widening the window later.
// `now_clear` and `next_clear` report whether a knockdown on this beat or the next
// would survive the clear-run rule.
//
// THE CALLER NEEDS THEM BECAUSE A THROW HAS TO BE ABLE TO NOT HAPPEN. A throw puts
// the other one down by definition, but the clear-run rule can still suppress that
// fall if somebody went down in the beats before -- and a fighter who gets hip
// thrown and simply stays on his feet is exactly the class of incoherence this
// pass exists to remove. So the throw is only performed when its own knockdown
// will survive, and the answer is free here: these are the same seeds.
void falls(float beat, float phase, out float age_l, out float age_r,
           out float thrown_l, out float thrown_r,
           out float now_clear, out float next_clear)
{
    bool s0 = knockdown_seed(beat);
    bool s1 = knockdown_seed(beat - 1.0);
    bool s2 = knockdown_seed(beat - 2.0);
    bool s3 = knockdown_seed(beat - 3.0);
    bool s4 = knockdown_seed(beat - 4.0);
    bool s5 = knockdown_seed(beat - 5.0);

    bool kd0 = s0 && !s1 && !s2 && !s3;
    bool kd1 = s1 && !s2 && !s3 && !s4;
    bool kd2 = s2 && !s3 && !s4 && !s5;

    // Adjacent beats alternate the attacker, so two of these are free.
    float a0 = attacker_for(beat);
    float a1 = -a0;
    float a2 = a0;

    // Given that this beat seeds a knockdown, kd0's condition IS the clear run --
    // and a knockdown on the next beat needs the three beats ending at this one.
    now_clear  = (!s1 && !s2 && !s3) ? 1.0 : 0.0;
    next_clear = (!s0 && !s1 && !s2) ? 1.0 : 0.0;

    age_l = -1.0;      age_r = -1.0;
    thrown_l = 0.0;    thrown_r = 0.0;

    // A fall belongs to whichever fighter was NOT attacking on that beat. Whether
    // it was THROWN comes back too, because a thrown body is already on the floor
    // at the moment of impact and must not play the topple -- see fall_curve.
    if (kd2) {
        float w = throw_on(beat - 2.0) ? 1.0 : 0.0;
        if (a2 > 0.0) { age_l = 2.0 + phase; thrown_l = w; }
        else          { age_r = 2.0 + phase; thrown_r = w; }
    }
    if (kd1) {
        float w = throw_on(beat - 1.0) ? 1.0 : 0.0;
        if (a1 > 0.0) { age_l = 1.0 + phase; thrown_l = w; }
        else          { age_r = 1.0 + phase; thrown_r = w; }
    }
    if (kd0) {
        float w = throw_on(beat) ? 1.0 : 0.0;
        if (a0 > 0.0) { age_l = phase; thrown_l = w; }
        else          { age_r = phase; thrown_r = w; }
    }
}

// 0 upright, 1 flat out. Down fast, up slowly -- which is both what happens and
// what reads as weight.
//
// A THROWN FIGHTER IS ALREADY DOWN WHEN IT LANDS, and skips the ramp. The ordinary
// topple from upright over a third of a beat is right for a body that has just
// been hit and wrong for one that was over somebody's shoulder a frame earlier:
// it would snap upright at the moment of impact and then fall over, which is the
// opposite of a slam.
float fall_curve(float age, bool thrown)
{
    if (age < 0.0)  { return 0.0; }
    if (!thrown && age < 0.32) { return smoothstep(0.0, 0.32, age); }
    if (age < 1.60) { return 1.0; }
    return 1.0 - smoothstep(1.60, 2.60, age);
}

// -- where on the stage the fight is -----------------------------------------
//
// TWO FIGHTERS TRADING BLOWS ON ONE SPOT READS AS A MACHINE. Ground won and lost
// is most of what a fight looks like from the back of a room, and it was the
// thing most obviously missing from the first version.
//
// Both fighters share this, so it moves the pair rather than the gap between
// them -- the spacing is the lunge's job and mixing the two would let them drift
// out of each other's reach.
//
// DRIVEN BY WHO HAS BEEN LANDING, not by a wander alone. Whoever connects pushes
// the exchange away from themselves and it decays over the beats after. Summed
// over recent history for the same reason the falls are: there is nowhere to
// accumulate anything, so "how far has this been pushed" is answered by asking
// the last few beats what they did.
const float kDriveBeats = 5.0;
const float kStageLimit = 0.255;   // 4:3 is the narrow case -- see main()

float stage_x(float beat, float phase)
{
    // ONE HASH PER BEAT, WHICH IS THE WHOLE REASON THIS ASKS ONLY THE OUTCOME.
    // The first version also excluded beats that ended in a knockdown, on the
    // grounds that the fall already slides the loser back -- eight more hashes
    // per beat for a refinement nobody could see, and it measured at 2.50 ms per
    // frame at 4K. A knockdown blow driving the pair apart is right anyway.
    float x = 0.0;
    for (float k = 0.0; k < kDriveBeats; k += 1.0) {
        float b = beat - k;
        if (outcome_for(b) == kLanded) {
            // Pushed AWAY from whoever threw it: attacker_for is -1 for the left
            // fighter, and a blow from the left drives the pair to the right.
            x += -attacker_for(b) * 0.075 * exp(-(k + phase) * 0.62);
        }
    }

    // Plus a slow circling, so an even exchange does not park them in the middle.
    // Off the beat count rather than off u_time, so it stays a function of the
    // music and survives a reload in the same place as everything else.
    float t = beat + phase;
    x += 0.135 * sin(t * 0.27) + 0.055 * sin(t * 0.61 + 1.7);

    return clamp(x, -kStageLimit, kStageLimit);
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
    s.toe_f  = mix(s.toe_f, s.hip, k * 0.38);
    s.knee_b = mix(s.knee_b, s.hip, k * 0.35);
    s.foot_b = mix(s.foot_b, s.hip, k * 0.30);
    s.toe_b  = mix(s.toe_b, s.hip, k * 0.28);
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
    s.toe_f = ROT(s.toe_f);       s.toe_b = ROT(s.toe_b);
    s.contact = ROT(s.contact);
    #undef ROT
    return s;
}

Pose mirror_pose(Pose s)
{
    #define MIR(v) (v = vec2(-(v).x, (v).y))
    MIR(s.hip);      MIR(s.shoulder);  MIR(s.head);
    MIR(s.elbow_a);  MIR(s.hand_a);    MIR(s.elbow_b);  MIR(s.hand_b);
    MIR(s.knee_f);   MIR(s.foot_f);    MIR(s.toe_f);
    MIR(s.knee_b);   MIR(s.foot_b);    MIR(s.toe_b);
    MIR(s.contact);
    #undef MIR
    return s;
}

Pose translate_pose(Pose s, vec2 d)
{
    s.hip += d;      s.shoulder += d;  s.head += d;
    s.elbow_a += d;  s.hand_a += d;    s.elbow_b += d;  s.hand_b += d;
    s.knee_f += d;   s.foot_f += d;    s.toe_f += d;
    s.knee_b += d;   s.foot_b += d;    s.toe_b += d;
    s.contact += d;
    return s;
}

// The body being thrown, in the THROWER'S coordinate space.
//
// WHY THIS EXISTS AT ALL. Every other move in this crystal is one figure doing
// something with the other figure reacting to it, and the two are posed
// independently in their own mirrored spaces. A throw is not that: it is a single
// shape made of two people, and the victim's joints are meaningless except
// relative to the hands holding it. Building it in the thrower's space and
// drawing it with the thrower's transform is what makes the two of them one
// object -- and it is why the draw below has a paired branch at all.
//
// The construction is four steps and each is a fact about being thrown. A body
// standing in front of the thrower, facing it. Limbs going slack and folding in,
// because somebody off the ground has no stance. Pulled IN, because nobody throws
// anybody at arm's length. And then swung about a point on the thrower -- which
// point and how far is the whole difference between the four of them.
Pose build_victim(int kind, float commit, float gap, float bob)
{
    Pose v = build_pose(kGuard, vec3(0.5, 0.5, 0.0), 0.0, 0.0, bob, 0.0, 0.0);
    v = mirror_pose(v);
    v = translate_pose(v, vec2(gap, 0.0));
    // HARD, because a thrown body is a compact one. The first pass curled it to
    // 0.75 and the legs stuck straight out, so the victim swung as a long bar and
    // the far end left the top of the frame -- and a body with a stance in it does
    // not read as a body somebody else is carrying.
    v = curl_pose(v, 0.45 + 0.50 * commit);

    // Over the hip and down the other side: the judo hip throw.
    //
    // THE PIVOT IS LOW AND THE PULL IS MOST OF THE GAP, and both are for the same
    // reason. Swinging about the shoulder at arm's length gives a radius of half
    // the gap, which puts the victim's far end above y = 1.19 in a frame that ends
    // at about 1.12 -- it was clipped. It is also just wrong: nobody throws
    // anybody from arm's length, and a hip throw has the other body ON the hip.
    vec2  pivot = vec2(0.0, kHipY + 0.140);
    float ang   = 2.40;
    float pull  = gap * 0.74;

    if (kind == kSuplex) {
        // Further over, because a suplex takes it past vertical and behind.
        pivot = vec2(-0.020, kShoulderY - 0.050);
        ang   = 3.00;
        pull  = gap * 0.72;
    } else if (kind == kClothesline) {
        // PIVOTS ABOUT THE VICTIM'S OWN CHEST, not about the thrower. That is the
        // difference between being thrown and being clotheslined: the arm stops
        // the top half and the legs keep going, so it rotates where it stands and
        // travels backwards rather than being carried.
        pivot = vec2(gap * 0.58, kShoulderY - 0.095);
        ang   = 1.75;
        pull  = gap * 0.16;
    } else if (kind == kTakedown) {
        // Low pivot, because a double-leg folds it over the hips rather than
        // lifting it over the shoulder.
        pivot = vec2(0.050, kHipY - 0.100);
        ang   = 1.35;
        pull  = gap * 0.52;
    }

    v = translate_pose(v, vec2(-pull * commit, 0.0));
    return rotate_pose(v, ang * smoothstep(0.0, 1.0, commit), pivot);
}

// ---------------------------------------------------------------------------
// One pair, fought inside its own little world
//
// WHY THIS IS A FUNCTION. The crystal used to be one duel written straight into
// main(). Three duels share every line of it and differ only in where they
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
    const float kHold = 0.09;
    float t = phase < kHold ? 0.0 : (phase - kHold) / (1.0 - kHold);

    // THE STRIKE TRAVELS FOR TWO THIRDS OF A BEAT AND ARRIVES EXACTLY ON IT.
    //
    // The first version wound up to 0.55 commitment and no further, and then the
    // striker's commitment was `recover`, which is 1.0 at the beat -- so the last
    // and fastest 45 percent of every strike happened in the single frame at the
    // beat boundary. The limb teleported the rest of the way. That is why the
    // strikes were a blur: the part you would actually watch did not exist as
    // motion at all.
    //
    // Winding up to a full 1.0 fixes the discontinuity as well as the speed. The
    // wind-up ends where the follow-through begins, at the same value, so the
    // whole strike is one continuous movement that peaks on the beat -- which is
    // the thing that must not change, because the grid is what everything else in
    // this crystal is hung on.
    float recover = 1.0 - smoothstep(0.0, 0.50, t);
    float windup  = smoothstep(0.34, 1.0, t);

    // Held back when the tempo is not trusted: a full-blooded jump kick landing
    // on nothing looks like the crystal is broken rather than like the estimate
    // being thin.
    float trust = clamp(u_confidence * 2.0, 0.30, 1.0);

    // -- WHO IS ON THE FLOOR, WORKED OUT FIRST --------------------------------
    //
    // ORDER MATTERS AND IT IS THE WHOLE BUG. The old code decided who was
    // attacking, put the other one into a block, and only then noticed that one
    // of them was lying down -- so a fighter flat on the floor had its opponent
    // dutifully blocking a punch it had not thrown. Everything about the
    // exchange now derives from the condition of the two bodies, which has to be
    // known before any of it.
    float age_l;    float age_r;
    float thrown_l; float thrown_r;
    float now_clear; float next_clear;
    falls(beat, phase, age_l, age_r, thrown_l, thrown_r, now_clear, next_clear);
    float fall_l = fall_curve(age_l, thrown_l > 0.5);
    float fall_r = fall_curve(age_r, thrown_r > 0.5);

    float striker = attacker_for(beat);
    float next    = attacker_for(beat + 1.0);

    bool striker_down = (striker < 0.0 ? fall_l : fall_r) > 0.02;
    bool target_down  = (striker < 0.0 ? fall_r : fall_l) > 0.02;
    bool next_down    = (next < 0.0 ? fall_l : fall_r) > 0.02;

    // A FIGHTER ON THE FLOOR THROWS NOTHING. Not a reduced strike, not a strike
    // that misses -- nothing at all, so there is no blow for the other one to
    // react to.
    bool live = !striker_down;

    int outcome = outcome_for(beat);
    // Nor can a fighter on the floor block or get out of the way. If a strike
    // arrives while it is down, it arrives.
    if (target_down) { outcome = kLanded; }

    // A THROW OVERRIDES THE MOVE THE SPECTRUM WOULD HAVE PICKED, because it is
    // chosen from the beat alone -- see throw_on for why it has to be.
    // Only if the fall it causes can actually happen -- see the note on falls.
    bool throw_now  = throw_on(beat)       && now_clear  > 0.5;
    bool throw_next = throw_on(beat + 1.0) && next_clear > 0.5;

    int kind_now  = throw_now  ? throw_kind_for(beat)       : kind_for(beat, bass, treble);
    int kind_next = throw_next ? throw_kind_for(beat + 1.0) : kind_for(beat + 1.0, bass, treble);
    vec3 shape_now  = shape_blended(beat, bass, mid, treble);
    vec3 shape_next = shape_blended(beat + 1.0, bass, mid, treble);

    // A MOVE THAT LANDS NO BLOW CANNOT BE BLOCKED OR EVADED. The acrobatics are
    // movement rather than strikes, so an exchange built on one is not an
    // exchange -- and a defender bracing against a cartwheel is the same mistake
    // as bracing against nothing.
    bool throws_blow = kind_now < kNoBlowFirst;
    if (!throws_blow) { live = false; }

    // AND A THROW IS NEVER BLOCKED OR EVADED. Not because it could not be, but
    // because a stick figure blocking a hip throw and a stick figure being hip
    // thrown are the same drawing -- and the fall on the following beats is
    // already committed to by knockdown_seed, so an outcome that said otherwise
    // would contradict it.
    if (throw_now) { outcome = kLanded; }

    float bob = 0.010 * sin(u_bar * 2.0 * kPi) + 0.018 * bass;

    // The exchange one beat ahead, because the defender has to start answering it
    // while it is still on its way.
    int  outcome_next = outcome_for(beat + 1.0);
    bool live_next    = kind_next < kNoBlowFirst && !next_down;
    bool next_target_down = (next < 0.0 ? fall_r : fall_l) > 0.02;
    if (next_target_down || throw_next) { outcome_next = kLanded; }

    // -- WHAT EACH FIGHTER IS DOING, AND WHAT IT IS ANSWERING -----------------
    //
    // EVERY FIGHTER HAS TWO THINGS IN FLIGHT AT ONCE, and choosing between them
    // is most of what the timing is. Its OWN move -- a strike travelling towards
    // the next beat, or the follow-through of the one that just landed. And its
    // ANSWER to a strike coming at it, which has to begin while that strike is
    // still travelling: a guard that comes up after the impact came up too late
    // to read as a reaction to anything, and with the strikes now taking two
    // thirds of a beat to arrive there is plenty of time to see it happen.
    //
    // Because the attacker alternates every beat, each fighter always has exactly
    // one of each. They overlap in the stretch where a follow-through runs into
    // the next wind-up, and there the one with more commitment behind it wins --
    // a smooth handover rather than the pose popping between them.
    //
    // The old arrangement summed a recovery and a wind-up into one number and
    // picked the kind separately, which is why a fighter could be drawn in the arm
    // shape of a strike it had not started.

    // My own move.
    int  own_l  = striker < 0.0 ? kind_now  : kind_next;
    int  own_r  = striker > 0.0 ? kind_now  : kind_next;
    vec3 own_sl = striker < 0.0 ? shape_now : shape_next;
    vec3 own_sr = striker > 0.0 ? shape_now : shape_next;
    float own_cl = striker < 0.0 ? (striker_down ? 0.0 : recover)
                                 : (next_down ? 0.0 : windup);
    float own_cr = striker > 0.0 ? (striker_down ? 0.0 : recover)
                                 : (next_down ? 0.0 : windup);

    // My answer to what is coming at me. The fighter being struck at THIS beat
    // answers with `recover`, which is the reaction still playing out after the
    // contact; the one being struck at the NEXT beat answers with `windup`, which
    // is the guard on its way up.
    int   ans_l  = kGuard;  float ans_cl = 0.0;  vec3 ans_sl = shape_now;
    int   ans_r  = kGuard;  float ans_cr = 0.0;  vec3 ans_sr = shape_now;

    // ONE ANSWER EACH, and which one falls out of the alternation rather than
    // needing to be worked out: the fighter being struck at this beat is the one
    // that strikes at the next, so the other is already the next beat's defender.
    // So one of them is answering with `recover` and the other with `windup`,
    // always, and they never contend for the same slot.
    if (striker > 0.0) {
        if (live && !target_down) {
            ans_l = answer_to(outcome, shape_now);  ans_cl = recover;  ans_sl = shape_now;
        }
        if (live_next && !next_target_down) {
            ans_r = answer_to(outcome_next, shape_next);  ans_cr = windup;  ans_sr = shape_next;
        }
    } else {
        if (live && !target_down) {
            ans_r = answer_to(outcome, shape_now);  ans_cr = recover;  ans_sr = shape_now;
        }
        if (live_next && !next_target_down) {
            ans_l = answer_to(outcome_next, shape_next);  ans_cl = windup;  ans_sl = shape_next;
        }
    }

    // Whichever is more committed. A guard is not a pose worth preferring, so an
    // answer of kGuard never displaces a move that is actually happening.
    int   kind_l  = own_l;   float commit_l = own_cl;   vec3 shape_l = own_sl;
    int   kind_r  = own_r;   float commit_r = own_cr;   vec3 shape_r = own_sr;
    if (ans_l != kGuard && ans_cl > commit_l) { kind_l = ans_l; commit_l = ans_cl; shape_l = ans_sl; }
    if (ans_r != kGuard && ans_cr > commit_r) { kind_r = ans_r; commit_r = ans_cr; shape_r = ans_sr; }

    // A MOVE AT NO COMMITMENT IS NOT A MOVE. A fighter waiting for its turn was
    // being drawn in the arm shape of the strike it had not started yet, which at
    // commit zero is just hands tucked at the chest -- close enough to a guard to
    // be confusing and far enough to look wrong. Give it an actual guard until it
    // begins.
    if (commit_l < 0.06) { kind_l = kGuard; }
    if (commit_r < 0.06) { kind_r = kGuard; }

    // -- AND A RECOIL ONLY WHERE IT LANDED ------------------------------------
    //
    // The old code recoiled the defender on every beat it was not attacking, so a
    // blocked punch and a clean one moved the other fighter identically -- which
    // is the same as neither of them meaning anything.
    float recoil_l = 0.0;
    float recoil_r = 0.0;
    if (live && outcome == kLanded) {
        // Hit while already on the floor still moves it, but the fall is doing
        // most of the work, so the recoil is folded down rather than added on.
        float scale = target_down ? 0.25 : 1.0;
        if (striker > 0.0) { recoil_l = recover * scale; }
        else               { recoil_r = recover * scale; }
    }

    // A downed fighter's move is forced to the guard so the rotated pose is a body
    // with its arms tucked rather than a body frozen mid-kick, and its commitment
    // is zeroed so the blend never leaves the stance.
    if (fall_l > 0.0) { kind_l = kGuard; commit_l = 0.0; }
    if (fall_r > 0.0) { kind_r = kGuard; commit_r = 0.0; }

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
    const float kApart = 0.285;

    // THE GAP BREATHES, and that is the other half of making the fight move.
    // stage_x slides the pair across the stage, but both roots take the same
    // offset -- so the DISTANCE between them never changed, and two figures
    // holding a fixed gap read as a mechanism wherever on the stage you put it.
    // Closing and opening range is most of what two people circling each other
    // actually do.
    float t_beats = beat + phase;
    float range   = kApart * (1.0 + 0.17 * sin(t_beats * 0.79 + 2.1));

    // THE LUNGE IS DERIVED FROM THE RANGE THAT IS OPEN, not from a constant. A
    // fixed lunge computed against a fixed gap was fine while the gap was fixed;
    // against a breathing one it would fall short at long range and overshoot at
    // short, and "sometimes the punches miss for no reason" is the least
    // diagnosable kind of wrong.
    //
    // kStandoff is what stops a committed strike burying itself. Aiming at the
    // opponent's ROOT -- which is what the arithmetic did before -- puts the fist
    // at the far side of its skull, and a limb emerging from the back of a head
    // reads as passing through it rather than as hitting it. Small, because the
    // impact test asks whether the contact point is inside the other body and a
    // generous standoff would stop every clean shot registering as one.
    const float kStandoff = 0.040;
    float lunge_max = clamp(range * 2.0 - kArm - kStandoff, 0.05, 0.26);

    float knock = 0.030;

    float lunge_l = lunge_max * clamp(commit_l * trust, 0.0, 1.0);
    float lunge_r = lunge_max * clamp(commit_r * trust, 0.0, 1.0);

    // The whole fight slides across the stage. Both roots take the same offset,
    // so the gap between them is the range's business and not the stage's.
    float stage = stage_x(beat, phase);

    // And neither of them is ever quite still. A fighter waiting its turn shifting
    // its weight is the difference between a guard and a statue, and the two are
    // out of phase so it does not read as the pair swaying together.
    float sway_l = 0.013 * sin(t_beats * 1.73);
    float sway_r = 0.013 * sin(t_beats * 1.73 + 2.4);

    // Sliding back along the floor while down. A body that goes over on the spot
    // reads as fainting rather than as being hit. Small, though: a curled body
    // still reaches half its height sideways, and any more than this puts the
    // head off the edge of the frame.
    float root_l = stage - range + sway_l + lunge_l - recoil_l * knock - fall_l * 0.035;
    float root_r = stage + range + sway_r - lunge_r + recoil_r * knock + fall_r * 0.035;

    // `progress` runs forward through the recovery for the flips. Zero during a
    // wind-up, because a figure crouching to jump is not yet turning.
    float prog_l = striker < 0.0 ? clamp(1.0 - recover, 0.0, 1.0) : 0.0;
    float prog_r = striker > 0.0 ? clamp(1.0 - recover, 0.0, 1.0) : 0.0;

    Pose pose_l = build_pose(kind_l, shape_l, commit_l * trust, prog_l, bob, recoil_l, flick_l);
    Pose pose_r = build_pose(kind_r, shape_r, commit_r * trust, prog_r, bob, recoil_r, flick_r);

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

    // -- a throw draws both bodies in ONE space -------------------------------
    //
    // For the duration of a throw the two of them are a single shape, so the
    // victim is built in the thrower's coordinate space and drawn with the
    // thrower's transform. Two independently-placed figures cannot express one
    // being carried by the other: the victim's joints only mean anything relative
    // to the hands holding it.
    //
    // ONLY DURING THE WIND-UP, which is where the throw actually happens. The
    // wind-up of beat N+1 runs through the tail of beat N, so the lift and the
    // swing are here; at the beat itself the victim hits the floor and the
    // knockdown machinery has it from there -- already flat, because fall_curve
    // skips its topple for a body that was thrown.
    bool paired = throw_next && live_next && !next_target_down && windup > 0.04;
    float gap   = root_r - root_l;

    float d_l;
    float d_r;
    if (paired) {
        Pose victim = build_victim(kind_next, windup, gap, bob);
        if (next < 0.0) {
            // The left is throwing, so both are drawn in the left's space.
            d_l = draw_pose(vec2(p.x - root_l, p.y), pose_l);
            d_r = draw_pose(vec2(p.x - root_l, p.y), victim);
        } else {
            d_r = draw_pose(vec2((p.x - root_r) * -1.0, p.y), pose_r);
            d_l = draw_pose(vec2((p.x - root_r) * -1.0, p.y), victim);
        }
    } else {
        // +1 on the left so it faces right; -1 on the right so it faces left.
        // Getting this backwards draws both facing outward, which is what the
        // first version did.
        d_l = draw_pose(vec2((p.x - root_l) * 1.0, p.y), pose_l);
        d_r = draw_pose(vec2((p.x - root_r) * -1.0, p.y), pose_r);
    }

    vec2 contact_l = vec2(root_l + pose_l.contact.x, pose_l.contact.y);
    vec2 contact_r = vec2(root_r - pose_r.contact.x, pose_r.contact.y);

    // -- the trail ---------------------------------------------------------
    //
    // The arc the striking limb just swept, from evaluating the pose at an EARLIER
    // commitment. Possible only because the pose is a pure function; there is no
    // stored previous frame to read.
    float trail = 1e9;
    if (live && recover > 0.05) {
        // A longer arc than before, because the strike now takes two thirds of a
        // beat to travel rather than teleporting the last half of it -- so there
        // is a real path behind the limb to smear.
        float a = clamp(recover * trust, 0.0, 1.0);
        float b = clamp((recover - 0.45) * trust, 0.0, 1.0);
        if (striker < 0.0) {
            Pose from = build_pose(kind_l, shape_l, b, prog_l, bob, 0.0, flick_l);
            Pose to   = build_pose(kind_l, shape_l, a, prog_l, bob, 0.0, flick_l);
            trail = sd_segment(vec2(p.x - root_l, p.y), from.contact, to.contact, 0.006);
        } else {
            Pose from = build_pose(kind_r, shape_r, b, prog_r, bob, 0.0, flick_r);
            Pose to   = build_pose(kind_r, shape_r, a, prog_r, bob, 0.0, flick_r);
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
    //
    // KEPT EVEN THOUGH THE CHOREOGRAPHY NOW SAYS WHETHER THE BLOW LANDED, because
    // the two answer different questions and both have to be yes. The
    // choreography says what was MEANT to happen; the field says whether the limb
    // is anywhere near the other body, which it is not when a low-confidence
    // tempo has held the commitment back. A flash needs both.
    vec2  strike   = striker < 0.0 ? contact_l : contact_r;
    float into     = striker < 0.0
                         ? draw_pose(vec2((strike.x - root_r) * -1.0, strike.y), pose_r)
                         : draw_pose(vec2(strike.x - root_l, strike.y), pose_l);
    float reached  = 1.0 - smoothstep(-0.01, 0.055, into);

    float landed  = live && outcome == kLanded  ? reached : 0.0;
    float parried = live && outcome == kBlocked ? reached : 0.0;

    float r = length(p - strike);

    colour += spark * landed * u_onset * exp(-r * 9.0) * 1.5;

    float ring = exp(-pow((r - (0.05 + recover * 0.22)) * 22.0, 2.0));
    colour += spark * ring * landed * u_onset * 0.65;

    // A BLOCK GETS ITS OWN, DULLER MARK. Cold, tight and without the ring: the
    // point of separating the outcomes is that they should not look the same, and
    // a blocked shot that flashed like a clean one would have thrown away the
    // information the outcome hash exists to carry.
    colour += mix(spark, vec3(0.75, 0.82, 1.0), 0.7) * parried * u_onset *
              exp(-r * 22.0) * 0.55;

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
    // can even represent, so there is no visible edge to the box. It also has to
    // cover the stage wander, which moves a pair by up to kStageLimit * kFarSc.
    vec2 half_box = vec2(0.62 + kStageLimit * kFarSc, 0.62);
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

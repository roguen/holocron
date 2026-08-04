# SPDX-License-Identifier: GPL-3.0-or-later
#
# Generates a calibration tone: one sharp click per second, silence between.
#
# WHY THIS EXISTS
#
# Measuring the audio/video trim needs a known audio event and a known visual
# event. Four attempts to find that inside real music failed, and the last
# failure explained the rest: music does not contain an unambiguous reference.
# Onsets fire around four times a second on dense material -- kick, snare, hats,
# vocal consonants -- and a listener cannot tell which of them a given flash
# belonged to. The flashes were landing on real transients and still looked
# random, because "which transient" was unanswerable.
#
# One event per second with silence either side removes the ambiguity. There is
# nothing else it could be, and the gap gives ear and eye a clean baseline. This
# is how lip-sync test patterns have always worked.
#
# WHY THE CLICK IS BRIGHT AND NOT KICK-SHAPED
#
# The first version of this script synthesised a kick drum -- a decaying 55 Hz
# sine -- on the reasoning that it would be comfortable to listen to and would
# read strongly in the low band. That was the wrong trade and it hurt the only
# thing that matters here.
#
# LOW FREQUENCIES ARE HARD TO LOCALISE IN TIME. One cycle of 55 Hz lasts 18 ms,
# so the ear cannot pin its onset anywhere near as precisely as it can a bright
# transient; the sound arrives as a swell rather than an instant. Making the
# reference pleasant made it less timeable, which is the opposite of the job.
#
# So the click is now bright and dry: an instantaneous noise transient carrying
# the timing edge, over a short high sine that gives it a pitch so it reads as a
# deliberate "tick" rather than a glitch. Nothing below about 1 kHz, no reverb,
# no tail.
#
# Every fourth click is accented, which costs nothing and makes it far easier to
# stay locked in over three minutes than an undifferentiated stream.

param(
    [string]$Out     = "$env:USERPROFILE\holocron-instruments\calibration-tone.wav",
    [int]   $Seconds = 180,
    [int]   $Rate    = 44100
)

$ErrorActionPreference = 'Stop'

$total   = $Seconds * $Rate
$samples = New-Object 'float[]' $total

for ($s = 0; $s -lt $Seconds; $s++) {
    $start    = $s * $Rate
    $accented = ($s % 4) -eq 0

    # Accent by PITCH rather than by level, so every click has the same attack
    # and is equally timeable. A quieter click would be a worse reference.
    $pitch = if ($accented) { 3200.0 } else { 2100.0 }

    # The body: a short bright sine, decayed fast enough to be a tick rather
    # than a tone. 28 ms to near-silence.
    $bodyLen = [int]($Rate * 0.045)
    for ($i = 0; $i -lt $bodyLen; $i++) {
        $t   = $i / $Rate
        $env = [math]::Exp(-$t * 150.0)
        $samples[$start + $i] += [float](0.55 * $env * [math]::Sin(2.0 * [math]::PI * $pitch * $t))
    }

    # The edge: 2 ms of noise with an instantaneous rise. This is what the onset
    # detector keys on and what the ear times against -- a sine alone ramps up
    # over its first cycle and blurs the very thing being measured.
    $edgeLen = [int]($Rate * 0.002)
    $rng = New-Object System.Random (1000 + $s)
    for ($i = 0; $i -lt $edgeLen; $i++) {
        $env = 1.0 - ($i / $edgeLen)
        $samples[$start + $i] += [float](0.75 * $env * (2.0 * $rng.NextDouble() - 1.0))
    }
}

# -- normalise ----------------------------------------------------------------
#
# NOT optional, and the first version got this wrong. The noise edge and the sine
# body sum to more than full scale where they overlap, so writing them directly
# hard-clips the first two milliseconds -- flattening the top of precisely the
# transient this file exists to make timeable, and turning it into a square edge
# that rings above 1.0 when the analysis tap resamples it. The harness reported a
# peak sample of 1.43 and that is what gave it away.
#
# Scaling once at the end keeps the shape and guarantees headroom.

$peak = 0.0
foreach ($v in $samples) { $a = [math]::Abs($v); if ($a -gt $peak) { $peak = $a } }
if ($peak -gt 0.0) {
    $gain = 0.89 / $peak
    for ($i = 0; $i -lt $total; $i++) { $samples[$i] = [float]($samples[$i] * $gain) }
}

# -- write a 16-bit stereo WAV ------------------------------------------------
#
# 16-bit because sample_convert.hpp round-trips 16- and 24-bit through float
# exactly, so this stays bit-perfect on the exclusive-mode path.

$dataBytes = $total * 2 * 2
$dir = Split-Path -Parent $Out
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter($fs)

$bw.Write([char[]]'RIFF')
$bw.Write([int](36 + $dataBytes))
$bw.Write([char[]]'WAVE')
$bw.Write([char[]]'fmt ')
$bw.Write([int]16)
$bw.Write([int16]1)                  # PCM
$bw.Write([int16]2)                  # stereo
$bw.Write([int]$Rate)
$bw.Write([int]($Rate * 2 * 2))
$bw.Write([int16]4)
$bw.Write([int16]16)
$bw.Write([char[]]'data')
$bw.Write([int]$dataBytes)

foreach ($v in $samples) {
    $clamped = [math]::Max(-1.0, [math]::Min(1.0, $v))
    $i16 = [int16]([math]::Round($clamped * 32000))
    $bw.Write($i16)
    $bw.Write($i16)
}

$bw.Close()
$fs.Close()

Write-Host "wrote $Out  ($Seconds s, $Rate Hz, one click per second, accent every 4th)"

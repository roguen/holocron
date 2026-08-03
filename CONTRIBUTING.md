# Contributing

**Holocron is a personal project and is not accepting code contributions.**

It is one person's music visualizer, built for one rack in one house. The
repository is public because there is no reason to hide it and someone may find
the code useful — not because it is looking for collaborators.

That is a deliberate choice rather than an oversight. Accepting an outside pull
request is the one irreversible act available to this project: once someone
else's copyrightable work is in the tree it cannot be removed by unilateral
decision, and the ability to relicense is permanently gone. For a project with
one user and no roadmap for anyone else, that trade buys nothing and costs
something real.

## What is welcome

**Issues.** Bug reports, questions, and "have you considered" are all genuinely
welcome. Open one.

**Forks.** GPL-3.0-or-later, so fork it, change it, run it, redistribute it under
the same terms. You do not need permission and you do not need to ask.

## If you are writing crystals

You do not need anything from this repository to do that. A **crystal** is a
`.frag` shader plus a `.toml` manifest, and `--vault` points at any directory:

```
holocron.exe track.flac --vault D:\my-crystals
```

**Nothing about a crystal's origin can stop it loading.** Adapt a Shadertoy
shader, port something from a blog post, do whatever you like — running it on
your own machine is private use, and no licence has anything to say about it. The
loader has no opinion, which is the point: hot reload exists to make authoring
fast, and a loader that refused to draw things would be in the way.

The manifest reserves three optional keys if you want to record where something
came from:

```toml
author     = "Your Name"
license    = "MIT"                         # SPDX identifier
source_url = "https://example.com/shader"  # if adapted from somewhere
```

They are inert. The only place they are enforced is `crystals/` in *this*
repository, which is published and therefore has to be carryable under
GPL-3.0-or-later — a rule that applies to this vault and to no other.

## The one hard rule

**No MilkDrop presets in the tree, ever.** Tens of thousands of files by hundreds
of authors with no licence statement. `.gitignore` blocks them and CI fails if
one is ever tracked. Point `gatekeeper.toml` at your own copy instead.

---
name: handoff
description: Write the session handoff document that lets a fresh instance pick up this project with full context. Use when the user says "write the handoff", "transition document", "I want to start a new instance", "clear the context window", "wrap up the session", or otherwise asks to capture current state so work can continue elsewhere.
---

# Writing the Holocron handoff

The point is that a **new instance with an empty context window** can carry on
without re-deriving anything. Write it for a competent stranger who has the repo
and nothing else.

Output goes to **`C:\Users\rogue\Documents\Development\HOLOCRON-HANDOFF.md`** —
outside the repo, because it is about the session rather than the code.

Overwrite the previous one. It is a snapshot, not a log.

## Verify state, do not recall it

Every factual claim in the document must come from a command run now. Memory
drifts within a session and the handoff is exactly where that becomes expensive.

```bash
git status --short
git log --oneline -6
git rev-list --count main..development
gh pr list --state open
gh issue list --state open --limit 20
git tag -l | tail -3
scripts\build.cmd
```

`gh` may not be on PATH — use `C:\Program Files\GitHub CLI\gh.exe`.

Check the wiki clone at `Development\holocron-wiki` is clean and pushed too.

## What the document must contain

Roughly in this order. Lead with the instruction, not the state — the previous
session had to be told its priorities twice.

**1. The standing instruction.** Quote it. Currently: *"I want you to finish the
project. Let's get something that basically works from a functional perspective
and then come back to refine."* Say explicitly whether anything is blocked on the
owner's judgement, because sessions keep treating polish as a gate.

**2. What to read first**, in order, with a note on which documents have proven
more accurate when they disagree.

**3. State**, as a table: branch positions, last tag, open PRs, open issues with
one line each, test count, whether both trees are clean. Flag any issue that is
**done but still open** pending a merge to `main` — otherwise it reads as
unfinished work.

**4. What was decided or discovered this session that is NOT in the repo.** This
is the highest-value section and the reason the document exists. Decisions,
rejected alternatives, and anything learned by experiment rather than reading.

**5. What to do next**, concretely enough to start on: the first task, why it is
first, and what is deliberately deferred. Name any decision waiting on the owner
so the next session can ask it immediately in ONE interruption rather than
discovering it half way through.

**6. How the owner wants to be worked with.** This never survives in the repo and
it matters:

- **Do not interrupt constantly.** 15 minutes of his time is charged per
  interruption; batching is cheaper. Hold questions and ask them together.
- **Write plainly. Mark opinions as opinions.** No aphorisms, no slogans, no
  doctrine in a voice that is not his. He cuts and pastes this prose.
- **Explain terms before using them as criteria.**
- **When he reports a symptom, treat it as data.** His plain descriptions have
  repeatedly been the diagnosis; the instinct to explain why he is mistaken has
  been wrong every time so far.

**7. Environment and traps that cost real time.** The rack, the phone, the build
script, `gh` not being on PATH, the PowerShell encoding trap, `git commit -F`
eating `#` lines, the squash-merge/reset-development requirement.

**8. Things established but not obvious from the code.** Measured constants and
what they mean, known-inaccurate fields, failure modes that look like something
else.

**9. What only the owner can do** — and whether he has said not to wait on it.

**10. Habits that actually caught bugs**, so they get repeated: point the harness
at a real file, render to a file and look at it, run a new regression test
against the *unfixed* code, measure before choosing a constant.

## Register

Plain and specific. Prefer "the trim is −90 because the projector is slower than
the audio path" over anything that sounds like a maxim. If a sentence would look
odd in the owner's mouth, rewrite it.

Include failures and dead ends with the reasoning that made them look right —
that is what stops them being repeated.

## Encoding

**Never round-trip the file through PowerShell string replacement.** Write it
with the editing tools, then verify:

```bash
grep -c $'\xc3\xa2' "C:/Users/rogue/Documents/Development/HOLOCRON-HANDOFF.md"
```

Expect `0`. A mojibake diff looks plausible at a glance, so check the byte count
rather than reading the diff.

## Finish

Tell the user the file path and **that they should paste it into the new
session**. Summarise in a few lines what it carries that the repo does not —
that is what makes it worth pasting rather than skipping.

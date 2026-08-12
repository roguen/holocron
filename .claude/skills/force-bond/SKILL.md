---
name: force-bond
description: Write the session handoff — the bond that carries everything this session learned to the instance that comes next, so it starts with full context instead of a blank slate. Use when the user says "force-bond", "write the handoff", "transition document", "I want to start a new instance", "clear the context window", "wrap up the session", or otherwise asks to capture current state so work can continue elsewhere.
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

<!-- measured: trim_ms.rack -->
Plain and specific. Prefer "the trim is −30 because the projector is slower than
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

Three things, in this order.

**1. The file path**, and that they should paste it into the new session.

**2. A few lines on what it carries that the repo does not** — that is what makes
it worth pasting rather than skipping.

**3. THE OPENING PHRASE, verbatim and in its own fenced block**, for the owner to
paste as the first message of the new session.

### The opening phrase

The handoff document is context. The phrase is the *instruction* — what to do with
that context. Without it, a fresh instance reads a status report and waits, or
worse, starts summarising it back.

Write it **fresh every time**, from the state you just verified. A generic phrase
is worse than none, because it wastes the one message that sets the session's
direction.

Put it in a fenced block with no language tag so the owner can copy it in one
gesture, and keep it to a short paragraph — it is a prompt, not a second handoff.

It must do four things:

- **Point at the document.** Name the full path; the new instance cannot see this
  conversation.
- **Give the standing instruction**, not a summary of it. If the owner has asked
  for autonomous operation, say so in the phrase, or the new session will check in
  after every step.
- **Name the first task concretely**, with its issue number. "Continue the project"
  produces a session that asks what to do. "Start on issue 139" does not.
- **Name what is unverified**, so the new instance does not build on top of work
  the owner has not confirmed, or re-report it as done.

Two things it must NOT do:

- **Do not restate the state.** Branch positions and test counts are in the
  document and will be stale in the phrase within an hour.
- **Do not include anything the owner would have to edit before sending.** If it
  contains a placeholder, it is not finished.

Adjust it to the situation rather than following a template. A session ending
mid-task needs a different first instruction from one ending at a clean release,
and a session with a decision waiting on the owner should have that decision in the
phrase so he can answer it in his first reply rather than being asked for it later.

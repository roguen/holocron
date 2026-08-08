---
name: land
description: Land finished work — commit it, PR into development, merge, optionally release to main with a version bump and tag, then open the branch and PR for the next stage. Use when the user says "land this", "merge it down", "ship it", "commit and merge", "cut a release", "tag it", or asks to close out a piece of work and start the next.
---

# Landing work in Holocron

The ritual is the same every time and several steps of it fail **silently** when
skipped. This exists so none of them is remembered rather than followed.

Read `CLAUDE.md` first if it is not already in context — the four standing rules
are the authority and this skill is only their procedure.

## Before anything

**`ctest` green on Windows AND Linux is the bar, not "it compiles."**

```bash
scripts\build.cmd
```

Run it. Report the actual count. If anything fails, stop and fix it — a
regression that reaches `main` is a process failure, not bad luck.

## 1. Commit

**Write the message to a file and use `-F`.** Two reasons, both learned the hard
way:

- Escaped quotes inside a PowerShell `-m` argument break parsing.
- `git commit -F` and `git tag -F` **strip every line beginning with `#`**, and
  this project's messages are full of issue references. There is no error and no
  warning.

```bash
git commit --cleanup=verbatim -F path/to/message.md
```

Always pass `--cleanup=verbatim`. Never start a line with `#` — write `issue 42`
or `PR #42` mid-line instead, which reads better anyway.

**What a good message says here:** what changed, *why the alternative was
rejected*, and what is deliberately NOT done. Plain register — no aphorisms, no
slogans. The owner cuts and pastes this prose, so it ends up representing him.

Close issues with `Closes #N, closes #M` — **one keyword per issue**. `Closes #1,
#2` closes only #1.

## 2. PR into `development`

`main` is protected and enforced for admins too. Everything flows
feature branch → `development` → `main`.

```bash
gh pr create --base development --head <branch> --title "..." --body-file body.md
gh pr checks <N> --watch --interval 25
```

`gh` may not be on PATH — use `C:\Program Files\GitHub CLI\gh.exe`.

**Watch CI to completion.** The Linux job is the only thing in the project that
sees filename case, line endings, `-Wold-style-cast`, `-Wconversion`, and
behaviour that differs off the target. It has caught real defects repeatedly, not
just style.

If it fails, read the log rather than guessing:

```bash
gh run view <run-id> --job <job-id> --log-failed
```

Then squash-merge:

```bash
gh pr merge <N> --squash --delete-branch
```

**`--squash`, never `--merge`.** `main` requires linear history, so a merge
commit is refused outright:

> GraphQL: Merge commits are not allowed on this repository.

That applies to the `development` → `main` merge too, not just feature branches.

## 3. Only if releasing to `main`

**Ask first unless the user has already said to.** A release is a deliberate act.

**The version bump belongs in the SAME change that creates the tag** — never
ahead of it (issue 29). It lives in three files that must move together:

- `CLAUDE.md` — the `Current version` line
- `vcpkg.json` — `version-string`
- `CMakeLists.txt` — `VERSION`

Plus the wiki's Home and Working-Agreement. Nothing checks that they agree
(issue 38), so check by grepping for the old version afterwards.

Semantic versioning: patch within a milestone, **minor per completed
milestone**, `1.0.0` reserved for the first build that plays music and renders
end to end.

Then PR `development` → `main`, merge, and tag:

```bash
git tag -a vX.Y.Z --cleanup=verbatim -F annotation.md
git push origin vX.Y.Z
git tag -l vX.Y.Z --format='%(contents)'
```

**Read the annotation back.** A tag has already published with three paragraphs
silently deleted because they began with `#`. The failure is silent, so the check
has to be deliberate.

**After every merge to `main`, reset `development` onto it and force-push.**
Squash merges plus `strict_up_to_date` make this required, not occasional:

```bash
git checkout development && git reset --hard main && git push --force-with-lease
```

**That reset ORPHANS every open feature branch, and rebasing them will not
work.** Their commits are now squashed into `main`, so a rebase tries to replay
work that is already there and conflicts immediately. **Cherry-pick instead** —
rebuild each branch on the new `development` and take only its own commits:

```bash
git checkout -B <branch> development
git cherry-pick <the branch's own commits>
git push --force-with-lease
```

Then rebuild and re-run the tests on each one before letting CI have it. A
cherry-pick that applies cleanly can still be wrong.

**Issues closed by `Closes #N` only close on a merge to the DEFAULT branch.**
Work merged to `development` leaves them open until it reaches `main` — say so
rather than letting it look like unfinished work.

## 4. Update the wiki

Clone at `Development\holocron-wiki` if it is not already there.

- **Time-Log** — append this session. Separate **agent work** from **the owner's
  time**, counted as **15 minutes per interruption** (any turn ending with
  something he has to react to). Report wall clock only if it was actually
  measured; mark estimates as estimates and never present a derived number as a
  measurement. Note his hands-on bench time separately — the convention has no
  slot for it and it has twice been the larger share.
- **Decision-Log** — a new `D-0NN` for anything settled that is not derivable
  from the code. Include what was rejected and why.
- **Roadmap** — tick exit criteria that actually completed. Do not tick a
  criterion that is only partly met; annotate it instead.

**Never round-trip a Markdown file through PowerShell string replacement.** It
double-encodes every non-ASCII character and CI does not catch it. Use the
editing tools, then verify:

```bash
grep -c $'\xc3\xa2' <file>   # expect 0
```

The tell is a one-line edit producing a diff that touches every line. Check the
byte count, not the diff summary.

## 5. Open the next stage

Create the branch and, if there is already a commit worth showing, the PR. If the
next stage is only starting, an issue is the better artifact — open it **when the
problem is identified**, not retroactively.

```bash
git checkout -b <area>/<what-it-does>
```

Branch names describe the work: `m5/timeline-reporting`, `fix/gitattributes-case`,
`docs/cutting-crystals`.

Labels: `bug` · `enhancement` · `decision` · `blocker` · `contract` · `legal` ·
`chore` · `portability` · `documentation`. Milestones are `M1 Spine and audio`
through `M7 eISCP receiver hook` — use the full title, `gh` will not match `M5`.

## Report back

State plainly: what merged, the test count, what is now open, and **what was not
done**. If something was skipped or is unverified, say which — a status that
overstates by one line makes the rest of it untrustworthy.

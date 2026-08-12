---
name: force-meld
description: Land finished work — commit it, PR into development, watch CI, merge, optionally release to main with a version bump and tag, update the README and wiki, and close the issues by hand. Use when the user says "force-meld", "land this", "merge it down", "ship it", "commit and merge", "cut a release", "tag it", or asks to close out a piece of work and start the next.
---

# Landing work in Holocron

A Force meld joins separate minds into one. This is the same job for branches:
several steps of it fail **silently** when skipped, so it exists to be followed
rather than remembered.

Read `CLAUDE.md` first if it is not already in context — the five standing rules
are the authority and this is only their procedure.

**Rule 5 lands here.** A release publishes for BOTH destinations and deploys to
both — a Windows build and an APK, on the GitHub release for the tag, installed
on the rack and on the Shield. Today neither artifact is published and CI does
not package the APK ([#293](https://github.com/roguen/holocron/issues/293)), so
until that closes, the release steps below build and attach both **by hand** —
`scripts/build.cmd` and `scripts/android-apk.sh` — and say plainly in the report
if either was skipped. A release that reached only the PC is a release that is
half done, not a release with a footnote.

**Do not ask permission for any of this.** The owner's standing instruction is to
commit, PR, watch CI, merge, release and close issues without checking in. The
only thing worth asking about is a *decision*, and this skill contains none.

## Before anything

**`ctest` green on Windows AND Linux is the bar, not "it compiles."**

```bash
scripts\build.cmd
```

Report the actual count. If anything fails, stop and fix it — a regression that
reaches `main` is a process failure, not bad luck.

**If the change touches anything with an `__ANDROID__` branch, run the guard too:**

```bash
bash scripts/android-check.sh
```

It fails deliberately when a file carrying an Android branch was skipped for a
missing dependency. On a machine with an `arm64-android` vcpkg tree, point
`HOLOCRON_ANDROID_INCLUDE` at its `include` directory.

**You cannot link `holocron.exe` while one is running**, including one a
background test started. If a result looks too convenient, check the build
actually succeeded — `ninja: build stopped` scrolls past and the previous binary
runs.

## 1. Commit

**Write the message to a file and use `-F`.** Two reasons, both learned the hard
way:

- Escaped quotes inside a PowerShell `-m` argument break parsing.
- `git commit -F` and `git tag -F` **strip every line beginning with `#`**, and
  this project's messages are full of issue references. No error, no warning.

```bash
git commit --cleanup=verbatim -F path/to/message.md
```

Always `--cleanup=verbatim`. Never start a line with `#` — write `issue 42`
mid-line instead, which reads better anyway.

**Write the message with a real editing tool, not a bash heredoc.** Long heredocs
through the tool layer have repeatedly broken on quoting and left the shell
hanging on an unterminated string. Write the file, then `-F` it.

**What a good message says:** what changed, *why the alternative was rejected*,
what is deliberately NOT done, and **how it was verified** — with the numbers.
Plain register, no aphorisms. The owner cuts and pastes this prose.

**Check the executable bit on any new script.** Git on Windows does not pick it
up from the filesystem:

```bash
git update-index --chmod=+x scripts/whatever.sh
```

## 2. PR into `development`

`main` is protected and enforced for admins too. Everything flows
feature branch → `development` → `main`.

```bash
gh pr create --base development --head <branch> --title "..." --body-file body.md
```

**Always `--body-file`, never `--body`.** Backticks inside a `--body` string are
executed by bash. That has produced a created PR with a mangled body and three
shell errors.

`gh` is not on PATH — use `C:\Program Files\GitHub CLI\gh.exe`.

**Watch CI to completion.** The Linux job is the only thing in the project that
sees filename case, line endings, `-Wold-style-cast` and `-Wconversion`. It has
caught real defects repeatedly. Note that Linux takes ~10 minutes on a vcpkg
cache miss because it builds OpenSSL.

If a check fails, read the log rather than guessing:

```bash
gh run view --repo roguen/holocron --job <job-id> --log-failed
```

Then squash-merge:

```bash
gh pr merge <N> --squash --delete-branch=false
```

**`--squash`, never `--merge`.** `main` requires linear history, so a merge commit
is refused outright: *"GraphQL: Merge commits are not allowed on this
repository."* That applies to `development` → `main` too.

**Branches are kept, not deleted.** The repo carries every historical branch on
purpose.

## 3. Only if releasing to `main`

**The version bump belongs in the SAME change that creates the tag** — never ahead
of it (issue 29). It now lives in **five** places that must move together:

- `CLAUDE.md` — the `Current version` line
- `vcpkg.json` — `version-string`
- `CMakeLists.txt` — `VERSION`
- `android/AndroidManifest.xml` — `versionName`, **and `versionCode`**, a separate
  integer that must increase monotonically or an installer refuses the upgrade
- the wiki's `Home` and `Roadmap`

Nothing checks that they agree (issue 38), so grep for the old version afterwards
and confirm nothing is left.

Semantic versioning: **patch within a milestone, minor per completed milestone.**
The minor number counts milestones *finished*, not which one. `1.0.0` is reserved
for the first build that plays music and renders end to end.

Then PR `development` → `main`, merge, and tag:

```bash
git tag -a vX.Y.Z --cleanup=verbatim -F annotation.md
git push origin vX.Y.Z
git tag -l vX.Y.Z --format='%(contents)' | head
```

**Read the annotation back.** A tag has already published with three paragraphs
silently deleted because they began with `#`.

### After merging to `main`, merge `main` back into `development`

```bash
git checkout development
git merge main -m "Merge branch 'main' into development"
git push
git merge-base --is-ancestor main development && echo ok
```

**A merge commit here, not a reset and force-push.** An older version of this
skill said to `reset --hard main && push --force-with-lease`; the force-push is
blocked by the permission classifier, and it orphans every open feature branch.
Ten releases have gone out with the merge commit and it is the settled practice.

`git rev-list --count main..development` will read high — that is expected, not
drift. Everything reaches `main` squashed, so `development` keeps the individual
commits. **The check that matters is `merge-base --is-ancestor`.**

## 4. Close the issues by hand

**A squash-merge does not close an issue**, even with `Closes #N` in the branch
commit — the keyword has to be in the body of the merge that reaches the default
branch. This has bitten in four separate sessions.

```bash
gh issue list --repo roguen/holocron --state open
```

**Run that after every release.** Close what shipped, with a comment saying what
landed and what is still open — and if only part of an issue shipped, say so and
**retitle it** to what remains rather than closing it.

## 5. Update the README and the wiki

Both, every time. They are the only record that versions with nothing else.

**`README.md`** — the front door, written for a stranger. Update it when
something *user-visible* changed: a new capability, a new platform, a changed
command, a version-worthy claim. Do not let it keep describing a project that no
longer exists.

**The wiki**, cloned at `Development\holocron-wiki`:

- **Decision-Log** — a new `D-0NN` for anything settled that is not derivable from
  the code. **Include what was rejected and why**, and the trigger for revisiting
  it. This is the highest-value page in the project.
- **Roadmap** — tick exit criteria that actually completed. Do not tick one that
  is partly met; annotate it. If a criterion turns out to be unachievable, say so
  and say whose call the amendment is.
- **Time-Log** — append the session. Separate **agent work** from **the owner's
  time**, counted as **15 minutes per interruption**. Report wall clock only if
  measured; mark estimates as estimates; never present a derived number as a
  measurement. The three figures overlap and must not be summed.
- **Home** — the version and the milestone table.

**Correct anything you find stale while you are in there.** The Roadmap called M6
"the only one in progress" for months after its own table said DONE, and
`cutting-crystals.md` handed authors a shader header that could not compile on the
target platform. Both were found by reading a neighbouring paragraph.

**Never round-trip a Markdown file through PowerShell string replacement.** It
double-encodes every non-ASCII character and CI does not catch it. Use the editing
tools or append raw bytes, then verify:

```bash
grep -c $'\xc3\xa2' <file>   # expect 0
```

The tell is a one-line edit producing a diff that touches every line. **Check the
byte count, not the diff summary.**

## 6. Open the next stage

Create the branch, and an issue if the next problem is only identified rather than
started — **open it when the problem is found**, not retroactively when it is
fixed.

```bash
git checkout -b <area>/<what-it-does>
```

Branch names describe the work: `m8/platform-layer`, `fix/gitattributes-case`,
`docs/cutting-crystals`.

Labels: `bug` · `enhancement` · `decision` · `blocker` · `contract` · `legal` ·
`chore` · `portability` · `documentation`.

Milestones must be given as the **full title** — `gh` will not match `M8`:

```
M1 Spine and audio · M2 Crystals · M3 Compositor · M4 projectM
M5 Plex · M6 On-screen UI · M7 eISCP receiver hook · M8 Android TV
```

## Report back

State plainly: what merged, the test count, what is now open, and **what was not
done**. Name anything unverified as unverified — a status that overstates by one
line makes the rest of it untrustworthy.

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Every document that quotes a measured value agrees with docs/measurements.toml.
#
# ISSUE 265. `trim_ms` was re-measured on 2026-08-10 and recorded in
# `gatekeeper.toml` -- which is gitignored, so CI cannot read it. Eight published
# documents went on quoting the superseded figure for a day, and it was found by
# an audit rather than by anything failing.
#
# The source of truth for the PROGRAM stays where it is and stays gitignored.
# What this checks is the source of truth for PROSE: docs/measurements.toml, a
# committed record with a value, a date, a bracket and a resolution per
# measurement. Two rules, enforced in both directions over every tracked `.md`
# and `.toml`:
#
#   1. A block quoting a recorded value must DECLARE which measurement it means.
#      Without this half, a document written next month starts drifting silently
#      and the check reports success for text it never looked at.
#
#   2. A block declaring a measurement must quote its CURRENT value. This is the
#      half that catches 265: edit `value` in the record and every stale
#      document fails by name and line.
#
# WHAT IT CANNOT DO, said plainly: it cannot check the record against
# `gatekeeper.toml`, because that file is gitignored by design and CI has none.
# Updating the record after re-measuring is a human step. It is one step instead
# of eight, and `--calibrate` now prints the reminder at the moment it matters.
#
# Run it from anywhere in the tree:
#
#   scripts/check-measurements.sh
#
# It is a script rather than a step pasted into the workflow file for the same
# reason scripts/android-check.sh is: the machine the work happens on can run it
# before pushing.

set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

record="docs/measurements.toml"

if [ ! -f "$record" ]; then
    echo "::error::$record is missing -- the check has nothing to check against"
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- the record ------------------------------------------------------------
#
# Flattened to `key<TAB>value` lines so the scanner below does not have to parse
# TOML twice. Only `key` and `value` are load-bearing; the date, bracket and
# resolution are there for the reader, and for whoever has to decide whether a
# figure is still trustworthy.
awk '
    /^\[\[measurement\]\]/          { key = ""; value = ""; next }
    /^[ \t]*key[ \t]*=/             { key   = extract($0); next }
    /^[ \t]*value[ \t]*=/           { value = extract($0)
                                      if (key == "") {
                                          print "MALFORMED: value with no key" > "/dev/stderr"
                                          exit 1
                                      }
                                      print key "\t" value
                                      next }
    function extract(line,   s) {
        sub(/^[^=]*=[ \t]*/, "", line)
        s = line
        sub(/^"/, "", s)
        sub(/"[ \t]*$/, "", s)
        return s
    }
' "$record" > "$tmp/record"

if [ ! -s "$tmp/record" ]; then
    echo "::error::$record parsed to nothing -- the check would pass on any document"
    exit 1
fi

echo "measurements on record:"
while IFS=$'\t' read -r k v; do
    printf '  %-28s %s\n' "$k" "$v"
done < "$tmp/record"
echo

# --- the documents ---------------------------------------------------------
#
# `git ls-files` rather than a directory walk or a list in this script: the set
# that matters is the set that is TRACKED, a list would rot, and a walk would
# pick up build output and whatever is parked in the tree unstaged.
#
# -F as well as -x on the exclusion: the record's own path contains a dot, and as
# a regex that matches any character. A check that quietly stops scanning a file
# because its name resembles another one is the failure mode this whole script
# exists to prevent.
git ls-files '*.md' '*.toml' | grep -v -F -x "$record" > "$tmp/files" || true

# AN EXTRA DIRECTORY, FOR THE WIKI. Issue 316.
#
# The wiki is a SEPARATE GIT REPOSITORY and `git ls-files` cannot see it, so
# every measured value it quotes -- both trims, the Shield's frame times, the
# compile figures, across four pages -- was unguarded. That is issue 265's
# failure mode one repository over: `trim_ms` moved, was correctly recorded, and
# eight published documents went on quoting the old figure because nothing could
# fail.
#
# A DIRECTORY WALK RATHER THAN ls-files, deliberately, because the caller may
# hand us a fresh clone or a working tree and neither is guaranteed to be a
# repository we can query. Everything else -- the markers, the normalisation, the
# awk -- is shared, so the wiki is held to exactly the same rule as `docs/` with
# no second implementation to drift.
if [ -n "${1:-}" ]; then
    if [ ! -d "$1" ]; then
        echo "::error::extra scan directory does not exist: $1"
        exit 1
    fi
    # TWO PAGES ARE EXEMPT, AND THE REASON IS WHAT THEY ARE RATHER THAN THEIR
    # SIZE. Time-Log and Decision-Log are append-only records of what was
    # believed or decided at a dated moment. A trim quoted in session 12's entry
    # is not a claim about today and must not start failing when today's figure
    # moves -- the entry would become false if it were edited to agree. The
    # superseded `@date` keys exist for the handful of places that deliberately
    # quote an old reading in a LIVING page.
    #
    # Everything else on the wiki asserts current truth and is held to the rule.
    find "$1" -type f \( -name '*.md' -o -name '*.toml' \) -not -path '*/.git/*'         -not -name 'Time-Log.md' -not -name 'Decision-Log.md'         >> "$tmp/files"
    echo "also scanning $1 (Time-Log and Decision-Log exempt -- append-only history)"
fi

count=$(wc -l < "$tmp/files" | tr -d ' ')
if [ "$count" -eq 0 ]; then
    echo "::error::no tracked documents to scan -- the check would pass on anything"
    exit 1
fi
echo "scanning $count tracked document(s) against $record"

status=0

while IFS= read -r file; do
    # U+2212 MINUS SIGN and U+2013 EN DASH normalise to ASCII '-'. The prose uses
    # the typographic minus and the config files use the ASCII one, and they are
    # the same number. U+2014 EM DASH is deliberately left alone: it is
    # punctuation here, never a sign.
    sed 's/\xe2\x88\x92/-/g; s/\xe2\x80\x93/-/g' "$file" > "$tmp/norm"

    case "$file" in
        *.md)   marker_re='^[ \t]*<!--[ \t]*measured:[ \t]*([^ \t]+)[ \t]*-->[ \t]*$' ;;
        *.toml) marker_re='^[ \t]*#[ \t]*measured:[ \t]*([^ \t]+)[ \t]*$' ;;
        *)      continue ;;
    esac

    awk -v FILE="$file" -v MARKER_RE="$marker_re" -v RECORD="$record" '
        # CRLF, STRIPPED HERE RATHER THAN IN THE sed ABOVE. Issue 316: the wiki
        # is a separate repository with no .gitattributes and mixed line
        # endings, and every marker regex below is anchored with $ -- so a
        # trailing CR makes a perfectly good marker invisible and the file
        # silently reports nothing. Doing it in awk covers every line the
        # program sees, including the quotation lines, not just the markers.
        { sub(/$/, "") }
        function fail(line, msg) {
            printf "::error file=%s,line=%d::%s\n", FILE, line, msg
            bad = 1
        }

        # A recorded value, matched as a whole token. The boundary rules are what
        # keep "android-30" and "2024-11-30" out of a check about a trim of -30:
        # the character before may not be alphanumeric, an underscore or a hyphen.
        #
        # The trailing rule has to tell a DECIMAL POINT from a FULL STOP, and
        # that distinction is not cosmetic -- it cost a debugging round here. A
        # sentence ending "...it was -90." is a quotation of -90; "-90.5" is a
        # different number. So a dot is allowed only when a digit does not follow
        # it. Rejecting every trailing dot silently misses every value that ends
        # a sentence, which is most of them in prose.
        function quotes(text, v,   re, s, hits) {
            re = "(^|[^0-9A-Za-z_-])" esc(v) "([^0-9.]|[.][^0-9]|[.]$|$)"
            s = text
            hits = 0
            while (match(s, re)) {
                hits++
                # Keep the trailing boundary character: it is the leading
                # boundary of whatever comes next, and dropping it hides a
                # second value written immediately after the first.
                s = substr(s, RSTART + RLENGTH - 1)
            }
            return hits
        }

        function esc(v,   s) {
            s = v
            gsub(/\./, "\\.", s)
            gsub(/\+/, "\\+", s)
            return s
        }

        # --- the record, read first -------------------------------------------
        NR == FNR {
            split($0, f, "\t")
            val[f[1]] = f[2]
            keys[++nkeys] = f[1]
            next
        }

        # --- block accumulation ------------------------------------------------
        {
            if ($0 ~ /^[ \t]*$/) { flush(); next }
            if (nlines == 0) start = FNR
            if (match($0, MARKER_RE)) {
                # The key is the marker regex capture, which awk does not give
                # us -- take it out of the line directly.
                k = $0
                sub(/^[ \t]*(<!--|#)[ \t]*measured:[ \t]*/, "", k)
                sub(/[ \t]*(-->)?[ \t]*$/, "", k)
                if (!(k in val)) {
                    fail(FNR, "marker names \"" k "\", which is not in " RECORD)
                } else {
                    declared[k] = FNR
                }
                next
            }
            body[++nlines] = $0
            bodyline[nlines] = FNR
        }

        END { at_end = 1; flush(); exit bad }

        function flush(   i, k, v, text) {
            if (nlines == 0) {
                # A block of markers and nothing else declares for the block that
                # follows it. People put a blank line after a comment; refusing
                # that would make the marker awkward to place, and an awkward
                # marker is one that gets left off.
                #
                # Unless there is no block after it. A marker dangling at the end
                # of a file declares nothing and would otherwise be the one way
                # to write a marker that is never checked.
                if (at_end) {
                    for (k in declared) {
                        fail(declared[k],
                             "declares " k " but no block follows it -- the " \
                             "marker covers nothing and checks nothing.")
                    }
                }
                return
            }

            text = ""
            for (i = 1; i <= nlines; i++) text = text "\n" body[i]

            # Rule 1 -- every recorded value quoted here must be declared.
            for (i = 1; i <= nkeys; i++) {
                k = keys[i]
                v = val[k]
                if (quotes(text, v) == 0) continue
                if (declared_value_present(v)) continue
                fail(line_quoting(v),
                     "quotes " v ", which is " k " in " RECORD \
                     ", without declaring it. Put " marker_form(k) \
                     " on its own line above this block -- and if the text " \
                     "means a different reading of the same quantity, declare " \
                     "that one instead.")
            }

            # Rule 2 -- every declared measurement must actually be quoted.
            for (k in declared) {
                if (quotes(text, val[k]) > 0) continue
                fail(declared[k],
                     "declares " k ", whose recorded value is " val[k] \
                     ", but this block does not quote it. Either the value " \
                     "moved and this text is stale, or the marker is in the " \
                     "wrong place.")
            }

            nlines = 0
            ndeclared = 0
            delete declared
            delete body
            delete bodyline
        }

        function declared_value_present(v,   k) {
            for (k in declared) if (val[k] == v) return 1
            return 0
        }

        function line_quoting(v,   i) {
            for (i = 1; i <= nlines; i++) if (quotes(body[i], v) > 0) return bodyline[i]
            return start
        }

        function marker_form(k) {
            return (FILE ~ /\.md$/) ? "<!-- measured: " k " -->" : "# measured: " k
        }
    ' "$tmp/record" "$tmp/norm" || status=1
done < "$tmp/files"

if [ "$status" -ne 0 ]; then
    echo
    echo "::error::A document disagrees with $record. If you re-measured, update"
    echo "::error::the record first -- every quotation then fails until it agrees,"
    echo "::error::which is the point."
    exit 1
fi

echo "every quotation agrees with $record"

# --- the one link this cannot enforce --------------------------------------
#
# ADVISORY, NEVER FATAL. A real gatekeeper.toml is gitignored, so CI has none and
# this is silent there. On the rack it is the only automatic sign that a
# re-measurement has not travelled out to the record yet. It is not an error
# because a second rack legitimately has a different trim, and failing a
# stranger's checkout for owning different hardware would be wrong.
if [ -f gatekeeper.toml ]; then
    local_trim=$(sed -n 's/^[ \t]*trim_ms[ \t]*=[ \t]*\(-\?[0-9.]*\).*/\1/p' gatekeeper.toml | head -1)
    recorded=$(awk -F'\t' '$1 == "trim_ms.rack" { print $2 }' "$tmp/record")
    if [ -n "$local_trim" ] && [ -n "$recorded" ]; then
        # Compared as numbers: the config writes -30.0 and the record writes -30.
        if awk -v a="$local_trim" -v b="$recorded" 'BEGIN { exit (a + 0 == b + 0) ? 0 : 1 }'; then
            echo "local gatekeeper.toml trim_ms = $local_trim, which is trim_ms.rack"
        else
            echo
            echo "NOTE: local gatekeeper.toml has trim_ms = $local_trim; $record"
            echo "NOTE: records trim_ms.rack = $recorded. If you re-measured, the record"
            echo "NOTE: has not caught up yet. If this is not the reference rack, ignore"
            echo "NOTE: this -- the record describes one specific signal chain."
        fi
    fi
else
    echo "no local gatekeeper.toml -- the config comparison is skipped, as it is in CI"
fi

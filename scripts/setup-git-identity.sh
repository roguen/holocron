#!/usr/bin/env bash
# Configure this clone's git identity. Run once per machine, before the first commit.
#
# WHY THIS EXISTS
#
# Holocron is a public repo. Author addresses in its history are permanent: once a
# commit is in a fork, rewriting history does not retract it. Two things make the
# default wrong here:
#
#   1. The owner's GLOBAL git identity is a work address. Committing with it would
#      publish that address and mis-attribute personal work.
#   2. Two GitHub accounts may be authenticated in `gh`. Without pinning, the
#      credential helper can resolve to the wrong one.
#
# So identity is configured LOCAL to this clone and the remote is pinned to the
# owning account. Nothing global is modified. This script is idempotent.

set -euo pipefail

NAME="Roguen Keller"
EMAIL="4869915+roguen@users.noreply.github.com"   # GitHub noreply: keeps the real address private
ACCOUNT="roguen"
REPO="roguen/holocron"

cd "$(dirname "$0")/.."

if ! git rev-parse --git-dir >/dev/null 2>&1; then
    echo "error: not a git repository — run this from inside the clone" >&2
    exit 1
fi

echo "Configuring git identity for this clone only."
echo

# --- identity, local to this repository -------------------------------------
git config --local user.name  "$NAME"
git config --local user.email "$EMAIL"

# --- pin the remote to the owning account -----------------------------------
# Embedding the username makes git request credentials for THAT account, so a
# multi-account `gh` setup cannot silently resolve to the other one.
if git remote get-url origin >/dev/null 2>&1; then
    git remote set-url origin "https://${ACCOUNT}@github.com/${REPO}.git"
else
    git remote add origin "https://${ACCOUNT}@github.com/${REPO}.git"
fi

# --- credential helper, also local ------------------------------------------
# Deliberately not global: leaves the machine's other repos alone.
if command -v gh >/dev/null 2>&1; then
    git config --local credential."https://github.com".helper '!gh auth git-credential'
else
    echo "  ! gh CLI not found. Install it, or configure credentials another way."
    echo "    Pushes will fail until then."
fi

echo "  user.name   $(git config --local user.name)"
echo "  user.email  $(git config --local user.email)"
echo "  origin      $(git remote get-url origin)"
echo "  helper      $(git config --local credential."https://github.com".helper 2>/dev/null || echo '(none)')"
echo

# --- report, do not modify, the global identity -----------------------------
GLOBAL_EMAIL="$(git config --global user.email 2>/dev/null || true)"
if [ -n "$GLOBAL_EMAIL" ] && [ "$GLOBAL_EMAIL" != "$EMAIL" ]; then
    echo "  note: global user.email is '$GLOBAL_EMAIL' and was left untouched."
    echo "        The local setting above overrides it for this repo only."
    echo
fi

# --- verify auth without pushing --------------------------------------------
if command -v gh >/dev/null 2>&1; then
    if GIT_TERMINAL_PROMPT=0 git ls-remote origin >/dev/null 2>&1; then
        echo "  auth OK (verified read-only against origin)"
    else
        echo "  ! could not reach origin. Try: gh auth login"
        echo "    If several accounts are authenticated: gh auth switch --user $ACCOUNT"
    fi
fi

echo
echo "Done. After your first commit, confirm with:"
echo "  git log -1 --format='%an <%ae>'"

#!/bin/sh
# Apply the OnePlus 3T (msm8996pro) device-enablement series onto a mainline base.
#
# Usage:  ./op3t-port/apply.sh [base-tag] [new-branch]
#   base-tag    mainline/stable tag to build on   (default: v6.12.95)
#   new-branch  branch to create                  (default: op3t/<base>)
#
# Example (port to a newer stable point release):
#   ./op3t-port/apply.sh v6.12.100 op3t/6.12.100
#
# On base drift, `git am` pauses on the first conflicting patch; resolve, then
# `git am --continue`. 6.12.x stable is an API *mix* (some 6.14 changes are
# backported, some not), so a different base may need different fixup patches.
set -eu

BASE="${1:-v6.12.95}"
BRANCH="${2:-op3t/${BASE#v}}"
DIR="$(cd "$(dirname "$0")" && pwd)"

command -v git >/dev/null || { echo "git not found" >&2; exit 1; }
git rev-parse -q --verify "${BASE}^{commit}" >/dev/null \
    || { echo "base tag '$BASE' not found — fetch it first (e.g. from linux-stable)" >&2; exit 1; }

echo ">> creating $BRANCH from $BASE"
git checkout -b "$BRANCH" "$BASE"

echo ">> applying $(ls "$DIR"/patches/*.patch | wc -l | tr -d ' ') patches (3-way)"
git am --3way "$DIR"/patches/*.patch

echo ">> done: $BRANCH = $BASE + OP3T series"

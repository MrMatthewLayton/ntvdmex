#!/usr/bin/env bash
#
# publish.sh -- push docs/wiki/ to the GitHub wiki.
#
# WHY THE SOURCE IS IN THIS REPO. A GitHub wiki is a separate git repository. Content
# authored only there does not arrive with a `git clone`, is not reviewable in a pull
# request alongside the change it describes, and drifts from the code silently -- which is
# the exact failure this project just spent a session cleaning up (docs/STATE.md had been
# wrong about the project's phase for three weeks).
#
# So: docs/wiki/ is the SOURCE OF TRUTH and the wiki is a PUBLISH TARGET. Edit the files
# here, review them like code, and run this to publish.
#
#   tools/wiki/publish.sh              # publish
#   tools/wiki/publish.sh --dry-run    # show what would change
#
# ⚠ FIRST RUN ONLY: a GitHub wiki repository does not exist until its first page has been
#   created through the web UI -- there is no API for it. If this script reports
#   "Repository not found", go to
#       https://github.com/MrMatthewLayton/ntvdmex/wiki
#   click "Create the first page", save anything at all, and re-run. This script will
#   overwrite it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/.."
ROOT="$(cd "$ROOT" && pwd)"
SRC="$ROOT/docs/wiki"
REMOTE="https://github.com/MrMatthewLayton/ntvdmex.wiki.git"
WORK="${TMPDIR:-/tmp}/ntvdmex-wiki"
DRY=0
[[ "${1:-}" == "--dry-run" ]] && DRY=1

[[ -d "$SRC" ]] || { echo "no $SRC" >&2; exit 1; }

rm -rf "$WORK"
if ! git clone --quiet "$REMOTE" "$WORK" 2>/dev/null; then
    cat >&2 <<'MSG'
Could not clone the wiki repository.

A GitHub wiki has no git remote until its first page is created in the web UI, and
there is no API to do it. Open:

    https://github.com/MrMatthewLayton/ntvdmex/wiki

click "Create the first page", save anything, then re-run this script -- it will
overwrite whatever you saved.
MSG
    exit 1
fi

# Mirror docs/wiki/ into the wiki working tree. Page name = file name, so
# docs/wiki/Traps-and-lessons.md becomes the "Traps and lessons" page.
find "$WORK" -maxdepth 1 -name '*.md' -delete
cp "$SRC"/*.md "$WORK"/

cd "$WORK"
if [[ -z "$(git status --porcelain)" ]]; then
    echo "wiki already up to date"
    exit 0
fi

git status --short
if (( DRY )); then
    echo "(dry run -- nothing pushed)"
    exit 0
fi

git add -A
git -c user.name="${GIT_AUTHOR_NAME:-$(git -C "$ROOT" config user.name)}" \
    -c user.email="${GIT_AUTHOR_EMAIL:-$(git -C "$ROOT" config user.email)}" \
    commit --quiet -m "docs: publish from docs/wiki/ ($(git -C "$ROOT" rev-parse --short HEAD))"
git push --quiet origin HEAD
echo "published $(ls "$SRC"/*.md | wc -l | tr -d ' ') pages"

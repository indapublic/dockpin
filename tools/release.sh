#!/bin/bash
# Releases DockPin <version>: bumps Info.plist, tags v<version>, pushes,
# creates the GitHub release and points Formula/dockpin.rb at the new tag's
# archive (commit + push).
#
# Usage: tools/release.sh <x.y.z>
#   GH_REPO overrides the default below.
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
GH_REPO=${GH_REPO:-indapublic/dockpin}
VERSION=${1:-}
TAG=v$VERSION

step() { echo; echo "=== $*"; }

# GitHub occasionally does not answer, and a fresh tag's archive can take a
# moment to appear.
retry() {
  local attempt
  for attempt in 1 2 3; do
    "$@" && return 0
    echo "Attempt $attempt failed: $*"
    sleep 10
  done
  return 1
}

if [[ ! $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Usage: tools/release.sh <x.y.z>"
  exit 2
fi
cd "$REPO_DIR"

step "Check the working tree"
if [ "$(git branch --show-current)" != main ]; then echo "Not on main"; exit 1; fi
if [ -n "$(git status --porcelain)" ]; then echo "Uncommitted changes"; exit 1; fi
git fetch -q origin main --tags
if [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]; then
  echo "main is not in sync with origin/main"
  exit 1
fi
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
  echo "$TAG already exists"
  exit 1
fi

step "Build and test"
make DockPin.app test
make clean

step "Tag $TAG"
BUILD=$(( $(plutil -extract CFBundleVersion raw Info.plist) + 1 ))
plutil -replace CFBundleShortVersionString -string "$VERSION" Info.plist
plutil -replace CFBundleVersion -string "$BUILD" Info.plist
git add Info.plist
git commit -q -m "DockPin $VERSION"
git tag "$TAG"
retry git push -q origin main "$TAG"

step "Publish $TAG to $GH_REPO"
retry gh release create "$TAG" --repo "$GH_REPO" --title "DockPin $VERSION" \
  --notes "Install or update: \`brew upgrade dockpin\` (see README)."

step "Point the formula at $TAG"
URL=https://github.com/$GH_REPO/archive/refs/tags/$TAG.tar.gz
ARCHIVE=$(mktemp)
trap 'rm -f "$ARCHIVE"' EXIT
retry curl -fsSL -o "$ARCHIVE" "$URL"
SHA256=$(shasum -a 256 "$ARCHIVE" | cut -d' ' -f1)
echo "sha256 $SHA256"
sed -i '' -E "s|^  url \".*\"|  url \"$URL\"|; s|^  sha256 \".*\"|  sha256 \"$SHA256\"|" Formula/dockpin.rb
git add Formula/dockpin.rb
git commit -q -m "dockpin $VERSION"
retry git push -q origin main
echo "Released. Users update with: brew update && brew upgrade dockpin"

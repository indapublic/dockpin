#!/bin/bash
# Releases DockPin <version>: builds a universal DockPin.app signed with the
# Developer ID and notarized, tags v<version>, publishes the app as a GitHub
# release and points Formula/dockpin.rb at it (commit + push).
#
# The Developer ID signature is what keeps the Accessibility permission across
# updates: macOS ties it to the team and bundle id instead of the exact build.
#
# One-time setup:
#   - "Developer ID Application" certificate in the login keychain;
#   - notarization credentials saved under a keychain profile:
#       xcrun notarytool store-credentials <profile> --apple-id <apple id> \
#         --team-id 4WSCP7LMZQ
#
# Usage: tools/release.sh <x.y.z>
#   DRY_RUN=1  build, sign and notarize only; nothing is committed or published
#   IDENTITY, NOTARY_PROFILE, GH_REPO override the defaults below.
set -euo pipefail

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
IDENTITY=${IDENTITY:-"Developer ID Application: Vasilii Zolotukhin (4WSCP7LMZQ)"}
NOTARY_PROFILE=${NOTARY_PROFILE:-plurium}
GH_REPO=${GH_REPO:-indapublic/dockpin}
DRY_RUN=${DRY_RUN:-0}
VERSION=${1:-}
TAG=v$VERSION

step() { echo; echo "=== $*"; }

# Apple's timestamp server and GitHub occasionally do not answer.
retry() {
  local attempt
  for attempt in 1 2 3; do
    "$@" && return 0
    echo "Attempt $attempt failed: $*"
    sleep 10
  done
  return 1
}

# Submits $1 to Apple's notary service, waits, fails with the log unless
# accepted.
notarize() {
  local result status id
  result=$(xcrun notarytool submit "$1" --keychain-profile "$NOTARY_PROFILE" \
    --wait --output-format plist)
  status=$(plutil -extract status raw -o - - <<< "$result")
  echo "Notarization: $status"
  if [ "$status" != Accepted ]; then
    id=$(plutil -extract id raw -o - - <<< "$result")
    xcrun notarytool log "$id" --keychain-profile "$NOTARY_PROFILE" || true
    exit 1
  fi
}

if [[ ! $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "Usage: tools/release.sh <x.y.z>"
  exit 2
fi
cd "$REPO_DIR"

step "Check the working tree and the signing setup"
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
security find-identity -v -p codesigning | grep -qF "$IDENTITY" ||
  { echo "Signing identity not found: $IDENTITY"; exit 1; }
xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null ||
  { echo "No notarization credentials in keychain profile '$NOTARY_PROFILE' (see the header)"; exit 1; }

step "Build and test"
make DockPin.app test
make clean

WORK=$(mktemp -d)
COMMITTED=0
cleanup() {
  rm -rf "$WORK"
  # Until the release commit exists, leave the tree as it was.
  if [ "$COMMITTED" -eq 0 ]; then git checkout -q -- Info.plist; make -s clean; fi
}
trap cleanup EXIT

step "Build, sign and notarize DockPin $VERSION"
BUILD=$(( $(plutil -extract CFBundleVersion raw Info.plist) + 1 ))
plutil -replace CFBundleShortVersionString -string "$VERSION" Info.plist
plutil -replace CFBundleVersion -string "$BUILD" Info.plist
retry make DockPin.app SIGN="$IDENTITY" \
  CFLAGS="-O2 -Wall -arch arm64 -arch x86_64 -mmacosx-version-min=11.0"
codesign --verify --strict DockPin.app
ditto -c -k --keepParent DockPin.app "$WORK/notarize.zip"
notarize "$WORK/notarize.zip"
retry xcrun stapler staple DockPin.app
spctl --assess --type execute -vv DockPin.app

# Homebrew steps into an archive's lone top-level directory, so the app sits
# one level down.
PKG=dockpin-$VERSION
ZIP=$WORK/DockPin-$VERSION.zip
mkdir "$WORK/$PKG"
ditto DockPin.app "$WORK/$PKG/DockPin.app"
cp LICENSE README.md "$WORK/$PKG/"
ditto -c -k --keepParent "$WORK/$PKG" "$ZIP"
SHA256=$(shasum -a 256 "$ZIP" | cut -d' ' -f1)
echo "sha256 $SHA256"

if [ "$DRY_RUN" = 1 ]; then
  echo
  echo "Dry run: nothing committed or published."
  exit 0
fi

step "Tag $TAG"
git add Info.plist
git commit -q -m "DockPin $VERSION"
COMMITTED=1
git tag "$TAG"
retry git push -q origin main "$TAG"

step "Publish $TAG to $GH_REPO"
# gh deletes the half-created release when the upload fails, so a retry
# starts clean.
retry gh release create "$TAG" "$ZIP" --repo "$GH_REPO" --title "DockPin $VERSION" \
  --notes "Universal build, signed with a Developer ID and notarized.

Install or update: \`brew upgrade dockpin\` (see README)."

step "Point the formula at $TAG"
URL=https://github.com/$GH_REPO/releases/download/$TAG/DockPin-$VERSION.zip
sed -i '' -E "s|^  url \".*\"|  url \"$URL\"|; s|^  sha256 \".*\"|  sha256 \"$SHA256\"|" Formula/dockpin.rb
git add Formula/dockpin.rb
git commit -q -m "dockpin $VERSION"
retry git push -q origin main
make -s clean
echo "Released. Users update with: brew update && brew upgrade dockpin"

#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: package-gfxbench-ipa.sh GFXBENCH_APP OUTPUT_DIR

Zips the GFXBench.app that build-droidvm-gfxbench.sh built into an IPA that
serves both the guest and a real iPad or iPhone (a reference for the guest's
rendering). The app is not changed: it keeps the build's ad-hoc signature, whose
CDHash the guest trusts and whose entitlements the guest needs, and it picks its
orientations from the panel at run time. A sideloading tool re-signs it for a
real device with its user's certificate and provisioning profile entitlements.
GitHub release assets stop at 2 GiB, so an IPA over 1.9 GiB is also cut into
parts: cat them back together in order. Writes SHA256SUMS. Requires macOS.
EOF
}

if [[ $# -ne 2 ]]; then
    usage >&2
    exit 2
fi

die() {
    printf 'package-gfxbench-ipa: %s\n' "$*" >&2
    exit 1
}

APP=$(cd "$1" && pwd -P)
mkdir -p "$2"
OUTPUT_DIR=$(cd "$2" && pwd -P)
[[ -f "$APP/Info.plist" ]] || die "not an app bundle: $APP"
for required in ditto codesign plutil shasum split; do
    command -v "$required" >/dev/null 2>&1 || die "required macOS tool not found: $required"
done
codesign --verify --deep --strict "$APP" || die "the app's signature does not verify: $APP"

VERSION=$(plutil -extract CFBundleShortVersionString raw "$APP/Info.plist")
IPA_NAME="GFXBench-Metal-$VERSION-droidvm.ipa"
IPA="$OUTPUT_DIR/$IPA_NAME"
PART_BYTES=$((1900 * 1024 * 1024))

STAGING_DIR=$(mktemp -d "$OUTPUT_DIR/.ipa-stage.XXXXXXXX")
trap 'rm -rf "$STAGING_DIR"' EXIT
STAGED_APP="$STAGING_DIR/Payload/GFXBench.app"
mkdir "$STAGING_DIR/Payload"
ditto "$APP" "$STAGED_APP"
codesign --verify --deep --strict "$STAGED_APP" || die 'the staged copy does not verify'
printf 'CDHash: %s\n' "$(codesign --display --verbose=4 "$STAGED_APP" 2>&1 | sed -n 's/^CDHash=//p' | head -n 1)"

rm -f "$IPA" "$IPA".part-*
(cd "$STAGING_DIR" && ditto -c -k --norsrc --noextattr --keepParent Payload "$IPA")
SIZE=$(stat -f %z "$IPA")
printf 'IPA: %s (%s bytes)\n' "$IPA" "$SIZE"

cd "$OUTPUT_DIR"
SUMS=("$IPA_NAME")
if (( SIZE > PART_BYTES )); then
    split -b "$PART_BYTES" -a 1 "$IPA_NAME" "$IPA_NAME.part-"
    for part in "$IPA_NAME".part-*; do
        SUMS+=("$part")
    done
    printf 'Cut into %d parts: cat %s.part-* > %s\n' $((${#SUMS[@]} - 1)) "$IPA_NAME" "$IPA_NAME"
fi
shasum -a 256 "${SUMS[@]}" > SHA256SUMS
cat SHA256SUMS

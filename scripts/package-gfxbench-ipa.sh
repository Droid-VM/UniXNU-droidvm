#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: package-gfxbench-ipa.sh GFXBENCH_APP OUTPUT_DIR

Makes a sideloadable IPA of the GFXBench.app that build-droidvm-gfxbench.sh
built for the guest, for a real iPad or iPhone (a reference for the guest's
rendering). The guest's signature and private entitlements are replaced by an
ad-hoc signature with no entitlements (a sideloading tool re-signs the app with
its user's certificate), and the iPad orientations go back to GFXBench's own,
landscape only (the guest build allows portrait for its landscape-native panel).
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

VERSION=$(plutil -extract CFBundleShortVersionString raw "$APP/Info.plist")
IPA_NAME="GFXBench-Metal-$VERSION-droidvm.ipa"
IPA="$OUTPUT_DIR/$IPA_NAME"
PART_BYTES=$((1900 * 1024 * 1024))

STAGING_DIR=$(mktemp -d "$OUTPUT_DIR/.ipa-stage.XXXXXXXX")
trap 'rm -rf "$STAGING_DIR"' EXIT
STAGED_APP="$STAGING_DIR/Payload/GFXBench.app"
mkdir "$STAGING_DIR/Payload"
ditto "$APP" "$STAGED_APP"
rm -rf "$STAGED_APP/_CodeSignature"
plutil -replace 'UISupportedInterfaceOrientations~ipad' \
    -json '["UIInterfaceOrientationLandscapeLeft","UIInterfaceOrientationLandscapeRight"]' "$STAGED_APP/Info.plist"
codesign --force --sign - "$STAGED_APP"
codesign --display --entitlements - --xml "$STAGED_APP" 2>/dev/null | grep -q '<key>' && die 'the ad-hoc signature still has entitlements'

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

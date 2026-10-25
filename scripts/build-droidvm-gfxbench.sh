#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-droidvm-gfxbench.sh SOURCE_DIR OUTPUT_DIR TREE_ROOT

Builds the pinned GFXBench Metal iOS app from a source checkout. The checkout
is never cleaned or reset; the packaged compatibility patch is applied if
needed. Requires macOS, Xcode, CMake, and the iOS Metal toolchain.
EOF
}

if [[ $# -ne 3 ]]; then
    usage >&2
    exit 2
fi

SOURCE_DIR=$(cd "$1" && pwd -P)
mkdir -p "$2"
OUTPUT_DIR=$(cd "$2" && pwd -P)
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
DEVICE_ROOT=$(cd "$SCRIPT_DIR/.." && pwd -P)
TREE_ROOT=$(cd "$3" && pwd -P)
PACKAGE_DIR="$DEVICE_ROOT/package/gfxbench"
PATCH_FILE="$PACKAGE_DIR/compat.patch"
PINNED_REVISION=89aa956f43836798d1eaaab8ec32d3dc8b02eead
PINNED_PATCH_SHA256=f79c92798f54212851fbcd0a88ad3e6fe7dfc964752c70991f71fad255fbb9bf
APP_NAME='GFXBench Metal'
APP_VERSION=5.0.0

die() {
    printf 'build-droidvm-gfxbench: %s\n' "$*" >&2
    exit 1
}

[[ -d "$SOURCE_DIR/.git" || -f "$SOURCE_DIR/.git" ]] || die "not a Git checkout: $SOURCE_DIR"
[[ -f "$SOURCE_DIR/scripts/build.sh" && -f "$SOURCE_DIR/scripts/build-3rdparty.sh" ]] || die 'source checkout is missing build scripts'
[[ -f "$PATCH_FILE" ]] || die "compatibility patch is missing: $PATCH_FILE"

SOURCE_ROOT=$(git -C "$SOURCE_DIR" rev-parse --show-toplevel)
[[ "$(cd "$SOURCE_ROOT" && pwd -P)" == "$SOURCE_DIR" ]] || die 'SOURCE_DIR must name the checkout root'
ACTUAL_REVISION=$(git -C "$SOURCE_DIR" rev-parse HEAD)
[[ "$ACTUAL_REVISION" == "$PINNED_REVISION" ]] || die "expected source revision $PINNED_REVISION, got $ACTUAL_REVISION"
ACTUAL_PATCH_SHA256=$(shasum -a 256 "$PATCH_FILE" | awk '{print $1}')
[[ "$ACTUAL_PATCH_SHA256" == "$PINNED_PATCH_SHA256" ]] || die 'compat.patch does not match the pinned package metadata'

if git -C "$SOURCE_DIR" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1; then
    printf 'Compatibility patch is already applied.\n'
elif git -C "$SOURCE_DIR" apply --check "$PATCH_FILE" >/dev/null 2>&1; then
    git -C "$SOURCE_DIR" apply "$PATCH_FILE"
    git -C "$SOURCE_DIR" apply --reverse --check "$PATCH_FILE" >/dev/null 2>&1 || die 'patch application did not validate'
    printf 'Applied compatibility patch.\n'
else
    die 'compat.patch does not apply cleanly; source checkout was left unchanged'
fi

for required in xcrun xcodebuild codesign ditto shasum; do
    command -v "$required" >/dev/null 2>&1 || die "required macOS tool not found: $required"
done
[[ -x /usr/bin/python3 ]] || die '/usr/bin/python3 is required for the isolated Python wrapper'
xcrun --sdk iphoneos --show-sdk-path >/dev/null || die 'the iPhoneOS SDK is unavailable'

TOOLS_DIR="$OUTPUT_DIR/tools"
INTERMEDIATE_DIR="$OUTPUT_DIR/intermediate"
BUILD_ROOT="$INTERMEDIATE_DIR/build"
INSTALL_ROOT="$INTERMEDIATE_DIR/install"
THIRDPARTY_ROOT="$INTERMEDIATE_DIR/3rdparty"
mkdir -p "$TOOLS_DIR" "$BUILD_ROOT" "$INSTALL_ROOT" "$THIRDPARTY_ROOT"

# CMake finds Python by the name "python". Keep that mapping private to this
# build instead of changing the host's Python installation.
cat > "$TOOLS_DIR/python" <<'EOF'
#!/bin/sh
exec /usr/bin/python3 "$@"
EOF
chmod 0755 "$TOOLS_DIR/python"
REAL_CMAKE=$(PATH="/opt/homebrew/bin:$PATH" command -v cmake || true)
[[ -n "$REAL_CMAKE" ]] || die 'CMake was not found on PATH'
REAL_CMAKE=$(cd "$(dirname "$REAL_CMAKE")" && pwd -P)/$(basename "$REAL_CMAKE")
export PATH="$TOOLS_DIR:/opt/homebrew/bin:$PATH"

# build-3rdparty.sh replaces COMMON_OPTS for iOS. Inject the policy option at
# configure time while leaving build, -E, and script-mode invocations alone.
cat > "$TOOLS_DIR/cmake" <<EOF
#!/bin/sh
case "\${1-}" in
    --build|-E|-P|--help|-h|--version)
        exec "$REAL_CMAKE" "\$@"
        ;;
    *)
        exec "$REAL_CMAKE" "\$@" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        ;;
esac
EOF
chmod 0755 "$TOOLS_DIR/cmake"
export cmake="$TOOLS_DIR/cmake"

export WORKSPACE="$SOURCE_DIR"
export NG_BUILD_ROOT="$BUILD_ROOT"
export NG_INSTALL_ROOT="$INSTALL_ROOT"
export NG_THIRDPARTY_ROOT="$THIRDPARTY_ROOT"
export KEEP_TFW_PACKAGE=true
export ARCHIVE_ROOT=

printf 'Building GFXBench third-party dependencies.\n'
(
    cd "$SOURCE_DIR"
    PRODUCT_ID=gfxbench_metal PRODUCT_NAME="$APP_NAME" PRODUCT_VERSION="$APP_VERSION" \
        BUNDLE_DATA=true PLATFORM=ios CONFIG=Release APPLICATION_TYPE=gui \
        COMMON_OPTS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        ./scripts/build-3rdparty.sh
)

printf 'Building GFXBench Metal iOS app.\n'
(
    cd "$SOURCE_DIR"
    DROIDVM_UNSIGNED_APP=true PRODUCT_ID=gfxbench_metal PRODUCT_NAME="$APP_NAME" \
        BENCHMARK_DIR="$SOURCE_DIR/gfxbench" PRODUCT_VERSION="$APP_VERSION" \
        BUNDLE_DATA=true PLATFORM=ios CONFIG=Release APPLICATION_TYPE=gui \
        COMMON_OPTS=-DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        ./scripts/build.sh
)

APP_CANDIDATES=()
while IFS= read -r -d '' candidate; do
    APP_CANDIDATES+=("$candidate")
done < <(find "$BUILD_ROOT/ios/app_ios" -type d -name 'app_ios.app' -prune -print0 2>/dev/null || true)
[[ ${#APP_CANDIDATES[@]} -eq 1 ]] || die "expected one built app_ios.app under $BUILD_ROOT/ios/app_ios, found ${#APP_CANDIDATES[@]}"
BUILT_APP=${APP_CANDIDATES[0]}
[[ -f "$BUILT_APP/Info.plist" ]] || die "built app is missing Info.plist: $BUILT_APP"

STAGING_DIR=$(mktemp -d "$OUTPUT_DIR/.gfxbench-stage.XXXXXXXX")
STAGED_APP="$STAGING_DIR/GFXBench.app"
ditto "$BUILT_APP" "$STAGED_APP"
cp "$PACKAGE_DIR/LICENSE" "$STAGED_APP/LICENSE"
cp "$PACKAGE_DIR/source-pin.json" "$STAGED_APP/SourcePin.json"

IOS_SDK_VERSION=$(xcrun --sdk iphoneos --show-sdk-version)
IOS_SDK_PATH=$(xcrun --sdk iphoneos --show-sdk-path)
XCODE_VERSION=$(xcodebuild -version | tr '\n' ';' | sed 's/;$//')
"$TOOLS_DIR/python" - "$STAGED_APP/Info.plist" "$STAGED_APP/BuildProvenance.json" \
    "$PINNED_REVISION" "$PINNED_PATCH_SHA256" "$IOS_SDK_VERSION" "$IOS_SDK_PATH" "$XCODE_VERSION" <<'PY'
import json
import plistlib
import sys
from pathlib import Path

plist_path, provenance_path = map(Path, sys.argv[1:3])
with plist_path.open('rb') as stream:
    info = plistlib.load(stream)
info.update({
    'CFBundleIdentifier': 'org.droidvm.gfxbench',
    'CFBundleName': 'GFXBench Metal',
    'CFBundleDisplayName': 'GFXBench Metal',
    'CFBundleShortVersionString': '5.0.0',
    'CFBundleVersion': '5.0.0',
    'MinimumOSVersion': '17.0',
    # The guest's panel is landscape-native (1366x1024): iPadOS's portrait is its wide, upright screen and its landscape is
    # sideways, so an app that allows only landscape on iPad (GFXBench's own list) is drawn turned 90 degrees.
    'UISupportedInterfaceOrientations~ipad': ['UIInterfaceOrientationPortrait', 'UIInterfaceOrientationPortraitUpsideDown',
                                              'UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'],
})
plist_path.write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_XML, sort_keys=True))
provenance = {
    'source_repository': 'https://github.com/Kishonti-Opensource/gfxbench.git',
    'source_revision': sys.argv[3],
    'compatibility_patch_sha256': sys.argv[4],
    'product': 'GFXBench Metal',
    'product_id': 'gfxbench_metal',
    'product_version': '5.0.0',
    'target': 'iphoneos; iOS 17.0 minimum; Release',
    'iphoneos_sdk': {'version': sys.argv[5], 'path': sys.argv[6]},
    'xcode_version': sys.argv[7],
}
provenance_path.write_text(json.dumps(provenance, indent=2) + '\n')
PY

ENTITLEMENTS="$INTERMEDIATE_DIR/gfxbench-entitlements.plist"
"$TOOLS_DIR/python" - "$ENTITLEMENTS" <<'PY'
import plistlib
import sys
from pathlib import Path

Path(sys.argv[1]).write_bytes(plistlib.dumps({
    'com.apple.private.security.no-container': True,
    'com.apple.security.iokit-user-client-class': [
        'IOSurfaceRootUserClient',
        'AGXDeviceUserClient',
    ],
    # SpringBoard runs an app in classic mode (a 768x1024 canvas, pillarboxed) when the screen type matches none it was built
    # for, which the guest's wide 1366x1024 portrait never does; an app with this entitlement bypasses classic mode
    # (-[SBApplication _bypassesClassicMode]) and gets the whole screen.
    'com.apple.UIKit.vends-view-services': True,
}))
PY
codesign --force --sign - --pagesize 4096 --identifier org.droidvm.gfxbench \
    --entitlements "$ENTITLEMENTS" "$STAGED_APP"
SIGNATURE=$(codesign --display --verbose=4 "$STAGED_APP" 2>&1)
grep -q '^Page size=4096$' <<<"$SIGNATURE" || die 'codesign did not produce 4K pages'
CDHASH=$(sed -n 's/^CDHash=\([[:xdigit:]]\{40\}\)$/\1/p' <<<"$SIGNATURE" | head -n 1)
[[ -n "$CDHASH" ]] || die 'codesign output did not contain a CDHash'

FINAL_APP="$OUTPUT_DIR/GFXBench.app"
rm -rf "$FINAL_APP"
mv "$STAGED_APP" "$FINAL_APP"
rmdir "$STAGING_DIR"

TRUST_DIR="$TREE_ROOT/build/owner-trust-extra"
mkdir -p "$TRUST_DIR"
BUNDLE_EXECUTABLE=$("$TOOLS_DIR/python" -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))["CFBundleExecutable"])' "$FINAL_APP/Info.plist")
[[ -x "$FINAL_APP/$BUNDLE_EXECUTABLE" ]] || die "app executable is missing: $BUNDLE_EXECUTABLE"
"$TOOLS_DIR/python" - "$TRUST_DIR/gfxbench.json" "$CDHASH" "$FINAL_APP/$BUNDLE_EXECUTABLE" "$TREE_ROOT" <<'PY'
import json
import sys
from pathlib import Path

trust_path = Path(sys.argv[1])
binary = Path(sys.argv[3]).resolve()
tree_root = Path(sys.argv[4]).resolve()
try:
    binary_ref = binary.relative_to(tree_root).as_posix()
except ValueError:
    binary_ref = str(binary)
trust_path.write_text(json.dumps({
    'cdhashes': [sys.argv[2]],
    'category': 0,
    'binary': binary_ref,
    'source': 'package/gfxbench; iphoneos; source 89aa956f43836798d1eaaab8ec32d3dc8b02eead',
}, indent=2) + '\n')
PY

printf 'Built signed app: %s\n' "$FINAL_APP"
printf 'CDHash: %s\n' "$CDHASH"
printf 'Owner trust entry: %s\n' "$TRUST_DIR/gfxbench.json"

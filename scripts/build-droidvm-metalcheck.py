#!/usr/bin/env python3
"""Build the Metal Check iOS app and macOS CLI.

  build-droidvm-metalcheck.py TREE_ROOT

The iOS app is ad-hoc 4K-signed and trusted as a foreground app through the owner module
(build/owner-trust-extra/metalcheck.json, category 0). It includes the iOS default.metallib;
the host CLI and its macOS default.metallib are in build/metalcheck/host/.
"""
import hashlib
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

if len(sys.argv) != 2:
    raise SystemExit(__doc__)

TREE_ROOT = Path(sys.argv[1]).resolve()
SOURCE_DIR = Path(__file__).resolve().parents[1] / 'package/metalcheck'
OUTPUT = TREE_ROOT / 'build/metalcheck'
APP = OUTPUT / 'MetalCheck.app'
HOST = OUTPUT / 'host'
OUTPUT.mkdir(parents=True, exist_ok=True)
APP.mkdir(parents=True, exist_ok=True)
HOST.mkdir(parents=True, exist_ok=True)

IOS_SDK = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
MACOS_SDK = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
IOS_SDK_VERSION = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-version'], text=True).strip()
MACOS_SDK_VERSION = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True).strip()
XCODE_VERSION = subprocess.check_output(['xcodebuild', '-version'], text=True).strip()

APP_BINARY = APP / 'MetalCheck'
HOST_BINARY = HOST / 'metalcheck'
IOS_AIR = OUTPUT / 'Checks-iphoneos.air'
MACOS_AIR = OUTPUT / 'Checks-macosx.air'
IOS_METALLIB = APP / 'default.metallib'
MACOS_METALLIB = HOST / 'default.metallib'

def build_metallib(sdk, language, target, air, metallib):
    subprocess.run(['xcrun', '--sdk', sdk, 'metal', f'-std={language}', '-target', target,
                    '-c', SOURCE_DIR / 'Checks.metal', '-o', air], check=True)
    subprocess.run(['xcrun', '--sdk', sdk, 'metallib', air, '-o', metallib], check=True)


build_metallib('iphoneos', 'ios-metal2.3', 'air64-apple-ios17.0', IOS_AIR, IOS_METALLIB)
build_metallib('macosx', 'macos-metal2.3', 'air64-apple-macosx11.0', MACOS_AIR, MACOS_METALLIB)
IOS_AIR.unlink(missing_ok=True)
MACOS_AIR.unlink(missing_ok=True)

COMMON_FLAGS = ['-std=c++17', '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations']
FRAMEWORKS = ['-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
              '-framework', 'QuartzCore']
SOURCES = [SOURCE_DIR / 'Core.mm', SOURCE_DIR / 'App.mm']

subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', IOS_SDK,
                *COMMON_FLAGS, '-DMETALCHECK_APP=1', *FRAMEWORKS,
                '-framework', 'UIKit', '-framework', 'CoreGraphics', *SOURCES, '-o', APP_BINARY], check=True)
subprocess.run(['xcrun', 'clang++', '-isysroot', MACOS_SDK, *COMMON_FLAGS, *FRAMEWORKS,
                *SOURCES, '-o', HOST_BINARY], check=True)

info = {
    'CFBundleDevelopmentRegion': 'en',
    'CFBundleExecutable': 'MetalCheck',
    'CFBundleIdentifier': 'org.droidvm.metalcheck',
    'CFBundleInfoDictionaryVersion': '6.0',
    'CFBundleName': 'MetalCheck',
    'CFBundlePackageType': 'APPL',
    'CFBundleShortVersionString': '1.0',
    'CFBundleVersion': '1.0',
    'LSRequiresIPhoneOS': True,
    'LSSupportsOpeningDocumentsInPlace': True,
    'MinimumOSVersion': '17.0',
    'NSPrincipalClass': 'UIApplication',
    'UIDeviceFamily': [1, 2],
    'UIFileSharingEnabled': True,
    'UISupportedInterfaceOrientations': [
        'UIInterfaceOrientationPortrait',
        'UIInterfaceOrientationPortraitUpsideDown',
        'UIInterfaceOrientationLandscapeLeft',
        'UIInterfaceOrientationLandscapeRight',
    ],
}
(APP / 'Info.plist').write_bytes(plistlib.dumps(info))

ENTITLEMENTS = OUTPUT / 'metalcheck-entitlements.plist'
ENTITLEMENTS.write_bytes(plistlib.dumps({
    'com.apple.private.security.no-container': True,
    'com.apple.security.iokit-user-client-class': [
        'IOSurfaceRootUserClient',
        'AGXDeviceUserClient',
    ],
}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096',
                '--identifier', 'org.droidvm.metalcheck', '--entitlements', ENTITLEMENTS, APP], check=True)
signature = subprocess.run(['codesign', '--display', '--verbose=4', APP], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', signature, re.M), 'app executable was not signed with 4K pages'
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', signature, re.M)[1]

TRUST = TREE_ROOT / 'build/owner-trust-extra'
TRUST.mkdir(parents=True, exist_ok=True)
(TRUST / 'metalcheck.json').write_text(json.dumps({
    'cdhashes': [cdhash],
    'category': 0,
    'binary': 'build/metalcheck/MetalCheck.app/MetalCheck',
    'source': 'package/metalcheck (iphoneos arm64)',
}, indent=2) + '\n')

source_files = {
    'package/metalcheck/App.mm': SOURCE_DIR / 'App.mm',
    'package/metalcheck/Core.h': SOURCE_DIR / 'Core.h',
    'package/metalcheck/Core.mm': SOURCE_DIR / 'Core.mm',
    'package/metalcheck/Checks.metal': SOURCE_DIR / 'Checks.metal',
    'scripts/build-droidvm-metalcheck.py': Path(__file__).resolve(),
}
provenance = {
    'sources_sha256': {
        name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in source_files.items()
    },
    'sdks': {
        'iphoneos': {'version': IOS_SDK_VERSION, 'path': IOS_SDK},
        'macosx': {'version': MACOS_SDK_VERSION, 'path': MACOS_SDK},
    },
    'xcode_version': XCODE_VERSION,
    'targets': {
        'ios': 'arm64-apple-ios17.0; ios-metal2.3',
        'host': 'native macOS; macos-metal2.3; AIR air64-apple-macosx11.0',
    },
    'ios_cdhash': cdhash,
}
(OUTPUT / 'provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print('MetalCheck iOS CDHash', cdhash)

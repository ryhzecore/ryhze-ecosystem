#!/usr/bin/env bash
# Compile only. This is not a Developer ID signed, notarized or distributable app.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo "macOS compilation requires macOS and Xcode." >&2
  exit 2
fi
export CI=true BOT=true FLUTTER_SUPPRESS_ANALYTICS=true
ryhze_flutter="${RYHZE_FLUTTER:-flutter}"
"$ryhze_flutter" --suppress-analytics --no-version-check --version --machine | python3 packaging/apple/check_sdk.py
"$ryhze_flutter" --suppress-analytics --no-version-check build macos --release --config-only --no-pub
mkdir -p build/apple-verification
python3 packaging/apple/run_bounded.py 900 xcodebuild build \
  -workspace macos/Runner.xcworkspace -scheme Runner -configuration Release \
  -destination 'generic/platform=macOS' -derivedDataPath build/apple-verification/macos-unsigned \
  CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= DEVELOPMENT_TEAM= \
  2>&1 | tee build/apple-verification/macos-unsigned.log
test -d build/apple-verification/macos-unsigned/Build/Products/Release/Ryhze.app
python3 packaging/apple/check_privacy.py build/apple-verification/macos-unsigned/Build/Products/Release/Ryhze.app/Contents/Resources/PrivacyInfo.xcprivacy
echo "Unsigned macOS compilation completed; no notarization or release acceptance is implied."

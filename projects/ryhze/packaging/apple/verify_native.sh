#!/usr/bin/env bash
# iOS simulator verification and unsigned compilation; no distribution or profile retrieval.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo "Apple native verification requires macOS, Xcode and an iOS simulator runtime." >&2
  exit 2
fi
export CI=true BOT=true FLUTTER_SUPPRESS_ANALYTICS=true
ryhze_flutter="${RYHZE_FLUTTER:-flutter}"
evidence="$(pwd)/build/apple-verification"
mkdir -p "$evidence"
"$ryhze_flutter" --suppress-analytics --no-version-check --version --machine > "$evidence/flutter-version.json"
python3 packaging/apple/check_sdk.py < "$evidence/flutter-version.json"
git rev-parse HEAD > "$evidence/source-commit.txt"
git status --porcelain=v2 --branch > "$evidence/source-status.txt"
xcodebuild -version > "$evidence/xcode-version.txt"
sw_vers > "$evidence/macos-version.txt"
"$ryhze_flutter" --suppress-analytics --no-version-check pub get --enforce-lockfile
"$ryhze_flutter" --suppress-analytics --no-version-check analyze --no-pub
"$ryhze_flutter" --suppress-analytics --no-version-check test --no-pub
python3 -m unittest discover -s packaging/apple -p 'test_*.py'
python3 -m unittest discover -s packaging/windows -p 'test_component_inventory.py'

# Require an already-installed runtime; do not download runtimes or accept licenses.
simulator="$(xcrun simctl list devices available -j | python3 -c '
import json,sys
devices = [(runtime,d) for runtime,group in json.load(sys.stdin)["devices"].items()
           if "iOS" in runtime for d in group if d.get("isAvailable") and "iPhone" in d["name"]]
if not devices: sys.exit("No available iPhone simulator. Install an authorized runtime first.")
devices.sort(key=lambda pair: pair[1]["state"] != "Booted")
print(devices[0][1]["udid"])
')"
xcrun simctl list devices available -j > "$evidence/simulator-devices.json"
if ! xcrun simctl list devices booted | grep -Fq "$simulator"; then
  xcrun simctl boot "$simulator"
fi
python3 packaging/apple/run_bounded.py 300 xcrun simctl bootstatus "$simulator" -b
python3 packaging/apple/run_bounded.py 900 "$ryhze_flutter" --suppress-analytics --no-version-check build ios --simulator --debug --no-pub

run_tests() {
  local platform="$1" phase="$2"
  local destination="$3"
  local only_tests=()
  case "$phase" in
    core) only_tests=(-only-testing:RunnerTests/RunnerTests/testKeychainRoundTripAndScopeIsolation -only-testing:RunnerTests/RunnerTests/testDurableSnapshotReopensAndRejectsStaleWrite -only-testing:RunnerTests/RunnerTests/testUnsupportedSchemaIsPreserved -only-testing:RunnerTests/RunnerTests/testStateSymlinksAreRejected -only-testing:RunnerTests/RunnerTests/testAtomicMigrationPreservesBackupAndReopens -only-testing:RunnerTests/RunnerTests/testInterruptedMigrationRollsBackBothWrites -only-testing:RunnerTests/RunnerTests/testFutureDocumentSchemaIsPreserved) ;;
    seed) only_tests=(-only-testing:RunnerTests/RunnerTests/testKeychainSeed) ;;
    restore) only_tests=(-only-testing:RunnerTests/RunnerTests/testKeychainRestoreAfterProcessRestart) ;;
  esac
  local result="$evidence/$platform-$phase-$(date +%s).xcresult"
  python3 packaging/apple/run_bounded.py 900 xcodebuild test \
    -workspace "$platform/Runner.xcworkspace" -scheme Runner -configuration Debug \
    -destination "$destination" -parallel-testing-enabled NO \
    -derivedDataPath "$evidence/ios-native" \
    -resultBundlePath "$result" "${only_tests[@]}" \
    CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual \
    DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER= \
    2>&1 | tee "$evidence/$platform-$phase.log"
  python3 packaging/apple/check_native_tests.py "$phase" "$evidence/$platform-$phase.log"
}

for phase in core seed restore; do
  xcrun simctl terminate "$simulator" com.ryhze.ryhze 2>/dev/null || true
  run_tests ios "$phase" "platform=iOS Simulator,id=$simulator"
done
codesign --display --entitlements - --xml \
  "$evidence/ios-native/Build/Products/Debug-iphonesimulator/Runner.app" \
  > "$evidence/ios-simulator-entitlements.plist" 2> "$evidence/ios-simulator-codesign.txt"
python3 packaging/apple/run_bounded.py 900 "$ryhze_flutter" --suppress-analytics --no-version-check build ios --release --no-codesign --no-pub
python3 packaging/apple/verify_package.py build/ios/iphoneos/Runner.app

bash packaging/apple/build_macos_unsigned.sh

# Reuse the shared offline inventory; hash each actual Apple build's notices.
# New output directories preserve evidence from previous verification attempts.
"$ryhze_flutter" --suppress-analytics --no-version-check pub deps --json > "$evidence/dependencies.json"
inventory="$(mktemp -d "$evidence/component-inventory.XXXXXX")"
for platform in ios macos; do
  case "$platform" in
    ios) notices=build/ios/iphoneos/Runner.app/Frameworks/App.framework/flutter_assets/NOTICES.Z ;;
    macos) notices=build/apple-verification/macos-unsigned/Build/Products/Release/Ryhze.app/Contents/Frameworks/App.framework/Versions/A/Resources/flutter_assets/NOTICES.Z ;;
  esac
  python3 packaging/windows/component_inventory.py \
    --package-config .dart_tool/package_config.json --dependencies "$evidence/dependencies.json" \
    --lockfile pubspec.lock --sdk "$evidence/flutter-version.json" --fonts assets/fonts \
    --notices "$notices" --output "$inventory/$platform"
done
echo "NOT RUN: macOS Keychain acceptance needs an existing authorized signing identity and provisioning profile. Run verify_macos_credentials.sh separately." | tee "$evidence/macos-credential-status.txt"
echo "iOS simulator checks and unsigned Apple compilation completed. macOS Keychain, physical devices and release readiness remain unverified."

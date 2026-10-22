#!/usr/bin/env bash
# Manual internal TestFlight preparation, existing identities only. No dispatch.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo "TestFlight preparation requires an authorized macOS/Xcode host and existing signing resources." >&2
  exit 2
fi
export CI=true BOT=true FLUTTER_SUPPRESS_ANALYTICS=true
export RYHZE_GOLDEN_DIAGNOSTICS=1
unset RYHZE_GOLDEN_CANDIDATES
ryhze_flutter="${RYHZE_FLUTTER:-flutter}"
evidence="$(pwd)/build/apple-testflight"
private="$(pwd)/build/apple-testflight-private"
if [[ -e "$evidence" || -e "$private" ]]; then
  echo "Preserve existing evidence and signing outputs; use a fresh checkout." >&2
  exit 2
fi
mkdir -p "$evidence" "$private"
chmod 700 "$private"
deadline=$((SECONDS + 3300))
cleanup_reserve=30

# Every direct command shares the 55-minute budget. Keep 30 seconds outside
# command execution for at most 10 seconds of process-group cleanup and the exit
# trap/artifact cleanup. No nested timeouts; the provider's hard limit is 60 min.
run_phase() {
  local name="$1" cap="$2" requested="$2" remaining
  shift 2
  remaining=$((deadline - SECONDS - cleanup_reserve))
  if ((remaining < cap)); then cap="$remaining"; fi
  if ((cap < 0)); then cap=0; fi
  printf '{"requested_seconds":%d,"allowed_seconds":%d,"cleanup_reserve_seconds":%d}\n' \
    "$requested" "$cap" "$cleanup_reserve" > "$evidence/$name-budget.json" || return
  if ((cap <= 0)); then
    echo "TestFlight preparation exhausted its script budget; no IPA may be published." >&2
    return 124
  fi
  python3 packaging/apple/run_bounded.py "$cap" "$@" 2>&1 | tee "$evidence/$name.log"
}

on_exit() {
  local result=$?
  trap - EXIT
  # Only this run's final gated copy is removed. Private outputs are retained on
  # the ephemeral builder, but never selected by publishing artifact globs.
  if ((result != 0)); then
    rm -f "$evidence/publish/Ryhze-internal-37.ipa" "$evidence/publish/Ryhze-internal-37.tmp"
    echo "TestFlight preparation failed; no validated IPA is available for publishing." >&2
  fi
  exit "$result"
}
trap on_exit EXIT

git rev-parse HEAD > "$evidence/source-commit.txt"
git status --porcelain=v2 --branch > "$evidence/source-status.txt"
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Signed preparation requires a clean source checkout." >&2
  exit 2
fi
cat > "$evidence/acceptance-limits.txt" <<'LIMITS'
This lane requires strict shared tests and actual simulator XCTest before signing.
Simulator success is not physical-device acceptance. A validated signed IPA is
not Apple processing, tester-group assignment, external review or release approval.
No certificate/profile creation, device registration or legal acceptance is allowed.
The coordinator must verify existing free allowance before the single manual run.
LIMITS
run_phase sdk 60 "$ryhze_flutter" --suppress-analytics --no-version-check --version --machine > "$evidence/flutter-version.json"
run_phase sdk-pin 30 python3 packaging/apple/check_sdk.py < "$evidence/flutter-version.json"
ryhze_dart="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["flutterRoot"] + "/bin/dart")' < "$evidence/flutter-version.json")"
run_phase xcode 30 xcodebuild -version
if ! grep -Fxq 'Xcode 26.6' "$evidence/xcode.log"; then
  echo "Expected the reviewed Xcode 26.6 host; stop before signing." >&2
  exit 2
fi
run_phase macos 30 sw_vers
run_phase cli 30 app-store-connect --version
run_phase latest-before 120 python3 packaging/apple/testflight_gate.py latest-live > "$evidence/latest-before.json"
run_phase signing-existing 120 python3 packaging/apple/testflight_gate.py signing "$private/signing" --diagnostics "$evidence/signing-diagnostics.json" > "$evidence/signing-preflight.json"

run_phase dependencies 300 "$ryhze_flutter" --suppress-analytics --no-version-check pub get --enforce-lockfile
run_phase analyze 180 "$ryhze_flutter" --suppress-analytics --no-version-check analyze --no-pub
run_phase format 60 "$ryhze_dart" format --output=none --set-exit-if-changed lib test
# No diagnostic candidate mode, missing-reference fallback, update or tolerance.
run_phase flutter-tests 450 "$ryhze_flutter" --suppress-analytics --no-version-check test --no-pub --machine
run_phase apple-packaging 180 python3 -m unittest discover -s packaging/apple -p 'test_*.py' -v
run_phase shared-packaging 90 python3 -m unittest discover -s packaging/windows -p 'test_*.py' -v

# A cold CoreSimulator service may need longer than a short metadata query.
# Retry a timed-out enumeration once, after its owned process group has ended.
# Keep stdout JSON separate from stderr/timeout logs, and never select a partial
# failed attempt. Both attempts consume the same global budget.
simulator_devices=
for attempt in 1 2; do
  attempt_json="$evidence/simulator-devices-$attempt.json"
  if run_phase "simulator-devices-$attempt" 180 bash -c \
    'exec xcrun simctl list devices available -j > "$1"' ryhze-simulator "$attempt_json"; then
    simulator_devices="$attempt_json"
    break
  else
    enumeration_status=$?
    if ((enumeration_status != 124 || attempt == 2)); then exit "$enumeration_status"; fi
  fi
done
simulator="$(run_phase simulator-selection 30 python3 packaging/apple/select_simulator.py \
  "$simulator_devices" --output "$evidence/simulator-selection.json")"
# bootstatus -b boots if necessary and waits for readiness on this exact device.
# No second service enumeration, global reset, runtime install or GUI startup.
run_phase simulator-ready 300 xcrun simctl bootstatus "$simulator" -b
# Flutter config-only avoids its implicit provisioning/device-registration flags.
# Direct XCTest builds and signs the simulator host ad-hoc, preserving Keychain.
run_phase simulator-config 180 "$ryhze_flutter" --suppress-analytics --no-version-check build ios --simulator --debug --config-only --no-codesign --no-pub
# Xcode owns test-host installation and launch. This run has only configured
# Flutter before core; a standalone terminate adds an unnecessary cold-service
# request. Separate invocations remain mandatory, and the native restore test
# requires the seed's persisted process ID to differ from its current host PID.
# Do not infer process absence from simctl errors or add an unverified liveness
# probe. Native command failures/timeouts still fail the lane within its budget.
for phase in core seed restore; do
  selectors=()
  case "$phase" in
    core) selectors=(-only-testing:RunnerTests/RunnerTests/testKeychainRoundTripAndScopeIsolation -only-testing:RunnerTests/RunnerTests/testDurableSnapshotReopensAndRejectsStaleWrite -only-testing:RunnerTests/RunnerTests/testUnsupportedSchemaIsPreserved -only-testing:RunnerTests/RunnerTests/testStateSymlinksAreRejected -only-testing:RunnerTests/RunnerTests/testAtomicMigrationPreservesBackupAndReopens -only-testing:RunnerTests/RunnerTests/testInterruptedMigrationRollsBackBothWrites -only-testing:RunnerTests/RunnerTests/testFutureDocumentSchemaIsPreserved) ;;
    seed) selectors=(-only-testing:RunnerTests/RunnerTests/testKeychainSeed) ;;
    restore) selectors=(-only-testing:RunnerTests/RunnerTests/testKeychainRestoreAfterProcessRestart) ;;
  esac
  run_phase "ios-$phase" 900 xcodebuild test \
    -workspace ios/Runner.xcworkspace -scheme Runner -configuration Debug \
    -destination "platform=iOS Simulator,id=$simulator" -parallel-testing-enabled NO \
    -derivedDataPath "$private/ios-native" -resultBundlePath "$evidence/ios-$phase.xcresult" \
    "${selectors[@]}" CODE_SIGNING_ALLOWED=YES CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual \
    DEVELOPMENT_TEAM= PROVISIONING_PROFILE_SPECIFIER=
  run_phase "ios-$phase-required" 30 python3 packaging/apple/check_native_tests.py "$phase" "$evidence/ios-$phase.log"
done
run_phase simulator-entitlements 30 codesign --display --entitlements - --xml \
  "$private/ios-native/Build/Products/Debug-iphonesimulator/Runner.app"
# This launch is distinct from the nine native test hosts. A PID alone does not
# establish rendered Flutter UI. Bind the debug-only probe to this new launch;
# read its app-container marker without repeatedly querying CoreSimulator.
probe_nonce="$(run_phase simulator-probe-request 30 python3 packaging/apple/simulator_ui_readiness.py prepare "$private/ui-request.json")"
run_phase simulator-container 30 bash -c \
  'exec xcrun simctl get_app_container "$1" com.ryhze.ryhze data > "$2"' \
  ryhze-simulator "$simulator" "$private/simulator-container.txt"
run_phase simulator-launch 30 env SIMCTL_CHILD_RYHZE_FIRST_FRAME_NONCE="$probe_nonce" \
  xcrun simctl launch --terminate-running-process "$simulator" com.ryhze.ryhze
run_phase simulator-first-frame 90 python3 packaging/apple/simulator_ui_readiness.py wait \
  "$private/ui-request.json" "$evidence/simulator-launch.log" "$private/simulator-container.txt" \
  --timeout 80 --report "$evidence/simulator-first-frame.json"
run_phase simulator-screenshot 30 xcrun simctl io "$simulator" screenshot "$evidence/ios-simulator.png"
run_phase simulator-capture-check 30 python3 packaging/apple/simulator_ui_readiness.py capture \
  "$private/ui-request.json" "$evidence/simulator-launch.log" "$private/simulator-container.txt" \
  "$evidence/ios-simulator.png" --report "$evidence/simulator-capture.json"

version="$(python3 -c 'import sys; sys.path.insert(0,"packaging/apple"); from verify_package import source_version; print(source_version()[0])')"
# Override only the generated Apple build. Shared pubspec remains 0.6.0+3.
run_phase release-config 180 "$ryhze_flutter" --suppress-analytics --no-version-check build ios --release --config-only --no-codesign --no-pub --build-number=37
run_phase runner-signing 30 python3 packaging/apple/runner_signing.py generate \
  "$private/signing/selection.json" "$private/signing/runner-signing.xcconfig"
# Standard signing overrides on xcodebuild's command line apply to SwiftPM
# dependencies too. Only Runner's optional Release/iphoneos config includes the
# selected profile. Raw effective settings stay private (paths/signing metadata).
run_phase archive-build-settings 120 bash -c \
  'exec xcodebuild -showBuildSettings -json -workspace ios/Runner.xcworkspace -scheme Runner -configuration Release -destination "generic/platform=iOS" > "$1" 2> "$2"' \
  ryhze-signing "$private/archive-build-settings.json" "$private/archive-build-settings.stderr"
run_phase runner-signing-check 30 python3 packaging/apple/runner_signing.py verify-build-settings \
  "$private/signing/selection.json" "$private/archive-build-settings.json"
run_phase archive 900 xcodebuild archive \
  -workspace ios/Runner.xcworkspace -scheme Runner -configuration Release \
  -destination 'generic/platform=iOS' -archivePath "$private/Runner.xcarchive"
run_phase export 300 xcodebuild -exportArchive -archivePath "$private/Runner.xcarchive" \
  -exportPath "$private/export" -exportOptionsPlist "$private/signing/export-options.plist"
shopt -s nullglob
ipas=("$private/export/"*.ipa)
if ((${#ipas[@]} != 1)); then
  echo "Expected exactly one exported IPA; no publishing artifact will be exposed." >&2
  exit 2
fi
run_phase signed-package 120 python3 packaging/apple/testflight_gate.py signed-ipa "${ipas[0]}" "$private/signing/selection.json" \
  --diagnostics "$evidence/signed-package-diagnostics.json" > "$evidence/signed-package.json"
run_phase dependency-graph 60 "$ryhze_flutter" --suppress-analytics --no-version-check pub deps --json > "$evidence/dependencies.json"
run_phase component-inventory 60 python3 packaging/windows/component_inventory.py \
  --package-config .dart_tool/package_config.json --dependencies "$evidence/dependencies.json" \
  --lockfile pubspec.lock --sdk "$evidence/flutter-version.json" --fonts assets/fonts \
  --notices "$private/Runner.xcarchive/Products/Applications/Runner.app/Frameworks/App.framework/flutter_assets/NOTICES.Z" \
  --output "$evidence/component-inventory/ios"
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Tracked source changed during preparation; stop before publishing." >&2
  exit 2
fi
# Repeat the global latest-number lookup after all expensive steps. This is not
# an atomic reservation: dispatch one build only; Apple still rejects collisions.
run_phase latest-before-publish 120 python3 packaging/apple/testflight_gate.py latest-live > "$evidence/latest-before-publish.json"
# This is the only IPA glob selected by Codemagic. Intermediate signed output is
# never eligible, including on a failed script. The copy also consumes the shared
# budget; timeout cleanup removes both this run's temporary and final artifact.
run_phase publish-artifact 30 bash -c '
  set -e
  mkdir "$2"
  cp "$1" "$2/Ryhze-internal-37.tmp"
  mv "$2/Ryhze-internal-37.tmp" "$2/Ryhze-internal-37.ipa"
' ryhze-publish "${ipas[0]}" "$evidence/publish"
echo "Validated $version (37) for internal TestFlight upload. Apple processing and physical-device acceptance remain pending."

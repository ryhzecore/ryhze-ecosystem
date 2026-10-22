#!/usr/bin/env bash
# Manual compilation only. The coordinator must verify free allowance before dispatch.
# No simulator, native credential tests, signing acquisition, or distribution.
set -euo pipefail
cd "$(dirname "$0")/../.."
if [[ "$(uname -s)" != Darwin ]]; then
  echo "Unsigned Apple compilation requires macOS and Xcode." >&2
  exit 2
fi
export CI=true BOT=true FLUTTER_SUPPRESS_ANALYTICS=true
export RYHZE_GOLDEN_DIAGNOSTICS=1
export RYHZE_GOLDEN_CANDIDATES=1
ryhze_flutter="${RYHZE_FLUTTER:-flutter}"
evidence="$(pwd)/build/apple-unsigned-validation"
if [[ -e "$evidence" ]]; then
  echo "Evidence output already exists; preserve it and use a fresh checkout." >&2
  exit 2
fi
mkdir -p "$evidence"
deadline=$((SECONDS + 1500))

# Preserve images even when the strict Flutter test phase fails under set -e.
# Keep the original failure status; evidence failure cannot turn a build green.
capture_evidence_on_exit() {
  local result=$? remaining cap capture_status=passed capture_code=0
  trap - EXIT
  remaining=$((deadline - SECONDS))
  cap=30
  if ((remaining < cap)); then cap="$remaining"; fi
  if ((cap <= 0)); then
    capture_status=not_run
    capture_code=124
    echo "No script budget remains for archiving; inspect retained raw files." >&2
    if ((result == 0)); then result=1; fi
  elif python3 packaging/apple/run_bounded.py "$cap" python3 packaging/apple/capture_test_evidence.py; then
    capture_status=passed
  else
    capture_code=$?
    capture_status=failed
    echo "Flutter visual evidence capture failed; inspect retained raw files." >&2
    if ((result == 0)); then result=1; fi
  fi
  if declare -F record_phase >/dev/null; then
    if ! record_phase evidence-archive "$capture_status" "$capture_code"; then result=1; fi
    if ! record_final_status "$result"; then result=1; fi
  fi
  exit "$result"
}
trap capture_evidence_on_exit EXIT

# Each phase owns one process group. Do not nest run_bounded.py invocations:
# a nested new session would escape the outer phase's termination signal.
run_phase() {
  local name="$1" cap="$2" remaining
  shift 2
  phase_not_started=0
  remaining=$((deadline - SECONDS))
  if ((remaining <= 0)); then
    echo "Unsigned validation exhausted its 25-minute script budget." >&2
    phase_not_started=1
    return 124
  fi
  if ((remaining < cap)); then cap="$remaining"; fi
  python3 packaging/apple/run_bounded.py "$cap" "$@" 2>&1 | tee "$evidence/$name.log"
}
source packaging/apple/verification_status.sh

git rev-parse HEAD > "$evidence/source-commit.txt"
git status --porcelain=v2 --branch > "$evidence/source-status.txt"
cat > "$evidence/acceptance-limits.txt" <<'LIMITS'
Unsigned compilation and shared tests only. Native XCTest, Keychain access,
simulator/device behavior, signing, TestFlight and notarization are NOT RUN.
No provider authentication or production operation is enabled by this lane.
LIMITS
run_phase sdk 60 "$ryhze_flutter" --suppress-analytics --no-version-check --version --machine > "$evidence/flutter-version.json"
run_phase sdk-pin 30 python3 packaging/apple/check_sdk.py < "$evidence/flutter-version.json"
ryhze_dart="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["flutterRoot"] + "/bin/dart")' < "$evidence/flutter-version.json")"
python3 - "$evidence/flutter-version.json" > "$evidence/font-hashes.json" <<'FONT_HASHES'
import hashlib, json, pathlib, sys
sdk = json.loads(pathlib.Path(sys.argv[1]).read_text())
paths = {'Inter': pathlib.Path('assets/fonts/Inter.ttf'),
         'SpaceGrotesk': pathlib.Path('assets/fonts/SpaceGrotesk.ttf'),
         'MaterialIcons': pathlib.Path(sdk['flutterRoot']) / 'bin/cache/artifacts/material_fonts/MaterialIcons-Regular.otf'}
print(json.dumps({name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}, indent=2))
FONT_HASHES
run_phase xcode 30 xcodebuild -version
run_phase macos 30 sw_vers
run_phase dependencies 300 "$ryhze_flutter" --suppress-analytics --no-version-check pub get --enforce-lockfile
run_phase analyze 180 "$ryhze_flutter" --suppress-analytics --no-version-check analyze --no-pub
run_phase format 60 "$ryhze_dart" format --output=none --set-exit-if-changed lib test
run_recorded flutter-tests 300 "$ryhze_flutter" --suppress-analytics --no-version-check test --no-pub --machine || true
run_recorded apple-packaging 60 python3 -m unittest discover -s packaging/apple -p 'test_*.py' -v || true
run_recorded shared-packaging 60 python3 -m unittest discover -s packaging/windows -p 'test_*.py' -v || true

ios_compiled=0
macos_compiled=0
if run_recorded ios-unsigned 600 "$ryhze_flutter" --suppress-analytics --no-version-check build ios --release --no-codesign --no-pub; then
  ios_compiled=1
  run_recorded ios-package 60 python3 packaging/apple/verify_package.py build/ios/iphoneos/Runner.app || true
else
  skip_phase ios-package 'Unsigned iOS compilation did not pass.'
fi
macos_app="$evidence/macos/Build/Products/Release/Ryhze.app"
if run_recorded macos-config 180 "$ryhze_flutter" --suppress-analytics --no-version-check build macos --release --config-only --no-pub; then
  if run_recorded macos-unsigned 600 xcodebuild build \
    -workspace macos/Runner.xcworkspace -scheme Runner -configuration Release \
    -destination 'generic/platform=macOS' -derivedDataPath "$evidence/macos" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= DEVELOPMENT_TEAM= \
    PROVISIONING_PROFILE_SPECIFIER= RYHZE_MACOS_PROVISIONING_PROFILE=; then
    if run_recorded macos-bundle 30 test -d "$macos_app"; then
      macos_compiled=1
      run_recorded macos-privacy 30 python3 packaging/apple/check_privacy.py "$macos_app/Contents/Resources/PrivacyInfo.xcprivacy" || true
    else
      skip_phase macos-privacy 'Unsigned macOS app bundle is missing.'
    fi
  else
    skip_phase macos-bundle 'Unsigned macOS compilation did not pass.'
    skip_phase macos-privacy 'Unsigned macOS compilation did not pass.'
  fi
else
  skip_phase macos-unsigned 'macOS configuration did not pass.'
  skip_phase macos-bundle 'macOS configuration did not pass.'
  skip_phase macos-privacy 'macOS configuration did not pass.'
fi

dependency_graph=0
if run_recorded dependency-graph 60 "$ryhze_flutter" --suppress-analytics --no-version-check pub deps --json > "$evidence/dependencies.json"; then
  dependency_graph=1
fi
for platform in ios macos; do
  if ((dependency_graph == 0)) ||
     [[ "$platform" == ios && "$ios_compiled" == 0 ]] ||
     [[ "$platform" == macos && "$macos_compiled" == 0 ]]; then
    skip_phase "$platform-inventory" 'Requires a successful build and dependency graph.'
    continue
  fi
  case "$platform" in
    ios) notices=build/ios/iphoneos/Runner.app/Frameworks/App.framework/flutter_assets/NOTICES.Z ;;
    macos) notices="$macos_app/Contents/Frameworks/App.framework/Versions/A/Resources/flutter_assets/NOTICES.Z" ;;
  esac
  run_recorded "$platform-inventory" 60 python3 packaging/windows/component_inventory.py \
    --package-config .dart_tool/package_config.json --dependencies "$evidence/dependencies.json" \
    --lockfile pubspec.lock --sdk "$evidence/flutter-version.json" --fonts assets/fonts \
    --notices "$notices" --output "$evidence/component-inventory/$platform" || true
done
finish_verification

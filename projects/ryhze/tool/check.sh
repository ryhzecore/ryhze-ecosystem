#!/usr/bin/env bash
set -euo pipefail
: "${FLUTTER:=flutter}"
export CI=true BOT=true FLUTTER_SUPPRESS_ANALYTICS=true
"$FLUTTER" --suppress-analytics --no-version-check pub get
"$FLUTTER" --suppress-analytics --no-version-check analyze
"$FLUTTER" --suppress-analytics --no-version-check test --reporter expanded
"$FLUTTER" --suppress-analytics --no-version-check build web --release --no-web-resources-cdn

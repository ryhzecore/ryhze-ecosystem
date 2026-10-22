#!/usr/bin/env bash
# Developer packaging helper. Input must be built inside the matching Flatpak SDK.
# Produces an unsigned local test bundle; does not install or publish it.
set -euo pipefail
if [[ $# != 4 ]]; then
  echo "Usage: $0 RUNTIME_BUILT_BUNDLE INITIALIZED_BUILD_DIR LOCAL_REPO OUTPUT.flatpak" >&2
  exit 2
fi
bundle=$(realpath "$1")
build_dir=$(realpath "$2")
repo=$(realpath -m "$3")
output=$(realpath -m "$4")
source_root=$(cd "$(dirname "$0")/../.." && pwd)
test -x "$bundle/ryhze"
test -f "$bundle/lib/libflutter_linux_gtk.so"
test -f "$bundle/lib/libapp.so"
test -f "$build_dir/metadata"
test ! -e "$build_dir/files/ryhze"
test ! -e "$output"
grep -qx 'name=com.ryhze.ryhze' "$build_dir/metadata"
grep -qx 'runtime=org.gnome.Platform/x86_64/50' "$build_dir/metadata"
mkdir -p "$build_dir/files/bin" "$(dirname "$output")"
cp -a "$bundle" "$build_dir/files/ryhze"
cat > "$build_dir/files/bin/ryhze" <<'EOF'
#!/bin/sh
exec /app/ryhze/ryhze "$@"
EOF
chmod 755 "$build_dir/files/bin/ryhze"
install -Dm644 "$source_root/packaging/linux/com.ryhze.ryhze.desktop" "$build_dir/files/share/applications/com.ryhze.ryhze.desktop"
install -Dm644 "$source_root/web/icons/Icon-512.png" "$build_dir/files/share/icons/hicolor/512x512/apps/com.ryhze.ryhze.png"
install -Dm644 "$source_root/packaging/linux/com.ryhze.ryhze.metainfo.xml" "$build_dir/files/share/metainfo/com.ryhze.ryhze.metainfo.xml"
flatpak build-finish --no-inherit-permissions --command=ryhze \
  --socket=wayland --socket=fallback-x11 --share=ipc --device=dri "$build_dir"
flatpak build-export "$repo" "$build_dir" deck-test
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  "$repo" "$output" com.ryhze.ryhze deck-test
sha256sum "$output"

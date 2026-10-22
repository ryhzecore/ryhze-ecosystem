#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -x .tools/rust/bin/rustc ]]; then export PATH="$PWD/.tools/rust/bin:$PATH"; fi
[[ "$(rustc --version)" == 'rustc 1.85.0 '* ]] || { echo 'Rust 1.85.0 required' >&2; exit 1; }
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-1}" RUST_TEST_THREADS="${RUST_TEST_THREADS:-1}" RUST_BACKTRACE=1 SOURCE_DATE_EPOCH=0
ulimit -c 0
ulimit -v 2097152
ulimit -t 180
ulimit -f 131072
cargo fmt --all -- --check
timeout 180s cargo clippy --offline --locked --workspace --all-targets -- -D warnings
timeout 180s cargo build --offline --locked -p axeon-tensor-runtime
timeout 300s cargo test --offline --locked --workspace --no-fail-fast -- --nocapture
timeout 180s cargo build --offline --locked --release -p axeon-tensor-runtime
timeout 300s cargo test --offline --locked --workspace --release --no-fail-fast -- --nocapture
timeout 30s cargo run --offline --locked --release -p axeon-numerics --example cpu_numerics

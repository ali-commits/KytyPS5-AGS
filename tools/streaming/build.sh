#!/bin/sh
# Build the GPL launcher and independent MIT native host. Never install into a live prefix.
set -eu
kyty_source=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
kyty_build=${KYTY_STREAM_BUILD_DIR:-"$kyty_source/_Build/streaming-release"}
host_source=${KYTY_STREAM_HOST_SOURCE:-"$kyty_source/../../streaming/ags"}
kyty_jobs=${KYTY_BUILD_JOBS:-6}
kyty_stage=${KYTY_STREAM_STAGE:-"$kyty_build/streaming-stage"}

if [ ! -f "$host_source/Cargo.toml" ]; then
    printf '%s\n' 'Set KYTY_STREAM_HOST_SOURCE to the independent streaming/ags workspace.' >&2
    exit 1
fi
cmake -S "$kyty_source" -B "$kyty_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DKYTY_BUILD_LAUNCHER=ON -DKYTY_BUILD_CONTROLLER_AUDIO_RECEIVER=OFF -DBUILD_TESTING=ON -DKYTY_EMULATOR_IPO=ON
cmake --build "$kyty_build" --target kyty_emulator launcher launcher_streaming_tests launcher_host_tests -j "$kyty_jobs"
cargo build --manifest-path "$host_source/Cargo.toml" --release -p ags-stream --bins
cargo test --manifest-path "$host_source/Cargo.toml" -p ags-stream -p ags-session
AGS_TEST_HOST_BINARY="$host_source/target/release/ags-host" \
    ctest --test-dir "$kyty_build" -R '^(launcher_streaming|launcher_host)$' --output-on-failure
if [ -e "$kyty_stage" ]; then
    printf '%s\n' 'Build passed. Choose a fresh KYTY_STREAM_STAGE before staging.' >&2
    exit 1
fi
cmake --install "$kyty_build" --prefix "$kyty_stage"
printf 'Staged launcher: %s\nNative host and sibling helpers: %s\n' "$kyty_stage" "$host_source/target/release"

#!/bin/sh
# Build both independent programs; this script never installs over a normal edition.
set -eu
kyty_source=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
kyty_build=${KYTY_STREAM_BUILD_DIR:-"$kyty_source/_Build/streaming-release"}
host_source=${KYTY_STREAM_HOST_SOURCE:-"/home/ali/projects/kyty-streaming-sunshine"}
host_build=${KYTY_STREAM_HOST_BUILD:-"$host_source/cmake-build-kyty"}
kyty_jobs=${KYTY_BUILD_JOBS:-6}
kyty_stage=${KYTY_STREAM_STAGE:-"$kyty_build/streaming-stage"}
kyty_install_prefix=${KYTY_STREAM_PREFIX:-"$HOME/.local/opt/kytyps5-streaming/20261016-native-v3"}

if [ ! -f "$host_source/CMakeLists.txt" ]; then
    printf '%s\n' 'Set KYTY_STREAM_HOST_SOURCE to the separate, patched Sunshine checkout.' >&2
    exit 1
fi
host_revision_file="$kyty_source/tools/streaming/host-source.rev"
if ! host_revision=$(cat "$host_revision_file"); then
    printf 'Cannot read required host revision from %s. Restore the revision file.\n' "$host_revision_file" >&2
    exit 1
fi
case "$host_revision" in
    ''|*[!0-9a-fA-F]*) host_revision_valid=no ;;
    *) host_revision_valid=yes ;;
esac
if [ "$host_revision_valid" != yes ] || [ "${#host_revision}" -ne 40 ]; then
    printf 'Invalid host revision in %s; expected one 40-digit hexadecimal Git commit.\n' "$host_revision_file" >&2
    exit 1
fi
host_revision=$(printf '%s' "$host_revision" | tr 'A-F' 'a-f')
if ! host_actual_revision=$(git -C "$host_source" rev-parse --verify HEAD); then
    printf 'Use a Git host checkout at revision %s: %s\n' "$host_revision" "$host_source" >&2
    exit 1
fi
if [ "$host_actual_revision" != "$host_revision" ]; then
    printf 'Host revision mismatch: expected %s, found %s. Set KYTY_STREAM_HOST_SOURCE to a clean checkout at the required revision.\n' "$host_revision" "$host_actual_revision" >&2
    exit 1
fi
if ! host_submodules=$(git -C "$host_source" submodule status --recursive); then
    printf 'Cannot verify host dependencies at revision %s. Check the recursive submodule configuration in %s.\n' "$host_revision" "$host_source" >&2
    exit 1
fi
if printf '%s\n' "$host_submodules" | LC_ALL=C grep '^[+-U]' >/dev/null; then
    printf 'Host dependencies do not match revision %s. In %s, initialize/update the recorded dependencies with git submodule update --init --recursive.\n%s\n' "$host_revision" "$host_source" "$host_submodules" >&2
    exit 1
fi
if ! host_changes=$(git --no-optional-locks -C "$host_source" status --porcelain --untracked-files=all --ignore-submodules=none); then
    printf 'Cannot verify a clean host checkout at revision %s: %s\n' "$host_revision" "$host_source" >&2
    exit 1
fi
if [ -n "$host_changes" ]; then
    printf 'Host checkout or dependencies contain source changes. Preserve those changes and use a clean checkout at revision %s before building:\n%s\n' "$host_revision" "$host_changes" >&2
    exit 1
fi
cmake -S "$kyty_source" -B "$kyty_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DKYTY_BUILD_LAUNCHER=ON -DKYTY_BUILD_CONTROLLER_AUDIO_RECEIVER=ON -DBUILD_TESTING=ON -DKYTY_EMULATOR_IPO=ON
cmake --build "$kyty_build" --target kyty_emulator launcher streaming_tests streaming_gpu_tests audio_output_device_tests launcher_streaming_tests controller_audio_tests controller_audio_producer_tests controller_audio_negotiation_tests controller_audio_native_routing_tests controller_audio_pipeline_producer controller-audio-receiver controller_audio_receiver_tests controller_audio_no_hardware -j "$kyty_jobs"

cmake -S "$host_source" -B "$host_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
    -DCMAKE_INSTALL_PREFIX="$kyty_install_prefix" -DSUNSHINE_ASSETS_DIR=share/kyty-stream-host \
    -DCMAKE_PROJECT_INCLUDE= -DSUNSHINE_CUDA_ARCHITECTURES=86 \
    -DSUNSHINE_ENABLE_CUDA=ON -DSUNSHINE_ENABLE_TRAY=OFF \
    -DSUNSHINE_ENABLE_WEB_UI=OFF -DBUILD_DOCS=OFF -DBUILD_TESTS=ON
cmake --build "$host_build" --target sunshine test_sunshine -j "$kyty_jobs"
ctest --test-dir "$kyty_build" -R '^(streaming_protocol|streaming_gpu|audio_output_device|launcher_streaming|controller_audio_.*)$' --output-on-failure
kyty_host_gcov=$(mktemp -d "$host_build/controller-audio-gcov.XXXXXX")
GCOV_PREFIX="$kyty_host_gcov" \
KYTY_AUDIO_NATIVE_FIXTURE="$kyty_build/controller_audio_pipeline_producer" \
KYTY_AUDIO_RECEIVER="$kyty_build/tools/controller-audio/controller-audio-receiver" \
KYTY_AUDIO_NO_HARDWARE="$kyty_build/tools/controller-audio/libcontroller_audio_no_hardware.so" \
cmake -E chdir "$host_build" "$host_build/tests/test_sunshine" --gtest_filter='Kyty*'

if [ -e "$kyty_stage" ]; then
    printf '%s\n' 'Build passed. Choose a fresh KYTY_STREAM_STAGE before staging.' >&2
    exit 1
fi
cmake --install "$kyty_build" --prefix "$kyty_stage"
printf 'Staged Kyty: %s\nStreaming host: %s\n' "$kyty_stage" "$host_build/sunshine"

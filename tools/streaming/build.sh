#!/bin/sh
# Build both independent programs; this script never installs over a normal edition.
set -eu
kyty_source=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
kyty_build=${KYTY_STREAM_BUILD_DIR:-"$kyty_source/_Build/streaming-release"}
host_source=${KYTY_STREAM_HOST_SOURCE:-"/home/ali/projects/kyty-streaming-sunshine"}
host_build=${KYTY_STREAM_HOST_BUILD:-"$host_source/cmake-build-kyty"}
kyty_jobs=${KYTY_BUILD_JOBS:-6}
kyty_stage=${KYTY_STREAM_STAGE:-"$kyty_build/streaming-stage"}

if [ ! -f "$host_source/CMakeLists.txt" ]; then
    printf '%s\n' 'Set KYTY_STREAM_HOST_SOURCE to the separate, patched Sunshine checkout.' >&2
    exit 1
fi
cmake -S "$kyty_source" -B "$kyty_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DKYTY_BUILD_LAUNCHER=ON -DBUILD_TESTING=ON -DKYTY_EMULATOR_IPO=ON
cmake --build "$kyty_build" --target kyty_emulator launcher streaming_tests -j "$kyty_jobs"

mkdir -p "$host_build"
# Upstream overwrites CMAKE_CUDA_ARCHITECTURES. Defer the local RTX3090 setting.
cat > "$host_build/kyty-cuda-arch.cmake" <<'CMAKE'
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND PROJECT_NAME STREQUAL "Sunshine")
    function(kyty_limit_cuda_architectures)
        foreach(kyty_cuda_target IN ITEMS sunshine test_sunshine)
            if(TARGET ${kyty_cuda_target})
                set_property(TARGET ${kyty_cuda_target} PROPERTY CUDA_ARCHITECTURES 86)
            endif()
        endforeach()
    endfunction()
    cmake_language(DEFER CALL kyty_limit_cuda_architectures)
endif()
CMAKE
cmake -S "$host_source" -B "$host_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=/opt/cuda/bin/nvcc \
    -DCMAKE_PROJECT_INCLUDE="$host_build/kyty-cuda-arch.cmake" \
    -DSUNSHINE_ENABLE_CUDA=ON -DSUNSHINE_ENABLE_TRAY=OFF \
    -DSUNSHINE_ENABLE_WEB_UI=OFF -DBUILD_DOCS=OFF -DBUILD_TESTS=ON
cmake --build "$host_build" --target sunshine test_sunshine -j "$kyty_jobs"
ctest --test-dir "$kyty_build" -R '^streaming_protocol$' --output-on-failure
"$host_build/tests/test_sunshine" --gtest_filter='Kyty*'

if [ -e "$kyty_stage" ]; then
    printf '%s\n' 'Build passed. Choose a fresh KYTY_STREAM_STAGE before staging.' >&2
    exit 1
fi
cmake --install "$kyty_build" --prefix "$kyty_stage"
printf 'Staged Kyty: %s\nStreaming host: %s\n' "$kyty_stage" "$host_build/sunshine"

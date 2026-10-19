#!/usr/bin/env bash
set -euo pipefail
native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cmake -S "$native_dir" -B "${KYTY_NATIVE_BUILD_DIR:-$native_dir/build}" -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@"
cmake --build "${KYTY_NATIVE_BUILD_DIR:-$native_dir/build}" --parallel

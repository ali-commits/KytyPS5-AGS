#!/usr/bin/env bash
# Standalone runtime-only Linux build; no CMake or development symlinks required.
set -euo pipefail
native_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source_root="${KYTY_SOURCE_ROOT:-$(cd "$native_dir/../../.." && pwd)}"
build_dir="${KYTY_NATIVE_BUILD_DIR:-$native_dir/build-runtime}"
sdl_include="${KYTY_SDL_INCLUDE:-/usr/include}"
opus_include="${KYTY_OPUS_INCLUDE:-/usr/include/opus}"
source_hash="$(sha256sum "$source_root/src/libs/dualSenseBluetooth.cpp")"
source_hash="${source_hash%% *}"
mkdir -p "$build_dir"
"${CXX:-c++}" -std=c++20 -O2 -fPIC -pthread -I"$native_dir/shim" -I"$source_root/src" \
  -I"$sdl_include" -I"$opus_include" -DSDL_hid_write=kyty_bt_hid_write -DSDL_hid_open_path=kyty_bt_hid_open_path -DSDL_hid_close=kyty_bt_hid_close \
  -c "$source_root/src/libs/dualSenseBluetooth.cpp" -o "$build_dir/original.o"
"${CXX:-c++}" -std=c++20 -O2 -fPIC -pthread -I"$source_root/src" -I"$sdl_include" \
  "-DKYTY_BT_SOURCE_SHA256=\"$source_hash\"" \
  -c "$native_dir/bridge.cpp" -o "$build_dir/bridge.o"
"${CXX:-c++}" -shared -pthread -Wl,-soname,libkyty_companion_bt.so \
  "$build_dir/original.o" "$build_dir/bridge.o" -l:libSDL3.so.0 -l:libopus.so.0 \
  -o "$build_dir/libkyty_companion_bt.so"
printf '%s\n' "$build_dir/libkyty_companion_bt.so"

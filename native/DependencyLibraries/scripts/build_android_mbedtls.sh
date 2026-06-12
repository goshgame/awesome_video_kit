#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=android_build_common.sh
source "$SCRIPT_DIR/android_build_common.sh"

TARGET="${1:-${ANDROID_TARGET:-all}}"
if [[ "$TARGET" != "all" ]]; then
  TARGET="$(canonicalize_android_abi "$TARGET")"
fi

build_mbedtls_for_abi() {
  local abi="$1"
  local build_dir
  local install_prefix

  build_dir="$MBEDTLS_BUILD_ROOT/$abi"
  install_prefix="$(mbedtls_prefix_for_abi "$abi")"

  log_step "Build mbedTLS for Android ($abi)"
  printf 'BUILD_DIR=%s\n' "$build_dir"
  printf 'PREFIX=%s\n' "$install_prefix"

  rm -rf "$build_dir" "$install_prefix"
  mkdir -p "$build_dir" "$install_prefix"

  cmake -S "$MBEDTLS_SOURCE_DIR" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
    -DCMAKE_SYSTEM_NAME=Android \
    -DCMAKE_ANDROID_NDK="$ANDROID_NDK_ROOT" \
    -DCMAKE_ANDROID_ARCH_ABI="$abi" \
    -DCMAKE_ANDROID_API="$ANDROID_API" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_INSTALL_PREFIX="$install_prefix"

  cmake --build "$build_dir" --parallel "$JOBS"
  cmake --install "$build_dir"
}

main() {
  require_command cmake
  require_dir "$MBEDTLS_SOURCE_DIR"
  configure_android_toolchain
  validate_android_api_for_target "$TARGET"
  print_android_environment
  run_for_android_target "$TARGET" build_mbedtls_for_abi
}

main "$@"

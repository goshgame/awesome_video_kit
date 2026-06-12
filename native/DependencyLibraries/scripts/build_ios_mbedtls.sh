#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ios_build_common.sh
source "$SCRIPT_DIR/ios_build_common.sh"

TARGET="${1:-${IOS_TARGET:-all}}"

build_mbedtls_for_sdk() {
  local sdk_name="$1"
  local arch
  local build_dir
  local install_prefix

  arch="$(ios_arch_for_sdk "$sdk_name")"
  build_dir="$MBEDTLS_BUILD_ROOT/$sdk_name"
  install_prefix="$(mbedtls_prefix_for_sdk "$sdk_name")"

  log_step "Build mbedTLS for $sdk_name ($arch)"
  printf 'BUILD_DIR=%s\n' "$build_dir"
  printf 'PREFIX=%s\n' "$install_prefix"

  rm -rf "$build_dir" "$install_prefix"
  mkdir -p "$build_dir" "$install_prefix"

  cmake -S "$MBEDTLS_SOURCE_DIR" -B "$build_dir" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk_name" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_IOS_VERSION" \
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
  validate_ios_arches
  print_ios_environment
  run_for_ios_target "$TARGET" build_mbedtls_for_sdk
}

main "$@"

#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=android_build_common.sh
source "$SCRIPT_DIR/android_build_common.sh"

TARGET="${1:-${ANDROID_TARGET:-all}}"
if [[ "$TARGET" != "all" ]]; then
  TARGET="$(canonicalize_android_abi "$TARGET")"
fi

FETCH_SOURCES="${FETCH_SOURCES:-0}"

main() {
  configure_android_toolchain
  validate_android_api_for_target "$TARGET"
  print_android_environment

  if [[ "$FETCH_SOURCES" == "1" ]]; then
    "$SCRIPT_DIR/fetch_dependency_sources.sh"
  fi

  "$SCRIPT_DIR/build_android_x264.sh" "$TARGET"
  "$SCRIPT_DIR/build_android_mbedtls.sh" "$TARGET"
  "$SCRIPT_DIR/build_android_ffmpeg.sh" "$TARGET"

  log_step "Done"
  printf 'FFmpeg headers and static libraries were installed to: %s\n' "$FFMPEG_INSTALL_ROOT"
}

main "$@"

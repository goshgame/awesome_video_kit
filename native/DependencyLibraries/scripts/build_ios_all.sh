#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ios_build_common.sh
source "$SCRIPT_DIR/ios_build_common.sh"

TARGET="${1:-${IOS_TARGET:-all}}"
FETCH_SOURCES="${FETCH_SOURCES:-0}"

main() {
  validate_ios_arches
  print_ios_environment

  if [[ "$FETCH_SOURCES" == "1" ]]; then
    "$SCRIPT_DIR/fetch_dependency_sources.sh"
  fi

  "$SCRIPT_DIR/build_ios_x264.sh" "$TARGET"
  "$SCRIPT_DIR/build_ios_mbedtls.sh" "$TARGET"
  "$SCRIPT_DIR/build_ios_ffmpeg.sh" "$TARGET"

  log_step "Done"
  printf 'FFmpeg headers and static libraries were installed to: %s\n' "$FFMPEG_INSTALL_ROOT"
}

main "$@"

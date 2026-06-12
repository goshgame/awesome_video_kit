#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ios_build_common.sh
source "$SCRIPT_DIR/ios_build_common.sh"

FFMPEG_REPO="${FFMPEG_REPO:-https://github.com/FFmpeg/FFmpeg.git}"
FFMPEG_REF="${FFMPEG_REF:-n6.1.1}"
X264_REPO="${X264_REPO:-https://code.videolan.org/videolan/x264.git}"
X264_REF="${X264_REF:-master}"
MBEDTLS_REPO="${MBEDTLS_REPO:-https://github.com/Mbed-TLS/mbedtls.git}"
MBEDTLS_REF="${MBEDTLS_REF:-v3.6.5}"

fetch_git_source() {
  local name="$1"
  local repo="$2"
  local ref="$3"
  local dest="$4"
  local use_submodules="$5"

  log_step "Fetch $name"
  printf 'repo=%s\n' "$repo"
  printf 'ref=%s\n' "$ref"
  printf 'dest=%s\n' "$dest"

  if [[ -d "$dest/.git" ]]; then
    git -C "$dest" fetch --tags origin
  elif [[ -e "$dest" ]]; then
    fail "$dest already exists but is not a git checkout."
  else
    mkdir -p "$(dirname "$dest")"
    git clone "$repo" "$dest"
  fi

  git -C "$dest" checkout "$ref"

  if [[ "$use_submodules" == "1" ]]; then
    git -C "$dest" submodule update --init --recursive
  fi
}

main() {
  require_command git

  fetch_git_source "FFmpeg" "$FFMPEG_REPO" "$FFMPEG_REF" "$FFMPEG_SOURCE_DIR" 0
  fetch_git_source "x264" "$X264_REPO" "$X264_REF" "$X264_SOURCE_DIR" 0
  fetch_git_source "mbedTLS" "$MBEDTLS_REPO" "$MBEDTLS_REF" "$MBEDTLS_SOURCE_DIR" 1

  log_step "Done"
}

main "$@"

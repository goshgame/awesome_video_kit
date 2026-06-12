#!/bin/bash

if [ -z "${BASH_VERSION:-}" ]; then
  exec /bin/bash "$0" "$@"
fi

if set -o | grep -Eq '^posix[[:space:]]+on$'; then
  exec /bin/bash "$0" "$@"
fi

set -euo pipefail

SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_ROOT="$(cd "$SCRIPTS_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$DEPS_ROOT/.." && pwd)"

IOS_PROJECT_DIR="${IOS_PROJECT_DIR:-$PROJECT_ROOT/iOSAwesomeVideoKit}"
FFMPEG_SOURCE_DIR="${FFMPEG_SOURCE_DIR:-$DEPS_ROOT/FFmpeg-n6.1.1}"
MBEDTLS_SOURCE_DIR="${MBEDTLS_SOURCE_DIR:-$DEPS_ROOT/mbedtls/mbedtls}"
X264_SOURCE_DIR="${X264_SOURCE_DIR:-$DEPS_ROOT/x264}"
X264_ROOT="${X264_ROOT:-$DEPS_ROOT/build/x264-ios}"

MIN_IOS_VERSION="${MIN_IOS_VERSION:-13.0}"
IOS_DEVICE_ARCH="${IOS_DEVICE_ARCH:-arm64}"
IOS_SIMULATOR_ARCH="${IOS_SIMULATOR_ARCH:-arm64}"
SIZE_OPTFLAGS="${SIZE_OPTFLAGS:--Oz}"
ENABLE_LTO="${ENABLE_LTO:-1}"
DISABLE_LOGGING="${DISABLE_LOGGING:-1}"
AGGRESSIVE_SIZE="${AGGRESSIVE_SIZE:-0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

FFMPEG_SOURCE_COPY="${FFMPEG_SOURCE_COPY:-$DEPS_ROOT/build/$(basename "$FFMPEG_SOURCE_DIR")-ios-src}"
MBEDTLS_BUILD_ROOT="${MBEDTLS_BUILD_ROOT:-$DEPS_ROOT/build/mbedtls-ios}"
MBEDTLS_INSTALL_ROOT="${MBEDTLS_INSTALL_ROOT:-$MBEDTLS_SOURCE_DIR/out}"
FFMPEG_STAGE_ROOT="${FFMPEG_STAGE_ROOT:-$DEPS_ROOT/build/ffmpeg-ios-stage}"
FFMPEG_INSTALL_ROOT="${FFMPEG_INSTALL_ROOT:-$IOS_PROJECT_DIR/FFmpeg}"

log_step() {
  printf '\n==> %s\n' "$1"
}

fail() {
  printf 'Error: %s\n' "$1" >&2
  exit 1
}

require_command() {
  local cmd="$1"
  if [[ -z "$cmd" ]] || ! command -v "$cmd" >/dev/null 2>&1; then
    fail "Missing required command: $cmd"
  fi
}

require_dir() {
  local path="$1"
  [[ -d "$path" ]] || fail "Missing required directory: $path"
}

require_file() {
  local path="$1"
  [[ -f "$path" ]] || fail "Missing required file: $path"
}

ios_arch_for_sdk() {
  local sdk_name="$1"
  case "$sdk_name" in
    iphoneos)
      printf '%s\n' "$IOS_DEVICE_ARCH"
      ;;
    iphonesimulator)
      printf '%s\n' "$IOS_SIMULATOR_ARCH"
      ;;
    *)
      fail "Unsupported SDK: $sdk_name"
      ;;
  esac
}

output_suffix_for_sdk() {
  local sdk_name="$1"
  case "$sdk_name" in
    iphoneos)
      printf 'ios-%s\n' "$IOS_DEVICE_ARCH"
      ;;
    iphonesimulator)
      printf 'ios-%s-simulator\n' "$IOS_SIMULATOR_ARCH"
      ;;
    *)
      fail "Unsupported SDK: $sdk_name"
      ;;
  esac
}

mbedtls_prefix_for_sdk() {
  local sdk_name="$1"
  printf '%s/%s\n' "$MBEDTLS_INSTALL_ROOT" "$(output_suffix_for_sdk "$sdk_name")"
}

x264_prefix_for_sdk() {
  local sdk_name="$1"
  case "$sdk_name" in
    iphoneos)
      printf '%s\n' "$X264_ROOT/arm64"
      ;;
    iphonesimulator)
      printf '%s\n' "$X264_ROOT/simulator-arm64"
      ;;
    *)
      fail "Unsupported SDK: $sdk_name"
      ;;
  esac
}

ffmpeg_stage_prefix_for_sdk() {
  local sdk_name="$1"
  printf '%s/%s\n' "$FFMPEG_STAGE_ROOT" "$(output_suffix_for_sdk "$sdk_name")"
}

ffmpeg_install_prefix_for_sdk() {
  local sdk_name="$1"
  printf '%s/%s\n' "$FFMPEG_INSTALL_ROOT" "$(output_suffix_for_sdk "$sdk_name")"
}

validate_ios_arches() {
  if [[ "$IOS_DEVICE_ARCH" != "arm64" ]]; then
    fail "This project currently supports only IOS_DEVICE_ARCH=arm64."
  fi

  if [[ "$IOS_SIMULATOR_ARCH" != "arm64" ]]; then
    fail "This project currently expects IOS_SIMULATOR_ARCH=arm64."
  fi
}

print_ios_environment() {
  log_step "iOS dependency build environment"
  printf 'PROJECT_ROOT=%s\n' "$PROJECT_ROOT"
  printf 'IOS_PROJECT_DIR=%s\n' "$IOS_PROJECT_DIR"
  printf 'FFMPEG_SOURCE_DIR=%s\n' "$FFMPEG_SOURCE_DIR"
  printf 'MBEDTLS_SOURCE_DIR=%s\n' "$MBEDTLS_SOURCE_DIR"
  printf 'X264_SOURCE_DIR=%s\n' "$X264_SOURCE_DIR"
  printf 'X264_ROOT=%s\n' "$X264_ROOT"
  printf 'FFMPEG_INSTALL_ROOT=%s\n' "$FFMPEG_INSTALL_ROOT"
}

run_for_ios_target() {
  local target="$1"
  local callback="$2"

  case "$target" in
    all)
      "$callback" iphoneos
      "$callback" iphonesimulator
      ;;
    device|iphoneos)
      "$callback" iphoneos
      ;;
    simulator|iphonesimulator)
      "$callback" iphonesimulator
      ;;
    *)
      fail "Unsupported target: $target (expected: all|device|simulator)"
      ;;
  esac
}

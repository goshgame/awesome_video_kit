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

ANDROID_PROJECT_DIR="${ANDROID_PROJECT_DIR:-$PROJECT_ROOT/AndroidAwesomeVideoKit}"
FFMPEG_SOURCE_DIR="${FFMPEG_SOURCE_DIR:-$DEPS_ROOT/FFmpeg-n6.1.1}"
MBEDTLS_SOURCE_DIR="${MBEDTLS_SOURCE_DIR:-$DEPS_ROOT/mbedtls/mbedtls}"
X264_SOURCE_DIR="${X264_SOURCE_DIR:-$DEPS_ROOT/x264}"
X264_ROOT="${X264_ROOT:-$DEPS_ROOT/build/x264-android}"

ANDROID_API="${ANDROID_API:-21}"
SIZE_OPTFLAGS="${SIZE_OPTFLAGS:--Oz}"
ENABLE_LTO="${ENABLE_LTO:-1}"
DISABLE_LOGGING="${DISABLE_LOGGING:-1}"
AGGRESSIVE_SIZE="${AGGRESSIVE_SIZE:-0}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
PKG_CONFIG_BIN="${PKG_CONFIG_BIN:-$(command -v pkg-config 2>/dev/null || true)}"

FFMPEG_SOURCE_COPY="${FFMPEG_SOURCE_COPY:-$DEPS_ROOT/build/$(basename "$FFMPEG_SOURCE_DIR")-android-src}"
MBEDTLS_BUILD_ROOT="${MBEDTLS_BUILD_ROOT:-$DEPS_ROOT/build/mbedtls-android}"
MBEDTLS_INSTALL_ROOT="${MBEDTLS_INSTALL_ROOT:-$DEPS_ROOT/build/mbedtls-android-install}"
FFMPEG_STAGE_ROOT="${FFMPEG_STAGE_ROOT:-$DEPS_ROOT/build/ffmpeg-android-stage}"
FFMPEG_BUILD_ROOT="${FFMPEG_BUILD_ROOT:-$DEPS_ROOT/build/ffmpeg-android}"
FFMPEG_INSTALL_ROOT="${FFMPEG_INSTALL_ROOT:-$ANDROID_PROJECT_DIR/jniLibs}"

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

canonicalize_android_abi() {
  local abi="${1:-}"

  case "$abi" in
    arm64-v8a|arm64|aarch64|armv8-a)
      printf 'arm64-v8a\n'
      ;;
    armeabi-v7a|armeabi|armv7|armv7-a|v7a)
      printf 'armeabi-v7a\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

min_android_api_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf '21\n'
      ;;
    armeabi-v7a)
      printf '16\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

resolve_android_sdk_root() {
  local candidate

  for candidate in \
    "${ANDROID_SDK_ROOT:-}" \
    "${ANDROID_HOME:-}" \
    "$HOME/Library/Android/sdk" \
    "$HOME/Android/Sdk"
  do
    if [[ -n "$candidate" && -d "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  fail "Unable to locate Android SDK. Set ANDROID_SDK_ROOT or ANDROID_HOME."
}

resolve_ndk_root() {
  local sdk_root
  local version_name

  for sdk_root in "${ANDROID_NDK_ROOT:-}" "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK:-}" "${NDK_HOME:-}"; do
    if [[ -n "$sdk_root" && -d "$sdk_root" ]]; then
      printf '%s\n' "$sdk_root"
      return 0
    fi
  done

  sdk_root="$(resolve_android_sdk_root)"

  if [[ -d "$sdk_root/ndk-bundle" ]]; then
    printf '%s\n' "$sdk_root/ndk-bundle"
    return 0
  fi

  if [[ -d "$sdk_root/ndk" ]]; then
    version_name="$(
      find "$sdk_root/ndk" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; \
        | sort -t. -k1,1n -k2,2n -k3,3n \
        | tail -n 1
    )"
    if [[ -n "$version_name" && -d "$sdk_root/ndk/$version_name" ]]; then
      printf '%s\n' "$sdk_root/ndk/$version_name"
      return 0
    fi
  fi

  fail "Unable to locate Android NDK. Set ANDROID_NDK_ROOT or ANDROID_NDK_HOME."
}

resolve_ndk_host_tag() {
  local ndk_root="$1"
  local os_name
  local arch_name
  local candidate

  os_name="$(uname -s)"
  arch_name="$(uname -m)"

  case "$os_name" in
    Darwin)
      for candidate in "darwin-$arch_name" darwin-arm64 darwin-x86_64; do
        if [[ -d "$ndk_root/toolchains/llvm/prebuilt/$candidate" ]]; then
          printf '%s\n' "$candidate"
          return 0
        fi
      done
      ;;
    Linux)
      for candidate in "linux-$arch_name" linux-x86_64 linux-arm64; do
        if [[ -d "$ndk_root/toolchains/llvm/prebuilt/$candidate" ]]; then
          printf '%s\n' "$candidate"
          return 0
        fi
      done
      ;;
  esac

  fail "Unable to locate a usable NDK prebuilt toolchain under: $ndk_root"
}

configure_android_toolchain() {
  if [[ ! "$ANDROID_API" =~ ^[0-9]+$ ]]; then
    fail "ANDROID_API must be an integer, got: $ANDROID_API"
  fi

  ANDROID_NDK_ROOT="$(resolve_ndk_root)"
  NDK_HOST_TAG="$(resolve_ndk_host_tag "$ANDROID_NDK_ROOT")"
  TOOLCHAIN_DIR="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$NDK_HOST_TAG"
  SYSROOT="$TOOLCHAIN_DIR/sysroot"

  require_dir "$TOOLCHAIN_DIR"
  require_dir "$SYSROOT"
}

validate_android_api_for_target() {
  local target="$1"
  local minimum_android_api

  if [[ "$target" == "all" ]]; then
    minimum_android_api="$(min_android_api_for_abi "arm64-v8a")"
  else
    minimum_android_api="$(min_android_api_for_abi "$target")"
  fi

  if (( ANDROID_API < minimum_android_api )); then
    fail "ANDROID_API must be >= $minimum_android_api for $target."
  fi
}

mbedtls_output_dir_for_abi() {
  local abi="$1"
  case "$abi" in
    arm64-v8a)
      printf 'android-arm64\n'
      ;;
    armeabi-v7a)
      printf 'android-armv7a\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

mbedtls_prefix_for_abi() {
  local abi="$1"
  printf '%s/%s\n' "$MBEDTLS_INSTALL_ROOT" "$(mbedtls_output_dir_for_abi "$abi")"
}

x264_prefix_for_abi() {
  local abi="$1"
  printf '%s/android-%s\n' "$X264_ROOT" "$abi"
}

ffmpeg_stage_prefix_for_abi() {
  local abi="$1"
  printf '%s/android-%s\n' "$FFMPEG_STAGE_ROOT" "$abi"
}

ffmpeg_install_prefix_for_abi() {
  local abi="$1"
  printf '%s/%s\n' "$FFMPEG_INSTALL_ROOT" "$abi"
}

pkg_config_work_dir_for_abi() {
  local abi="$1"
  printf '%s/pkgconfig/%s\n' "$FFMPEG_BUILD_ROOT" "$abi"
}

toolchain_triple_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf 'aarch64-linux-android\n'
      ;;
    armeabi-v7a)
      printf 'armv7a-linux-androideabi\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

x264_host_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf 'aarch64-linux-android\n'
      ;;
    armeabi-v7a)
      printf 'arm-linux-androideabi\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

ffmpeg_arch_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf 'aarch64\n'
      ;;
    armeabi-v7a)
      printf 'arm\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

ffmpeg_cpu_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf 'armv8-a\n'
      ;;
    armeabi-v7a)
      printf 'armv7-a\n'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

android_arch_cflags_for_abi() {
  local abi="$1"

  case "$abi" in
    arm64-v8a)
      printf '%s\n' '-march=armv8-a'
      ;;
    armeabi-v7a)
      printf '%s\n' '-march=armv7-a -mthumb -mfpu=neon -mfloat-abi=softfp'
      ;;
    *)
      fail "Unsupported ANDROID_ABI: $abi"
      ;;
  esac
}

ffmpeg_base_cflags_for_abi() {
  local abi="$1"
  printf '%s %s\n' '-fPIC -ffunction-sections -fdata-sections -fvisibility=hidden -fomit-frame-pointer' "$(android_arch_cflags_for_abi "$abi")"
}

run_for_android_target() {
  local target="$1"
  local callback="$2"

  case "$target" in
    all)
      "$callback" arm64-v8a
      "$callback" armeabi-v7a
      ;;
    arm64-v8a|armeabi-v7a)
      "$callback" "$target"
      ;;
    *)
      fail "Unsupported target: $target (expected: all|arm64-v8a|armeabi-v7a)"
      ;;
  esac
}

print_android_environment() {
  log_step "Android dependency build environment"
  printf 'PROJECT_ROOT=%s\n' "$PROJECT_ROOT"
  printf 'ANDROID_PROJECT_DIR=%s\n' "$ANDROID_PROJECT_DIR"
  printf 'ANDROID_NDK_ROOT=%s\n' "${ANDROID_NDK_ROOT:-}"
  printf 'TOOLCHAIN_DIR=%s\n' "${TOOLCHAIN_DIR:-}"
  printf 'FFMPEG_SOURCE_DIR=%s\n' "$FFMPEG_SOURCE_DIR"
  printf 'MBEDTLS_SOURCE_DIR=%s\n' "$MBEDTLS_SOURCE_DIR"
  printf 'MBEDTLS_INSTALL_ROOT=%s\n' "$MBEDTLS_INSTALL_ROOT"
  printf 'X264_SOURCE_DIR=%s\n' "$X264_SOURCE_DIR"
  printf 'X264_ROOT=%s\n' "$X264_ROOT"
  printf 'FFMPEG_INSTALL_ROOT=%s\n' "$FFMPEG_INSTALL_ROOT"
}

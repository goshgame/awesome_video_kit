#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPENDENCY_LIBRARIES_DIR="${SCRIPT_DIR}/../DependencyLibraries"

normalize_android_abi() {
  local abi="${1:-}"

  case "$abi" in
    arm64-v8a|arm64|aarch64|armv8-a)
      printf 'arm64-v8a\n'
      ;;
    armeabi-v7a|armeabi|armv7|armv7-a|v7a)
      printf 'armeabi-v7a\n'
      ;;
    *)
      echo "Unsupported ANDROID_ABI: $abi" >&2
      exit 1
      ;;
  esac
}

resolve_android_abis() {
  local abi_request="${1:-all}"
  local abi_token
  local normalized_abi
  local seen_abis=""

  case "$abi_request" in
    all|ALL|both)
      printf 'arm64-v8a\n'
      printf 'armeabi-v7a\n'
      return 0
      ;;
  esac

  abi_request="${abi_request//,/ }"
  for abi_token in $abi_request; do
    normalized_abi="$(normalize_android_abi "$abi_token")"
    case " $seen_abis " in
      *" $normalized_abi "*)
        ;;
      *)
        printf '%s\n' "$normalized_abi"
        seen_abis="${seen_abis} ${normalized_abi}"
        ;;
    esac
  done
}

min_android_api_for_abi() {
  case "$1" in
    arm64-v8a)
      printf '21\n'
      ;;
    armeabi-v7a)
      printf '16\n'
      ;;
    *)
      echo "Unsupported ANDROID_ABI: $1" >&2
      exit 1
      ;;
  esac
}

mbedtls_output_dir_for_abi() {
  case "$1" in
    arm64-v8a)
      printf 'android-arm64\n'
      ;;
    armeabi-v7a)
      printf 'android-armv7a\n'
      ;;
    *)
      echo "Unsupported ANDROID_ABI: $1" >&2
      exit 1
      ;;
  esac
}

android_api_from_platform() {
  local platform="${1:-}"

  platform="${platform#android-}"
  if [[ ! "$platform" =~ ^[0-9]+$ ]]; then
    echo "Unsupported ANDROID_PLATFORM: $1 (expected: android-<api> or <api>)" >&2
    exit 1
  fi

  printf '%s\n' "$platform"
}

ABI_REQUEST="${ANDROID_ABI:-all}"
BUILD_DIR_OVERRIDE="${BUILD_DIR:-}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-21}"
BUILD_TYPE="${CMAKE_BUILD_TYPE:-MinSizeRel}"
STRIP_OUTPUT="${STRIP_OUTPUT:-auto}"
STRIP_MODE="${STRIP_MODE:-auto}"

is_debug_build() {
  #MinSizeRel
  case "$BUILD_TYPE" in
    Debug|debug)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_strip_mode() {
  case "$STRIP_OUTPUT" in
    0|false|FALSE|no|NO)
      printf 'none\n'
      return 0
      ;;
    1|true|TRUE|yes|YES)
      ;;
    auto)
      if is_debug_build; then
        printf 'none\n'
        return 0
      fi
      ;;
    *)
      echo "Unsupported STRIP_OUTPUT: $STRIP_OUTPUT (expected: auto|0|1)" >&2
      exit 1
      ;;
  esac

  case "$STRIP_MODE" in
    auto)
      if is_debug_build; then
        printf 'none\n'
      else
        printf 'unneeded\n'
      fi
      ;;
    debug|unneeded|none)
      printf '%s\n' "$STRIP_MODE"
      ;;
    *)
      echo "Unsupported STRIP_MODE: $STRIP_MODE (expected: auto|debug|unneeded|none)" >&2
      exit 1
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

  return 1
}

resolve_ndk_root() {
  local sdk_root
  local version_name

  for sdk_root in \
    "${ANDROID_NDK_ROOT:-}" \
    "${ANDROID_NDK_HOME:-}" \
    "${ANDROID_NDK:-}"
  do
    if [[ -n "$sdk_root" && -d "$sdk_root" ]]; then
      printf '%s\n' "$sdk_root"
      return 0
    fi
  done

  sdk_root="$(resolve_android_sdk_root)" || return 1

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

  return 1
}

NDK_DIR="$(resolve_ndk_root)" || {
  echo "Unable to locate Android NDK. Set ANDROID_NDK_ROOT/ANDROID_NDK_HOME or install it under the Android SDK directory." >&2
  exit 1
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

  return 1
}

NDK_HOST_TAG="$(resolve_ndk_host_tag "$NDK_DIR")" || {
  echo "Unable to locate an NDK prebuilt toolchain under: $NDK_DIR" >&2
  exit 1
}

STRIP_BIN="$NDK_DIR/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/llvm-strip"
if [[ ! -x "$STRIP_BIN" ]]; then
  echo "Unable to locate llvm-strip: $STRIP_BIN" >&2
  exit 1
fi

READELF_BIN="$NDK_DIR/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/llvm-readelf"
if [[ ! -x "$READELF_BIN" ]]; then
  echo "Unable to locate llvm-readelf: $READELF_BIN" >&2
  exit 1
fi

RESOLVED_STRIP_MODE="$(resolve_strip_mode)"
ANDROID_API_LEVEL="$(android_api_from_platform "$ANDROID_PLATFORM")"
ABIS=()
for abi in $(resolve_android_abis "$ABI_REQUEST"); do
  if [[ -n "$abi" ]]; then
    ABIS+=("$abi")
  fi
done

if (( ${#ABIS[@]} == 0 )); then
  echo "No Android ABI resolved from ANDROID_ABI: $ABI_REQUEST" >&2
  exit 1
fi

build_dir_for_abi() {
  local abi="$1"

  if [[ -n "$BUILD_DIR_OVERRIDE" ]]; then
    if (( ${#ABIS[@]} == 1 )); then
      printf '%s\n' "$BUILD_DIR_OVERRIDE"
    else
      printf '%s/%s\n' "$BUILD_DIR_OVERRIDE" "$abi"
    fi
    return 0
  fi

  printf '%s/build/%s\n' "$SCRIPT_DIR" "$abi"
}

verify_16kb_alignment() {
  local so_path="$1"
  local alignments
  local alignment
  local alignment_value

  alignments="$("$READELF_BIN" -l "$so_path" | awk '/^[[:space:]]*LOAD[[:space:]]/ { print $NF }')"
  if [[ -z "$alignments" ]]; then
    echo "Unable to determine LOAD segment alignment for: $so_path" >&2
    exit 1
  fi

  while IFS= read -r alignment; do
    [[ -n "$alignment" ]] || continue
    alignment_value=$((alignment))
    if (( alignment_value < 16384 || alignment_value % 16384 != 0 )); then
      echo "LOAD segment alignment is not 16 KB compatible for: $so_path (found: $alignment)" >&2
      exit 1
    fi
  done <<< "$alignments"

  echo "Verified 16 KB ELF alignment: $so_path"
}

build_one_abi() {
  local abi="$1"
  local build_dir
  local min_android_api
  local mbedtls_prefix
  local x264_prefix
  local output_so

  build_dir="$(build_dir_for_abi "$abi")"
  min_android_api="$(min_android_api_for_abi "$abi")"
  mbedtls_prefix="${MBEDTLS_PREFIX:-$DEPENDENCY_LIBRARIES_DIR/mbedtls/mbedtls/out/$(mbedtls_output_dir_for_abi "$abi")}"
  x264_prefix="${X264_PREFIX:-$DEPENDENCY_LIBRARIES_DIR/build/x264-android/android-$abi}"
  if [[ ! -f "$x264_prefix/lib/libx264.a" && -f "$DEPENDENCY_LIBRARIES_DIR/x264/libx264.a" ]]; then
    x264_prefix="$DEPENDENCY_LIBRARIES_DIR/x264"
  fi

  if (( ANDROID_API_LEVEL < min_android_api )); then
    echo "$abi requires ANDROID_PLATFORM >= android-$min_android_api, got: $ANDROID_PLATFORM" >&2
    exit 1
  fi

  if [[ ! -f "$mbedtls_prefix/lib/libmbedtls.a" ]]; then
    echo "Missing mbedTLS static library for $abi: $mbedtls_prefix/lib/libmbedtls.a" >&2
    exit 1
  fi

  if [[ ! -f "$x264_prefix/lib/libx264.a" && ! -f "$x264_prefix/libx264.a" ]]; then
    echo "Missing x264 static library for $abi: expected $x264_prefix/lib/libx264.a or $x264_prefix/libx264.a" >&2
    exit 1
  fi

  echo "Using Android NDK: $NDK_DIR"
  echo "Using Android ABI: $abi"
  echo "Using build type: $BUILD_TYPE"
  echo "Using strip mode: $RESOLVED_STRIP_MODE (raw output=$STRIP_OUTPUT raw mode=$STRIP_MODE)"

  rm -rf "$build_dir"
  mkdir -p "$build_dir"

  cmake -S "$SCRIPT_DIR" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK_DIR/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DMBEDTLS_PREFIX="$mbedtls_prefix" \
    -DX264_PREFIX="$x264_prefix"

  cmake --build "$build_dir" --config "$BUILD_TYPE"

  output_so="$(find "$build_dir" -type f -name 'libAwesomeVideoKit.so' | head -n 1)"
  if [[ -z "$output_so" ]]; then
    echo "Build finished but libAwesomeVideoKit.so was not found." >&2
    exit 1
  fi

  case "$RESOLVED_STRIP_MODE" in
    debug)
      "$STRIP_BIN" --strip-debug "$output_so"
      ;;
    unneeded)
      "$STRIP_BIN" --strip-unneeded "$output_so"
      ;;
    none)
      ;;
    *)
      echo "Unsupported resolved strip mode: $RESOLVED_STRIP_MODE" >&2
      exit 1
      ;;
  esac

  verify_16kb_alignment "$output_so"

  echo "$output_so"
}

for abi in "${ABIS[@]}"; do
  build_one_abi "$abi"
done

#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=ios_build_common.sh
source "$SCRIPT_DIR/ios_build_common.sh"

TARGET="${1:-${IOS_TARGET:-all}}"
BIT_DEPTH="${BIT_DEPTH:-8}"
CHROMA_FORMAT="${CHROMA_FORMAT:-420}"

build_x264_for_sdk() {
  local sdk_name="$1"
  local arch
  local sdk_path
  local cc
  local prefix
  local min_flag
  local common_cflags
  local common_ldflags
  local strip_bin

  arch="$(ios_arch_for_sdk "$sdk_name")"
  sdk_path="$(xcrun --sdk "$sdk_name" --show-sdk-path)"
  cc="$(xcrun --sdk "$sdk_name" -f clang)"
  prefix="$(x264_prefix_for_sdk "$sdk_name")"
  strip_bin="$(xcrun -f strip)"

  case "$sdk_name" in
    iphoneos)
      min_flag="-miphoneos-version-min=$MIN_IOS_VERSION"
      ;;
    iphonesimulator)
      min_flag="-mios-simulator-version-min=$MIN_IOS_VERSION"
      ;;
    *)
      fail "Unsupported SDK: $sdk_name"
      ;;
  esac

  common_cflags="-arch $arch -isysroot $sdk_path $min_flag $SIZE_OPTFLAGS -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables"
  common_ldflags="-arch $arch -isysroot $sdk_path $min_flag"

  log_step "Build x264 for $sdk_name ($arch)"
  printf 'PREFIX=%s\n' "$prefix"
  printf 'BIT_DEPTH=%s\n' "$BIT_DEPTH"
  printf 'CHROMA_FORMAT=%s\n' "$CHROMA_FORMAT"

  pushd "$X264_SOURCE_DIR" >/dev/null
  CC="$cc" ./configure \
    --host=arm-apple-darwin \
    --sysroot="$sdk_path" \
    --prefix="$prefix" \
    --enable-static \
    --enable-pic \
    --disable-cli \
    --disable-opencl \
    --disable-thread \
    --disable-interlaced \
    --disable-avs \
    --disable-swscale \
    --disable-lavf \
    --disable-ffms \
    --disable-gpac \
    --disable-lsmash \
    --disable-asm \
    --bit-depth="$BIT_DEPTH" \
    --chroma-format="$CHROMA_FORMAT" \
    --extra-cflags="$common_cflags" \
    --extra-ldflags="$common_ldflags"

  make clean
  make -j"$JOBS"
  make install-lib-static

  "$strip_bin" -S -x "$prefix/lib/libx264.a" || true
  popd >/dev/null
}

main() {
  require_command make
  require_command xcrun
  require_dir "$X264_SOURCE_DIR"
  require_file "$X264_SOURCE_DIR/configure"
  validate_ios_arches
  print_ios_environment
  run_for_ios_target "$TARGET" build_x264_for_sdk
}

main "$@"

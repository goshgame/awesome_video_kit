#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=android_build_common.sh
source "$SCRIPT_DIR/android_build_common.sh"

TARGET="${1:-${ANDROID_TARGET:-all}}"
if [[ "$TARGET" != "all" ]]; then
  TARGET="$(canonicalize_android_abi "$TARGET")"
fi

BIT_DEPTH="${BIT_DEPTH:-8}"
CHROMA_FORMAT="${CHROMA_FORMAT:-420}"

build_x264_for_abi() {
  local abi="$1"
  local min_android_api
  local clang_triple
  local host
  local prefix
  local cc
  local ar
  local ranlib
  local strip_bin
  local common_cflags
  local common_ldflags

  min_android_api="$(min_android_api_for_abi "$abi")"
  if (( ANDROID_API < min_android_api )); then
    fail "$abi requires ANDROID_API >= $min_android_api, got: $ANDROID_API"
  fi

  clang_triple="$(toolchain_triple_for_abi "$abi")"
  host="$(x264_host_for_abi "$abi")"
  prefix="$(x264_prefix_for_abi "$abi")"
  cc="$TOOLCHAIN_DIR/bin/${clang_triple}${ANDROID_API}-clang"
  ar="$TOOLCHAIN_DIR/bin/llvm-ar"
  ranlib="$TOOLCHAIN_DIR/bin/llvm-ranlib"
  strip_bin="$TOOLCHAIN_DIR/bin/llvm-strip"

  require_file "$cc"
  require_file "$ar"
  require_file "$ranlib"
  require_file "$strip_bin"

  common_cflags="--sysroot=$SYSROOT -fPIC $SIZE_OPTFLAGS -fomit-frame-pointer -fno-unwind-tables -fno-asynchronous-unwind-tables $(android_arch_cflags_for_abi "$abi")"
  common_ldflags="--sysroot=$SYSROOT"

  log_step "Build x264 for Android ($abi)"
  printf 'PREFIX=%s\n' "$prefix"
  printf 'ANDROID_API=%s\n' "$ANDROID_API"
  printf 'BIT_DEPTH=%s\n' "$BIT_DEPTH"
  printf 'CHROMA_FORMAT=%s\n' "$CHROMA_FORMAT"

  pushd "$X264_SOURCE_DIR" >/dev/null
  make distclean >/dev/null 2>&1 || true

  CC="$cc" \
  AR="$ar" \
  RANLIB="$ranlib" \
  STRIP="$strip_bin" \
  ./configure \
    --host="$host" \
    --sysroot="$SYSROOT" \
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
    --bit-depth="$BIT_DEPTH" \
    --chroma-format="$CHROMA_FORMAT" \
    --extra-cflags="$common_cflags" \
    --extra-ldflags="$common_ldflags"

  make -j"$JOBS"
  make install-lib-static
  "$strip_bin" --strip-debug "$prefix/lib/libx264.a" || true
  popd >/dev/null
}

main() {
  require_command make
  require_dir "$X264_SOURCE_DIR"
  require_file "$X264_SOURCE_DIR/configure"
  configure_android_toolchain
  validate_android_api_for_target "$TARGET"
  print_android_environment
  run_for_android_target "$TARGET" build_x264_for_abi
}

main "$@"
